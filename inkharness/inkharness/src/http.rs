//! The HTTP API `serve` answers beside the device port (`http` in inkharness.toml): text turns
//! for scripts and other programs, with the model, tools, skills and instructions a chat gets.
//!
//! ```text
//! GET  /health  {"ok": true, "model": "…"}, and with the token the tools and skills
//! POST /chat    {"text": "…", "conversation": "c1"?, "stream": false?}
//!               → {"reply": "…", "conversation": "c1", "tools": ["run_js"]}
//! ```
//!
//! A turn without `conversation` starts one and returns its id; pass it back to go on. The last
//! MAX_CONVERSATIONS are kept, in memory. With `"stream": true` the reply comes as server-sent
//! events, each a JSON object: `{"text": "…"}` as it is written, `{"tool": "…"}` as a call
//! starts, then `{"done": true, "reply": "…", "conversation": "…"}` or `{"error": "…"}`.
//!
//! With a token set (`token`, the one devices send), /chat wants `Authorization: Bearer <token>`.

use std::collections::HashMap;
use std::convert::Infallible;
use std::sync::{Arc, Mutex};
use std::time::Instant;

use anyhow::Result;
use axum::Json;
use axum::extract::State;
use axum::http::{HeaderMap, StatusCode, header};
use axum::response::sse::{Event, Sse};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use serde::Deserialize;
use serde_json::{Value, json};
use tokio::net::TcpListener;
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

use crate::chat::{self, Chat};
use crate::llm::{Piece, Turn};

/// Conversations kept; past this, the one left longest goes.
const MAX_CONVERSATIONS: usize = 100;

pub struct Api {
    pub chat: Chat,
    pub token: String,
    pub skills: Vec<String>,
    conversations: Mutex<Conversations>,
}

#[derive(Default)]
struct Conversations {
    next: u64,
    by_id: HashMap<String, (Instant, Vec<Turn>)>,
}

impl Api {
    pub fn new(chat: Chat, token: String, skills: Vec<String>) -> Api {
        Api {
            chat,
            token,
            skills,
            conversations: Mutex::default(),
        }
    }

    fn authorized(&self, headers: &HeaderMap) -> bool {
        self.token.is_empty()
            || headers
                .get(header::AUTHORIZATION)
                .and_then(|v| v.to_str().ok())
                .and_then(|v| v.strip_prefix("Bearer "))
                .is_some_and(|t| t == self.token)
    }

    /// The conversation's id (a new one if none was given) and its history so far.
    fn open(&self, id: Option<String>) -> (String, Vec<Turn>) {
        let mut all = self.conversations.lock().unwrap();
        let id = id.unwrap_or_else(|| {
            all.next += 1;
            format!("c{}", all.next)
        });
        let history = all.by_id.get(&id).map(|(_, h)| h.clone()).unwrap_or_default();
        (id, history)
    }

    fn remember(&self, id: &str, user: &str, reply: &str) {
        let mut all = self.conversations.lock().unwrap();
        let (used, history) = all
            .by_id
            .entry(id.to_string())
            .or_insert_with(|| (Instant::now(), Vec::new()));
        *used = Instant::now();
        chat::remember(history, user, reply);
        if all.by_id.len() > MAX_CONVERSATIONS
            && let Some(oldest) = all
                .by_id
                .iter()
                .min_by_key(|(_, (used, _))| *used)
                .map(|(id, _)| id.clone())
        {
            all.by_id.remove(&oldest);
        }
    }
}

pub async fn serve(listener: TcpListener, api: Arc<Api>) -> Result<()> {
    let app = axum::Router::new()
        .route("/health", get(health))
        .route("/chat", post(chat_turn))
        .with_state(api);
    axum::serve(listener, app).await?;
    Ok(())
}

async fn health(State(api): State<Arc<Api>>, headers: HeaderMap) -> Json<Value> {
    let mut status = json!({"ok": true, "model": api.chat.model});
    if api.authorized(&headers) {
        let tools: Vec<&str> = api.chat.tools().defs().iter().map(|d| d.name.as_str()).collect();
        status["tools"] = json!(tools);
        status["skills"] = json!(api.skills);
    }
    Json(status)
}

#[derive(Deserialize)]
struct ChatRequest {
    text: String,
    conversation: Option<String>,
    #[serde(default)]
    stream: bool,
}

fn error(status: StatusCode, message: String) -> Response {
    (status, Json(json!({"error": message}))).into_response()
}

async fn chat_turn(
    State(api): State<Arc<Api>>,
    headers: HeaderMap,
    Json(request): Json<ChatRequest>,
) -> Response {
    if !api.authorized(&headers) {
        return error(StatusCode::UNAUTHORIZED, "wrong or missing token".into());
    }
    if request.text.trim().is_empty() {
        return error(StatusCode::BAD_REQUEST, "no text".into());
    }
    let (id, history) = api.open(request.conversation);
    let (tx, mut rx) = mpsc::channel::<Piece>(64);
    // In a task of its own, which owns the history. A client that goes away drops `rx`, and
    // the turn ends there, unremembered.
    let task: JoinHandle<Result<String>> = {
        let (api, id, text) = (api.clone(), id.clone(), request.text);
        tokio::spawn(async move {
            let reply = api.chat.turn(&history, &text, tx).await?;
            api.remember(&id, &text, &reply);
            Ok(reply)
        })
    };

    if request.stream {
        return Sse::new(events(rx, task, id)).into_response();
    }
    let mut tools = Vec::new();
    while let Some(piece) = rx.recv().await {
        if let Piece::Tool(name) = piece {
            tools.push(name);
        }
    }
    match task.await {
        Ok(Ok(reply)) => Json(json!({"reply": reply.trim(), "conversation": id, "tools": tools}))
            .into_response(),
        Ok(Err(e)) => error(StatusCode::BAD_GATEWAY, format!("{e:#}")),
        Err(e) => error(StatusCode::INTERNAL_SERVER_ERROR, e.to_string()),
    }
}

enum Stream {
    Turn(mpsc::Receiver<Piece>, JoinHandle<Result<String>>, String),
    Done,
}

/// The turn as server-sent events: its text and tool calls, then how it ended.
fn events(
    rx: mpsc::Receiver<Piece>,
    task: JoinHandle<Result<String>>,
    id: String,
) -> impl futures_util::Stream<Item = Result<Event, Infallible>> {
    let event = |value: Value| Ok(Event::default().data(value.to_string()));
    futures_util::stream::unfold(Stream::Turn(rx, task, id), move |state| async move {
        let Stream::Turn(mut rx, task, id) = state else {
            return None;
        };
        let next = loop {
            match rx.recv().await {
                Some(Piece::Audio(_)) => continue,
                other => break other,
            }
        };
        Some(match next {
            Some(Piece::Text(text) | Piece::Transcript(text)) => {
                (event(json!({"text": text})), Stream::Turn(rx, task, id))
            }
            Some(Piece::Tool(name)) => (event(json!({"tool": name})), Stream::Turn(rx, task, id)),
            Some(Piece::Audio(_)) => unreachable!(),
            None => {
                let end = match task.await {
                    Ok(Ok(reply)) => json!({"done": true, "reply": reply.trim(), "conversation": id}),
                    Ok(Err(e)) => json!({"error": format!("{e:#}")}),
                    Err(e) => json!({"error": e.to_string()}),
                };
                (event(end), Stream::Done)
            }
        })
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::Config;
    use crate::harness::Harness;
    use crate::place::Place;

    async fn start(token: &str) -> String {
        let place = Place {
            config: Config::from_table(toml::from_str("[llm]\nbackend = \"mock\"\n").unwrap())
                .unwrap(),
            read: vec![],
            instructions: vec![],
            skill_dirs: vec![],
        };
        let harness = Harness::build(&place, false).await.unwrap();
        let chat = Chat::build(&place.config, &harness).unwrap();
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = format!("http://{}", listener.local_addr().unwrap());
        let api = Arc::new(Api::new(chat, token.into(), vec![]));
        tokio::spawn(serve(listener, api));
        url
    }

    #[tokio::test]
    async fn chat_goes_on_in_a_conversation() {
        let url = start("").await;
        let http = reqwest::Client::new();
        let first: Value = http
            .post(format!("{url}/chat"))
            .json(&json!({"text": "one"}))
            .send()
            .await
            .unwrap()
            .json()
            .await
            .unwrap();
        assert!(first["reply"].as_str().unwrap().starts_with("You said: one."));
        let id = first["conversation"].as_str().unwrap().to_string();
        let streamed = http
            .post(format!("{url}/chat"))
            .json(&json!({"text": "two", "conversation": id, "stream": true}))
            .send()
            .await
            .unwrap()
            .text()
            .await
            .unwrap();
        assert!(streamed.contains(r#"data: {"text":"#), "{streamed}");
        assert!(streamed.contains(r#""done":true"#), "{streamed}");
        assert!(streamed.contains(&format!(r#""conversation":"{id}""#)), "{streamed}");
        let health: Value = http.get(format!("{url}/health")).send().await.unwrap().json().await.unwrap();
        assert_eq!(health["model"], "mock");
        assert!(health["tools"].as_array().unwrap().iter().any(|t| t == "run_js"));
    }

    #[tokio::test]
    async fn a_token_is_asked_for() {
        let url = start("s3cret").await;
        let http = reqwest::Client::new();
        let chat = |token: Option<&str>| {
            let mut request = http.post(format!("{url}/chat")).json(&json!({"text": "hi"}));
            if let Some(token) = token {
                request = request.bearer_auth(token);
            }
            request.send()
        };
        assert_eq!(chat(None).await.unwrap().status(), 401);
        assert_eq!(chat(Some("nope")).await.unwrap().status(), 401);
        assert_eq!(chat(Some("s3cret")).await.unwrap().status(), 200);
        let health: Value = http.get(format!("{url}/health")).send().await.unwrap().json().await.unwrap();
        assert!(health.get("tools").is_none());
    }
}
