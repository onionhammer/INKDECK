//! How a claude run reaches inkharness's own tools (`ask_choice`, `plan`, `herdr_run`). Claude
//! Code starts MCP servers itself, so each run's `--mcp-config` names this binary in its
//! `mcp-bridge` mode, a small MCP server on stdio. That server passes every tool call to the
//! daemon over a local TCP connection (the bridge), where it runs as the session's own tool
//! call: `ask_choice` puts the question up and waits for the answer, as it does for the api
//! backend.
//!
//! The bridge listens on 127.0.0.1 only. Each run gets a key of its own (random, 128 bits),
//! which names the session and run its calls belong to; the key is in the run's MCP config file
//! (only the daemon's user can read it). One request per connection, one JSON line each way:
//! `{"key", "list": true}` → `{"tools": [{name, description, input_schema}]}`, and
//! `{"key", "tool", "input"}` → `{"text"}` or `{"error"}`.

use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::{Arc, Mutex, Weak};

use anyhow::{Context, Result, bail};
use serde_json::{Value, json};
use tokio::io::{AsyncBufReadExt, AsyncRead, AsyncWrite, AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, TcpStream};

use super::Agent;
use super::tools::SessionTools;
use crate::tools::Extra;

/// The environment variables the MCP server finds the bridge by.
pub const ADDR_VAR: &str = "INKHARNESS_BRIDGE";
pub const KEY_VAR: &str = "INKHARNESS_BRIDGE_KEY";
/// The MCP server's name in Claude Code: its tools are `mcp__inkharness__<tool>`.
pub const SERVER: &str = "inkharness";

pub struct Bridge {
    pub addr: SocketAddr,
    /// Key → the session and run it belongs to.
    keys: Mutex<HashMap<String, (String, u64)>>,
}

impl Bridge {
    /// Listens on a free port on 127.0.0.1, answering for `agent`'s sessions.
    pub fn start(agent: Weak<Agent>) -> Result<Arc<Bridge>> {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").context("the tool bridge")?;
        listener.set_nonblocking(true)?;
        let listener = TcpListener::from_std(listener)?;
        let bridge = Arc::new(Bridge {
            addr: listener.local_addr()?,
            keys: Mutex::default(),
        });
        tokio::spawn(serve(listener, bridge.clone(), agent));
        Ok(bridge)
    }

    /// A key for one run of a session (a run's old keys stop working as its run ends: the
    /// session's tools check the run is still its own).
    pub fn issue(&self, session: &str, run: u64) -> String {
        let mut bytes = [0u8; 16];
        let _ = getrandom::fill(&mut bytes);
        let key: String = bytes.iter().map(|b| format!("{b:02x}")).collect();
        let mut keys = self.keys.lock().unwrap();
        keys.retain(|_, (s, _)| s != session);
        keys.insert(key.clone(), (session.to_string(), run));
        key
    }

    fn lookup(&self, key: &str) -> Option<(String, u64)> {
        self.keys.lock().unwrap().get(key).cloned()
    }
}

async fn serve(listener: TcpListener, bridge: Arc<Bridge>, agent: Weak<Agent>) {
    loop {
        let Ok((stream, _)) = listener.accept().await else {
            continue;
        };
        let (bridge, agent) = (bridge.clone(), agent.clone());
        tokio::spawn(async move {
            if let Err(e) = handle(stream, &bridge, &agent).await {
                tracing::debug!("tool bridge: {e:#}");
            }
        });
    }
}

async fn handle(stream: TcpStream, bridge: &Bridge, agent: &Weak<Agent>) -> Result<()> {
    let (reader, mut writer) = stream.into_split();
    let mut line = String::new();
    BufReader::new(reader).read_line(&mut line).await?;
    let request: Value = serde_json::from_str(&line).context("a request that isn't JSON")?;
    let answer = match bridge.lookup(request["key"].as_str().unwrap_or_default()) {
        None => json!({"error": "this run's tools have ended"}),
        Some((session, run)) => {
            let Some(agent) = agent.upgrade() else {
                bail!("the daemon is going")
            };
            let tools = SessionTools {
                agent,
                id: session,
                run,
            };
            if request["list"].as_bool() == Some(true) {
                let defs: Vec<Value> = tools
                    .defs()
                    .iter()
                    .map(|d| json!({"name": d.name, "description": d.description, "input_schema": d.schema}))
                    .collect();
                json!({"tools": defs})
            } else {
                let name = request["tool"].as_str().unwrap_or_default();
                let input = request.get("input").cloned().unwrap_or_else(|| json!({}));
                match tools.call(name, &input).await {
                    Some(Ok(text)) => json!({"text": text}),
                    Some(Err(error)) => json!({"error": error}),
                    None => json!({"error": format!("no tool named {name}")}),
                }
            }
        }
    };
    writer.write_all(format!("{answer}\n").as_bytes()).await?;
    Ok(())
}

/// One request to the bridge, and its answer.
async fn ask_bridge(addr: &str, request: Value) -> Result<Value> {
    let stream = TcpStream::connect(addr)
        .await
        .with_context(|| format!("the daemon's tool bridge at {addr}"))?;
    let (reader, mut writer) = stream.into_split();
    writer.write_all(format!("{request}\n").as_bytes()).await?;
    let mut line = String::new();
    BufReader::new(reader).read_line(&mut line).await?;
    if line.trim().is_empty() {
        bail!("the daemon's tool bridge hung up");
    }
    Ok(serde_json::from_str(&line)?)
}

/// `inkharness mcp-bridge`: the MCP server a claude run starts, on stdin and stdout.
pub async fn mcp_stdio() -> Result<()> {
    let addr = std::env::var(ADDR_VAR).with_context(|| format!("{ADDR_VAR} isn't set"))?;
    let key = std::env::var(KEY_VAR).with_context(|| format!("{KEY_VAR} isn't set"))?;
    mcp_serve(tokio::io::stdin(), tokio::io::stdout(), &addr, &key).await
}

/// An MCP server (JSON-RPC, a message a line) whose tools are the bridge's.
pub async fn mcp_serve<R, W>(reader: R, mut writer: W, addr: &str, key: &str) -> Result<()>
where
    R: AsyncRead + Unpin,
    W: AsyncWrite + Unpin,
{
    let mut lines = BufReader::new(reader).lines();
    while let Some(line) = lines.next_line().await? {
        let Ok(message) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        let Some(id) = message.get("id").filter(|id| !id.is_null()).cloned() else {
            continue; // a notification
        };
        let params = &message["params"];
        let result = match message["method"].as_str().unwrap_or_default() {
            "initialize" => Ok(json!({
                "protocolVersion": params["protocolVersion"].as_str().unwrap_or("2025-06-18"),
                "capabilities": {"tools": {"listChanged": false}},
                "serverInfo": {"name": SERVER, "version": env!("CARGO_PKG_VERSION")},
            })),
            "ping" => Ok(json!({})),
            "tools/list" => match ask_bridge(addr, json!({"key": key, "list": true})).await {
                Ok(answer) => {
                    let tools: Vec<Value> = answer["tools"]
                        .as_array()
                        .into_iter()
                        .flatten()
                        .map(|t| json!({"name": t["name"], "description": t["description"], "inputSchema": t["input_schema"]}))
                        .collect();
                    Ok(json!({"tools": tools}))
                }
                Err(e) => Err(format!("{e:#}")),
            },
            "tools/call" => {
                let request =
                    json!({"key": key, "tool": params["name"], "input": params["arguments"]});
                let (text, is_error) = match ask_bridge(addr, request).await {
                    Ok(answer) => match answer["text"].as_str() {
                        Some(text) => (text.to_string(), false),
                        None => (
                            answer["error"].as_str().unwrap_or("no answer").to_string(),
                            true,
                        ),
                    },
                    Err(e) => (format!("{e:#}"), true),
                };
                Ok(json!({"content": [{"type": "text", "text": text}], "isError": is_error}))
            }
            other => Err(format!("no method {other}")),
        };
        let reply = match result {
            Ok(result) => json!({"jsonrpc": "2.0", "id": id, "result": result}),
            Err(message) => {
                json!({"jsonrpc": "2.0", "id": id, "error": {"code": -32601, "message": message}})
            }
        };
        writer.write_all(format!("{reply}\n").as_bytes()).await?;
        writer.flush().await?;
    }
    Ok(())
}
