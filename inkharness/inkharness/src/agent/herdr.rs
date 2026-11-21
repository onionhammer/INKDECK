//! The herdr backend: programs in herdr terminals (Claude Code, Codex, a shell…), here or on
//! other herdr machines, as sessions. Nobody starts them from the board: every terminal whose
//! program counts (`[herdr] machines`, `programs`) is a session, and its program's own status
//! (OSC 7501) is the session's state. A question it blocks on is read off its screen and put to
//! the board; the answer presses the keys the choice stands for. Words said into it are typed.
//!
//! herdr is reached through hc.rs. If it goes away, its sessions keep their last state, actions
//! on them say herdr is not reachable, and the subscription comes back on its own.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, Weak};
use std::time::{Duration, Instant};

use serde_json::{Value, json};
use tokio::sync::mpsc;
use voice_proto::{AskOption, Backend, State};

use super::store::{self, HerdrSide, Question, Session};
use super::{Agent, Live, Refusal, Said};
use crate::config::{self, Policy};
use crate::hc::{Connector, Hc, HcError};
use crate::llm::{self, Piece};

/// How long a look at a herdr session keeps its screen fresh.
#[cfg(not(test))]
pub const WATCH: Duration = Duration::from_secs(6);
#[cfg(test)]
pub const WATCH: Duration = Duration::from_millis(300);
/// How often a watched screen is read again.
#[cfg(not(test))]
const SCREEN_EVERY: Duration = Duration::from_secs(2);
#[cfg(test)]
const SCREEN_EVERY: Duration = Duration::from_millis(50);
/// Lines of screen a session shows, and their width.
const SCREEN_LINES: usize = 12;
const SCREEN_CHARS: usize = 80;
/// Lines read to find a question.
const QUESTION_LINES: usize = 40;
/// How often everything is listed again, in case an event went missing.
const RELIST_EVERY: Duration = Duration::from_secs(60);
/// A label inkharness puts on the terminals it starts: they count whatever their program.
const LABEL: &str = "inkharness.session";
/// How long a program started with a task has to say it is ready before the task is typed.
const READY_WAIT: Duration = Duration::from_secs(20);
/// How long one that says nothing at all is given.
const QUIET_READY: Duration = Duration::from_secs(5);
pub const RUN_TOOL: &str = "herdr_run";

pub struct Herdr {
    cfg: config::Herdr,
    hc: Hc,
    /// What herdr calls this machine.
    core: Mutex<String>,
    /// Terminals by address, as herdr last described them.
    terminals: Mutex<HashMap<String, Value>>,
    /// Sessions deleted from the list while their terminal lives: address → its turn then. One
    /// comes back when its turn moves on.
    forgotten: Mutex<HashMap<String, u64>>,
    forgotten_file: PathBuf,
    /// Whether to ask the session model to read questions off a screen (not the mock).
    use_model: bool,
    up: AtomicBool,
    first_backoff: Duration,
}

impl Herdr {
    /// herdr as `[herdr]` says, reached through `connector`; what it remembers goes in `dir`.
    pub fn new(
        cfg: config::Herdr,
        connector: Connector,
        dir: &std::path::Path,
        use_model: bool,
        first_backoff: Duration,
    ) -> Arc<Herdr> {
        let forgotten_file = dir.join("herdr-forgotten.json");
        let forgotten = std::fs::read(&forgotten_file)
            .ok()
            .and_then(|bytes| serde_json::from_slice(&bytes).ok())
            .unwrap_or_default();
        Arc::new(Herdr {
            cfg,
            hc: Hc::with_backoff(connector, first_backoff),
            core: Mutex::default(),
            terminals: Mutex::default(),
            forgotten: Mutex::new(forgotten),
            forgotten_file,
            use_model,
            up: AtomicBool::new(false),
            first_backoff,
        })
    }

    pub fn allows(&self, policy: Policy) -> bool {
        policy == Policy::Allow
    }

    /// Whether sessions get herdr_run.
    pub fn runs(&self) -> bool {
        self.allows(self.cfg.run)
    }

    /// Whether words may be typed into its programs.
    pub fn sends(&self) -> bool {
        self.allows(self.cfg.send)
    }

    fn check(&self, policy: Policy) -> Result<(), Refusal> {
        if self.allows(policy) {
            Ok(())
        } else {
            Err(Refusal::NotAllowed)
        }
    }

    /// Forgets a session deleted from the list, until its terminal's turn moves past `turn`.
    pub fn forget(&self, address: &str, turn: u64) {
        let mut forgotten = self.forgotten.lock().unwrap();
        forgotten.insert(address.to_string(), turn);
        self.save_forgotten(&forgotten);
    }

    fn save_forgotten(&self, forgotten: &HashMap<String, u64>) {
        if let Ok(json) = serde_json::to_vec(forgotten) {
            let _ = std::fs::write(&self.forgotten_file, json);
        }
    }

    fn machine_name<'a>(&self, address: &'a str, core: &'a str) -> &'a str {
        match address.split_once('/') {
            Some((machine, _)) => machine,
            None => core,
        }
    }

    /// Whether a terminal is a session: on a machine that counts, running a program that does
    /// (or started by inkharness).
    fn counts(&self, t: &Value, core: &str) -> bool {
        let address = t["address"].as_str().unwrap_or_default();
        let machine = self.machine_name(address, core);
        if !self.cfg.machines.is_empty()
            && !self
                .cfg
                .machines
                .iter()
                .any(|m| m.eq_ignore_ascii_case(machine))
        {
            return false;
        }
        if t["labels"].get(LABEL).is_some() {
            return true;
        }
        if self.cfg.programs.is_empty() {
            return t["status"].is_object();
        }
        let names = names(t);
        self.cfg
            .programs
            .iter()
            .any(|p| names.iter().any(|n| n.eq_ignore_ascii_case(p)))
    }

    fn rpc_refusal(e: HcError) -> Refusal {
        match e {
            HcError::Down(_) => Refusal::Unreachable,
            HcError::Rpc { kind, .. } => match kind.as_str() {
                "status_mismatch" | "blocked" => Refusal::NotWaiting,
                "finished" | "not_found" => Refusal::Other("its program has ended".into()),
                _ => Refusal::Other(format!("herdr: {kind}")),
            },
        }
    }

    // What the board and HTTP ask of a herdr session.

    /// Presses the keys the question's choice stands for, if it still waits for that question.
    pub async fn answer(
        &self,
        agent: &Arc<Agent>,
        id: &str,
        ask: &str,
        choice: usize,
    ) -> Result<(), Refusal> {
        self.check(self.cfg.answer)?;
        let (side, title) = {
            let sessions = agent.sessions.lock().unwrap();
            let live = sessions.get(id).ok_or(Refusal::NoSuchSession)?;
            let question = live
                .session
                .ask
                .as_ref()
                .filter(|q| q.id == ask)
                .ok_or(Refusal::AlreadyAnswered)?;
            let title = question
                .options
                .get(choice)
                .ok_or(Refusal::NoSuchChoice)?
                .title
                .clone();
            (live.session.herdr.clone().unwrap_or_default(), title)
        };
        let keys = side.keys.get(choice).cloned().unwrap_or_default();
        if keys.is_empty() {
            return Err(Refusal::NoSuchChoice);
        }
        self.still_blocked(agent, &side).await?;
        self.hc
            .call(
                "terminal.keys",
                json!({"terminal": side.address, "keys": keys}),
            )
            .await
            .map_err(Self::rpc_refusal)?;
        agent.herdr_answered(id, &format!("answered: {title}"));
        Ok(())
    }

    /// The terminal still waits on the question put at `side.turn`: else the session catches up
    /// and the answer is refused.
    async fn still_blocked(&self, agent: &Arc<Agent>, side: &HerdrSide) -> Result<(), Refusal> {
        let t = self
            .hc
            .call("terminal.get", json!({"terminal": side.address}))
            .await
            .map_err(Self::rpc_refusal)?;
        let blocked =
            t["status"]["state"] == "blocked" && t["turn"].as_u64().unwrap_or(0) == side.turn;
        if !blocked {
            self.terminals
                .lock()
                .unwrap()
                .insert(side.address.clone(), t.clone());
            let core = self.core.lock().unwrap().clone();
            agent.herdr_apply(self, &t, &core);
            return Err(Refusal::NotWaiting);
        }
        Ok(())
    }

    /// Types words into it: when it is ready for them (idle, done), or as the answer to a
    /// question it blocked on. While it works they wait.
    pub async fn say(&self, agent: &Arc<Agent>, id: &str, text: &str) -> Result<Said, Refusal> {
        self.check(self.cfg.send)?;
        let (state, side, program, place) = {
            let sessions = agent.sessions.lock().unwrap();
            let live = sessions.get(id).ok_or(Refusal::NoSuchSession)?;
            let s = &live.session;
            (
                s.state,
                s.herdr.clone().unwrap_or_default(),
                s.program.clone().unwrap_or_else(|| "the program".into()),
                s.place.clone().unwrap_or_else(|| agent.place.clone()),
            )
        };
        let params = match state {
            State::Working => return Err(Refusal::Busy),
            State::Ask => {
                if matches!(side.kind.as_deref(), Some("permission" | "auth")) {
                    return Err(Refusal::Other(
                        "it's asking for permission: pick one of its choices".into(),
                    ));
                }
                self.still_blocked(agent, &side).await?;
                json!({"terminal": side.address, "text": text, "force": true})
            }
            // Guarded, so words never land in the middle of its work; a program that reports
            // no status can't be.
            State::Idle if side.reports => {
                json!({"terminal": side.address, "text": text, "if": "idle"})
            }
            State::Idle => json!({"terminal": side.address, "text": text}),
            State::Done => json!({"terminal": side.address, "text": text}),
            _ => return Err(Refusal::NotWaiting),
        };
        self.hc
            .call("terminal.send", params)
            .await
            .map_err(|e| match e.kind() {
                "status_mismatch" => Refusal::Busy,
                _ => Self::rpc_refusal(e),
            })?;
        let line = match state {
            State::Ask => format!("answered: {text}"),
            _ => format!("you: {text}"),
        };
        agent.herdr_answered(id, &line);
        Ok(Said::Typed { program, place })
    }

    /// Presses its stop key (the board's STOP).
    pub async fn interrupt(&self, agent: &Arc<Agent>, id: &str) -> Result<(), Refusal> {
        self.check(self.cfg.interrupt)?;
        let side = agent.herdr_side(id)?;
        self.hc
            .call("terminal.interrupt", json!({"terminal": side.address}))
            .await
            .map_err(Self::rpc_refusal)?;
        agent.herdr_log(id, "stopped");
        Ok(())
    }

    /// Ends its program.
    pub async fn kill(&self, agent: &Arc<Agent>, id: &str) -> Result<(), Refusal> {
        self.check(self.cfg.kill)?;
        let side = agent.herdr_side(id)?;
        self.hc
            .call("terminal.kill", json!({"terminal": side.address}))
            .await
            .map_err(Self::rpc_refusal)?;
        agent.herdr_log(id, "ended");
        Ok(())
    }

    /// A new session from the board when `[agent] backend = "herdr"`: the default program on
    /// the default machine, given the words as its task.
    pub async fn start_default(&self, agent: &Arc<Agent>, task: &str) -> Result<String, Refusal> {
        let machine =
            (!self.cfg.default_machine.is_empty()).then(|| self.cfg.default_machine.clone());
        self.run(
            agent,
            machine,
            &self.cfg.default_program.clone(),
            task,
            None,
            None,
        )
        .await
        .map(|(id, _)| id)
    }

    /// Starts `program` (a template name or a command) in a new terminal and types `task` into
    /// it once it is ready. Its session's id, and where it runs.
    pub async fn run(
        &self,
        agent: &Arc<Agent>,
        machine: Option<String>,
        program: &str,
        task: &str,
        dir: Option<String>,
        from: Option<&str>,
    ) -> Result<(String, String), Refusal> {
        self.check(self.cfg.run)?;
        let mut params = json!({
            "labels": {LABEL: from.unwrap_or("board")},
            "no_parent": true,
        });
        if let Some(machine) = machine.as_deref().filter(|m| !m.is_empty()) {
            params["on"] = json!(machine);
        }
        if let Some(dir) = dir.filter(|d| !d.is_empty()) {
            params["cwd"] = json!(dir);
        }
        // An hc template starts its agent and hands it the task itself, once it is ready.
        let hc_template = self.hc_templates().await.contains(&program.to_string());
        let typed_later = if hc_template {
            params["argv"] = json!([program, task]);
            false
        } else if let Some(argv) = self.cfg.templates.get(program) {
            params["argv"] = json!(argv);
            true
        } else {
            params["line"] = json!(program);
            true
        };
        let t = self
            .hc
            .call("terminal.run", params)
            .await
            .map_err(Self::rpc_refusal)?;
        let address = t["address"].as_str().unwrap_or_default().to_string();
        let core = self.core().await;
        self.terminals
            .lock()
            .unwrap()
            .insert(address.clone(), t.clone());
        let id = agent
            .herdr_apply(self, &t, &core)
            .unwrap_or_else(|| session_id(&address));
        agent.herdr_prompt(&id, task);
        if typed_later && !task.trim().is_empty() {
            self.wait_ready(&address).await;
            self.hc
                .call("terminal.send", json!({"terminal": address, "text": task}))
                .await
                .map_err(Self::rpc_refusal)?;
        }
        let place = self.machine_name(&address, &core).to_uppercase();
        tracing::info!(session = %id, %address, program, "started in herdr");
        Ok((id, place))
    }

    /// Waits until a new terminal's program says it is ready (idle), up to READY_WAIT; one
    /// that has said nothing after QUIET_READY doesn't report, and is taken as ready.
    async fn wait_ready(&self, address: &str) {
        let start = Instant::now();
        while start.elapsed() < READY_WAIT {
            let t = self
                .hc
                .call("terminal.get", json!({"terminal": address}))
                .await;
            match t {
                Ok(t) if t["status"]["state"] == "idle" || t["state"] == "finished" => return,
                Ok(t) if t["status"].is_null() && start.elapsed() >= QUIET_READY => return,
                Err(_) => return,
                _ => tokio::time::sleep(Duration::from_millis(500)).await,
            }
        }
    }

    async fn hc_templates(&self) -> Vec<String> {
        match self.hc.call("templates.list", json!({})).await {
            Ok(list) => list["templates"]
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(|t| t["name"].as_str().map(str::to_string))
                .collect(),
            Err(_) => Vec::new(),
        }
    }

    async fn core(&self) -> String {
        let known = self.core.lock().unwrap().clone();
        if !known.is_empty() {
            return known;
        }
        let core = self.hc.core().await;
        *self.core.lock().unwrap() = core.clone();
        core
    }

    /// The herdr_run tool for a session: `{machine?, program, task, dir?}`.
    pub async fn run_tool(
        &self,
        agent: &Arc<Agent>,
        from: &str,
        input: &Value,
    ) -> Result<String, String> {
        let program = input["program"].as_str().filter(|p| !p.trim().is_empty());
        let Some(program) = program else {
            return Err("invalid input: program is required".into());
        };
        let task = input["task"].as_str().unwrap_or_default();
        let machine = input["machine"].as_str().map(str::to_string);
        let dir = input["dir"].as_str().map(str::to_string);
        match self
            .run(agent, machine, program, task, dir, Some(from))
            .await
        {
            Ok((id, place)) => Ok(format!(
                "Started {program} on {place} as session {id}; it shows on the board's list as a session of its own."
            )),
            Err(refusal) => Err(refusal.to_string()),
        }
    }

    // Following herdr.

    /// Follows every terminal's changes for as long as the daemon runs, subscribing again
    /// (with backoff) whenever the connection drops.
    async fn follow(self: Arc<Self>, agent: Weak<Agent>) {
        let mut backoff = self.first_backoff;
        loop {
            match self.hc.subscribe().await {
                Ok((snapshot, core, mut notes)) => {
                    backoff = self.first_backoff;
                    if !self.up.swap(true, Ordering::Relaxed) {
                        tracing::info!(core, "herdr: following its terminals");
                    }
                    *self.core.lock().unwrap() = core;
                    let Some(agent) = agent.upgrade() else { return };
                    self.snapshot(&agent, &snapshot["terminals"]).await;
                    let mut relist = tokio::time::interval(RELIST_EVERY);
                    relist.tick().await;
                    loop {
                        tokio::select! {
                            note = notes.recv() => {
                                let Some(note) = note else { break };
                                self.note(&agent, &note).await;
                            }
                            _ = relist.tick() => {
                                if let Ok(list) = self.hc.call("terminal.list", json!({})).await {
                                    self.snapshot(&agent, &list["terminals"]).await;
                                }
                            }
                        }
                    }
                    tracing::warn!(
                        "herdr: the connection dropped; its sessions keep their last state"
                    );
                }
                Err(e) => {
                    if self.up.swap(false, Ordering::Relaxed) {
                        tracing::warn!("herdr: {e}");
                    } else {
                        tracing::debug!("herdr: {e}");
                    }
                }
            }
            self.up.store(false, Ordering::Relaxed);
            tokio::time::sleep(backoff).await;
            backoff = (backoff * 2).min(Duration::from_secs(30));
        }
    }

    /// Everything herdr has: each terminal's session brought up to date, and the sessions whose
    /// terminal has gone (on a machine that is connected) finished.
    async fn snapshot(&self, agent: &Arc<Agent>, terminals: &Value) {
        let core = self.core.lock().unwrap().clone();
        let list: Vec<Value> = terminals.as_array().cloned().unwrap_or_default();
        {
            let mut known = self.terminals.lock().unwrap();
            known.clear();
            for t in &list {
                if let Some(address) = t["address"].as_str() {
                    known.insert(address.to_string(), t.clone());
                }
            }
        }
        let mut seen = Vec::new();
        for t in &list {
            if let Some(id) = agent.herdr_apply(self, t, &core) {
                seen.push(id);
            }
        }
        // A machine that isn't connected says nothing about its terminals.
        let connected: Vec<String> = match self.hc.call("machine.list", json!({})).await {
            Ok(list) => list["machines"]
                .as_array()
                .into_iter()
                .flatten()
                .filter(|m| m["state"] == "connected")
                .filter_map(|m| m["name"].as_str().map(str::to_string))
                .collect(),
            Err(_) => Vec::new(),
        };
        agent.herdr_gone_except(&seen, |address| match address.split_once('/') {
            Some((machine, _)) => connected.iter().any(|m| m == machine),
            None => true,
        });
    }

    /// One notification.
    async fn note(&self, agent: &Arc<Agent>, note: &Value) {
        let params = &note["params"];
        let address = params["terminal"].as_str().unwrap_or_default().to_string();
        let kind = params["type"].as_str().unwrap_or_default();
        let core = self.core.lock().unwrap().clone();
        match kind {
            "heartbeat" => {}
            "missed" => {
                if let Ok(list) = self.hc.call("terminal.list", json!({})).await {
                    self.snapshot(agent, &list["terminals"]).await;
                }
            }
            "removed" => {
                self.terminals.lock().unwrap().remove(&address);
                agent.herdr_gone(&session_id(&address), None);
            }
            // What changed came with the event: merged into what was known.
            "status" | "title" | "labels" | "turn" => {
                let merged = {
                    let mut known = self.terminals.lock().unwrap();
                    known.get_mut(&address).map(|t| {
                        if let Some(data) = params["data"].as_object() {
                            for (key, value) in data {
                                t[key.as_str()] = value.clone();
                            }
                        }
                        t.clone()
                    })
                };
                match merged {
                    Some(t) => {
                        agent.herdr_apply(self, &t, &core);
                    }
                    None => self.refetch(agent, &address, &core).await,
                }
            }
            // Anything else: ask for the terminal as it is now.
            _ if !address.is_empty() => self.refetch(agent, &address, &core).await,
            _ => {}
        }
    }

    async fn refetch(&self, agent: &Arc<Agent>, address: &str, core: &str) {
        match self
            .hc
            .call("terminal.get", json!({"terminal": address}))
            .await
        {
            Ok(t) => {
                self.terminals
                    .lock()
                    .unwrap()
                    .insert(address.to_string(), t.clone());
                agent.herdr_apply(self, &t, core);
            }
            Err(HcError::Rpc { kind, .. }) if kind == "not_found" => {
                self.terminals.lock().unwrap().remove(address);
                agent.herdr_gone(&session_id(address), None);
            }
            Err(_) => {}
        }
    }

    /// Reads a blocked program's screen and puts its question to the board.
    async fn read_question(
        self: Arc<Self>,
        agent: Arc<Agent>,
        id: String,
        side: HerdrSide,
        msg: String,
    ) {
        let screen = match self
            .hc
            .call(
                "terminal.read",
                json!({"terminal": side.address, "lines": QUESTION_LINES}),
            )
            .await
        {
            Ok(read) => plain_lines(read["text"].as_str().unwrap_or_default()),
            Err(e) => {
                tracing::debug!(session = %id, "herdr: couldn't read the screen: {e}");
                Vec::new()
            }
        };
        let by_model = if self.use_model {
            question_by_model(&agent, &screen, &msg).await
        } else {
            None
        };
        let (question, options) = by_model.unwrap_or_else(|| question_by_rule(&screen, &msg));
        agent.herdr_question(&id, side.turn, question, msg, options);
    }

    /// Reads the screens someone is looking at, every SCREEN_EVERY.
    async fn screens(self: Arc<Self>, agent: Weak<Agent>) {
        let mut tick = tokio::time::interval(SCREEN_EVERY);
        loop {
            tick.tick().await;
            let Some(agent) = agent.upgrade() else { return };
            for (id, address) in agent.watched_screens() {
                let read = self
                    .hc
                    .call("terminal.read", json!({"terminal": address, "lines": 0}))
                    .await;
                if let Ok(read) = read {
                    let lines = last_lines(&plain_lines(read["text"].as_str().unwrap_or_default()));
                    agent.herdr_screen(&id, lines);
                }
            }
        }
    }
}

/// A herdr session's id: the same for a terminal every time ("h" and seven letters and digits
/// from its address).
pub fn session_id(address: &str) -> String {
    const ALPHABET: &[u8] = b"abcdefghijkmnpqrstuvwxyz23456789";
    // FNV-1a: stable across runs and machines.
    let mut hash: u64 = 0xcbf29ce484222325;
    for byte in address.bytes() {
        hash ^= byte as u64;
        hash = hash.wrapping_mul(0x100000001b3);
    }
    let mut id = String::from("h");
    for _ in 0..7 {
        id.push(ALPHABET[(hash % 32) as usize] as char);
        hash /= 32;
    }
    id
}

/// What a terminal's program goes by: its status app, foreground name and command, template,
/// `program` label.
fn names(t: &Value) -> Vec<String> {
    let mut names = Vec::new();
    for value in [
        &t["status"]["app"],
        &t["foreground"]["name"],
        &t["template"],
        &t["labels"]["program"],
    ] {
        if let Some(name) = value.as_str() {
            names.push(name.to_string());
        }
    }
    if let Some(command) = t["foreground"]["argv"][0].as_str() {
        names.push(command.rsplit('/').next().unwrap_or(command).to_string());
    }
    names
}

/// The program, lowercase, as the board shows it: "claude code", "codex", "bash".
fn program(t: &Value) -> String {
    let name = t["status"]["app"]
        .as_str()
        .or_else(|| {
            t["foreground"]["argv"][0]
                .as_str()
                .map(|c| c.rsplit('/').next().unwrap_or(c))
        })
        .or_else(|| t["foreground"]["name"].as_str())
        .or_else(|| {
            t["argv"][0]
                .as_str()
                .map(|c| c.rsplit('/').next().unwrap_or(c))
        })
        .unwrap_or("program");
    name.replace(['-', '_'], " ").to_lowercase()
}

/// The title a terminal has: its name, a `title` label or its own title.
fn given_title(t: &Value) -> Option<String> {
    [&t["name"], &t["labels"]["title"], &t["title"]]
        .into_iter()
        .filter_map(Value::as_str)
        .map(str::trim)
        .find(|s| !s.is_empty())
        .map(|title| store::one_line(title, store::TITLE_CHARS))
}

/// A title for a terminal that has none: its program and folder.
fn fallback_title(t: &Value, program: &str) -> String {
    let cwd = t["foreground"]["cwd"]
        .as_str()
        .or_else(|| t["cwd"].as_str())
        .unwrap_or_default();
    let folder = cwd
        .trim_end_matches('/')
        .rsplit('/')
        .next()
        .unwrap_or_default();
    store::one_line(&format!("{program} · {folder}"), store::TITLE_CHARS)
}

/// The session state a terminal's program is in, and the program's own words.
fn state(t: &Value) -> (State, Option<String>) {
    let msg = t["status"]["msg"]
        .as_str()
        .filter(|m| !m.trim().is_empty())
        .map(str::to_string);
    if t["state"] == "finished" {
        return match t["exit_code"].as_i64() {
            Some(code) if code != 0 => (State::Fail, Some(format!("exited with code {code}"))),
            _ => (State::Done, msg),
        };
    }
    let word = t["status"]["state"].as_str();
    let state = match word {
        Some("working") => State::Working,
        Some("blocked") => State::Ask,
        Some("idle") => State::Idle,
        Some("done") => State::Done,
        Some("error") => State::Fail,
        // A program that says nothing: working while something runs in its shell.
        _ => {
            let shell = t["argv"][0]
                .as_str()
                .map(|c| c.rsplit('/').next().unwrap_or(c));
            let fg = t["foreground"]["name"].as_str();
            if fg.is_some() && fg != shell {
                State::Working
            } else {
                State::Idle
            }
        }
    };
    (state, msg)
}

impl Agent {
    /// Attaches herdr: its terminals become sessions from now on.
    pub fn attach_herdr(self: &Arc<Self>, herdr: Arc<Herdr>) {
        if self.herdr.set(herdr.clone()).is_ok() {
            tokio::spawn(herdr.clone().follow(Arc::downgrade(self)));
            tokio::spawn(herdr.screens(Arc::downgrade(self)));
        }
    }

    /// Brings a terminal's session up to date (making it if it is new); its id, or None if
    /// the terminal isn't a session.
    fn herdr_apply(self: &Arc<Self>, herdr: &Herdr, t: &Value, core: &str) -> Option<String> {
        let address = t["address"].as_str()?.to_string();
        let id = session_id(&address);
        let counts = herdr.counts(t, core);
        let turn = t["turn"].as_u64().unwrap_or(0);
        {
            let mut forgotten = herdr.forgotten.lock().unwrap();
            if let Some(&then) = forgotten.get(&address) {
                if turn <= then && t["state"] != "finished" {
                    return None;
                }
                forgotten.remove(&address);
                herdr.save_forgotten(&forgotten);
            }
        }
        let (state, msg) = state(t);
        let program = program(t);
        let machine = herdr.machine_name(&address, core).to_uppercase();
        let now = store::now();
        let mut sessions = self.sessions.lock().unwrap();
        let finished = t["state"] == "finished";
        if !counts && (!finished || !sessions.contains_key(&id)) {
            // A session whose program has gone (Claude Code ended, its shell is back).
            if let Some(live) = sessions.get_mut(&id)
                && !live.session.state.finished()
            {
                let s = &mut live.session;
                s.state = State::Done;
                s.ask = None;
                s.result = Some(format!(
                    "{} ended",
                    s.program.as_deref().unwrap_or("its program")
                ));
                s.unread = true;
                s.updated = now;
                self.save(s);
                self.notify(&id, false);
            }
            return None;
        }
        let new = !sessions.contains_key(&id);
        let live = sessions.entry(id.clone()).or_insert_with(|| {
            let mut s = Session::new(id.clone(), "", now);
            s.inbox.clear();
            s.prompt = t["foreground"]["argv"]
                .as_array()
                .or_else(|| t["argv"].as_array())
                .map(|a| {
                    a.iter()
                        .filter_map(Value::as_str)
                        .collect::<Vec<_>>()
                        .join(" ")
                })
                .unwrap_or_default();
            s.started = t["created_ms"].as_u64().map_or(now, |ms| ms / 1000);
            s.backend = Backend::Herdr;
            s.state = State::Idle;
            s.herdr = Some(HerdrSide {
                address: address.clone(),
                ..Default::default()
            });
            Live::new(s)
        });
        let s = &mut live.session;
        let before = (s.state, s.herdr.as_ref().map(|h| h.turn));
        // A title it was given wins; else the one it had (a task's), else program and folder.
        if let Some(title) = given_title(t) {
            s.title = title;
        } else if new {
            s.title = fallback_title(t, &program);
        }
        if finished && s.program.is_some() {
            // The program has gone from the finished terminal: keep the name it had.
        } else {
            s.program = Some(program.clone());
        }
        s.place = Some(machine);
        s.progress = t["status"]["progress"].as_u64().map(|p| p.min(100) as u8);
        let side = s.herdr.get_or_insert_with(Default::default);
        side.address = address.clone();
        side.reports = t["status"].is_object();
        let mut ask_now = None;
        match state {
            State::Ask => {
                let same_question = before == (State::Ask, Some(turn)) && s.ask.is_some();
                if !same_question {
                    side.turn = turn;
                    side.kind = t["status"]["kind"].as_str().map(str::to_string);
                    side.keys.clear();
                    s.asked += 1;
                    let question = msg
                        .clone()
                        .unwrap_or_else(|| format!("{program} needs you"));
                    s.ask = Some(Question {
                        id: format!("a{}", s.asked),
                        question: store::one_line(&question, 200),
                        detail: None,
                        options: Vec::new(),
                        recommend: None,
                        at: now,
                        deadline: None,
                    });
                    s.unread = true;
                    ask_now = Some((side.clone(), msg.clone().unwrap_or_default()));
                }
            }
            _ => {
                s.ask = None;
                side.turn = turn;
                side.keys.clear();
            }
        }
        if let Some(msg) = &msg
            && s.log.last().is_none_or(|l| &l.text != msg)
            && state != State::Ask
        {
            s.add_log(msg, now);
        }
        match state {
            State::Done | State::Fail => {
                if !s.state.finished() || s.result.is_none() {
                    s.unread = true;
                }
                s.result = Some(msg.clone().unwrap_or_else(|| match state {
                    State::Fail => format!("{program} failed"),
                    _ => format!("{program} finished"),
                }));
            }
            _ => s.result = None,
        }
        let changed = before.0 != state || ask_now.is_some();
        s.state = state;
        if changed || s.updated + 1 < now {
            s.updated = now;
        }
        self.save(s);
        drop(sessions);
        self.notify(&id, false);
        if let (Some((side, msg)), Some(herdr)) = (ask_now, self.herdr.get()) {
            tokio::spawn(
                herdr
                    .clone()
                    .read_question(self.clone(), id.clone(), side, msg),
            );
        }
        Some(id)
    }

    /// The question read off a blocked program's screen, with what each choice presses. It
    /// is put to the board unless the program has moved on.
    fn herdr_question(
        &self,
        id: &str,
        turn: u64,
        question: String,
        detail: String,
        options: Vec<(String, Vec<String>)>,
    ) {
        let mut sessions = self.sessions.lock().unwrap();
        let Some(live) = sessions.get_mut(id) else {
            return;
        };
        let s = &mut live.session;
        let current = s.state == State::Ask && s.herdr.as_ref().is_some_and(|h| h.turn == turn);
        let (Some(ask), true) = (s.ask.as_mut(), current) else {
            return;
        };
        ask.question = store::one_line(&question, 200);
        ask.detail = (!detail.trim().is_empty() && detail.trim() != ask.question)
            .then(|| store::cut(detail.trim(), 400));
        ask.options = options
            .iter()
            .map(|(title, keys)| AskOption {
                title: store::one_line(title, 32),
                right: Some(sends(keys)),
                detail: None,
            })
            .collect();
        if let Some(side) = s.herdr.as_mut() {
            side.keys = options.into_iter().map(|(_, keys)| keys).collect();
        }
        s.add_log(
            &format!(
                "asked: {}",
                s.ask
                    .as_ref()
                    .map(|q| q.question.as_str())
                    .unwrap_or_default()
            ),
            store::now(),
        );
        self.save(s);
        drop(sessions);
        self.notify(id, true);
    }

    /// After an answer or words typed in: logged, and working until its program says more.
    fn herdr_answered(&self, id: &str, line: &str) {
        let mut sessions = self.sessions.lock().unwrap();
        let Some(live) = sessions.get_mut(id) else {
            return;
        };
        let now = store::now();
        let s = &mut live.session;
        s.add_log(line, now);
        s.ask = None;
        s.state = State::Working;
        s.result = None;
        s.unread = false;
        s.updated = now;
        if let Some(side) = s.herdr.as_mut() {
            side.keys.clear();
        }
        self.save(s);
        drop(sessions);
        self.notify(id, false);
    }

    fn herdr_log(&self, id: &str, line: &str) {
        let mut sessions = self.sessions.lock().unwrap();
        if let Some(live) = sessions.get_mut(id) {
            live.session.add_log(line, store::now());
            self.save(&live.session);
            drop(sessions);
            self.notify(id, false);
        }
    }

    /// The task a session started with herdr_run was given.
    fn herdr_prompt(&self, id: &str, task: &str) {
        let mut sessions = self.sessions.lock().unwrap();
        if let Some(live) = sessions.get_mut(id)
            && !task.trim().is_empty()
        {
            live.session.prompt = task.trim().to_string();
            live.session.title = store::title_from(task);
            self.save(&live.session);
        }
    }

    fn herdr_side(&self, id: &str) -> Result<HerdrSide, Refusal> {
        let sessions = self.sessions.lock().unwrap();
        let live = sessions.get(id).ok_or(Refusal::NoSuchSession)?;
        live.session.herdr.clone().ok_or(Refusal::NoSuchSession)
    }

    /// A terminal that has gone: its session finishes (failing with `exit`, if it says so).
    fn herdr_gone(&self, id: &str, exit: Option<i64>) {
        let mut sessions = self.sessions.lock().unwrap();
        let Some(live) = sessions.get_mut(id) else {
            return;
        };
        let s = &mut live.session;
        if s.state.finished() {
            return;
        }
        let program = s.program.clone().unwrap_or_else(|| "its program".into());
        (s.state, s.result) = match exit {
            Some(code) if code != 0 => (
                State::Fail,
                Some(format!("{program} exited with code {code}")),
            ),
            _ => (State::Done, Some(format!("{program}'s terminal has gone"))),
        };
        s.ask = None;
        s.unread = true;
        s.updated = store::now();
        self.save(s);
        drop(sessions);
        self.notify(id, false);
    }

    /// The herdr sessions not in `seen` whose terminal herdr would know of (`known(address)`)
    /// have gone.
    fn herdr_gone_except(&self, seen: &[String], known: impl Fn(&str) -> bool) {
        let gone: Vec<String> = {
            let sessions = self.sessions.lock().unwrap();
            sessions
                .values()
                .filter(|l| l.session.backend == Backend::Herdr && !l.session.state.finished())
                .filter(|l| !seen.contains(&l.session.id))
                .filter(|l| l.session.herdr.as_ref().is_some_and(|h| known(&h.address)))
                .map(|l| l.session.id.clone())
                .collect()
        };
        for id in gone {
            self.herdr_gone(&id, None);
        }
    }

    /// The herdr sessions someone is looking at, and their terminals.
    fn watched_screens(&self) -> Vec<(String, String)> {
        let now = Instant::now();
        let sessions = self.sessions.lock().unwrap();
        sessions
            .values()
            .filter(|l| l.watched.is_some_and(|until| until > now))
            .filter_map(|l| {
                Some((
                    l.session.id.clone(),
                    l.session.herdr.as_ref()?.address.clone(),
                ))
            })
            .collect()
    }

    /// A session's screen as read now: kept, and followers told if it changed.
    fn herdr_screen(&self, id: &str, lines: Vec<String>) {
        let mut sessions = self.sessions.lock().unwrap();
        let Some(live) = sessions.get_mut(id) else {
            return;
        };
        if live.screen.as_ref() == Some(&lines) {
            return;
        }
        live.screen = Some(lines);
        drop(sessions);
        self.notify(id, false);
    }
}

/// What a choice sends, as the board shows it on the right: "KEY 1", "ENTER", "Y".
fn sends(keys: &[String]) -> String {
    let shown = match keys {
        [key] if key.len() == 1 && key.chars().all(|c| c.is_ascii_digit()) => format!("KEY {key}"),
        [key] if key.len() == 1 => key.to_uppercase(),
        [key, enter] if key.len() == 1 && enter == "enter" => key.to_uppercase(),
        keys => keys.join(" ").to_uppercase(),
    };
    store::one_line(&shown, 12)
}

/// A screen's text as plain lines: escape sequences and other control characters out, the
/// trailing blank lines dropped.
pub fn plain_lines(text: &str) -> Vec<String> {
    let mut clean = String::with_capacity(text.len());
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '\u{1b}' => {
                // CSI (ESC [ … final byte) or OSC (ESC ] … BEL or ESC \), else one character.
                match chars.peek() {
                    Some('[') => {
                        chars.next();
                        for c in chars.by_ref() {
                            if ('@'..='~').contains(&c) {
                                break;
                            }
                        }
                    }
                    Some(']') => {
                        chars.next();
                        while let Some(c) = chars.next() {
                            if c == '\u{7}' || (c == '\u{1b}' && chars.peek() == Some(&'\\')) {
                                break;
                            }
                        }
                    }
                    _ => {
                        chars.next();
                    }
                }
            }
            '\n' => clean.push('\n'),
            '\t' => clean.push_str("    "),
            c if c.is_control() => {}
            c => clean.push(c),
        }
    }
    let mut lines: Vec<String> = clean.lines().map(|l| l.trim_end().to_string()).collect();
    while lines.last().is_some_and(|l| l.is_empty()) {
        lines.pop();
    }
    lines
}

/// The last SCREEN_LINES lines, each cut to SCREEN_CHARS.
fn last_lines(lines: &[String]) -> Vec<String> {
    let start = lines.len().saturating_sub(SCREEN_LINES);
    lines[start..]
        .iter()
        .map(|l| store::cut(l, SCREEN_CHARS))
        .collect()
}

/// A screen line without the frame and cursor around it: `│ ❯ 1. Yes │` → `1. Yes`.
fn bare(line: &str) -> &str {
    line.trim()
        .trim_matches(|c: char| matches!(c, '│' | '┃' | '║' | '|'))
        .trim()
        .trim_start_matches(['❯', '>', '›', '→', '▶', '●', '○', '◯', '*'])
        .trim()
}

/// The question on a screen, by rule: the last run of numbered choices (`1. Yes`), each
/// pressing its number, or a yes/no prompt; the line above them, or the program's own words,
/// as the question.
pub fn question_by_rule(lines: &[String], msg: &str) -> (String, Vec<(String, Vec<String>)>) {
    let numbered = |line: &str| -> Option<(u32, String)> {
        let line = bare(line);
        let digits: String = line.chars().take_while(char::is_ascii_digit).collect();
        let rest = line[digits.len()..].strip_prefix(['.', ')'])?;
        let number: u32 = digits.parse().ok()?;
        let text = rest.trim();
        (number <= 9 && !text.is_empty()).then(|| (number, text.to_string()))
    };
    // The last block of numbered lines that counts up from 1.
    let mut options: Vec<(String, Vec<String>)> = Vec::new();
    let mut first_line = None;
    let mut i = lines.len();
    while i > 0 {
        i -= 1;
        let Some((number, _)) = numbered(&lines[i]) else {
            continue;
        };
        // Walk up to its 1., then down through the block.
        let mut start = i;
        let mut expect = number;
        let mut j = i;
        loop {
            if let Some((n, _)) = numbered(&lines[j]) {
                if n != expect {
                    break;
                }
                start = j;
                if n == 1 {
                    break;
                }
                expect -= 1;
            }
            if j == 0 || i - j > 12 {
                break;
            }
            j -= 1;
        }
        if numbered(&lines[start]).map(|(n, _)| n) == Some(1) {
            let mut next = 1;
            for line in &lines[start..=i] {
                if let Some((n, text)) = numbered(line)
                    && n == next
                {
                    options.push((text, vec![n.to_string()]));
                    next += 1;
                }
            }
            first_line = Some(start);
        }
        break;
    }
    if options.len() > 6 {
        options.truncate(6);
    }
    if options.len() < 2 {
        options.clear();
        first_line = None;
        // A yes/no prompt.
        if let Some(at) = lines.iter().rposition(|l| {
            let l = l.to_lowercase();
            l.contains("(y/n)") || l.contains("[y/n]") || l.contains("(yes/no)")
        }) {
            options = vec![
                (
                    "Yes".to_string(),
                    vec!["y".to_string(), "enter".to_string()],
                ),
                ("No".to_string(), vec!["n".to_string(), "enter".to_string()]),
            ];
            first_line = Some(at + 1);
        }
    }
    let above = first_line
        .and_then(|first| {
            lines[..first]
                .iter()
                .rev()
                .map(|l| bare(l))
                .find(|l| !l.is_empty() && !l.chars().all(|c| "─━-=_".contains(c)))
        })
        .map(str::to_string);
    let question = match (above, msg.trim()) {
        (Some(above), _) => above,
        (None, msg) if !msg.is_empty() => msg.to_string(),
        _ => "It needs you".to_string(),
    };
    (question, options)
}

/// Keys a choice may press, as the model gives them.
fn safe_key(key: &str) -> bool {
    (key.chars().count() == 1 && key.chars().all(|c| c.is_ascii_alphanumeric()))
        || matches!(
            key,
            "enter" | "esc" | "tab" | "up" | "down" | "left" | "right" | "space"
        )
}

/// The question on a screen, read by the session model: None if it can't (then the rules do).
async fn question_by_model(
    agent: &Agent,
    lines: &[String],
    msg: &str,
) -> Option<(String, Vec<(String, Vec<String>)>)> {
    if lines.is_empty() {
        return None;
    }
    let system = "A program in a terminal is waiting for its user. From the end of its screen, \
        write the question it asks and its choices, as JSON only, nothing else: \
        {\"question\": \"...\", \"options\": [{\"title\": \"...\", \"keys\": [\"1\"]}]}. \
        2 to 6 options, titles of at most 32 characters, keys from: a single letter or digit, \
        enter, esc, tab, up, down, space (in the order to press them). If there are no \
        choices to pick from, give \"options\": [].";
    let user = format!(
        "Its own words: {msg}\n\nThe end of its screen:\n{}",
        lines.join("\n")
    );
    let no_tools = agent.tools.only(&[]);
    let (tx, mut rx) = mpsc::channel::<Piece>(64);
    let request = llm::Request {
        system,
        history: &[],
        user: &user,
        audio: None,
        tools: &no_tools,
        rounds: 1,
        steer: None,
    };
    let collect = async {
        let mut text = String::new();
        while let Some(piece) = rx.recv().await {
            if let Piece::Text(t) | Piece::Transcript(t) = piece {
                text.push_str(&t);
            }
        }
        text
    };
    let (result, text) = tokio::join!(agent.llm.respond(request, tx), collect);
    result.ok()?;
    let json = text.get(text.find('{')?..=text.rfind('}')?)?;
    let parsed: Value = serde_json::from_str(json).ok()?;
    let question = parsed["question"].as_str()?.trim().to_string();
    let mut options = Vec::new();
    for option in parsed["options"].as_array()? {
        let title = option["title"].as_str()?.trim().to_string();
        let keys: Vec<String> = option["keys"]
            .as_array()?
            .iter()
            .filter_map(|k| k.as_str().map(|k| k.to_lowercase()))
            .collect();
        if title.is_empty() || keys.is_empty() || !keys.iter().all(|k| safe_key(k)) {
            return None;
        }
        options.push((title, keys));
    }
    if question.is_empty() || options.len() == 1 || options.len() > 6 {
        return None;
    }
    Some((question, options))
}

/// Waits for `check` to hold (for tests).
#[cfg(test)]
pub async fn tests_soon(mut check: impl FnMut() -> bool) {
    for _ in 0..500 {
        if check() {
            return;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    panic!("it never happened");
}

#[cfg(test)]
mod tests {
    use async_trait::async_trait;
    use voice_proto::Via;

    use super::*;
    use crate::agent::tests::{cfg, tools, until};
    use crate::agent::tools::SessionTools;
    use crate::agent::{Backends, prompt};
    use crate::hc::fake::{FakeCore, terminal};
    use crate::tools::Extra;

    fn lines(text: &str) -> Vec<String> {
        text.lines().map(str::to_string).collect()
    }

    const PERMISSION: &str = "\
╭──────────────────────────────────────╮
│ Bash command                         │
│   cargo build --release              │
│ Do you want to proceed?              │
│ ❯ 1. Yes                             │
│   2. Yes, and don't ask again        │
│   3. No, and tell Claude what to do  │
╰──────────────────────────────────────╯";

    #[test]
    fn questions_read_by_rule() {
        let (question, options) =
            question_by_rule(&lines(PERMISSION), "Claude needs your permission");
        assert_eq!(question, "Do you want to proceed?");
        let titles: Vec<&str> = options.iter().map(|(t, _)| t.as_str()).collect();
        assert_eq!(
            titles,
            [
                "Yes",
                "Yes, and don't ask again",
                "No, and tell Claude what to do"
            ]
        );
        assert_eq!(options[1].1, ["2"]);
        // An older list further up doesn't count; the last one does.
        let screen = "1. old\n2. older\nsome output\nPick a branch:\n  1) main\n  2) dev\n";
        let (question, options) = question_by_rule(&lines(screen), "");
        assert_eq!((question.as_str(), options.len()), ("Pick a branch:", 2));
        assert_eq!(options[0], ("main".to_string(), vec!["1".to_string()]));
        // A yes/no prompt.
        let (question, options) = question_by_rule(&lines("Overwrite config.toml? (y/N)"), "");
        assert_eq!(question, "Overwrite config.toml? (y/N)");
        assert_eq!(options[0].1, ["y", "enter"]);
        let (_, options) = question_by_rule(&lines("Delete it? [y/n]"), "");
        assert_eq!(options.len(), 2);
        // Nothing to pick: the program's words, no choices.
        let (question, options) =
            question_by_rule(&lines("Enter your name:"), "Waiting for a name");
        assert_eq!(
            (question.as_str(), options.len()),
            ("Waiting for a name", 0)
        );
        assert_eq!(sends(&["1".into()]), "KEY 1");
        assert_eq!(sends(&["y".into(), "enter".into()]), "Y");
        assert_eq!(sends(&["enter".into()]), "ENTER");
    }

    #[test]
    fn screens_as_plain_text() {
        let raw = "\u{1b}[1;32mok\u{1b}[0m done\r\n\u{1b}]0;title\u{7}next\tline\n\n\n";
        assert_eq!(plain_lines(raw), ["ok done", "next    line"]);
        let long: Vec<String> = (0..30)
            .map(|i| format!("{i} {}", "x".repeat(100)))
            .collect();
        let last = last_lines(&long);
        assert_eq!(last.len(), SCREEN_LINES);
        assert!(last[0].starts_with("18 ") && last[0].chars().count() == SCREEN_CHARS);
        assert_eq!(session_id("t7"), session_id("t7"));
        assert_ne!(session_id("t7"), session_id("desktop/t7"));
        assert!(store::valid_id(&session_id("desktop/t7")) && session_id("t7").len() == 8);
    }

    /// An agent with herdr from `core`, as `[herdr]` in `toml` says.
    fn with_herdr(name: &str, core: &Arc<FakeCore>, toml: &str) -> (Arc<Agent>, Arc<Herdr>) {
        let dir = crate::skills::tests::scratch(name);
        let agent = Agent::open(
            &cfg(&dir),
            Box::new(crate::agent::mock::MockAgent),
            tools(),
            prompt(),
            "test",
            Backends::default(),
        )
        .unwrap();
        let herdr_cfg: config::Herdr = toml::from_str(toml).unwrap();
        let herdr = Herdr::new(
            herdr_cfg,
            core.connector(),
            &dir,
            false,
            Duration::from_millis(20),
        );
        agent.attach_herdr(herdr.clone());
        (agent, herdr)
    }

    /// Waits for `check` to hold.
    async fn soon(mut check: impl FnMut() -> bool, what: &str) {
        for _ in 0..300 {
            if check() {
                return;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        panic!("never: {what}");
    }

    fn remote(address: &str, program: &str, status: Value) -> Value {
        let mut t = terminal(address, program, None);
        t["status"] = status;
        t["reported"] = json!(true);
        t
    }

    #[tokio::test]
    async fn terminals_become_sessions() {
        let core = FakeCore::new(
            "testbox",
            vec![
                terminal("t1", "claude", Some("idle")),
                terminal("t2", "bash", None),
                remote(
                    "desktop/t7",
                    "codex",
                    json!({"state": "working", "msg": "Running tests", "progress": 40, "app": "codex"}),
                ),
            ],
        );
        core.terminals.lock().unwrap()[0]["status"]["app"] = json!("claude-code");
        core.terminals.lock().unwrap()[0]["title"] = json!("Fix the build");
        let (agent, _) = with_herdr("herdr-discover", &core, "");
        let claude = session_id("t1");
        let codex = session_id("desktop/t7");
        until(&agent, &codex, State::Working).await;
        let view = until(&agent, &claude, State::Idle).await;
        assert_eq!(
            (view.via, view.backend, view.program.as_deref()),
            (Via::Herdr, Backend::Herdr, Some("claude code"))
        );
        assert_eq!(
            (view.place.as_str(), view.title.as_str()),
            ("TESTBOX", "Fix the build")
        );
        let list = agent.list(false);
        assert_eq!(
            list.sessions.len(),
            2,
            "a shell that reports nothing doesn't count"
        );
        let row = list.sessions.iter().find(|s| s.id == codex).unwrap();
        assert_eq!(
            (row.place.as_str(), row.progress, row.sub.as_str()),
            ("DESKTOP", Some(40), "Running tests")
        );
        assert_eq!((list.counts.running, list.counts.idle), (1, 1));
        assert_eq!(agent.view(&codex).unwrap().title, "codex · app");

        // It works, finishes, and its terminal goes.
        core.set_status(
            "t1",
            json!({"state": "working", "msg": "Editing src/main.rs", "app": "claude-code"}),
        );
        let view = until(&agent, &claude, State::Working).await;
        assert_eq!(view.log[0].text, "Editing src/main.rs");
        core.set_status(
            "t1",
            json!({"state": "done", "msg": "Fixed: 2 files changed", "app": "claude-code"}),
        );
        let view = until(&agent, &claude, State::Done).await;
        assert_eq!(view.result.as_deref(), Some("Fixed: 2 files changed"));
        assert!(
            agent
                .list(false)
                .sessions
                .iter()
                .any(|s| s.id == claude && s.unread)
        );
        core.finish("desktop/t7", 3);
        let view = until(&agent, &codex, State::Fail).await;
        assert_eq!(view.result.as_deref(), Some("exited with code 3"));
        core.event("removed", "desktop/t7", json!({}));
        tokio::time::sleep(Duration::from_millis(50)).await;
        assert_eq!(
            agent.view(&codex).unwrap().state,
            State::Fail,
            "finished ones stay listed"
        );

        // Deleting one only forgets it, until it does something new.
        agent.delete(&claude).await.unwrap();
        assert!(agent.view(&claude).is_none());
        core.set_status("t1", json!({"state": "working", "app": "claude-code"}));
        core.set_status("t1", json!({"state": "idle", "app": "claude-code"}));
        until(&agent, &claude, State::Idle).await;
        assert!(core.calls("terminal.kill").is_empty());
    }

    #[tokio::test]
    async fn a_question_off_the_screen_and_its_answer() {
        let core = FakeCore::new("testbox", vec![terminal("t1", "claude", Some("working"))]);
        let (agent, _) = with_herdr("herdr-ask", &core, "");
        let id = session_id("t1");
        until(&agent, &id, State::Working).await;
        core.screens
            .lock()
            .unwrap()
            .insert("t1".into(), PERMISSION.into());
        core.set_status("t1", json!({"state": "blocked", "kind": "permission", "msg": "Claude needs your permission to use Bash"}));
        until(&agent, &id, State::Ask).await;
        soon(
            || agent.question(&id).is_some_and(|q| q.options.len() == 3),
            "the choices",
        )
        .await;
        let ask = agent.question(&id).unwrap();
        assert_eq!(ask.question, "Do you want to proceed?");
        assert_eq!(
            ask.detail.as_deref(),
            Some("Claude needs your permission to use Bash")
        );
        assert_eq!(ask.options[0].right.as_deref(), Some("KEY 1"));
        // Words don't answer a permission prompt.
        let refused = agent.say(&id, "go ahead").await.unwrap_err();
        assert!(refused.to_string().contains("permission"), "{refused}");
        agent.answer(&id, &ask.id, 0).await.unwrap();
        assert_eq!(
            core.calls("terminal.keys"),
            [json!({"terminal": "t1", "keys": ["1"]})]
        );
        assert_eq!(agent.view(&id).unwrap().state, State::Working);
        assert_eq!(
            agent.answer(&id, &ask.id, 0).await,
            Err(Refusal::AlreadyAnswered)
        );

        // Blocked again; it moves on before the answer comes: the answer is stale.
        core.set_status(
            "t1",
            json!({"state": "blocked", "kind": "permission", "msg": "Again?"}),
        );
        soon(
            || agent.question(&id).is_some_and(|q| q.options.len() == 3),
            "the second question",
        )
        .await;
        let ask = agent.question(&id).unwrap();
        assert_eq!(ask.id, "a2");
        {
            // herdr moves on, and the daemon hasn't heard yet.
            let mut terminals = core.terminals.lock().unwrap();
            terminals[0]["status"] = json!({"state": "working"});
        }
        assert_eq!(
            agent.answer(&id, &ask.id, 1).await,
            Err(Refusal::NotWaiting)
        );
        assert_eq!(
            core.calls("terminal.keys").len(),
            1,
            "no keys for a stale answer"
        );
        assert_eq!(
            agent.view(&id).unwrap().state,
            State::Working,
            "it catches up"
        );
    }

    #[tokio::test]
    async fn talking_into_it_when_it_is_ready() {
        let core = FakeCore::new("testbox", vec![terminal("t1", "claude", Some("idle"))]);
        let (agent, _) = with_herdr("herdr-say", &core, "");
        let id = session_id("t1");
        until(&agent, &id, State::Idle).await;
        let said = agent.say(&id, "now fix the tests").await.unwrap();
        assert_eq!(
            said,
            Said::Typed {
                program: "claude".into(),
                place: "TESTBOX".into()
            }
        );
        assert_eq!(
            core.calls("terminal.send"),
            [json!({"terminal": "t1", "text": "now fix the tests", "if": "idle"})]
        );
        // Working: the words wait.
        core.set_status("t1", json!({"state": "working"}));
        until(&agent, &id, State::Working).await;
        assert_eq!(agent.say(&id, "and the docs").await, Err(Refusal::Busy));
        // A question in words.
        core.set_status(
            "t1",
            json!({"state": "blocked", "kind": "question", "msg": "Which branch?"}),
        );
        until(&agent, &id, State::Ask).await;
        agent.say(&id, "main").await.unwrap();
        assert_eq!(
            core.calls("terminal.send")[1],
            json!({"terminal": "t1", "text": "main", "force": true})
        );
        // Idle by the daemon's account, working by herdr's: refused as busy.
        core.set_status("t1", json!({"state": "idle"}));
        until(&agent, &id, State::Idle).await;
        core.terminals.lock().unwrap()[0]["status"] = json!({"state": "working"});
        assert_eq!(agent.say(&id, "more").await, Err(Refusal::Busy));
    }

    #[tokio::test]
    async fn a_shell_that_reports_nothing() {
        let core = FakeCore::new("testbox", vec![terminal("t2", "bash", None)]);
        let (agent, _) = with_herdr("herdr-shell", &core, "programs = [\"bash\"]\n");
        let id = session_id("t2");
        until(&agent, &id, State::Idle).await;
        agent.say(&id, "ls").await.unwrap();
        assert_eq!(
            core.calls("terminal.send"),
            [json!({"terminal": "t2", "text": "ls"})]
        );
        // Something running in it: working.
        {
            let mut terminals = core.terminals.lock().unwrap();
            terminals[0]["foreground"]["name"] = json!("cargo");
            terminals[0]["title"] = json!("cargo build");
        }
        core.event("created", "t2", json!({}));
        soon(
            || agent.view(&id).unwrap().title == "cargo build",
            "the new title",
        )
        .await;
        assert_eq!(agent.view(&id).unwrap().state, State::Working);
    }

    #[tokio::test]
    async fn starting_programs_in_herdr() {
        let core = FakeCore::new("testbox", vec![]);
        let (agent, _) = with_herdr(
            "herdr-run",
            &core,
            "templates = { codex = [\"codex\", \"--no-daemon\"] }\ndefault_program = \"codex\"\ndefault_machine = \"desktop\"\n",
        );
        // From an api session, with herdr_run.
        let from = agent.start_here("fix the build on the desktop", Backend::Api);
        let tools = SessionTools {
            agent: agent.clone(),
            id: from.clone(),
            run: 0,
        };
        assert!(tools.defs().iter().any(|d| d.name == RUN_TOOL));
        let out = tools
            .call(RUN_TOOL, &json!({"program": "codex", "task": "fix the build", "machine": "desktop", "dir": "/src/app"}))
            .await
            .unwrap()
            .unwrap();
        let id = session_id("desktop/t50");
        assert!(
            out.contains(&format!("on DESKTOP as session {id}")),
            "{out}"
        );
        let run = &core.calls("terminal.run")[0];
        assert_eq!(run["argv"], json!(["codex", "--no-daemon"]));
        assert_eq!(
            (run["on"].as_str(), run["cwd"].as_str()),
            (Some("desktop"), Some("/src/app"))
        );
        assert_eq!(run["labels"][LABEL], json!(from));
        assert_eq!(
            core.calls("terminal.send"),
            [json!({"terminal": "desktop/t50", "text": "fix the build"})]
        );
        let view = agent.view(&id).unwrap();
        assert_eq!(
            (view.prompt.as_str(), view.title.as_str()),
            ("fix the build", "Fix the build")
        );

        // An hc template takes the task itself; a command line runs as a line.
        let tools = SessionTools {
            agent: agent.clone(),
            id: from.clone(),
            run: 0,
        };
        tools
            .call(
                RUN_TOOL,
                &json!({"program": "reviewer", "task": "review it"}),
            )
            .await
            .unwrap()
            .unwrap();
        assert_eq!(
            core.calls("terminal.run")[1]["argv"],
            json!(["reviewer", "review it"])
        );
        assert_eq!(core.calls("terminal.send").len(), 1);
        tools
            .call(RUN_TOOL, &json!({"program": "npm test", "task": ""}))
            .await
            .unwrap()
            .unwrap();
        assert_eq!(core.calls("terminal.run")[2]["line"], "npm test");
    }

    #[tokio::test]
    async fn new_sessions_in_herdr_from_the_board() {
        let core = FakeCore::new("testbox", vec![]);
        let dir = crate::skills::tests::scratch("herdr-default");
        let agent = Agent::open(
            &cfg(&dir),
            Box::new(crate::agent::mock::MockAgent),
            tools(),
            prompt(),
            "test",
            Backends {
                default: Backend::Herdr,
                claude: None,
            },
        )
        .unwrap();
        let herdr_cfg: config::Herdr =
            toml::from_str("templates = { claude = [\"claude\"] }\n").unwrap();
        agent.attach_herdr(Herdr::new(
            herdr_cfg,
            core.connector(),
            &dir,
            false,
            Duration::from_millis(20),
        ));
        let id = agent.start("tidy the repo").await.unwrap();
        assert_eq!(id, session_id("t50"));
        let view = agent.view(&id).unwrap();
        assert_eq!(
            (view.backend, view.prompt.as_str()),
            (Backend::Herdr, "tidy the repo")
        );
        assert_eq!(core.calls("terminal.run")[0]["argv"], json!(["claude"]));
    }

    #[tokio::test]
    async fn every_policy_says_no() {
        let core = FakeCore::new("testbox", vec![terminal("t1", "claude", Some("idle"))]);
        let deny = "run = \"deny\"\nanswer = \"deny\"\nsend = \"deny\"\ninterrupt = \"deny\"\nkill = \"deny\"\n";
        let (agent, herdr) = with_herdr("herdr-deny", &core, deny);
        let id = session_id("t1");
        until(&agent, &id, State::Idle).await;
        assert_eq!(agent.say(&id, "hi").await, Err(Refusal::NotAllowed));
        assert_eq!(agent.stop(&id).await, Err(Refusal::NotAllowed));
        assert_eq!(agent.kill(&id).await, Err(Refusal::NotAllowed));
        assert_eq!(Refusal::NotAllowed.to_string(), "not allowed by [herdr]");
        core.set_status("t1", json!({"state": "blocked", "msg": "Proceed? (y/n)"}));
        core.screens
            .lock()
            .unwrap()
            .insert("t1".into(), "Proceed? (y/n)".into());
        soon(
            || agent.question(&id).is_some_and(|q| q.options.len() == 2),
            "the choices",
        )
        .await;
        assert_eq!(agent.answer(&id, "a1", 0).await, Err(Refusal::NotAllowed));
        // No herdr_run, and a direct call is refused.
        let from = agent.start_here("x", Backend::Api);
        let tools = SessionTools {
            agent: agent.clone(),
            id: from.clone(),
            run: 0,
        };
        assert!(!tools.defs().iter().any(|d| d.name == RUN_TOOL));
        let refused = herdr.run(&agent, None, "bash", "", None, None).await;
        assert_eq!(refused, Err(Refusal::NotAllowed));
        let only = [
            "terminal.list",
            "terminal.read",
            "machine.list",
            "terminal.get",
        ];
        let calls = core.calls.lock().unwrap();
        assert!(
            calls.iter().all(|(m, _)| only.contains(&m.as_str())),
            "{calls:?}"
        );
    }

    #[tokio::test]
    async fn stop_and_kill_when_allowed() {
        let core = FakeCore::new("testbox", vec![terminal("t1", "claude", Some("working"))]);
        let (agent, _) = with_herdr(
            "herdr-stop",
            &core,
            "interrupt = \"allow\"\nkill = \"allow\"\n",
        );
        let id = session_id("t1");
        until(&agent, &id, State::Working).await;
        agent.stop(&id).await.unwrap();
        agent.kill(&id).await.unwrap();
        assert_eq!(
            core.calls("terminal.interrupt"),
            [json!({"terminal": "t1"})]
        );
        assert_eq!(core.calls("terminal.kill"), [json!({"terminal": "t1"})]);
        let api = agent.start_here("x", Backend::Api);
        assert!(agent.kill(&api).await.is_err());
    }

    #[tokio::test]
    async fn herdr_down_and_coming_back() {
        let core = FakeCore::new("testbox", vec![terminal("t1", "claude", Some("idle"))]);
        core.set_up(false);
        let (agent, _) = with_herdr("herdr-down", &core, "");
        tokio::time::sleep(Duration::from_millis(60)).await;
        assert!(agent.list(false).sessions.is_empty());
        core.set_up(true);
        let id = session_id("t1");
        until(&agent, &id, State::Idle).await;
        // It goes again: the session keeps its state, and actions say so.
        core.set_up(false);
        tokio::time::sleep(Duration::from_millis(30)).await;
        assert_eq!(agent.view(&id).unwrap().state, State::Idle);
        assert_eq!(agent.say(&id, "hi").await, Err(Refusal::Unreachable));
        core.set_up(true);
        core.set_status("t1", json!({"state": "working", "msg": "back"}));
        // The new subscription's snapshot, or the event, brings it up to date.
        soon(
            || agent.view(&id).is_some_and(|v| v.state == State::Working),
            "caught up",
        )
        .await;
    }

    #[tokio::test]
    async fn the_screen_while_someone_looks() {
        let core = FakeCore::new("testbox", vec![terminal("t1", "claude", Some("working"))]);
        core.screens.lock().unwrap().insert(
            "t1".into(),
            "\u{1b}[1m> fix the build\u{1b}[0m\n  Running cargo build…\n".into(),
        );
        let (agent, _) = with_herdr("herdr-screen", &core, "");
        let id = session_id("t1");
        let view = until(&agent, &id, State::Working).await;
        assert_eq!(view.screen, None, "not read before anyone looks");
        soon(|| agent.view(&id).unwrap().screen.is_some(), "the screen").await;
        assert_eq!(
            agent.view(&id).unwrap().screen.unwrap(),
            ["> fix the build", "  Running cargo build…"]
        );
        // Nobody looking: it isn't read.
        let reads = core.calls("terminal.read").len();
        tokio::time::sleep(WATCH + Duration::from_millis(200)).await;
        let later = core.calls("terminal.read").len();
        tokio::time::sleep(Duration::from_millis(200)).await;
        assert_eq!(core.calls("terminal.read").len(), later);
        assert!(later >= reads);
    }

    /// Reads screens as asked: the question and choices as JSON.
    struct Reader;

    #[async_trait]
    impl crate::llm::Llm for Reader {
        async fn respond(
            &self,
            request: crate::llm::Request<'_>,
            out: mpsc::Sender<Piece>,
        ) -> anyhow::Result<crate::llm::Outcome> {
            assert!(request.user.contains("Deploy to production?"));
            let json = r#"Here: {"question": "Deploy to production?", "options": [{"title": "Deploy", "keys": ["d"]}, {"title": "Cancel", "keys": ["esc"]}]}"#;
            out.send(Piece::Text(json.into())).await?;
            Ok(crate::llm::Outcome::Done)
        }
    }

    #[tokio::test]
    async fn the_session_model_reads_a_question() {
        let core = FakeCore::new(
            "testbox",
            vec![terminal("t1", "deploybot", Some("working"))],
        );
        core.screens.lock().unwrap().insert(
            "t1".into(),
            "Deploy to production?\n[d]eploy  [esc] cancel".into(),
        );
        let dir = crate::skills::tests::scratch("herdr-model");
        let agent = Agent::open(
            &cfg(&dir),
            Box::new(Reader),
            tools(),
            prompt(),
            "test",
            Backends::default(),
        )
        .unwrap();
        let herdr = Herdr::new(
            config::Herdr::default(),
            core.connector(),
            &dir,
            true,
            Duration::from_millis(20),
        );
        agent.attach_herdr(herdr);
        let id = session_id("t1");
        until(&agent, &id, State::Working).await;
        core.set_status("t1", json!({"state": "blocked", "kind": "question"}));
        soon(
            || agent.question(&id).is_some_and(|q| q.options.len() == 2),
            "the choices",
        )
        .await;
        let ask = agent.question(&id).unwrap();
        assert_eq!(ask.options[1].right.as_deref(), Some("ESC"));
        agent.answer(&id, &ask.id, 0).await.unwrap();
        assert_eq!(core.calls("terminal.keys")[0]["keys"], json!(["d"]));
    }
}
