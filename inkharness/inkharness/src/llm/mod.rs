//! The language model. A backend answers one user turn: it streams the reply's text and runs
//! the tools the model calls. The hosted backends are a provider's API (anthropic.rs, openai.rs)
//! driven by the shared tool loop (tool_loop.rs).

mod anthropic;
mod openai;
mod tool_loop;

use std::sync::Arc;
use std::time::Duration;

use anyhow::Result;
use async_trait::async_trait;
use base64::Engine;
use bytes::Bytes;
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
    /// A user turn nobody transcribed: the recording, for models that hear. Its `text` is
    /// empty then.
    pub audio: Option<Arc<Recording>>,
}

impl Turn {
    /// The text for a model that reads: a stand-in for a recording with no transcript.
    pub fn readable(&self) -> &str {
        if self.text.is_empty() && self.audio.is_some() {
            "(something said aloud that wasn't transcribed)"
        } else {
            &self.text
        }
    }
}

/// An utterance as a WAV file, base64-encoded once for every request that sends it.
pub struct Recording {
    pub wav_base64: String,
}

impl Recording {
    /// `pcm`: s16le mono at `rate`.
    pub fn new(pcm: &[u8], rate: u32) -> Recording {
        Recording {
            wav_base64: base64::engine::general_purpose::STANDARD
                .encode(crate::stt::wav(pcm, rate)),
        }
    }
}

pub struct Request<'a> {
    pub system: &'a str,
    pub history: &'a [Turn],
    /// What the user said, for a model that reads (empty if it wasn't transcribed).
    pub user: &'a str,
    /// The recording of it, for a model that hears.
    pub audio: Option<&'a Recording>,
    pub tools: &'a ToolSet,
    /// Model rounds allowed before the reply fails with [`TooManyRounds`]: [`MAX_ROUNDS`] for
    /// a turn someone waits on, more for an agent session.
    pub rounds: usize,
    /// What the user said while the reply was being worked on (an agent session that is
    /// talked into): asked after each round's tool results, and whatever it returns joins the
    /// conversation for the next round.
    pub steer: Option<&'a Steer<'a>>,
}

/// See [`Request::steer`].
pub type Steer<'a> = dyn Fn() -> Vec<String> + Send + Sync + 'a;

/// Model rounds in a turn someone is waiting on, before giving up.
pub const MAX_ROUNDS: usize = 6;

/// The model was still calling tools after the rounds a request allows.
#[derive(Debug)]
pub struct TooManyRounds(pub usize);

impl std::fmt::Display for TooManyRounds {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "the model kept calling tools ({} rounds)", self.0)
    }
}

impl std::error::Error for TooManyRounds {}

/// A piece of the reply as it is generated.
#[derive(Debug, Clone, PartialEq)]
pub enum Piece {
    /// Text to show and to speak.
    Text(String),
    /// What the model says in its `Audio` pieces: shown, not spoken again.
    Transcript(String),
    /// The model's own speech: s16le mono at [`Llm::speaks`]'s rate. A chunk may end in
    /// the middle of a sample.
    Audio(Bytes),
    /// A tool the model is calling, as the call starts, and its input (null if it wasn't a
    /// JSON object): shown in a chat and logged by a session, not said.
    Tool(String, serde_json::Value),
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
    /// Sends the reply to `out` as it is generated. Text from separate rounds (before and
    /// after a tool call) is separated by a newline. A call to the think tool ends the reply
    /// at once, without running it or any other call from that round.
    async fn respond(&self, request: Request<'_>, out: mpsc::Sender<Piece>) -> Result<Outcome>;

    /// True if the model takes the recording (`Request::audio`) rather than a transcript.
    fn hears(&self) -> bool {
        false
    }

    /// The sample rate of its speech, if it speaks its reply ([`Piece::Audio`]) rather than
    /// writing it.
    fn speaks(&self) -> Option<u32> {
        None
    }
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
            input,
            output,
            voice,
            audio_rate,
        } => {
            let mut api = openai::OpenAiCompat::new(
                http,
                base_url,
                api_key_env,
                model,
                *max_tokens,
                reasoning
                    .as_deref()
                    .map(config::reasoning_param)
                    .transpose()?,
            );
            api.hears = *input == config::Modality::Audio;
            if *output == config::Modality::Audio {
                api.speech = Some(openai::Speech {
                    voice: voice.clone().unwrap_or_default(),
                    rate: *audio_rate,
                });
            }
            Box::new(ToolLoop(api))
        }
        config::Llm::Mock => Box::new(Mock),
    })
}

/// Repeats what it heard, a word at a time.
struct Mock;

#[async_trait]
impl Llm for Mock {
    async fn respond(&self, request: Request<'_>, out: mpsc::Sender<Piece>) -> Result<Outcome> {
        let reply = format!(
            "You said: {}. This is a test reply from the mock model.",
            request.user
        );
        for word in reply.split_inclusive(' ') {
            tokio::time::sleep(Duration::from_millis(15)).await;
            if out.send(Piece::Text(word.to_string())).await.is_err() {
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

    /// Runs a backend to the end and returns the text it sent to `out` (transcripts included).
    pub async fn collect(
        llm: &dyn super::Llm,
        user: &str,
    ) -> (anyhow::Result<super::Outcome>, String) {
        let (result, text, _) = collect_all(llm, user, None).await;
        (result, text)
    }

    /// Runs a backend to the end: what it returned, its text, and the audio it sent.
    pub async fn collect_all(
        llm: &dyn super::Llm,
        user: &str,
        audio: Option<&super::Recording>,
    ) -> (anyhow::Result<super::Outcome>, String, Vec<u8>) {
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
            audio,
            tools: &tools,
            rounds: super::MAX_ROUNDS,
            steer: None,
        };
        let (result, (text, pcm)) = tokio::join!(llm.respond(request, tx), async {
            let (mut text, mut pcm) = (String::new(), Vec::new());
            while let Some(piece) = rx.recv().await {
                match piece {
                    super::Piece::Text(t) | super::Piece::Transcript(t) => text.push_str(&t),
                    super::Piece::Audio(a) => pcm.extend_from_slice(&a),
                    super::Piece::Tool(..) => {}
                }
            }
            (text, pcm)
        });
        (result, text, pcm)
    }
}
