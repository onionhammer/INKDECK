//! herdr-core's API, as `hc connect` speaks it on stdin and stdout: JSON-RPC 2.0, one message a
//! line (`hc schema` has it all). Requests share one connection, opened when first needed and
//! again after it drops, never more often than the backoff allows, so a herdr that is down
//! costs a failed request now and then and never takes the daemon down. A subscription
//! (`events.subscribe`) gets a connection of its own, which then carries only notifications.

use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use futures_util::future::BoxFuture;
use serde_json::{Value, json};
use tokio::io::{AsyncBufReadExt, AsyncRead, AsyncWrite, AsyncWriteExt, BufReader};
use tokio::sync::{mpsc, oneshot};

/// The protocol generation spoken.
const GENERATION: u32 = 1;
/// A request that takes longer fails (terminal.run waits for its program to start).
const TIMEOUT: Duration = Duration::from_secs(120);
const LAST_BACKOFF: Duration = Duration::from_secs(30);

pub type Reader = Box<dyn AsyncRead + Send + Unpin>;
pub type Writer = Box<dyn AsyncWrite + Send + Unpin>;
/// A connection's two halves, and what to keep alive while it lasts (the `hc connect` process).
pub type Transport = (Reader, Writer, Option<Box<dyn std::any::Any + Send>>);
/// Opens a connection.
pub type Connector = Arc<dyn Fn() -> BoxFuture<'static, std::io::Result<Transport>> + Send + Sync>;

/// `<command> connect`, as a child process.
pub fn command_connector(command: Vec<String>) -> Connector {
    Arc::new(move || {
        let command = command.clone();
        Box::pin(async move {
            let (program, args) = command
                .split_first()
                .ok_or_else(|| std::io::Error::other("[herdr] command is empty"))?;
            let mut child = tokio::process::Command::new(program)
                .args(args)
                .arg("connect")
                .stdin(std::process::Stdio::piped())
                .stdout(std::process::Stdio::piped())
                .stderr(std::process::Stdio::null())
                .kill_on_drop(true)
                .spawn()?;
            let stdin = child.stdin.take().expect("piped");
            let stdout = child.stdout.take().expect("piped");
            let keep: Box<dyn std::any::Any + Send> = Box::new(child);
            Ok((
                Box::new(stdout) as Reader,
                Box::new(stdin) as Writer,
                Some(keep),
            ))
        })
    })
}

#[derive(Debug, Clone, PartialEq)]
pub enum HcError {
    /// No connection to herdr.
    Down(String),
    /// herdr answered with an error: its `data.kind` (`not_found`, `blocked`,
    /// `status_mismatch`…) and message.
    Rpc { kind: String, message: String },
}

impl std::fmt::Display for HcError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            HcError::Down(why) => write!(f, "herdr is not reachable: {why}"),
            HcError::Rpc { kind, message } => write!(f, "herdr: {kind}: {message}"),
        }
    }
}

impl HcError {
    pub fn kind(&self) -> &str {
        match self {
            HcError::Down(_) => "down",
            HcError::Rpc { kind, .. } => kind,
        }
    }
}

type Pending = Arc<Mutex<HashMap<u64, oneshot::Sender<Result<Value, HcError>>>>>;

/// One open connection.
struct Conn {
    lines: mpsc::Sender<String>,
    pending: Pending,
    next: AtomicU64,
    open: Arc<std::sync::atomic::AtomicBool>,
    /// What herdr calls this machine, from its hello.
    core: String,
}

impl Conn {
    /// Opens a connection and says hello. Notifications go to `notes`, if given.
    async fn open(
        connector: &Connector,
        notes: Option<mpsc::Sender<Value>>,
    ) -> Result<Arc<Conn>, HcError> {
        let (reader, mut writer, keep) = connector()
            .await
            .map_err(|e| HcError::Down(e.to_string()))?;
        let pending: Pending = Arc::default();
        let open = Arc::new(std::sync::atomic::AtomicBool::new(true));
        let (lines, mut out) = mpsc::channel::<String>(64);
        tokio::spawn(async move {
            while let Some(line) = out.recv().await {
                if writer.write_all(line.as_bytes()).await.is_err() || writer.flush().await.is_err()
                {
                    break;
                }
            }
        });
        {
            let (pending, open) = (pending.clone(), open.clone());
            tokio::spawn(async move {
                let _keep = keep; // the process lives as long as its output is read
                let mut input = BufReader::new(reader).lines();
                while let Ok(Some(line)) = input.next_line().await {
                    let Ok(message) = serde_json::from_str::<Value>(&line) else {
                        continue;
                    };
                    match message.get("id").and_then(Value::as_u64) {
                        Some(id) if message.get("method").is_none() => {
                            let answer = match message.get("error") {
                                Some(error) => Err(HcError::Rpc {
                                    kind: error["data"]["kind"]
                                        .as_str()
                                        .or_else(|| error["message"].as_str())
                                        .unwrap_or("error")
                                        .to_string(),
                                    message: error["message"]
                                        .as_str()
                                        .unwrap_or_default()
                                        .to_string(),
                                }),
                                None => Ok(message.get("result").cloned().unwrap_or(Value::Null)),
                            };
                            if let Some(waiting) = pending.lock().unwrap().remove(&id) {
                                let _ = waiting.send(answer);
                            }
                        }
                        _ => {
                            if let Some(notes) = &notes
                                && notes.send(message).await.is_err()
                            {
                                break;
                            }
                        }
                    }
                }
                open.store(false, Ordering::Relaxed);
                for (_, waiting) in pending.lock().unwrap().drain() {
                    let _ = waiting.send(Err(HcError::Down("the connection closed".into())));
                }
            });
        }
        let mut conn = Conn {
            lines,
            pending,
            next: AtomicU64::new(1),
            open,
            core: String::new(),
        };
        let hello = conn
            .call("hello", json!({"generation": GENERATION}))
            .await?;
        conn.core = hello["core"]["name"]
            .as_str()
            .unwrap_or_default()
            .to_string();
        Ok(Arc::new(conn))
    }

    async fn call(&self, method: &str, params: Value) -> Result<Value, HcError> {
        let id = self.next.fetch_add(1, Ordering::Relaxed);
        let (done, answer) = oneshot::channel();
        self.pending.lock().unwrap().insert(id, done);
        let request = json!({"jsonrpc": "2.0", "id": id, "method": method, "params": params});
        if self.lines.send(format!("{request}\n")).await.is_err() {
            self.pending.lock().unwrap().remove(&id);
            return Err(HcError::Down("the connection closed".into()));
        }
        match tokio::time::timeout(TIMEOUT, answer).await {
            Ok(Ok(answer)) => answer,
            Ok(Err(_)) => Err(HcError::Down("the connection closed".into())),
            Err(_) => {
                self.pending.lock().unwrap().remove(&id);
                Err(HcError::Down(format!(
                    "{method} took over {} s",
                    TIMEOUT.as_secs()
                )))
            }
        }
    }

    fn is_open(&self) -> bool {
        self.open.load(Ordering::Relaxed)
    }
}

/// herdr, as the daemon talks to it.
pub struct Hc {
    connector: Connector,
    conn: tokio::sync::Mutex<Option<Arc<Conn>>>,
    /// No new connection before this, and how long to wait after the next failure.
    retry: Mutex<(Option<Instant>, Duration)>,
    first_backoff: Duration,
}

impl Hc {
    /// `first_backoff`: the first wait between attempts, doubling to LAST_BACKOFF.
    pub fn with_backoff(connector: Connector, first_backoff: Duration) -> Hc {
        Hc {
            connector,
            conn: tokio::sync::Mutex::new(None),
            retry: Mutex::new((None, first_backoff)),
            first_backoff,
        }
    }

    /// The request connection, opened if it isn't.
    async fn conn(&self) -> Result<Arc<Conn>, HcError> {
        let mut slot = self.conn.lock().await;
        if let Some(conn) = slot.as_ref().filter(|c| c.is_open()) {
            return Ok(conn.clone());
        }
        if let (Some(at), _) = *self.retry.lock().unwrap()
            && Instant::now() < at
        {
            return Err(HcError::Down("waiting to try again".into()));
        }
        match Conn::open(&self.connector, None).await {
            Ok(conn) => {
                *self.retry.lock().unwrap() = (None, self.first_backoff);
                *slot = Some(conn.clone());
                Ok(conn)
            }
            Err(e) => {
                let mut retry = self.retry.lock().unwrap();
                let wait = retry.1;
                *retry = (Some(Instant::now() + wait), (wait * 2).min(LAST_BACKOFF));
                Err(e)
            }
        }
    }

    pub async fn call(&self, method: &str, params: Value) -> Result<Value, HcError> {
        self.conn().await?.call(method, params).await
    }

    /// What herdr calls this machine ("" until a connection has said).
    pub async fn core(&self) -> String {
        match self.conn().await {
            Ok(conn) => conn.core.clone(),
            Err(_) => String::new(),
        }
    }

    /// Subscribes to every terminal's changes on a connection of its own: the snapshot (the
    /// `events.subscribe` result), what herdr calls this machine, and the notifications, which
    /// end when the connection does.
    pub async fn subscribe(&self) -> Result<(Value, String, mpsc::Receiver<Value>), HcError> {
        let (notes, events) = mpsc::channel(256);
        let conn = Conn::open(&self.connector, Some(notes)).await?;
        let snapshot = conn.call("events.subscribe", json!({})).await?;
        let core = conn.core.clone();
        // The connection lives as long as its notifications are read.
        let (forward, received) = mpsc::channel(256);
        tokio::spawn(async move {
            let _conn = conn;
            let mut events = events;
            while let Some(note) = events.recv().await {
                if forward.send(note).await.is_err() {
                    break;
                }
            }
        });
        Ok((snapshot, core, received))
    }
}

#[cfg(test)]
pub(crate) mod fake {
    //! A herdr core for tests: terminals, screens and a log of what it was asked to do,
    //! reached through a Connector of in-memory pipes.

    use std::sync::atomic::{AtomicBool, AtomicUsize};

    use tokio::sync::broadcast;

    use super::*;

    pub struct FakeCore {
        pub name: String,
        pub terminals: Mutex<Vec<Value>>,
        pub screens: Mutex<HashMap<String, String>>,
        /// Every request but hello and the subscription: method and params.
        pub calls: Mutex<Vec<(String, Value)>>,
        pub templates: Vec<String>,
        events: broadcast::Sender<Value>,
        /// Down: connections are refused, and open ones close.
        pub up: AtomicBool,
        down: broadcast::Sender<()>,
        pub connects: AtomicUsize,
        seq: AtomicU64,
        next_terminal: AtomicU64,
    }

    impl FakeCore {
        pub fn new(name: &str, terminals: Vec<Value>) -> Arc<FakeCore> {
            Arc::new(FakeCore {
                name: name.into(),
                terminals: Mutex::new(terminals),
                screens: Mutex::default(),
                calls: Mutex::default(),
                templates: vec!["reviewer".into()],
                events: broadcast::channel(64).0,
                up: AtomicBool::new(true),
                down: broadcast::channel(4).0,
                connects: AtomicUsize::new(0),
                seq: AtomicU64::new(100),
                next_terminal: AtomicU64::new(50),
            })
        }

        pub fn connector(self: &Arc<Self>) -> Connector {
            let core = self.clone();
            Arc::new(move || {
                let core = core.clone();
                Box::pin(async move {
                    if !core.up.load(Ordering::Relaxed) {
                        return Err(std::io::Error::other("connection refused"));
                    }
                    core.connects.fetch_add(1, Ordering::Relaxed);
                    let (ours, theirs) = tokio::io::duplex(1 << 16);
                    tokio::spawn(core.clone().serve(theirs));
                    let (reader, writer) = tokio::io::split(ours);
                    Ok((Box::new(reader) as Reader, Box::new(writer) as Writer, None))
                })
            })
        }

        /// Takes herdr down (open connections close) or brings it back.
        pub fn set_up(&self, up: bool) {
            self.up.store(up, Ordering::Relaxed);
            if !up {
                let _ = self.down.send(());
            }
        }

        pub fn terminal(&self, address: &str) -> Option<Value> {
            let terminals = self.terminals.lock().unwrap();
            terminals.iter().find(|t| t["address"] == address).cloned()
        }

        /// Changes a terminal's status (null: none), as its program would, and tells
        /// subscribers. Leaving `working` counts a turn, as herdr does.
        pub fn set_status(&self, address: &str, status: Value) {
            let mut terminals = self.terminals.lock().unwrap();
            let Some(t) = terminals.iter_mut().find(|t| t["address"] == address) else {
                return;
            };
            if t["status"]["state"] == "working" && status["state"] != "working" {
                t["turn"] = json!(t["turn"].as_u64().unwrap_or(0) + 1);
            }
            t["status"] = status.clone();
            t["reported"] = json!(true);
            let data = json!({"status": status, "reported": true, "turn": t["turn"]});
            drop(terminals);
            self.event("status", address, data);
        }

        /// Ends a terminal's program.
        pub fn finish(&self, address: &str, exit_code: i64) {
            let mut terminals = self.terminals.lock().unwrap();
            if let Some(t) = terminals.iter_mut().find(|t| t["address"] == address) {
                t["state"] = json!("finished");
                t["exit_code"] = json!(exit_code);
                t["status"] = Value::Null;
                t["foreground"] = Value::Null;
            }
            drop(terminals);
            self.event("finished", address, json!({}));
        }

        pub fn event(&self, kind: &str, address: &str, data: Value) {
            let seq = self.seq.fetch_add(1, Ordering::Relaxed);
            let _ = self.events.send(json!({
                "jsonrpc": "2.0", "method": "event",
                "params": {"type": kind, "terminal": address, "seq": seq, "data": data},
            }));
        }

        pub fn calls(&self, method: &str) -> Vec<Value> {
            let calls = self.calls.lock().unwrap();
            calls
                .iter()
                .filter(|(m, _)| m == method)
                .map(|(_, p)| p.clone())
                .collect()
        }

        async fn serve(self: Arc<Self>, stream: tokio::io::DuplexStream) {
            let (reader, mut writer) = tokio::io::split(stream);
            let mut lines = BufReader::new(reader).lines();
            let mut down = self.down.subscribe();
            let mut events: Option<broadcast::Receiver<Value>> = None;
            loop {
                let next = async {
                    match events.as_mut() {
                        Some(events) => events.recv().await.ok(),
                        None => std::future::pending().await,
                    }
                };
                let out = tokio::select! {
                    line = lines.next_line() => {
                        let Ok(Some(line)) = line else { return };
                        let request: Value = serde_json::from_str(&line).unwrap();
                        let method = request["method"].as_str().unwrap_or_default().to_string();
                        if method == "events.subscribe" {
                            events = Some(self.events.subscribe());
                        }
                        let answer = self.handle(&method, &request["params"]);
                        match answer {
                            Ok(result) => json!({"jsonrpc": "2.0", "id": request["id"], "result": result}),
                            Err(kind) => json!({"jsonrpc": "2.0", "id": request["id"],
                                                "error": {"code": -32000, "message": kind, "data": {"kind": kind}}}),
                        }
                    }
                    note = next => match note {
                        Some(note) => note,
                        None => continue,
                    },
                    _ = down.recv() => return,
                };
                if writer
                    .write_all(format!("{out}\n").as_bytes())
                    .await
                    .is_err()
                {
                    return;
                }
            }
        }

        fn handle(&self, method: &str, params: &Value) -> Result<Value, &'static str> {
            if !matches!(method, "hello" | "events.subscribe") {
                self.calls
                    .lock()
                    .unwrap()
                    .push((method.into(), params.clone()));
            }
            let address = params["terminal"].as_str().unwrap_or_default();
            let found = || self.terminal(address).ok_or("not_found");
            match method {
                "hello" => Ok(
                    json!({"core": {"id": "c1", "name": self.name}, "generation": 1,
                                     "version": "0.1.18", "framed": false, "methods": [], "features": []}),
                ),
                "terminal.list" => Ok(json!({"terminals": *self.terminals.lock().unwrap(),
                                             "hidden_finished": 0, "machines": []})),
                "events.subscribe" => {
                    Ok(json!({"seq": 99, "terminals": *self.terminals.lock().unwrap()}))
                }
                "machine.list" => Ok(
                    json!({"machines": [{"name": "desktop", "state": "connected",
                    "host": "desktop", "id": "m1", "target": "desktop", "version": "0.1.18"}]}),
                ),
                "templates.list" => Ok(json!({"templates": self.templates.iter()
                    .map(|t| json!({"name": t, "description": ""})).collect::<Vec<_>>()})),
                "terminal.get" => found(),
                "terminal.read" => {
                    found()?;
                    let screens = self.screens.lock().unwrap();
                    let text = screens.get(address).cloned().unwrap_or_default();
                    Ok(json!({"text": text, "complete": true, "how": "screen"}))
                }
                "terminal.keys" | "terminal.interrupt" | "terminal.kill" => {
                    let t = found()?;
                    Ok(json!({"turn": t["turn"], "outcome": "stopped", "key": "esc"}))
                }
                "terminal.send" => {
                    let t = found()?;
                    let status = t["status"]["state"].as_str().unwrap_or_default();
                    if let Some(want) = params["if"].as_str()
                        && want != status
                    {
                        return Err("status_mismatch");
                    }
                    if status == "blocked" && params["force"] != true {
                        return Err("blocked");
                    }
                    Ok(json!({"turn": t["turn"], "status": status}))
                }
                "terminal.run" => {
                    let n = self.next_terminal.fetch_add(1, Ordering::Relaxed);
                    let on = params["on"].as_str().unwrap_or_default();
                    let address = if on.is_empty() {
                        format!("t{n}")
                    } else {
                        format!("{on}/t{n}")
                    };
                    let argv = params["argv"].clone();
                    let name = argv[0].as_str().unwrap_or("sh").to_string();
                    let terminal = json!({
                        "address": address, "argv": argv, "cwd": params["cwd"].as_str().unwrap_or("/home"),
                        "state": "live", "labels": params["labels"], "title": "", "turn": 0,
                        "reported": true, "status": {"state": "idle", "app": name},
                        "foreground": {"name": name, "argv": argv, "cwd": "/home", "pid": 1},
                        "created_ms": 1_795_000_000_000u64, "screen": "exact", "cols": 200, "rows": 120,
                    });
                    self.terminals.lock().unwrap().push(terminal.clone());
                    self.event("created", &address, json!({}));
                    Ok(terminal)
                }
                _ => Err("not_found"),
            }
        }
    }

    /// A terminal as herdr lists it.
    pub fn terminal(address: &str, program: &str, status: Option<&str>) -> Value {
        let mut t = json!({
            "address": address, "argv": ["/bin/bash"], "cwd": "/home/erik", "state": "live",
            "labels": {}, "title": "", "turn": 0, "reported": status.is_some(),
            "status": null, "foreground": {"name": program, "argv": [program], "cwd": "/home/erik/app", "pid": 7},
            "created_ms": 1_795_000_000_000u64, "screen": "exact", "cols": 200, "rows": 120,
        });
        if let Some(state) = status {
            t["status"] = json!({"state": state, "app": program});
        }
        t
    }
}

#[cfg(test)]
mod tests {
    use super::fake::{FakeCore, terminal};
    use super::*;

    #[tokio::test]
    async fn requests_and_a_subscription() {
        let core = FakeCore::new("testbox", vec![terminal("t1", "claude", Some("idle"))]);
        let hc = Hc::with_backoff(core.connector(), Duration::from_secs(1));
        assert_eq!(hc.core().await, "testbox");
        let list = hc.call("terminal.list", json!({})).await.unwrap();
        assert_eq!(list["terminals"][0]["address"], "t1");
        let err = hc
            .call("terminal.get", json!({"terminal": "t9"}))
            .await
            .unwrap_err();
        assert_eq!(err.kind(), "not_found");

        let (snapshot, name, mut events) = hc.subscribe().await.unwrap();
        assert_eq!(
            (snapshot["terminals"][0]["address"].as_str(), name.as_str()),
            (Some("t1"), "testbox")
        );
        core.set_status("t1", json!({"state": "working", "msg": "Reading"}));
        let note = events.recv().await.unwrap();
        assert_eq!(note["params"]["type"], "status");
        assert_eq!(note["params"]["data"]["status"]["msg"], "Reading");
    }

    #[tokio::test]
    async fn down_and_back() {
        let core = FakeCore::new("testbox", vec![]);
        core.set_up(false);
        let hc = Hc::with_backoff(core.connector(), Duration::from_millis(50));
        assert!(matches!(
            hc.call("terminal.list", json!({})).await,
            Err(HcError::Down(_))
        ));
        // Within the backoff, no new attempt is made.
        assert!(matches!(
            hc.call("terminal.list", json!({})).await,
            Err(HcError::Down(_))
        ));
        assert_eq!(core.connects.load(Ordering::Relaxed), 0);
        core.set_up(true);
        tokio::time::sleep(Duration::from_millis(60)).await;
        assert!(hc.call("terminal.list", json!({})).await.is_ok());
        // An open connection that drops is opened again on the next request.
        core.set_up(false);
        tokio::time::sleep(Duration::from_millis(10)).await;
        core.set_up(true);
        assert!(hc.call("terminal.list", json!({})).await.is_ok());
        assert_eq!(core.connects.load(Ordering::Relaxed), 2);
    }
}
