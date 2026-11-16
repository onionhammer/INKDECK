//! The language model. A backend answers one user turn: it streams the reply's text and runs
//! the tools the model calls. The hosted backends are a provider's API (anthropic.rs, openai.rs)
//! driven by the shared tool loop (tool_loop.rs).

mod anthropic;
mod openai;
mod tool_loop;

use std::time::Duration;

use anyhow::Result;
use async_trait::async_trait;
use tokio::sync::mpsc;

use crate::config;
use crate::tools::ToolSet;
use tool_loop::ToolLoop;

#[derive(Clone, Copy, PartialEq)]
pub enum Role {
    User,
    Assistant,
}

/// An earlier turn of the conversation: what was said, without the tool calls behind it.
#[derive(Clone)]
pub struct Turn {
    pub role: Role,
    pub text: String,
}

pub struct Request<'a> {
    pub system: &'a str,
    pub history: &'a [Turn],
    pub user: &'a str,
    pub tools: &'a ToolSet,
}

/// How a reply ended.
#[derive(Debug, PartialEq)]
pub enum Outcome {
    Done,
    /// The model called the think tool (tools::THINK_TOOL): the session hands the turn to the
    /// thinking model. `spoke`: the model had already said something this turn.
    Think {
        spoke: bool,
    },
}

#[async_trait]
pub trait Llm: Send + Sync {
    /// Sends the reply's text to `out` as it is generated. Text from separate rounds (before
    /// and after a tool call) is separated by a newline. A call to the think tool ends the
    /// reply at once, without running it or any other call from that round.
    async fn respond(&self, request: Request<'_>, out: mpsc::Sender<String>) -> Result<Outcome>;
}

pub fn build(cfg: &config::Llm, http: reqwest::Client) -> Result<Box<dyn Llm>> {
    Ok(match cfg {
        config::Llm::Anthropic {
            model,
            base_url,
            api_key_env,
            effort,
            max_tokens,
            fallbacks,
        } => Box::new(ToolLoop(anthropic::Anthropic::new(
            http,
            base_url,
            api_key_env,
            model,
            effort,
            *max_tokens,
            *fallbacks,
        )?)),
        config::Llm::OpenaiCompat {
            base_url,
            model,
            api_key_env,
            max_tokens,
            reasoning,
        } => Box::new(ToolLoop(openai::OpenAiCompat::new(
            http,
            base_url,
            api_key_env,
            model,
            *max_tokens,
            reasoning
                .as_deref()
                .map(config::reasoning_param)
                .transpose()?,
        ))),
        config::Llm::Mock => Box::new(Mock),
    })
}

/// Repeats what it heard, a word at a time.
struct Mock;

#[async_trait]
impl Llm for Mock {
    async fn respond(&self, request: Request<'_>, out: mpsc::Sender<String>) -> Result<Outcome> {
        let reply = format!(
            "You said: {}. This is a test reply from the mock model.",
            request.user
        );
        for word in reply.split_inclusive(' ') {
            tokio::time::sleep(Duration::from_millis(15)).await;
            if out.send(word.to_string()).await.is_err() {
                break;
            }
        }
        Ok(Outcome::Done)
    }
}

/// A local HTTP server that answers each request with the next canned event stream and
/// records the JSON bodies it was sent.
#[cfg(test)]
pub(crate) mod testserver {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::TcpListener;
    use tokio::sync::mpsc;

    pub async fn serve(
        replies: Vec<String>,
    ) -> (String, mpsc::UnboundedReceiver<serde_json::Value>) {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = format!("http://{}", listener.local_addr().unwrap());
        let (tx, rx) = mpsc::unbounded_channel();
        tokio::spawn(async move {
            for reply in replies {
                let (mut socket, _) = listener.accept().await.unwrap();
                let mut request = Vec::new();
                let body = loop {
                    let mut chunk = [0u8; 4096];
                    let n = socket.read(&mut chunk).await.unwrap();
                    request.extend_from_slice(&chunk[..n]);
                    let text = String::from_utf8_lossy(&request);
                    let Some(split) = text.find("\r\n\r\n") else {
                        continue;
                    };
                    let length: usize = text[..split]
                        .lines()
                        .find_map(|l| {
                            l.to_ascii_lowercase()
                                .strip_prefix("content-length:")
                                .map(|v| v.trim().parse().unwrap())
                        })
                        .unwrap();
                    if request.len() >= split + 4 + length {
                        break request[split + 4..split + 4 + length].to_vec();
                    }
                };
                tx.send(serde_json::from_slice(&body).unwrap()).unwrap();
                let response = format!(
                    "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\nconnection: close\r\n\r\n{reply}"
                );
                socket.write_all(response.as_bytes()).await.unwrap();
                socket.shutdown().await.unwrap();
            }
        });
        (url, rx)
    }

    /// Runs a backend to the end and returns everything it sent to `out`.
    pub async fn collect(
        llm: &dyn super::Llm,
        user: &str,
    ) -> (anyhow::Result<super::Outcome>, String) {
        // run_js and the think tool: something real to call, and the hand-off.
        let tools = crate::tools::ToolSet::build(
            None,
            &crate::config::Js::default(),
            Default::default(),
            Default::default(),
            reqwest::Client::new(),
        )
        .unwrap()
        .with_think();
        let (tx, mut rx) = mpsc::channel(64);
        let request = super::Request {
            system: "sys",
            history: &[],
            user,
            tools: &tools,
        };
        let (result, text) = tokio::join!(llm.respond(request, tx), async {
            let mut text = String::new();
            while let Some(piece) = rx.recv().await {
                text.push_str(&piece);
            }
            text
        });
        (result, text)
    }
}
