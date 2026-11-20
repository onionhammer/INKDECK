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
//!
//! With an [agent] table, the agent sessions (agent/) too, with the same JSON a board gets
//! (docs/voice-protocol.md, "Agent sessions"), and with a PIN set, `X-Inkharness-Pin: <pin>` on
//! every request (403 without it or with a wrong one, 429 while wrong ones back off):
//!
//! ```text
//! GET    /sessions                      the `sessions` message, unlocked
//! GET    /sessions/{id}                 the `session` message
//! POST   /sessions          {"text"}    starts one: {"session": "k7q2"}
//! POST   /sessions/{id}/say {"text"}    talks into it (an answer in words, if it is asking)
//! POST   /sessions/{id}/answer {"ask", "choice"}
//! POST   /sessions/{id}/stop
//! POST   /sessions/{id}/kill            a herdr session: end its program ([herdr] kill)
//! DELETE /sessions/{id}
//! ```

use std::collections::HashMap;
use std::convert::Infallible;
use std::sync::{Arc, Mutex};
use std::time::Instant;

use anyhow::Result;
use axum::Json;
use axum::extract::{Path, State};
use axum::http::{HeaderMap, StatusCode, header};
use axum::response::sse::{Event, Sse};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use serde::Deserialize;
use serde_json::{Value, json};
use tokio::net::TcpListener;
use tokio::sync::mpsc;
use tokio::task::JoinHandle;
use voice_proto::ServerMsg;

use crate::agent::{Agent, Refusal, Unlock};
use crate::chat::{self, Chat};
use crate::llm::{Piece, Turn};

/// Conversations kept; past this, the one left longest goes.
const MAX_CONVERSATIONS: usize = 100;

pub struct Api {
    pub chat: Chat,
    pub token: String,
    pub skills: Vec<String>,
    conversations: Mutex<Conversations>,
    agent: Option<Arc<Agent>>,
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
            agent: None,
        }
    }

    /// The same API with the agent sessions' endpoints answering.
    pub fn with_agent(mut self, agent: Option<Arc<Agent>>) -> Api {
        self.agent = agent;
        self
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
        let history = all
            .by_id
            .get(&id)
            .map(|(_, h)| h.clone())
            .unwrap_or_default();
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
        .route("/sessions", get(list_sessions).post(start_session))
        .route("/sessions/{id}", get(get_session).delete(delete_session))
        .route("/sessions/{id}/say", post(say_session))
        .route("/sessions/{id}/answer", post(answer_session))
        .route("/sessions/{id}/stop", post(stop_session))
        .route("/sessions/{id}/kill", post(kill_session))
        .with_state(api);
    axum::serve(listener, app).await?;
    Ok(())
}

async fn health(State(api): State<Arc<Api>>, headers: HeaderMap) -> Json<Value> {
    let mut status = json!({"ok": true, "model": api.chat.model});
    if api.authorized(&headers) {
        let tools: Vec<&str> = api
            .chat
            .tools()
            .defs()
            .iter()
            .map(|d| d.name.as_str())
            .collect();
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
        if let Piece::Tool(name, _) = piece {
            tools.push(name);
        }
    }
    match task.await {
        Ok(Ok(reply)) => {
            Json(json!({"reply": reply.trim(), "conversation": id, "tools": tools})).into_response()
        }
        Ok(Err(e)) => error(StatusCode::BAD_GATEWAY, format!("{e:#}")),
        Err(e) => error(StatusCode::INTERNAL_SERVER_ERROR, e.to_string()),
    }
}

/// The header an HTTP request sends the agent's PIN in.
const PIN_HEADER: &str = "x-inkharness-pin";

/// The agent sessions, if this request may use them: the token, then the PIN if one is set.
fn sessions<'a>(api: &'a Api, headers: &HeaderMap) -> Result<&'a Arc<Agent>, Box<Response>> {
    if !api.authorized(headers) {
        return Err(Box::new(error(
            StatusCode::UNAUTHORIZED,
            "wrong or missing token".into(),
        )));
    }
    let Some(agent) = &api.agent else {
        return Err(Box::new(error(
            StatusCode::NOT_FOUND,
            "no sessions on this daemon".into(),
        )));
    };
    if !agent.has_pin() {
        return Ok(agent);
    }
    let Some(pin) = headers.get(PIN_HEADER).and_then(|v| v.to_str().ok()) else {
        return Err(Box::new(error(
            StatusCode::FORBIDDEN,
            "the agent needs its PIN: X-Inkharness-Pin".into(),
        )));
    };
    match agent.check_pin(pin) {
        None | Some(Unlock::Ok { .. }) => Ok(agent),
        Some(Unlock::Wrong { wait_s }) => {
            let body = json!({"error": "wrong PIN", "wait_s": wait_s});
            Err(Box::new(
                (StatusCode::FORBIDDEN, Json(body)).into_response(),
            ))
        }
        Some(Unlock::Wait { wait_s }) => {
            let body =
                json!({"error": format!("too many wrong PINs: wait {wait_s} s"), "wait_s": wait_s});
            let retry = [(header::RETRY_AFTER, wait_s.to_string())];
            Err(Box::new(
                (StatusCode::TOO_MANY_REQUESTS, retry, Json(body)).into_response(),
            ))
        }
    }
}

fn refused(refusal: Refusal) -> Response {
    let status = match refusal {
        Refusal::NoSuchSession => StatusCode::NOT_FOUND,
        Refusal::NoSuchChoice => StatusCode::BAD_REQUEST,
        Refusal::AlreadyAnswered | Refusal::NotRunning | Refusal::NotWaiting | Refusal::Busy => {
            StatusCode::CONFLICT
        }
        Refusal::NotAllowed => StatusCode::FORBIDDEN,
        Refusal::Unreachable => StatusCode::BAD_GATEWAY,
        Refusal::Other(_) => StatusCode::UNPROCESSABLE_ENTITY,
    };
    error(status, refusal.to_string())
}

fn done(result: Result<(), Refusal>) -> Response {
    match result {
        Ok(()) => Json(json!({"ok": true})).into_response(),
        Err(refusal) => refused(refusal),
    }
}

#[derive(Deserialize)]
struct Text {
    text: String,
}

#[derive(Deserialize)]
struct AnswerRequest {
    ask: String,
    choice: usize,
}

async fn list_sessions(State(api): State<Arc<Api>>, headers: HeaderMap) -> Response {
    match sessions(&api, &headers) {
        Ok(agent) => Json(ServerMsg::Sessions(agent.list(false))).into_response(),
        Err(response) => *response,
    }
}

async fn get_session(
    State(api): State<Arc<Api>>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Response {
    match sessions(&api, &headers) {
        Ok(agent) => match agent.view(&id) {
            Some(view) => Json(ServerMsg::Session(Box::new(view))).into_response(),
            None => refused(Refusal::NoSuchSession),
        },
        Err(response) => *response,
    }
}

async fn start_session(
    State(api): State<Arc<Api>>,
    headers: HeaderMap,
    Json(request): Json<Text>,
) -> Response {
    let agent = match sessions(&api, &headers) {
        Ok(agent) => agent,
        Err(response) => return *response,
    };
    if request.text.trim().is_empty() {
        return error(StatusCode::BAD_REQUEST, "no text".into());
    }
    match agent.start(&request.text).await {
        Ok(id) => Json(json!({"session": id})).into_response(),
        Err(refusal) => refused(refusal),
    }
}

async fn say_session(
    State(api): State<Arc<Api>>,
    headers: HeaderMap,
    Path(id): Path<String>,
    Json(request): Json<Text>,
) -> Response {
    let agent = match sessions(&api, &headers) {
        Ok(agent) => agent,
        Err(response) => return *response,
    };
    if request.text.trim().is_empty() {
        return error(StatusCode::BAD_REQUEST, "no text".into());
    }
    done(agent.say(&id, &request.text).await.map(|_| ()))
}

async fn answer_session(
    State(api): State<Arc<Api>>,
    headers: HeaderMap,
    Path(id): Path<String>,
    Json(request): Json<AnswerRequest>,
) -> Response {
    match sessions(&api, &headers) {
        Ok(agent) => done(agent.answer(&id, &request.ask, request.choice).await),
        Err(response) => *response,
    }
}

async fn stop_session(
    State(api): State<Arc<Api>>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Response {
    match sessions(&api, &headers) {
        Ok(agent) => done(agent.stop(&id).await),
        Err(response) => *response,
    }
}

async fn kill_session(
    State(api): State<Arc<Api>>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Response {
    match sessions(&api, &headers) {
        Ok(agent) => done(agent.kill(&id).await),
        Err(response) => *response,
    }
}

async fn delete_session(
    State(api): State<Arc<Api>>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Response {
    match sessions(&api, &headers) {
        Ok(agent) => done(agent.delete(&id).await),
        Err(response) => *response,
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
            Some(Piece::Tool(name, _)) => {
                (event(json!({"tool": name})), Stream::Turn(rx, task, id))
            }
            Some(Piece::Audio(_)) => unreachable!(),
            None => {
                let end = match task.await {
                    Ok(Ok(reply)) => {
                        json!({"done": true, "reply": reply.trim(), "conversation": id})
                    }
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
        assert!(
            first["reply"]
                .as_str()
                .unwrap()
                .starts_with("You said: one.")
        );
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
        assert!(
            streamed.contains(&format!(r#""conversation":"{id}""#)),
            "{streamed}"
        );
        let health: Value = http
            .get(format!("{url}/health"))
            .send()
            .await
            .unwrap()
            .json()
            .await
            .unwrap();
        assert_eq!(health["model"], "mock");
        assert!(
            health["tools"]
                .as_array()
                .unwrap()
                .iter()
                .any(|t| t == "run_js")
        );
    }

    /// An API with agent sessions (the mock session model) behind the token and the PIN 1234.
    async fn start_agent(name: &str) -> String {
        let place = Place {
            config: Config::from_table(toml::from_str("[llm]\nbackend = \"mock\"\n").unwrap())
                .unwrap(),
            read: vec![],
            instructions: vec![],
            skill_dirs: vec![],
        };
        let harness = Harness::build(&place, false).await.unwrap();
        let chat = Chat::build(&place.config, &harness).unwrap();
        let dir = crate::skills::tests::scratch(name);
        let mut cfg = crate::agent::tests::cfg(&dir);
        cfg.pin_sha256 = Some(crate::agent::sha256_hex("1234"));
        let llm = crate::agent::model(&crate::config::Llm::Mock, harness.http.clone()).unwrap();
        let agent = Agent::open(
            &cfg,
            llm,
            harness.tools.clone(),
            crate::agent::prompt(),
            "test",
            Default::default(),
        )
        .unwrap();
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = format!("http://{}", listener.local_addr().unwrap());
        let api = Arc::new(Api::new(chat, "s3cret".into(), vec![]).with_agent(Some(agent)));
        tokio::spawn(serve(listener, api));
        url
    }

    #[tokio::test]
    async fn sessions_over_http() {
        let url = start_agent("http-agent").await;
        let http = reqwest::Client::new();
        let call = |method: reqwest::Method, path: &str, pin: Option<&str>, body: Option<Value>| {
            let mut request = http
                .request(method, format!("{url}{path}"))
                .bearer_auth("s3cret");
            if let Some(pin) = pin {
                request = request.header("X-Inkharness-Pin", pin);
            }
            if let Some(body) = body {
                request = request.json(&body);
            }
            async move {
                let response = request.send().await.unwrap();
                let status = response.status().as_u16();
                (
                    status,
                    response.json::<Value>().await.unwrap_or(Value::Null),
                )
            }
        };
        use reqwest::Method;
        let pin = Some("1234");
        assert_eq!(call(Method::GET, "/sessions", None, None).await.0, 403);
        let (status, body) = call(Method::GET, "/sessions", Some("0000"), None).await;
        assert_eq!((status, body["error"].as_str()), (403, Some("wrong PIN")));
        let (status, list) = call(Method::GET, "/sessions", pin, None).await;
        assert_eq!(
            (status, list["type"].as_str(), list["locked"].as_bool()),
            (200, Some("sessions"), Some(false))
        );

        let (status, started) = call(
            Method::POST,
            "/sessions",
            pin,
            Some(json!({"text": "plan a trip and ask me"})),
        )
        .await;
        assert_eq!(status, 200);
        let id = started["session"].as_str().unwrap().to_string();
        let path = format!("/sessions/{id}");
        let wait_for = |state: &'static str| {
            let call = &call;
            let path = path.clone();
            async move {
                for _ in 0..300 {
                    let (_, view) = call(Method::GET, &path, pin, None).await;
                    if view["state"] == state {
                        return view;
                    }
                    tokio::time::sleep(std::time::Duration::from_millis(10)).await;
                }
                panic!("never {state}")
            }
        };
        let view = wait_for("ask").await;
        assert_eq!(view["type"], "session");
        assert_eq!(view["ask"]["id"], "a1");
        let answer = format!("{path}/answer");
        assert_eq!(
            call(
                Method::POST,
                &answer,
                pin,
                Some(json!({"ask": "a1", "choice": 9}))
            )
            .await
            .0,
            400
        );
        assert_eq!(
            call(
                Method::POST,
                &answer,
                pin,
                Some(json!({"ask": "a1", "choice": 1}))
            )
            .await
            .0,
            200
        );
        let view = wait_for("done").await;
        assert!(
            view["result"].as_str().unwrap().contains("Northgate"),
            "{view}"
        );
        assert_eq!(
            call(
                Method::POST,
                &answer,
                pin,
                Some(json!({"ask": "a1", "choice": 1}))
            )
            .await
            .0,
            409
        );
        assert_eq!(
            call(Method::POST, &format!("{path}/stop"), pin, None)
                .await
                .0,
            409
        );
        // Talking into a finished one starts it again.
        let say = format!("{path}/say");
        assert_eq!(
            call(Method::POST, &say, pin, Some(json!({"text": "thanks"})))
                .await
                .0,
            200
        );
        let view = wait_for("done").await;
        assert!(
            view["result"]
                .as_str()
                .unwrap()
                .starts_with("You said: thanks"),
            "{view}"
        );
        assert_eq!(call(Method::DELETE, &path, pin, None).await.0, 200);
        assert_eq!(call(Method::GET, &path, pin, None).await.0, 404);
        assert_eq!(
            call(
                Method::POST,
                "/sessions/nope/say",
                pin,
                Some(json!({"text": "x"}))
            )
            .await
            .0,
            404
        );
    }

    #[tokio::test]
    async fn no_sessions_without_an_agent() {
        let url = start("").await;
        let response = reqwest::get(format!("{url}/sessions")).await.unwrap();
        assert_eq!(response.status(), 404);
        let body: Value = response.json().await.unwrap();
        assert_eq!(body["error"], "no sessions on this daemon");
    }

    #[tokio::test]
    async fn a_token_is_asked_for() {
        let url = start("s3cret").await;
        let http = reqwest::Client::new();
        let chat = |token: Option<&str>| {
            let mut request = http
                .post(format!("{url}/chat"))
                .json(&json!({"text": "hi"}));
            if let Some(token) = token {
                request = request.bearer_auth(token);
            }
            request.send()
        };
        assert_eq!(chat(None).await.unwrap().status(), 401);
        assert_eq!(chat(Some("nope")).await.unwrap().status(), 401);
        assert_eq!(chat(Some("s3cret")).await.unwrap().status(), 200);
        let health: Value = http
            .get(format!("{url}/health"))
            .send()
            .await
            .unwrap()
            .json()
            .await
            .unwrap();
        assert!(health.get("tools").is_none());
    }
}
