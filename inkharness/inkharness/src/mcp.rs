//! MCP servers ([[mcp]] in inkharness.toml, and .mcp.json: place.rs) as tools: each server's tools join the tool set under
//! "<server>_<tool>", for the models and for run_js scripts alike.
//!
//! Servers are connected at startup, all at once. One that can't be reached is logged and left
//! out, so the assistant still talks without it. A server that stops later makes its calls
//! fail with a message the model can relay; there is no reconnection yet.

use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, Result, bail};
use reqwest::header::{HeaderName, HeaderValue};
use rmcp::ServiceExt;
use rmcp::model::{
    CallToolRequestParams, CallToolResult, ClientConfig, ContentBlock, ProtocolVersion,
};
use rmcp::service::{RoleClient, RunningService};
use rmcp::transport::streamable_http_client::StreamableHttpClientTransportConfig;
use rmcp::transport::{StreamableHttpClientTransport, TokioChildProcess};
use serde_json::Value;

use crate::config;

/// Reaching a server and listing its tools may take this long at startup.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
/// Longest tool name the model APIs accept.
const MAX_NAME: usize = 64;
/// A result longer than this is cut before it goes to the model.
const MAX_RESULT: usize = 8000;

pub type Client = RunningService<RoleClient, ClientConfig>;

/// A tool from an MCP server, as the models see it.
pub struct McpTool {
    /// "<server>_<tool>", made safe for the model APIs.
    pub name: String,
    pub description: String,
    /// JSON Schema of the input object.
    pub schema: Value,
    server: usize,
    /// The server's own name for it.
    remote: String,
}

struct Server {
    name: String,
    client: Client,
    timeout: Duration,
    /// Talk turns get its tools (`[[mcp]] talk`).
    talk: bool,
}

#[derive(Default)]
pub struct Mcp {
    servers: Vec<Server>,
    tools: Vec<McpTool>,
}

impl Mcp {
    /// Connects to every configured server. Failures are logged and leave that server out.
    /// `logs`: what the servers started as commands write to stderr goes to ours (otherwise
    /// nowhere, so it doesn't land in the middle of a chat).
    pub async fn connect(cfgs: &[config::McpServer], logs: bool) -> Arc<Mcp> {
        let clients = futures_util::future::join_all(cfgs.iter().map(|cfg| async move {
            let client = tokio::time::timeout(CONNECT_TIMEOUT, open(cfg, logs))
                .await
                .unwrap_or_else(|_| Err(anyhow::anyhow!("no answer in {CONNECT_TIMEOUT:?}")));
            (cfg, client)
        }))
        .await;
        let mut mcp = Mcp::default();
        for (cfg, client) in clients {
            let timeout = Duration::from_millis(cfg.timeout_ms);
            let attached = match client {
                Ok(client) => {
                    mcp.attach(&cfg.name, client, cfg.tools.as_deref(), timeout)
                        .await
                }
                Err(e) => Err(e),
            };
            if attached.is_ok()
                && let Some(server) = mcp.servers.last_mut()
            {
                server.talk = cfg.talk;
            }
            match attached {
                Ok(n) => tracing::info!(server = %cfg.name, tools = n, "MCP server connected"),
                Err(e) => tracing::error!(server = %cfg.name, "MCP server left out: {e:#}"),
            }
        }
        Arc::new(mcp)
    }

    /// Adds a connected server's tools (those in `allow`, if given). Returns how many.
    pub async fn attach(
        &mut self,
        name: &str,
        client: Client,
        allow: Option<&[String]>,
        timeout: Duration,
    ) -> Result<usize> {
        let listed = tokio::time::timeout(CONNECT_TIMEOUT, client.list_all_tools())
            .await
            .context("listing tools timed out")?
            .context("listing tools")?;
        let server = self.servers.len();
        let mut added = 0;
        for tool in listed {
            let remote = tool.name.to_string();
            if allow.is_some_and(|allow| !allow.contains(&remote)) {
                continue;
            }
            let exposed = tool_name(name, &remote);
            if self.tools.iter().any(|t| t.name == exposed) {
                tracing::warn!(
                    server = name,
                    tool = remote,
                    "two tools named {exposed}; keeping the first"
                );
                continue;
            }
            self.tools.push(McpTool {
                name: exposed,
                description: tool
                    .description
                    .map(|d| d.to_string())
                    .or(tool.title.clone())
                    .unwrap_or_default(),
                schema: Value::Object((*tool.input_schema).clone()),
                server,
                remote,
            });
            added += 1;
        }
        if let Some(allow) = allow {
            for wanted in allow {
                if !self
                    .tools
                    .iter()
                    .any(|t| t.server == server && t.remote == *wanted)
                {
                    tracing::warn!(server = name, tool = %wanted, "allowed tool not offered by the server");
                }
            }
        }
        self.servers.push(Server {
            name: name.to_string(),
            client,
            timeout,
            talk: true,
        });
        Ok(added)
    }

    pub fn tools(&self) -> &[McpTool] {
        &self.tools
    }

    /// The tools of servers that talk turns don't get (`[[mcp]] talk = false`).
    pub fn talk_hidden(&self) -> Vec<String> {
        self.tools
            .iter()
            .filter(|t| self.servers.get(t.server).is_some_and(|s| !s.talk))
            .map(|t| t.name.clone())
            .collect()
    }

    /// The connected servers' names, with how many tools each gives.
    pub fn servers(&self) -> Vec<(&str, usize)> {
        self.servers
            .iter()
            .enumerate()
            .map(|(i, s)| {
                (
                    s.name.as_str(),
                    self.tools.iter().filter(|t| t.server == i).count(),
                )
            })
            .collect()
    }

    /// Calls the tool the models know as `name`. `None` if no MCP server offers it.
    pub async fn call(&self, name: &str, input: Value) -> Option<Result<String, String>> {
        let tool = self.tools.iter().find(|t| t.name == name)?;
        let server = &self.servers[tool.server];
        let mut params = CallToolRequestParams::new(tool.remote.clone());
        if let Value::Object(arguments) = input {
            params = params.with_arguments(arguments);
        }
        let result = tokio::time::timeout(server.timeout, server.client.call_tool(params)).await;
        Some(match result {
            Err(_) => Err(format!(
                "{} didn't answer within {} ms",
                server.name,
                server.timeout.as_millis()
            )),
            Ok(Err(e)) => Err(format!("{} failed: {e}", server.name)),
            Ok(Ok(result)) => result_text(result),
        })
    }
}

/// What inkharness says in the `initialize` handshake. rmcp's default asks for its newest
/// protocol version, which has no `initialize` any more (2026-07-28, SEP-2567): servers from
/// before it answer with an older one, but servers that know it refuse ("not available
/// through the initialize handshake"). This asks for the newest version that has the
/// handshake, which both kinds speak.
fn client() -> ClientConfig {
    ClientConfig::default().with_protocol_version(ProtocolVersion::LATEST_WITH_INITIALIZE)
}

/// Starts or reaches one server and completes the MCP handshake.
async fn open(cfg: &config::McpServer, logs: bool) -> Result<Client> {
    match (&cfg.command, &cfg.url) {
        (Some(command), None) => {
            let mut process = tokio::process::Command::new(command);
            process.args(&cfg.args).envs(&cfg.env);
            let stderr = if logs {
                std::process::Stdio::inherit()
            } else {
                std::process::Stdio::null()
            };
            let (transport, _) = TokioChildProcess::builder(process)
                .stderr(stderr)
                .spawn()
                .with_context(|| format!("starting {command}"))?;
            Ok(client().serve(transport).await?)
        }
        (None, Some(url)) => {
            let mut transport = StreamableHttpClientTransportConfig::with_uri(url.as_str());
            if let Some(var) = &cfg.token_env {
                let token = config::env_key(var).with_context(|| format!("no token in ${var}"))?;
                transport = transport.auth_header(token);
            }
            if !cfg.headers.is_empty() {
                transport = transport.custom_headers(headers(&cfg.headers)?);
            }
            Ok(client()
                .serve(StreamableHttpClientTransport::from_config(transport))
                .await?)
        }
        _ => bail!("give either command or url"),
    }
}

fn headers(
    values: &std::collections::HashMap<String, String>,
) -> Result<std::collections::HashMap<HeaderName, HeaderValue>> {
    values
        .iter()
        .map(|(name, value)| {
            Ok((
                HeaderName::from_bytes(name.as_bytes())
                    .with_context(|| format!("header name {name:?}"))?,
                HeaderValue::from_str(value)
                    .with_context(|| format!("the {name} header's value"))?,
            ))
        })
        .collect()
}

/// "<server>_<tool>" with anything but letters, digits, _ and - replaced, as the model APIs
/// require.
fn tool_name(server: &str, tool: &str) -> String {
    format!("{server}_{tool}")
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '_' || c == '-' {
                c
            } else {
                '_'
            }
        })
        .take(MAX_NAME)
        .collect()
}

/// A result as text for the model: its text blocks, else its structured content.
fn result_text(result: CallToolResult) -> Result<String, String> {
    let mut parts: Vec<String> = result
        .content
        .iter()
        .map(|block| match block {
            ContentBlock::Text(text) => text.text.clone(),
            ContentBlock::Image(_) => "[an image]".into(),
            ContentBlock::Audio(_) => "[audio]".into(),
            _ => "[a resource]".into(),
        })
        .collect();
    if parts.is_empty()
        && let Some(structured) = &result.structured_content
    {
        parts.push(structured.to_string());
    }
    let mut text = parts.join("\n");
    if text.chars().count() > MAX_RESULT {
        text = text.chars().take(MAX_RESULT).collect();
        text.push_str("\n(cut)");
    }
    if result.is_error == Some(true) {
        Err(if text.is_empty() {
            "the tool failed".into()
        } else {
            text
        })
    } else {
        Ok(text)
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use rmcp::handler::server::router::tool::ToolRouter;
    use rmcp::handler::server::wrapper::Parameters;
    use rmcp::{ErrorData, ServerHandler, schemars, tool, tool_handler, tool_router};

    use super::*;

    #[derive(serde::Deserialize, schemars::JsonSchema)]
    pub struct Pair {
        a: f64,
        b: f64,
    }

    /// A server with three tools, served in-process.
    #[derive(Clone)]
    pub struct TestServer {
        tool_router: ToolRouter<Self>,
    }

    #[tool_router]
    impl TestServer {
        #[tool(description = "Adds two numbers")]
        async fn add(&self, Parameters(p): Parameters<Pair>) -> Result<CallToolResult, ErrorData> {
            Ok(CallToolResult::success(vec![ContentBlock::text(
                (p.a + p.b).to_string(),
            )]))
        }

        #[tool(description = "Always fails")]
        async fn broken(&self) -> Result<CallToolResult, ErrorData> {
            Ok(CallToolResult::error(vec![ContentBlock::text("it broke")]))
        }

        #[tool(name = "unlock.door", description = "Not allowed")]
        async fn unlock(&self) -> Result<CallToolResult, ErrorData> {
            Ok(CallToolResult::success(vec![ContentBlock::text(
                "unlocked",
            )]))
        }
    }

    #[tool_handler(router = self.tool_router)]
    impl ServerHandler for TestServer {}

    /// An `Mcp` holding the test server as "test", with `allow` as its allowlist.
    pub async fn connected(allow: Option<&[String]>) -> Mcp {
        let (server_io, client_io) = tokio::io::duplex(64 * 1024);
        let server = TestServer {
            tool_router: TestServer::tool_router(),
        };
        tokio::spawn(async move {
            if let Ok(running) = server.serve(server_io).await {
                let _ = running.waiting().await;
            }
        });
        let client = client().serve(client_io).await.unwrap();
        let mut mcp = Mcp::default();
        mcp.attach("test", client, allow, Duration::from_secs(5))
            .await
            .unwrap();
        mcp
    }

    #[tokio::test]
    async fn tools_are_named_listed_and_called() {
        let mcp = connected(None).await;
        let mut names: Vec<&str> = mcp.tools().iter().map(|t| t.name.as_str()).collect();
        names.sort();
        assert_eq!(names, ["test_add", "test_broken", "test_unlock_door"]);
        let add = mcp.tools().iter().find(|t| t.name == "test_add").unwrap();
        assert_eq!(add.description, "Adds two numbers");
        assert_eq!(add.schema["properties"]["a"]["type"], "number");

        let sum = mcp
            .call("test_add", serde_json::json!({"a": 2, "b": 3.5}))
            .await;
        assert_eq!(sum, Some(Ok("5.5".into())));
        let failed = mcp.call("test_broken", serde_json::json!({})).await;
        assert_eq!(failed, Some(Err("it broke".into())));
        let bad = mcp
            .call("test_add", serde_json::json!({"a": "x"}))
            .await
            .unwrap();
        assert!(bad.is_err(), "{bad:?}");
        assert_eq!(mcp.call("list_speakers", serde_json::json!({})).await, None);
    }

    #[tokio::test]
    async fn the_allowlist_limits_the_tools() {
        let allow = ["add".to_string(), "missing".to_string()];
        let mcp = connected(Some(&allow)).await;
        let names: Vec<&str> = mcp.tools().iter().map(|t| t.name.as_str()).collect();
        assert_eq!(names, ["test_add"]);
        assert_eq!(
            mcp.call("test_unlock_door", serde_json::json!({})).await,
            None
        );
    }

    /// The test server, said to stay out of talk (`[[mcp]] talk = false`).
    pub async fn connected_quiet() -> Mcp {
        let mut mcp = connected(None).await;
        for server in &mut mcp.servers {
            server.talk = false;
        }
        mcp
    }

    #[test]
    fn names_are_safe() {
        assert_eq!(tool_name("home", "HassTurnOn"), "home_HassTurnOn");
        assert_eq!(tool_name("my server", "a.b/c"), "my_server_a_b_c");
        assert_eq!(tool_name("s", &"x".repeat(100)).len(), MAX_NAME);
    }
}
