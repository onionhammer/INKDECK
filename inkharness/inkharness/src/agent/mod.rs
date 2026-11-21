//! Agent sessions (issue #52): runs that live on the daemon, are kept on disk (store.rs), and
//! go on working with no board connected. Each has a title, a state, an optional plan, a log of
//! its tool calls, a result, and at most one question waiting for an answer.
//!
//! A session runs as a series of model requests ("legs"), each with the daemon's tools plus
//! `ask_choice` and `plan` (tools.rs) and the conversation so far (what was said, without the
//! tool calls behind it). What the user says while it works joins its next round (the tool
//! loop's `steer`), or starts the next leg if the model was already writing its answer.
//! `ask_choice` waits in place for the answer, which becomes the tool's result. If the daemon
//! restarts, a working session starts a new leg that is told what it had done (its plan and
//! log), and an asking one waits on; its answer then starts a new leg the same way.
//!
//! Boards reach sessions through their connection (session.rs) and programs over HTTP
//! (http.rs); both see the same sessions. A PIN can guard them (lock.rs). Talk turns never do.
//!
//! That is the `api` backend. Two more run sessions elsewhere: `claude` runs headless Claude
//! Code per session (claude.rs), which reaches ask_choice and plan through an MCP server that
//! calls back into the daemon (bridge.rs); `herdr` follows programs in herdr terminals, here or
//! on other machines, as sessions of their own (herdr.rs, over hc.rs).

pub mod bridge;
mod claude;
mod herdr;
mod lock;
mod mock;
mod schedule;
pub mod store;
mod tools;

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, OnceLock, Weak};
use std::time::Duration;

use anyhow::{Context, Result};
use tokio::sync::{Notify, broadcast, mpsc, oneshot};
use tokio::task::AbortHandle;
use voice_proto::{AgentHello, AskView, Backend, OnDeadline, SessionList, SessionView, State};

use crate::config;
use crate::harness::Harness;
use crate::llm::{self, Llm, Piece, Role, TooManyRounds, Turn};
use crate::meter;
use crate::tools::ToolSet;
pub use claude::Claude;
pub use herdr::Herdr;
#[cfg(test)]
pub use herdr::session_id as herdr_session_id;
#[cfg(test)]
pub use herdr::tests_soon as herdr_tests_soon;
pub use lock::Unlock;
#[cfg(test)]
pub use lock::sha256_hex;
use store::{Question, Session, Who};
pub use tools::Answer;
pub use tools::ProposeTool;
use tools::SessionTools;

/// How often finished sessions past `keep_days` are looked for.
const PRUNE_EVERY: Duration = Duration::from_secs(3600);
/// The scheduler looks at least this often (seconds), whatever is due: a clock change can move
/// a schedule's time.
const TICK_MAX: u64 = 300;
/// A run missed by more than this (the daemon was down) is skipped, not run late.
const MISSED_AFTER: u64 = 3600;
/// What this daemon can do beyond the first version, for the board (AgentHello::features).
pub const FEATURES: &[&str] = &[
    "pause", "run", "jobs", "schedule", "watch", "deadline", "panel", "speak",
];
/// How long words for a herdr session wait for the board to send them.
#[cfg(not(test))]
const DRAFT_TTL: Duration = Duration::from_secs(120);
#[cfg(test)]
const DRAFT_TTL: Duration = Duration::from_millis(300);

/// Words said into a herdr session, kept until the board sends them (`send_draft`) or drops
/// them, or DRAFT_TTL passes. A program in a terminal does what it is typed, so nothing heard
/// goes in unseen.
#[derive(Debug, Clone, PartialEq)]
pub struct Draft {
    pub id: String,
    pub session: String,
    pub text: String,
}

pub struct Agent {
    place: String,
    dir: PathBuf,
    keep_days: u64,
    max_rounds: usize,
    llm: Box<dyn Llm>,
    /// The daemon's tools: each run adds its own ask_choice and plan.
    tools: ToolSet,
    system: String,
    lock: lock::Lock,
    sessions: Mutex<HashMap<String, Live>>,
    events: broadcast::Sender<Event>,
    /// What new sessions run on.
    backend: Backend,
    /// Whether an agent turn gets a spoken reply.
    reply: config::Reply,
    /// The claude backend, when it is on.
    claude: Option<Claude>,
    /// Where claude runs reach ask_choice and plan.
    bridge: OnceLock<Arc<bridge::Bridge>>,
    /// herdr, when there is a [herdr] table (attach_herdr).
    herdr: OnceLock<Arc<Herdr>>,
    /// Drafts by id, and when each was made.
    drafts: Mutex<HashMap<String, (Draft, std::time::Instant)>>,
    next_draft: std::sync::atomic::AtomicU64,
    /// Wakes the scheduler (tick) after any change: a new schedule, a deadline, a pause.
    clock: Arc<Notify>,
}

/// What sessions can run on besides the api backend, built at startup (agent::start).
#[derive(Default)]
pub struct Backends {
    /// What new sessions run on.
    pub default: Backend,
    pub claude: Option<Claude>,
}

/// A session and what is working on it.
struct Live {
    session: Session,
    run: Option<AbortHandle>,
    /// The ask_choice call waiting for its answer, when the run that asked is still going.
    waiting: Option<oneshot::Sender<Answer>>,
    /// Which run is the session's: each new run, and each stop, counts one up. A run that is
    /// no longer the session's (stopped, or replaced while it finished) changes nothing.
    generation: u64,
    /// Tells a claude run that something changed: words to pass on, or a stop.
    wake: Arc<Notify>,
    /// A herdr session's last lines of screen, and until when someone is looking at them.
    screen: Option<Vec<String>>,
    watched: Option<std::time::Instant>,
    /// It was paused and resumed while it asked: the run that asked is gone, and an answer
    /// starts a new one that is told so.
    after_pause: bool,
}

impl Live {
    fn new(session: Session) -> Live {
        Live {
            session,
            run: None,
            waiting: None,
            generation: 0,
            wake: Arc::new(Notify::new()),
            screen: None,
            watched: None,
            after_pause: false,
        }
    }

    /// A run is working on it (one that ended without saying so, by a panic, doesn't count).
    fn running(&self) -> bool {
        self.run.as_ref().is_some_and(|run| !run.is_finished())
    }
}

/// The session, if `run` is still its run.
fn own<'a>(sessions: &'a mut HashMap<String, Live>, id: &str, run: u64) -> Option<&'a mut Live> {
    sessions.get_mut(id).filter(|live| live.generation == run)
}

/// A session changed (or went): what a board connection that follows the list listens for.
#[derive(Debug, Clone)]
pub struct Event {
    pub session: String,
    /// It has just asked a question.
    pub asked: bool,
}

/// Why a request about a session can't be done; the message a board or HTTP gets.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Refusal {
    NoSuchSession,
    AlreadyAnswered,
    NoSuchChoice,
    NotRunning,
    /// A herdr program that has moved on since its question was put.
    NotWaiting,
    /// A herdr program at work: words wait until it is ready.
    Busy,
    /// No draft by that id for that session (sent, dropped, or never made).
    NoSuchDraft,
    /// The draft waited longer than DRAFT_TTL.
    DraftExpired,
    /// A [herdr] policy says no.
    NotAllowed,
    /// herdr doesn't answer.
    Unreachable,
    /// Anything else, said as it is.
    Other(String),
}

impl std::fmt::Display for Refusal {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Refusal::NoSuchSession => "no such session",
            Refusal::AlreadyAnswered => "already answered",
            Refusal::NoSuchChoice => "no such choice",
            Refusal::NotRunning => "not running",
            Refusal::NotWaiting => "not waiting any more",
            Refusal::Busy => "it's busy",
            Refusal::NoSuchDraft => "no such draft",
            Refusal::DraftExpired => "the draft has expired",
            Refusal::NotAllowed => "not allowed by [herdr]",
            Refusal::Unreachable => "herdr is not reachable",
            Refusal::Other(message) => message,
        })
    }
}

/// What talking into a session did, for the spoken reply.
#[derive(Debug, Clone, PartialEq)]
pub enum Said {
    /// It was asking; the words answered it.
    Answered { question: String },
    /// It was working; it hears them at its next round.
    Added { title: String, now: String },
    /// It had finished; it starts again with them.
    Resumed {
        title: String,
        result: Option<String>,
    },
    /// They were typed into a program in a herdr terminal.
    Typed { program: String, place: String },
}

/// Why a run starts again without its tool results: what it is told (resume_note).
#[derive(Debug, Clone, Copy, PartialEq)]
enum Why {
    Restart,
    Pause,
}

/// The session model for `cfg`: the mock plays a whole session (mock.rs).
pub fn model(cfg: &config::Llm, http: reqwest::Client) -> Result<Box<dyn Llm>> {
    Ok(match cfg {
        config::Llm::Mock => Box::new(mock::MockAgent),
        other => llm::build(other, http)?,
    })
}

/// The agent sessions `cfg` asks for, if it has an [agent] table: on [agent.llm] (else
/// [chat.llm], else [llm]), with the daemon's tools and their own (none of a device's), and
/// on Claude Code and herdr as configured.
pub async fn start(cfg: &config::Config, harness: &Harness) -> Result<Option<Arc<Agent>>> {
    let Some(agent_cfg) = &cfg.agent else {
        return Ok(None);
    };
    let (table, chosen) = cfg.agent_model();
    if let config::Llm::OpenaiCompat {
        output: config::Modality::Audio,
        voice,
        ..
    } = chosen
    {
        if voice.is_none() {
            anyhow::bail!("{table} has output = \"audio\": give it a voice");
        }
        tracing::warn!(
            "sessions run on {table}, which speaks: they pay for audio nobody hears (a text \
             model in [agent.llm] is cheaper)"
        );
    }
    let llm = model(chosen, harness.http.clone()).context(table)?;
    let system = harness.prompt_from(prompt());
    let name = format!("{} {table}", chosen.name());

    // Claude Code, if it is configured or new sessions run on it, and it answers.
    let mut backends = Backends {
        default: match agent_cfg.backend {
            config::BackendName::Api => Backend::Api,
            config::BackendName::Claude => Backend::Claude,
            config::BackendName::Herdr => Backend::Herdr,
        },
        claude: None,
    };
    if agent_cfg.claude.is_some() || backends.default == Backend::Claude {
        let claude = Claude::new(&agent_cfg.claude.clone().unwrap_or_default());
        match claude.check().await {
            Ok(version) => {
                tracing::info!(version, "claude backend: {}", claude.credential());
                backends.claude = Some(claude);
            }
            Err(e) => tracing::warn!("the claude backend is off: {e:#}"),
        }
    }
    if backends.default == Backend::Claude && backends.claude.is_none() {
        tracing::warn!("new sessions run on the api backend instead");
        backends.default = Backend::Api;
    }
    if backends.default == Backend::Herdr && cfg.herdr.is_none() {
        anyhow::bail!("[agent] backend = \"herdr\" needs a [herdr] table");
    }
    let agent = Agent::open(
        agent_cfg,
        llm,
        harness.tools.clone(),
        system,
        &name,
        backends,
    )?;
    if let Some(herdr_cfg) = &cfg.herdr {
        let connector = crate::hc::command_connector(herdr_cfg.command.clone());
        let use_model = !matches!(chosen, config::Llm::Mock);
        let herdr = Herdr::new(
            herdr_cfg.clone(),
            connector,
            &agent.dir,
            use_model,
            Duration::from_secs(1),
        );
        agent.attach_herdr(herdr);
    }
    Ok(Some(agent))
}

/// The base of a session's system prompt (the instructions files and skills follow it).
pub fn prompt() -> String {
    "You are an agent working through a task on your own, on a home server, for someone who \
     checks on it now and then on a small black-and-white e-paper screen. Nobody watches while \
     you work, and a task can take many steps.\n\
     - For a job with more than one step, call plan first with a short checklist and a title, \
     and call it again as you move through it.\n\
     - Use the tools to find things out and to act. Only say you did something if a tool call \
     in this conversation did it.\n\
     - When a choice is genuinely the user's (which plumber to book, which flight to buy), \
     call ask_choice and wait for the answer. Decide everything else yourself, and don't ask \
     permission for what the task already asked for.\n\
     - What you write between tool calls goes to the task's log: a few words at most, or \
     nothing.\n\
     - End with the result, in Markdown, short and to the point as it should read on the \
     screen: a few lines, a short list or a small table. If it is something to keep and read \
     later, like a brief, a summary or a recipe, start it with a `# ` heading: it is kept as a \
     note."
        .into()
}

/// The system prompt for the short spoken reply to an agent turn.
pub const VOICE_PROMPT: &str = "You are the voice of an agent that works through longer tasks \
     in the background on a home server. Answer in one or two short plain sentences, spoken \
     aloud, with no markdown, lists or emoji. You have no tools and do nothing yourself: the \
     agent does the work after you speak, so never say it is done or make up results.";

/// What the spoken reply is asked for: what the user said, and what it did to the session.
pub fn voice_request(text: &str, said: Option<&Said>) -> String {
    match said {
        None => format!(
            "The user just asked the agent: \"{text}\". Say briefly what it will do, for example \
             \"I'll compare plumbers who can come this week and ask you to pick.\""
        ),
        Some(Said::Answered { question }) => format!(
            "The agent asked \"{question}\" and the user answered: \"{text}\". Acknowledge it \
             in a few words; the agent carries on."
        ),
        Some(Said::Added { title, now }) => format!(
            "The agent is working on \"{title}\" (now: {now}). The user added: \"{text}\". \
             Acknowledge it in a few words; the agent takes it into account."
        ),
        Some(Said::Resumed { title, result }) => format!(
            "The agent finished \"{title}\" with this result:\n\n{}\n\nThe user now says: \
             \"{text}\". If the result answers it, answer in a sentence or two. Otherwise say \
             briefly what the agent will do next.",
            result.as_deref().unwrap_or("(none)")
        ),
        Some(Said::Typed { program, place }) => format!(
            "The user's words \"{text}\" were typed into {program} on {place}. Say so in a few \
             words."
        ),
    }
}

/// What a leg wrote: all of it (for the conversation), and its last stretch of text (the result).
struct Written {
    all: String,
    result: String,
}

impl Agent {
    /// Opens the sessions in `[agent] dir`: deletes the ones past `keep_days`, starts the
    /// working ones again, and leaves the asking ones waiting. They run on `llm`, which the log
    /// calls `model`.
    pub fn open(
        cfg: &config::Agent,
        llm: Box<dyn Llm>,
        tools: ToolSet,
        system: String,
        model: &str,
        backends: Backends,
    ) -> Result<Arc<Agent>> {
        let dir = PathBuf::from(&cfg.dir);
        std::fs::create_dir_all(&dir).with_context(|| format!("making {}", dir.display()))?;
        let lock = lock::Lock::new(
            cfg.pin_sha256.as_deref(),
            cfg.ticket_minutes,
            dir.join("pin-tries.json"),
        )?;
        let place = cfg
            .place
            .clone()
            .filter(|p| !p.trim().is_empty())
            .unwrap_or_else(hostname);
        let now = store::now();
        let mut sessions = HashMap::new();
        for session in store::load_all(&dir) {
            if store::expired(&session, cfg.keep_days, now) {
                let _ = store::remove(&dir, &session.id);
                continue;
            }
            sessions.insert(session.id.clone(), Live::new(session));
        }
        let agent = Arc::new(Agent {
            place,
            dir,
            keep_days: cfg.keep_days,
            max_rounds: cfg.max_rounds.max(1),
            llm,
            tools,
            system,
            lock,
            sessions: Mutex::new(sessions),
            events: broadcast::channel(256).0,
            backend: backends.default,
            reply: cfg.reply,
            claude: backends.claude,
            bridge: OnceLock::new(),
            herdr: OnceLock::new(),
            drafts: Mutex::default(),
            next_draft: Default::default(),
            clock: Arc::new(Notify::new()),
        });
        if agent.claude.is_some() {
            let bridge = bridge::Bridge::start(Arc::downgrade(&agent))?;
            let _ = agent.bridge.set(bridge);
        }

        let mut resumed = 0;
        {
            let mut sessions = agent.sessions.lock().unwrap();
            for live in sessions.values_mut() {
                // herdr sessions aren't run by the daemon: herdr.rs catches up with them.
                if live.session.state == State::Working && live.session.backend != Backend::Herdr {
                    let note = resume_note(&live.session, Why::Restart, None);
                    live.session.inbox.push(note);
                    agent.save(&live.session);
                    agent.spawn_run(live);
                    resumed += 1;
                }
            }
            tracing::info!(
                dir = %agent.dir.display(),
                place = %agent.place,
                model,
                backend = ?agent.backend,
                sessions = sessions.len(),
                resumed,
                pin = agent.lock.has_pin(),
                "agent sessions"
            );
        }
        let weak: Weak<Agent> = Arc::downgrade(&agent);
        tokio::spawn(async move {
            loop {
                tokio::time::sleep(PRUNE_EVERY).await;
                let Some(agent) = weak.upgrade() else { return };
                agent.prune(store::now());
            }
        });
        // The scheduler: schedules, watches and deadlines, at the earliest due, or sooner when
        // something changes.
        let (weak, clock) = (Arc::downgrade(&agent), agent.clock.clone());
        tokio::spawn(async move {
            loop {
                let wait = {
                    let Some(agent) = weak.upgrade() else { return };
                    let now = store::now();
                    agent
                        .tick(now)
                        .map_or(TICK_MAX, |due| due.saturating_sub(now).clamp(1, TICK_MAX))
                };
                tokio::select! {
                    () = tokio::time::sleep(Duration::from_secs(wait)) => {}
                    () = clock.notified() => {}
                }
            }
        });
        Ok(agent)
    }

    pub fn hello(&self, unlocked: bool) -> AgentHello {
        AgentHello {
            place: self.place.clone(),
            pin: self.lock.has_pin(),
            unlocked,
            features: FEATURES.iter().map(|f| f.to_string()).collect(),
        }
    }

    pub fn has_pin(&self) -> bool {
        self.lock.has_pin()
    }

    /// Whether an agent turn answers with a spoken reply (`[agent] reply`).
    pub fn speaks(&self) -> bool {
        self.reply == config::Reply::Spoken
    }

    pub fn unlock_pin(&self, pin: &str) -> Unlock {
        self.lock.pin(pin, store::now())
    }

    pub fn unlock_ticket(&self, ticket: &str) -> Unlock {
        self.lock.ticket(ticket, store::now())
    }

    /// Checks a PIN without issuing a ticket: `None` if it is right (or none is set).
    pub fn check_pin(&self, pin: &str) -> Option<Unlock> {
        self.lock.check(pin, store::now())
    }

    pub fn subscribe(&self) -> broadcast::Receiver<Event> {
        self.events.subscribe()
    }

    /// The list; `locked`, only the counts.
    pub fn list(&self, locked: bool) -> SessionList {
        let sessions = self.sessions.lock().unwrap();
        store::list(
            sessions.values().map(|l| &l.session),
            &self.place,
            locked,
            store::now(),
        )
    }

    /// One session in full. Looking at a herdr session keeps its screen fresh for a while.
    pub fn view(&self, id: &str) -> Option<SessionView> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = sessions.get_mut(id)?;
        if live.session.backend == Backend::Herdr {
            live.watched = Some(std::time::Instant::now() + herdr::WATCH);
        }
        Some(live.session.view(&self.place, live.screen.clone()))
    }

    /// Someone is still looking at it (a board with it open): keeps a herdr session's screen
    /// fresh.
    pub fn watch(&self, id: &str) {
        if let Some(live) = self.sessions.lock().unwrap().get_mut(id)
            && live.session.backend == Backend::Herdr
        {
            live.watched = Some(std::time::Instant::now() + herdr::WATCH);
        }
    }

    /// What runs a session (None: no such session).
    fn backend_of(&self, id: &str) -> Option<Backend> {
        let sessions = self.sessions.lock().unwrap();
        sessions.get(id).map(|l| l.session.backend)
    }

    /// What a session is doing (None: no such session).
    fn state_of(&self, id: &str) -> Option<State> {
        let sessions = self.sessions.lock().unwrap();
        sessions.get(id).map(|l| l.session.state)
    }

    /// The session's pending question.
    pub fn question(&self, id: &str) -> Option<AskView> {
        let sessions = self.sessions.lock().unwrap();
        sessions.get(id).and_then(|l| l.session.question())
    }

    pub fn exists(&self, id: &str) -> bool {
        self.sessions.lock().unwrap().contains_key(id)
    }

    /// Marks it seen (opened on a board). False if there is no such session.
    pub fn seen(&self, id: &str) -> bool {
        let mut sessions = self.sessions.lock().unwrap();
        let Some(live) = sessions.get_mut(id) else {
            return false;
        };
        if live.session.unread {
            live.session.unread = false;
            self.save(&live.session);
            self.notify(id, false);
        }
        true
    }

    /// Starts a session for `text` on the backend new sessions run on; its id. With herdr,
    /// that is a program in a new terminal, given `text` as its task.
    pub async fn start(self: &Arc<Self>, text: &str) -> Result<String, Refusal> {
        match self.backend {
            Backend::Herdr => {
                let herdr = self.herdr.get().ok_or_else(no_herdr)?;
                herdr.start_default(self, text).await
            }
            backend => Ok(self.start_here(text, backend)),
        }
    }

    /// A session that runs here (api or claude).
    fn start_here(self: &Arc<Self>, text: &str, backend: Backend) -> String {
        let mut sessions = self.sessions.lock().unwrap();
        let id = store::new_id(|id| sessions.contains_key(id));
        let mut session = Session::new(id.clone(), text, store::now());
        session.backend = backend;
        if backend == Backend::Claude {
            session.program = Some(claude::PROGRAM.into());
        }
        tracing::info!(session = %id, title = %session.title, ?backend, "a new session");
        self.save(&session);
        let mut live = Live::new(session);
        self.spawn_run(&mut live);
        sessions.insert(id.clone(), live);
        self.notify(&id, false);
        id
    }

    /// Talks into a session: answers its question in words, joins its work, or starts it again
    /// if it had finished. Into a herdr session, the words are typed.
    pub async fn say(self: &Arc<Self>, id: &str, text: &str) -> Result<Said, Refusal> {
        if self.state_of(id) == Some(State::Proposed) {
            let title = self
                .sessions
                .lock()
                .unwrap()
                .get(id)
                .map(|l| l.session.title.clone())
                .unwrap_or_default();
            self.approve(id, Some(text)).await?;
            return Ok(Said::Added {
                title,
                now: "starting it".into(),
            });
        }
        match self.backend_of(id).ok_or(Refusal::NoSuchSession)? {
            Backend::Herdr => {
                let herdr = self.herdr.get().ok_or_else(no_herdr)?;
                herdr.say(self, id, text).await
            }
            _ => self.say_here(id, text),
        }
    }

    fn say_here(self: &Arc<Self>, id: &str, text: &str) -> Result<Said, Refusal> {
        let text = text.trim();
        let mut sessions = self.sessions.lock().unwrap();
        let live = sessions.get_mut(id).ok_or(Refusal::NoSuchSession)?;
        let now = store::now();
        let said = match live.session.state {
            State::Ask => {
                let question = live
                    .session
                    .ask
                    .as_ref()
                    .map(|q| q.question.clone())
                    .unwrap_or_default();
                self.deliver(live, Answer::Text(text.to_string()));
                Said::Answered { question }
            }
            State::Working => {
                live.session.inbox.push(text.to_string());
                live.session.add_log(&format!("you: {text}"), now);
                live.session.updated = now;
                if live.running() {
                    live.wake.notify_one();
                } else {
                    self.spawn_run(live);
                }
                Said::Added {
                    title: live.session.title.clone(),
                    now: live.session.sub(),
                }
            }
            State::Paused => {
                // Words for a paused session pick it up again.
                live.session.inbox.push(text.to_string());
                live.session.add_log(&format!("you: {text}"), now);
                self.resume_live(live, now);
                Said::Added {
                    title: live.session.title.clone(),
                    now: live.session.sub(),
                }
            }
            State::Scheduled | State::Watch => {
                // Changing it by voice: a leg that hears the words and can call schedule or
                // watch again. It isn't a run, and it leaves the last run's result alone.
                let s = &mut live.session;
                s.inbox.push(text.to_string());
                s.add_log(&format!("you: {text}"), now);
                s.state = State::Working;
                s.updated = now;
                self.spawn_run(live);
                Said::Added {
                    title: live.session.title.clone(),
                    now: "waiting for its next run".into(),
                }
            }
            State::Proposed => return Err(Refusal::Other("it waits to be started".into())),
            State::Done
            | State::Note
            | State::Fail
            | State::Stopped
            | State::Idle
            | State::Unknown => {
                let said = Said::Resumed {
                    title: live.session.title.clone(),
                    result: live.session.result.clone(),
                };
                let s = &mut live.session;
                s.inbox.push(text.to_string());
                s.add_log(&format!("you: {text}"), now);
                s.state = State::Working;
                // A result belongs to a finished session; the run writes a new one.
                s.result = None;
                s.unread = false;
                s.updated = now;
                self.spawn_run(live);
                said
            }
        };
        self.save(&live.session);
        self.notify(id, false);
        Ok(said)
    }

    /// Whether words for this session go through a draft (a herdr session's do).
    pub fn takes_drafts(&self, id: &str) -> bool {
        self.backend_of(id) == Some(Backend::Herdr)
    }

    /// Keeps words for a herdr session until the board sends them: the draft to show it.
    /// Refused at once if `[herdr] send` would refuse them anyway.
    pub fn draft(&self, id: &str, text: &str) -> Result<Draft, Refusal> {
        match self.backend_of(id) {
            None => return Err(Refusal::NoSuchSession),
            Some(Backend::Herdr) => {}
            Some(_) => return Err(Refusal::Other("only herdr sessions take drafts".into())),
        }
        let herdr = self.herdr.get().ok_or_else(no_herdr)?;
        if !herdr.sends() {
            return Err(Refusal::NotAllowed);
        }
        let n = self
            .next_draft
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed)
            + 1;
        let draft = Draft {
            id: format!("d{n}"),
            session: id.to_string(),
            text: text.trim().to_string(),
        };
        let now = std::time::Instant::now();
        let mut drafts = self.drafts.lock().unwrap();
        drafts.retain(|_, (_, at)| now.duration_since(*at) < DRAFT_TTL);
        drafts.insert(draft.id.clone(), (draft.clone(), now));
        Ok(draft)
    }

    /// Types a draft into its herdr session (`[herdr] send` and the session's state decide, as
    /// for any words). A refused one is kept, to send again while it lasts.
    pub async fn send_draft(self: &Arc<Self>, session: &str, draft: &str) -> Result<Said, Refusal> {
        let text = {
            let mut drafts = self.drafts.lock().unwrap();
            let Some((kept, at)) = drafts.get(draft).filter(|(d, _)| d.session == session) else {
                return Err(Refusal::NoSuchDraft);
            };
            if at.elapsed() >= DRAFT_TTL {
                drafts.remove(draft);
                return Err(Refusal::DraftExpired);
            }
            kept.text.clone()
        };
        let said = self.say(session, &text).await?;
        self.drafts.lock().unwrap().remove(draft);
        Ok(said)
    }

    /// Throws a draft away.
    pub fn drop_draft(&self, session: &str, draft: &str) {
        let mut drafts = self.drafts.lock().unwrap();
        if drafts.get(draft).is_some_and(|(d, _)| d.session == session) {
            drafts.remove(draft);
        }
    }

    /// Answers a session's question `ask` with its option `choice`. For a herdr session, that
    /// presses the keys the option stands for.
    pub async fn answer(
        self: &Arc<Self>,
        id: &str,
        ask: &str,
        choice: usize,
    ) -> Result<(), Refusal> {
        if self.backend_of(id).ok_or(Refusal::NoSuchSession)? == Backend::Herdr {
            let herdr = self.herdr.get().ok_or_else(no_herdr)?;
            return herdr.answer(self, id, ask, choice).await;
        }
        let mut sessions = self.sessions.lock().unwrap();
        let live = sessions.get_mut(id).ok_or(Refusal::NoSuchSession)?;
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
        self.deliver(live, Answer::Choice { choice, title });
        self.save(&live.session);
        self.notify(id, false);
        Ok(())
    }

    /// Hands the answer to the run that asked, or, when there is none (the daemon restarted
    /// while it waited), to a new run.
    fn deliver(self: &Arc<Self>, live: &mut Live, answer: Answer) {
        let now = store::now();
        let s = &mut live.session;
        let Some(question) = s.ask.take() else {
            return;
        };
        s.add_log(&format!("answered: {}", answer.short()), now);
        s.state = State::Working;
        s.unread = false;
        s.updated = now;
        let unsent = match live.waiting.take() {
            Some(waiting) => waiting.send(answer).err(),
            None => Some(answer),
        };
        if let Some(answer) = unsent {
            let why = if std::mem::take(&mut live.after_pause) {
                Why::Pause
            } else {
                Why::Restart
            };
            let note = resume_note(&live.session, why, Some((&question, &answer)));
            live.session.inbox.push(note);
            if live.running() {
                live.wake.notify_one();
            } else {
                self.spawn_run(live);
            }
        }
    }

    /// Stops a working or asking session: a claude run's process is ended (SIGTERM, then
    /// SIGKILL), and a herdr program gets its stop key (the `interrupt` policy).
    pub async fn stop(self: &Arc<Self>, id: &str) -> Result<(), Refusal> {
        if self.backend_of(id).ok_or(Refusal::NoSuchSession)? == Backend::Herdr {
            let herdr = self.herdr.get().ok_or_else(no_herdr)?;
            return herdr.interrupt(self, id).await;
        }
        let mut sessions = self.sessions.lock().unwrap();
        let live = sessions.get_mut(id).ok_or(Refusal::NoSuchSession)?;
        if !matches!(
            live.session.state,
            State::Working | State::Ask | State::Paused | State::Scheduled | State::Watch
        ) {
            return Err(Refusal::NotRunning);
        }
        if live.session.backend == Backend::Claude {
            // The run ends its process itself, gently first.
            live.wake.notify_one();
        } else if let Some(run) = live.run.take() {
            run.abort();
        }
        live.generation += 1;
        live.waiting = None;
        let now = store::now();
        let s = &mut live.session;
        s.ask = None;
        s.inbox.clear();
        s.state = State::Stopped;
        s.unread = false;
        s.updated = now;
        s.add_log("stopped", now);
        tracing::info!(session = %id, "stopped");
        self.save(&live.session);
        self.notify(id, false);
        Ok(())
    }

    /// Ends a herdr session's program (the `kill` policy). Only herdr sessions have one to end.
    pub async fn kill(self: &Arc<Self>, id: &str) -> Result<(), Refusal> {
        if self.backend_of(id).ok_or(Refusal::NoSuchSession)? != Backend::Herdr {
            return Err(Refusal::Other(
                "only a herdr session's program can be ended".into(),
            ));
        }
        let herdr = self.herdr.get().ok_or_else(no_herdr)?;
        herdr.kill(self, id).await
    }

    /// Deletes a session for good, stopping it first. A herdr session is only forgotten: its
    /// program goes on, and it comes back when it does something new.
    pub async fn delete(&self, id: &str) -> Result<(), Refusal> {
        let live = self
            .sessions
            .lock()
            .unwrap()
            .remove(id)
            .ok_or(Refusal::NoSuchSession)?;
        match live.session.backend {
            Backend::Herdr => {
                if let (Some(herdr), Some(side)) = (self.herdr.get(), &live.session.herdr) {
                    herdr.forget(&side.address, side.turn);
                }
            }
            // The run sees the session gone and ends its process.
            Backend::Claude => live.wake.notify_one(),
            _ => {
                if let Some(run) = live.run {
                    run.abort();
                }
            }
        }
        if let Err(e) = store::remove(&self.dir, id) {
            tracing::warn!(session = %id, "couldn't delete its file: {e}");
        }
        let _ = std::fs::remove_file(self.dir.join(format!("{id}.mcp.json")));
        let _ = std::fs::remove_dir_all(self.work_dir(id));
        tracing::info!(session = %id, "deleted");
        self.notify(id, false);
        Ok(())
    }

    /// Deletes the sessions that finished more than `keep_days` ago.
    fn prune(&self, now: u64) {
        let mut sessions = self.sessions.lock().unwrap();
        let old: Vec<String> = sessions
            .values()
            .filter(|l| !l.running() && store::expired(&l.session, self.keep_days, now))
            .map(|l| l.session.id.clone())
            .collect();
        for id in old {
            sessions.remove(&id);
            let _ = store::remove(&self.dir, &id);
            self.notify(&id, false);
        }
    }

    /// Pauses a working or asking session, a schedule or a watch: its run stops (a claude
    /// run's process ends), and it keeps its plan, its inbox and its question. herdr sessions
    /// can't be paused.
    pub fn pause(self: &Arc<Self>, id: &str) -> Result<(), Refusal> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = sessions.get_mut(id).ok_or(Refusal::NoSuchSession)?;
        if live.session.backend == Backend::Herdr {
            return Err(Refusal::Other("a herdr session can't be paused".into()));
        }
        match live.session.state {
            State::Working | State::Ask | State::Scheduled | State::Watch => {}
            State::Paused => return Ok(()),
            _ => return Err(Refusal::NotRunning),
        }
        if live.session.backend == Backend::Claude {
            live.wake.notify_one(); // the run sees it isn't its session's any more
        } else if let Some(run) = live.run.take() {
            run.abort();
        }
        live.generation += 1;
        live.waiting = None;
        let now = store::now();
        let s = &mut live.session;
        s.paused_from = Some(s.state);
        s.state = State::Paused;
        s.updated = now;
        s.add_log("paused", now);
        tracing::info!(session = %id, "paused");
        self.save(&live.session);
        self.notify(id, false);
        Ok(())
    }

    /// Picks a paused session up where it was.
    pub fn resume(self: &Arc<Self>, id: &str) -> Result<(), Refusal> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = sessions.get_mut(id).ok_or(Refusal::NoSuchSession)?;
        if live.session.state != State::Paused {
            return Err(Refusal::Other("not paused".into()));
        }
        let now = store::now();
        live.session.add_log("resumed", now);
        self.resume_live(live, now);
        tracing::info!(session = %id, state = ?live.session.state, "resumed");
        self.save(&live.session);
        self.notify(id, false);
        Ok(())
    }

    /// Back to what it was doing before it was paused.
    fn resume_live(self: &Arc<Self>, live: &mut Live, now: u64) {
        let s = &mut live.session;
        s.updated = now;
        match s.paused_from.take().unwrap_or(State::Working) {
            State::Ask if s.ask.is_some() => {
                // The run that asked is gone: the answer starts a new one.
                s.state = State::Ask;
                live.after_pause = true;
                if !s.inbox.is_empty() {
                    // Words said while paused answer it.
                    let text = std::mem::take(&mut s.inbox).join("\n\n");
                    self.deliver(live, Answer::Text(text));
                }
            }
            State::Scheduled if s.schedule.is_some() => {
                s.state = State::Scheduled;
                if let Some(sched) = &mut s.schedule {
                    sched.next_run = schedule::next_run(
                        schedule::parse_at(&sched.at).unwrap_or((0, 0)),
                        sched.days,
                        now,
                    );
                }
            }
            State::Watch if s.watch.is_some() => {
                s.state = State::Watch;
                if let Some(watch) = &mut s.watch {
                    watch.next_look = now + watch.every_minutes * 60;
                }
            }
            _ => {
                s.state = State::Working;
                let note = resume_note(s, Why::Pause, None);
                s.inbox.insert(0, note);
                self.spawn_run(live);
            }
        }
    }

    /// Files a job for later (talk's `propose_job`): a session that waits, with no run, until
    /// someone behind the PIN starts it. Its id.
    pub fn propose(&self, prompt: &str, title: Option<&str>) -> String {
        let mut sessions = self.sessions.lock().unwrap();
        let id = store::new_id(|id| sessions.contains_key(id));
        let now = store::now();
        let mut session = Session::new(id.clone(), prompt, now);
        session.state = State::Proposed;
        if let Some(title) = title.map(str::trim).filter(|t| !t.is_empty()) {
            session.title = store::one_line(title, store::TITLE_CHARS);
        }
        session.add_log("filed by talk: waiting to be started", now);
        session.unread = true;
        tracing::info!(session = %id, title = %session.title, "a job filed by talk");
        self.save(&session);
        sessions.insert(id.clone(), Live::new(session));
        self.notify(&id, false);
        id
    }

    /// Starts a job talk filed, with more words if there are any: on the backend new sessions
    /// run on (with herdr, a program of its own, and the job's placeholder goes). Its id.
    pub async fn approve(
        self: &Arc<Self>,
        id: &str,
        more: Option<&str>,
    ) -> Result<String, Refusal> {
        let prompt = {
            let mut sessions = self.sessions.lock().unwrap();
            let live = sessions.get_mut(id).ok_or(Refusal::NoSuchSession)?;
            if live.session.state != State::Proposed {
                return Err(Refusal::Other("not a job waiting to be started".into()));
            }
            let more = more.map(str::trim).filter(|m| !m.is_empty());
            if self.backend != Backend::Herdr {
                let now = store::now();
                let s = &mut live.session;
                s.state = State::Working;
                s.backend = self.backend;
                if self.backend == Backend::Claude {
                    s.program = Some(claude::PROGRAM.into());
                }
                if let Some(more) = more {
                    s.inbox.push(more.to_string());
                    s.add_log(&format!("you: {more}"), now);
                }
                s.add_log("started", now);
                s.unread = false;
                s.updated = now;
                self.spawn_run(live);
                tracing::info!(session = %id, "a filed job started");
                self.save(&live.session);
                self.notify(id, false);
                return Ok(id.to_string());
            }
            match more {
                Some(more) => format!("{}\n\n{more}", live.session.prompt),
                None => live.session.prompt.clone(),
            }
        };
        let herdr = self.herdr.get().ok_or_else(no_herdr)?;
        let started = herdr.start_default(self, &prompt).await?;
        let _ = self.delete(id).await;
        Ok(started)
    }

    /// Runs a schedule now, or has a watch look now.
    pub fn run_now(self: &Arc<Self>, id: &str) -> Result<(), Refusal> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = sessions.get_mut(id).ok_or(Refusal::NoSuchSession)?;
        let now = store::now();
        match live.session.state {
            State::Scheduled if live.session.schedule.is_some() => self.start_scheduled(live, now),
            State::Watch if live.session.watch.is_some() => self.start_look(live, now),
            _ => {
                return Err(Refusal::Other(
                    "only a schedule or a watch can run now".into(),
                ));
            }
        }
        self.save(&live.session);
        self.notify(id, false);
        Ok(())
    }

    /// What is due: schedules' runs, watches' looks, questions past their deadline. Returns
    /// when the next thing is due.
    pub fn tick(self: &Arc<Self>, now: u64) -> Option<u64> {
        let mut sessions = self.sessions.lock().unwrap();
        let mut next: Option<u64> = None;
        let ids: Vec<String> = sessions.keys().cloned().collect();
        for id in ids {
            let Some(live) = sessions.get_mut(&id) else {
                continue;
            };
            let (changed, asked) = self.tick_one(live, now);
            if changed {
                self.save(&live.session);
                self.notify(&id, asked);
            }
            let s = &live.session;
            let due = match s.state {
                State::Scheduled => s.schedule.as_ref().map(|x| x.next_run),
                State::Watch => s.watch.as_ref().map(|x| x.next_look),
                State::Ask => s.ask.as_ref().and_then(|q| q.deadline).map(|d| d.at),
                _ => None,
            };
            if let Some(due) = due {
                next = Some(next.map_or(due, |n| n.min(due)));
            }
        }
        next
    }

    /// One session's part of tick: whether it changed, and whether it asks again.
    fn tick_one(self: &Arc<Self>, live: &mut Live, now: u64) -> (bool, bool) {
        let s = &mut live.session;
        match s.state {
            State::Scheduled => {
                let Some(sched) = &mut s.schedule else {
                    return (false, false);
                };
                if sched.next_run > now {
                    return (false, false);
                }
                if now > sched.next_run + MISSED_AFTER {
                    let missed = sched.next_run;
                    let at = schedule::parse_at(&sched.at).unwrap_or((0, 0));
                    sched.next_run = schedule::next_run(at, sched.days, now);
                    s.add_log(
                        &format!("missed the run at {}", schedule::local_text(missed)),
                        now,
                    );
                    return (true, false);
                }
                self.start_scheduled(live, now);
                (true, false)
            }
            State::Watch => match &s.watch {
                Some(watch) if watch.next_look <= now => {
                    self.start_look(live, now);
                    (true, false)
                }
                _ => (false, false),
            },
            State::Ask => {
                let Some(q) = s.ask.as_mut() else {
                    return (false, false);
                };
                let Some(deadline) = q.deadline.filter(|d| d.at <= now) else {
                    return (false, false);
                };
                match deadline.then {
                    OnDeadline::AskAgain => {
                        q.at = now;
                        q.deadline = Some(store::QuestionDeadline {
                            at: now + deadline.minutes.max(1) * 60,
                            ..deadline
                        });
                        let question = q.question.clone();
                        s.unread = true;
                        s.updated = now;
                        s.add_log(&format!("asked again: {question}"), now);
                        (true, true)
                    }
                    OnDeadline::Recommend if q.recommend.is_some() => {
                        let choice = q.recommend.unwrap_or_default();
                        let title = q
                            .options
                            .get(choice)
                            .map(|o| o.title.clone())
                            .unwrap_or_default();
                        s.add_log("nobody answered in time: took the recommendation", now);
                        self.deliver(live, Answer::Choice { choice, title });
                        (true, false)
                    }
                    OnDeadline::Recommend | OnDeadline::GiveUp => {
                        s.add_log("nobody answered in time", now);
                        self.deliver(
                            live,
                            Answer::Text(
                                "(Nobody answered in time. Give up on this choice and finish \
                                 with what you have, saying so.)"
                                    .into(),
                            ),
                        );
                        (true, false)
                    }
                }
            }
            _ => (false, false),
        }
    }

    /// Starts a schedule's run: a fresh run (no conversation, plan or Claude Code session from
    /// the last one) told to do its prompt now.
    fn start_scheduled(self: &Arc<Self>, live: &mut Live, now: u64) {
        let Some(sched) = &mut live.session.schedule else {
            return;
        };
        sched.running_since = Some(now);
        let prompt = format!(
            "(A scheduled run, {}: do the task below now, and end with the result as it should \
             read on the board. The schedule stays as it is: don't call schedule.)\n\n{}",
            schedule::local_text(now),
            sched.prompt
        );
        tracing::info!(session = %live.session.id, "a scheduled run");
        self.fresh_run(live, prompt, now);
    }

    /// Starts a watch's look, fresh like a scheduled run.
    fn start_look(self: &Arc<Self>, live: &mut Live, now: u64) {
        let Some(watch) = &mut live.session.watch else {
            return;
        };
        watch.looking_since = Some(now);
        let prompt = format!(
            "(A look for your watch, which looks every {} minutes until: {}. Look now: {}. \
             Then call watch with value set to the reading to chart, if there is a number, or \
             with met set to true if the condition holds now, and end with one line on what \
             you found.)",
            watch.every_minutes, watch.condition, watch.prompt
        );
        self.fresh_run(live, prompt, now);
    }

    fn fresh_run(self: &Arc<Self>, live: &mut Live, prompt: String, now: u64) {
        let s = &mut live.session;
        s.claude = None;
        s.conversation.clear();
        s.plan = None;
        s.ask = None;
        s.expect_at = None;
        s.inbox = vec![prompt];
        s.state = State::Working;
        s.updated = now;
        self.spawn_run(live);
    }

    /// Where a session's files are (the files tools).
    fn work_dir(&self, id: &str) -> PathBuf {
        self.dir.join("work").join(id)
    }

    // What the runs and their tools do.

    fn spawn_run(self: &Arc<Self>, live: &mut Live) {
        live.generation += 1;
        let (agent, id, run) = (self.clone(), live.session.id.clone(), live.generation);
        let task = match live.session.backend {
            Backend::Claude => tokio::spawn(agent.run_claude(id, run, live.wake.clone())),
            Backend::Herdr => return, // herdr runs it
            _ => tokio::spawn(agent.run(id, run)),
        };
        live.run = Some(task.abort_handle());
    }

    /// The session's run: legs until it has nothing more to hear. `run` is its generation.
    async fn run(self: Arc<Self>, id: String, run: u64) {
        loop {
            let Some((history, user)) = self.begin_leg(&id, run) else {
                return;
            };
            let tools = self.tools.with_extra(Arc::new(SessionTools {
                agent: self.clone(),
                id: id.clone(),
                run,
            }));
            let steer = {
                let (agent, id) = (self.clone(), id.clone());
                move || agent.hear(&id, run)
            };
            let (tx, rx) = mpsc::channel::<Piece>(64);
            let request = llm::Request {
                system: &self.system,
                history: &history,
                user: &user,
                audio: None,
                tools: &tools,
                rounds: self.max_rounds,
                steer: Some(&steer),
            };
            let leg =
                async { tokio::join!(self.llm.respond(request, tx), self.follow(&id, run, rx)) };
            let ((outcome, written), charges) = meter::measure(leg).await;
            let (cost, unpriced) = meter::total(&charges);
            self.add_cost(&id, run, cost, false);
            tracing::info!(
                session = %id,
                ok = outcome.is_ok(),
                cost,
                unpriced,
                requests = %meter::ids(&charges),
                "session leg"
            );
            let again = match outcome {
                Ok(_) => self.end_leg(&id, run, written),
                Err(e) => {
                    self.fail(&id, run, &e);
                    false
                }
            };
            if !again {
                return;
            }
        }
    }

    /// What the next leg hears: the conversation so far, and the user's words since its last
    /// answer (what was waiting moves into the conversation). None if the session has gone,
    /// stopped or isn't this run's.
    fn begin_leg(&self, id: &str, run: u64) -> Option<(Vec<Turn>, String)> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = own(&mut sessions, id, run).filter(|l| l.session.state == State::Working)?;
        let s = &mut live.session;
        for text in std::mem::take(&mut s.inbox) {
            s.add_said(Who::User, &text);
        }
        self.save(s);
        let unanswered = s
            .conversation
            .iter()
            .rev()
            .take_while(|m| m.role == Who::User)
            .count();
        let split = s.conversation.len() - unanswered;
        let mut user: Vec<&str> = s.conversation[split..]
            .iter()
            .map(|m| m.text.as_str())
            .collect();
        if user.is_empty() {
            user.push("Carry on.");
        }
        // Turns of one role in a row are joined: the APIs want them to alternate.
        let mut history: Vec<Turn> = Vec::new();
        for said in &s.conversation[..split] {
            let role = match said.role {
                Who::User => Role::User,
                Who::Assistant => Role::Assistant,
            };
            match history.last_mut() {
                Some(last) if last.role == role => {
                    last.text.push_str("\n\n");
                    last.text.push_str(&said.text);
                }
                _ => history.push(Turn {
                    role,
                    text: said.text.clone(),
                    audio: None,
                }),
            }
        }
        Some((history, user.join("\n\n")))
    }

    /// What the user has said since the last round, for the next one (the tool loop's steer).
    fn hear(&self, id: &str, run: u64) -> Vec<String> {
        let mut sessions = self.sessions.lock().unwrap();
        let Some(live) = own(&mut sessions, id, run) else {
            return Vec::new();
        };
        let heard = std::mem::take(&mut live.session.inbox);
        if !heard.is_empty() {
            for text in &heard {
                live.session.add_said(Who::User, text);
            }
            self.save(&live.session);
        }
        heard
    }

    /// Follows a leg's reply: each stretch of text before a tool call goes to the log as a
    /// line, then the call, and the last stretch is the result (or the last there was, if the
    /// model ended on a tool call).
    async fn follow(&self, id: &str, run: u64, mut rx: mpsc::Receiver<Piece>) -> Written {
        let (mut all, mut stretch, mut last) = (String::new(), String::new(), String::new());
        while let Some(piece) = rx.recv().await {
            match piece {
                Piece::Text(text) | Piece::Transcript(text) => {
                    all.push_str(&text);
                    stretch.push_str(&text);
                }
                Piece::Tool(name, input) => {
                    if !stretch.trim().is_empty() {
                        self.log(id, run, &store::plain(&stretch));
                        last = std::mem::take(&mut stretch);
                    }
                    stretch.clear();
                    // The plan and the question show for themselves.
                    if !tools::shows_itself(&name) {
                        self.log(id, run, &tools::short_call(&name, &input));
                    }
                }
                Piece::Audio(_) => {}
            }
        }
        let result = if stretch.trim().is_empty() {
            last
        } else {
            stretch
        };
        Written { all, result }
    }

    /// A leg ended with an answer. True if the user said more meanwhile and another leg should
    /// hear it; otherwise the session is done (or a note, if its result opens with a heading).
    fn end_leg(&self, id: &str, run: u64, written: Written) -> bool {
        let mut sessions = self.sessions.lock().unwrap();
        let Some(live) = own(&mut sessions, id, run) else {
            return false;
        };
        let s = &mut live.session;
        if s.state != State::Working {
            return false;
        }
        s.add_said(Who::Assistant, &written.all);
        if !s.inbox.is_empty() {
            self.save(s);
            return true;
        }
        let now = store::now();
        let result = written.result.trim();
        if self.end_planned(live, result, None, now) {
            return false;
        }
        let s = &mut live.session;
        s.state = if result.starts_with("# ") {
            State::Note
        } else {
            State::Done
        };
        s.result = Some(if result.is_empty() {
            "Finished, with nothing to report.".into()
        } else {
            store::cut_bytes(result, store::RESULT_BYTES)
        });
        if let Some(plan) = &mut s.plan {
            plan.current = plan.steps.len() + 1;
        }
        s.unread = true;
        s.updated = now;
        live.run = None;
        live.waiting = None;
        tracing::info!(session = %id, state = ?live.session.state, "session finished");
        self.save(&live.session);
        self.notify(id, false);
        false
    }

    /// The end of a leg of a schedule or a watch (`failed`: why it failed): the run is
    /// recorded and it waits for the next. False if it is neither, or a watch whose condition
    /// held (that ends as any session does).
    fn end_planned(&self, live: &mut Live, result: &str, failed: Option<&str>, now: u64) -> bool {
        let s = &mut live.session;
        let first =
            |text: &str| store::plain(text.lines().find(|l| !l.trim().is_empty()).unwrap_or(""));
        if let Some(mut sched) = s.schedule.take() {
            if let Some(since) = sched.running_since.take() {
                let state = match failed {
                    Some(_) => State::Fail,
                    None if result.starts_with("# ") => State::Note,
                    None => State::Done,
                };
                sched.runs.push(store::Run {
                    at: since,
                    state,
                    secs: now.saturating_sub(since),
                });
                let extra = sched.runs.len().saturating_sub(store::RUNS_KEPT);
                sched.runs.drain(..extra);
                s.result = Some(match failed {
                    Some(message) => store::cut_bytes(message, store::RESULT_BYTES),
                    None if result.is_empty() => "Finished, with nothing to report.".into(),
                    None => store::cut_bytes(result, store::RESULT_BYTES),
                });
                s.unread = true;
            } else {
                // Words said into it, or the leg that made it: the reply goes to the log.
                match failed {
                    Some(message) => s.add_log(&format!("failed: {message}"), now),
                    None => s.add_log(&first(result), now),
                }
            }
            let at = schedule::parse_at(&sched.at).unwrap_or((0, 0));
            sched.next_run = schedule::next_run(at, sched.days, now);
            s.schedule = Some(sched);
            s.state = State::Scheduled;
        } else if let Some(mut watch) = s.watch.take() {
            if watch.met && failed.is_none() {
                s.watch = Some(watch);
                return false;
            }
            watch.looking_since = None;
            watch.met = false;
            match failed {
                Some(message) => s.add_log(&format!("failed: {message}"), now),
                None => s.add_log(&first(result), now),
            }
            watch.next_look = now + watch.every_minutes * 60;
            s.watch = Some(watch);
            s.state = State::Watch;
        } else {
            return false;
        }
        s.plan = None;
        s.expect_at = None;
        s.updated = now;
        live.run = None;
        live.waiting = None;
        tracing::info!(session = %s.id, state = ?s.state, "waits for its next run");
        self.save(&live.session);
        self.notify(&live.session.id, false);
        true
    }

    fn fail(&self, id: &str, run: u64, error: &anyhow::Error) {
        let mut sessions = self.sessions.lock().unwrap();
        let Some(live) = own(&mut sessions, id, run) else {
            return;
        };
        if live.session.state != State::Working {
            return;
        }
        let message = if error.downcast_ref::<TooManyRounds>().is_some() {
            format!(
                "too many steps: it stopped after {} model rounds",
                self.max_rounds
            )
        } else {
            format!("{error:#}")
        };
        tracing::warn!(session = %id, "session failed: {message}");
        let now = store::now();
        if self.end_planned(live, "", Some(&message), now) {
            return;
        }
        let s = &mut live.session;
        s.add_log(&format!("failed: {message}"), now);
        s.state = State::Fail;
        s.result = Some(store::cut_bytes(&message, store::RESULT_BYTES));
        s.unread = true;
        s.updated = now;
        live.run = None;
        live.waiting = None;
        self.save(&live.session);
        self.notify(id, false);
    }

    /// ask_choice: puts the question up; the answer comes on the receiver.
    fn put_question(
        &self,
        id: &str,
        run: u64,
        mut question: Question,
    ) -> Result<oneshot::Receiver<Answer>, String> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = own(&mut sessions, id, run).ok_or("the session was stopped")?;
        let s = &mut live.session;
        if s.state != State::Working || s.ask.is_some() {
            return Err("a question is already waiting for its answer: ask one at a time".into());
        }
        let now = store::now();
        s.asked += 1;
        question.id = format!("a{}", s.asked);
        question.at = now;
        if let Some(deadline) = &mut question.deadline {
            deadline.at = now + deadline.minutes * 60;
        }
        s.add_log(&format!("asked: {}", question.question), now);
        s.ask = Some(question);
        s.state = State::Ask;
        s.unread = true;
        s.updated = now;
        let (tx, rx) = oneshot::channel();
        live.waiting = Some(tx);
        tracing::info!(session = %id, "asking");
        self.save(&live.session);
        self.notify(id, true);
        Ok(rx)
    }

    /// plan: the checklist, and the title if the model gave one.
    fn set_plan(
        &self,
        id: &str,
        run: u64,
        steps: Vec<String>,
        current: usize,
        title: Option<String>,
        expect_minutes: Option<u64>,
    ) -> Result<(), String> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = own(&mut sessions, id, run).ok_or("the session was stopped")?;
        let s = &mut live.session;
        let now = store::now();
        s.plan = Some(store::Plan { steps, current });
        if let Some(title) = title {
            s.title = title;
        }
        if let Some(minutes) = expect_minutes {
            s.expect_at = Some(now + minutes.max(1) * 60);
        }
        s.updated = now;
        self.save(s);
        self.notify(id, false);
        Ok(())
    }

    /// schedule: makes the session a schedule (or changes it), keeping the runs it had.
    fn set_schedule(&self, id: &str, run: u64, mut sched: store::Schedule) -> Result<(), String> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = own(&mut sessions, id, run).ok_or("the session was stopped")?;
        let s = &mut live.session;
        if let Some(old) = s.schedule.take() {
            sched.runs = old.runs;
            sched.running_since = old.running_since;
        }
        s.add_log(
            &format!(
                "scheduled {} at {}",
                schedule::days_words(sched.days),
                sched.at
            ),
            store::now(),
        );
        s.schedule = Some(sched);
        s.updated = store::now();
        self.save(s);
        self.notify(id, false);
        Ok(())
    }

    /// schedule with cancel: the session is a schedule no more, and ends as any session does.
    fn cancel_schedule(&self, id: &str, run: u64) -> Result<bool, String> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = own(&mut sessions, id, run).ok_or("the session was stopped")?;
        let had = live.session.schedule.take().is_some();
        if had {
            live.session.add_log("schedule cancelled", store::now());
            self.save(&live.session);
            self.notify(id, false);
        }
        Ok(had)
    }

    /// watch, set up: makes the session a watch (or changes it), keeping its readings.
    fn set_watch(&self, id: &str, run: u64, mut watch: store::Watch) -> Result<(), String> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = own(&mut sessions, id, run).ok_or("the session was stopped")?;
        let s = &mut live.session;
        if let Some(old) = s.watch.take() {
            watch.values = old.values;
            watch.looking_since = old.looking_since;
        }
        s.add_log(
            &format!(
                "watching every {} min: {}",
                watch.every_minutes, watch.condition
            ),
            store::now(),
        );
        s.watch = Some(watch);
        s.updated = store::now();
        self.save(s);
        self.notify(id, false);
        Ok(())
    }

    /// watch, at a look: a reading, and whether the condition holds. Err if it isn't a watch.
    fn watch_reading(
        &self,
        id: &str,
        run: u64,
        value: Option<f64>,
        met: bool,
    ) -> Result<(), String> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = own(&mut sessions, id, run).ok_or("the session was stopped")?;
        let watch = live.session.watch.as_mut().ok_or(
            "this task isn't a watch yet: give every_minutes and condition to make it one",
        )?;
        if let Some(value) = value.filter(|v| v.is_finite()) {
            watch.values.push(value);
            let extra = watch.values.len().saturating_sub(store::VALUES_KEPT);
            watch.values.drain(..extra);
        }
        watch.met |= met;
        live.session.updated = store::now();
        self.save(&live.session);
        self.notify(id, false);
        Ok(())
    }

    /// show_panel: the session's screen, replacing the one before. True if it had one by
    /// that title.
    fn set_panel(&self, id: &str, run: u64, panel: voice_proto::Panel) -> Result<bool, String> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = own(&mut sessions, id, run).ok_or("the session was stopped")?;
        let s = &mut live.session;
        let replaced = s.panel.as_ref().is_some_and(|p| p.title == panel.title);
        if !replaced {
            s.add_log(&format!("showed the panel {}", panel.title), panel.at);
        }
        s.updated = panel.at;
        s.panel = Some(panel);
        self.save(s);
        self.notify(id, false);
        Ok(replaced)
    }

    /// A choice on a session's panel: it reaches the session as the user's words.
    pub async fn panel_choice(
        self: &Arc<Self>,
        id: &str,
        block: &str,
        choice: usize,
    ) -> Result<Said, Refusal> {
        let (text, title) = {
            let sessions = self.sessions.lock().unwrap();
            let live = sessions.get(id).ok_or(Refusal::NoSuchSession)?;
            let panel = live
                .session
                .panel
                .as_ref()
                .ok_or(Refusal::Other("no panel".into()))?;
            tools::panel_answer(panel, block, choice).ok_or(Refusal::NoSuchChoice)?
        };
        tracing::info!(session = %id, block, "panel choice: {title}");
        self.say(id, &text).await
    }

    /// A session's last result as words to say (a schedule with speak): plain text, no
    /// Markdown, cut to a minute or so.
    pub fn result_to_say(&self, id: &str) -> Option<String> {
        let sessions = self.sessions.lock().unwrap();
        let result = sessions.get(id)?.session.result.clone()?;
        let lines: Vec<String> = result
            .lines()
            .map(store::plain)
            .filter(|l| !l.trim().is_empty())
            .collect();
        Some(store::cut(&lines.join(". ").replace("..", "."), 1200))
    }

    /// Whether a session is a watch (for the tool, which sets one up or takes a reading).
    fn is_watch(&self, id: &str) -> bool {
        let sessions = self.sessions.lock().unwrap();
        sessions.get(id).is_some_and(|l| l.session.watch.is_some())
    }

    /// Adds a line to the session's log, from its run.
    fn log(&self, id: &str, run: u64, text: &str) {
        let mut sessions = self.sessions.lock().unwrap();
        let Some(live) = own(&mut sessions, id, run) else {
            return;
        };
        let now = store::now();
        live.session.add_log(text, now);
        live.session.updated = now;
        self.save(&live.session);
        self.notify(id, false);
    }

    /// Adds what a leg's model requests cost.
    fn add_cost(&self, id: &str, run: u64, usd: f64, notional: bool) {
        if usd <= 0.0 {
            return;
        }
        let mut sessions = self.sessions.lock().unwrap();
        if let Some(live) = own(&mut sessions, id, run) {
            live.session.cost += usd;
            live.session.cost_notional |= notional;
            self.save(&live.session);
        }
    }

    fn save(&self, session: &Session) {
        if let Err(e) = store::save(&self.dir, session) {
            tracing::warn!(session = %session.id, "couldn't save it: {e}");
        }
    }

    fn notify(&self, id: &str, asked: bool) {
        // No one following is fine.
        let _ = self.events.send(Event {
            session: id.to_string(),
            asked,
        });
        // Something changed: what is due may have too.
        self.clock.notify_one();
    }

    /// Stops every run, as a daemon that quits does (for tests of a restart).
    #[cfg(test)]
    fn halt(&self) {
        for live in self.sessions.lock().unwrap().values_mut() {
            if let Some(run) = live.run.take() {
                run.abort();
            }
        }
    }
}

fn no_herdr() -> Refusal {
    Refusal::Other("no [herdr] on this daemon".into())
}

/// What a new run is told when it starts without the last one's tool results (the daemon
/// restarted, or the user paused it): its plan and what it did, and the answer to its question
/// if that is what it was waiting for.
fn resume_note(s: &Session, why: Why, answered: Option<(&Question, &Answer)>) -> String {
    let when = if answered.is_some() {
        "waited for an answer"
    } else {
        "was being worked on"
    };
    let mut note = match why {
        Why::Restart => format!(
            "(inkharness restarted while this task {when}, so the results of the tool calls \
             before that are gone."
        ),
        // A claude run resumes its own session, tool results and all.
        Why::Pause if s.backend == Backend::Claude => {
            format!("(The user paused this task while it {when}, and has resumed it.")
        }
        Why::Pause => format!(
            "(The user paused this task while it {when}, and has resumed it. The results of \
             the tool calls before the pause are gone."
        ),
    };
    if let Some(plan) = &s.plan {
        let steps: Vec<String> = plan
            .steps
            .iter()
            .enumerate()
            .map(|(i, step)| {
                let mark = match (i + 1).cmp(&plan.current) {
                    std::cmp::Ordering::Less => " (done)",
                    std::cmp::Ordering::Equal => " (now)",
                    std::cmp::Ordering::Greater => "",
                };
                format!("{}. {step}{mark}", i + 1)
            })
            .collect();
        note.push_str(&format!(" Your plan: {}.", steps.join("; ")));
    }
    let did: Vec<&str> = s
        .log
        .iter()
        .rev()
        .take(15)
        .rev()
        .map(|l| l.text.as_str())
        .collect();
    if !did.is_empty() {
        note.push_str(&format!(" What you did, oldest first: {}.", did.join("; ")));
    }
    if let Some((question, answer)) = answered {
        let options: Vec<String> = question
            .options
            .iter()
            .enumerate()
            .map(|(i, o)| format!("{i}. {}", o.title))
            .collect();
        note.push_str(&format!(
            " You asked: \"{}\" ({}). The answer: {}.",
            question.question,
            options.join("; "),
            answer.for_model()
        ));
    }
    note.push_str(" Carry on from there.)");
    note
}

/// This machine's name, uppercased, for `place` when it isn't set.
fn hostname() -> String {
    let name = std::env::var("HOSTNAME")
        .ok()
        .filter(|n| !n.trim().is_empty())
        .or_else(|| std::fs::read_to_string("/proc/sys/kernel/hostname").ok())
        .or_else(|| std::fs::read_to_string("/etc/hostname").ok())
        .or_else(|| {
            std::process::Command::new("hostname")
                .output()
                .ok()
                .and_then(|o| String::from_utf8(o.stdout).ok())
        })
        .unwrap_or_default();
    let name = name
        .trim()
        .split('.')
        .next()
        .unwrap_or_default()
        .to_uppercase();
    if name.is_empty() {
        "INKHARNESS".into()
    } else {
        name
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use std::path::Path;

    use async_trait::async_trait;
    use tokio::sync::Semaphore;

    use super::*;
    use crate::llm::{Outcome, Request};
    use tools::PROPOSE_TOOL;

    pub fn cfg(dir: &Path) -> config::Agent {
        config::Agent {
            dir: dir.display().to_string(),
            place: Some("HOME".into()),
            pin_sha256: None,
            ticket_minutes: 60,
            keep_days: 30,
            max_rounds: 40,
            llm: None,
            backend: Default::default(),
            claude: None,
            reply: Default::default(),
        }
    }

    pub fn tools() -> ToolSet {
        ToolSet::build(
            None,
            &config::Js::default(),
            Default::default(),
            Default::default(),
            reqwest::Client::new(),
        )
        .unwrap()
    }

    fn mock(dir: &Path) -> Arc<Agent> {
        Agent::open(
            &cfg(dir),
            Box::new(mock::MockAgent),
            tools(),
            prompt(),
            "test",
            Default::default(),
        )
        .unwrap()
    }

    /// The session once it is in `state`.
    pub async fn until(agent: &Agent, id: &str, state: State) -> SessionView {
        for _ in 0..500 {
            match agent.view(id) {
                Some(view) if view.state == state => return view,
                _ => tokio::time::sleep(Duration::from_millis(10)).await,
            }
        }
        panic!("never {state:?}: {:?}", agent.view(id))
    }

    fn log(view: &SessionView) -> Vec<&str> {
        view.log.iter().rev().map(|l| l.text.as_str()).collect()
    }

    #[tokio::test]
    async fn a_session_asks_and_carries_on_with_the_answer() {
        let dir = crate::skills::tests::scratch("agent-ask");
        let agent = mock(&dir);
        let mut events = agent.subscribe();
        let id = agent
            .start("find a plumber and ask me which")
            .await
            .unwrap();
        let view = until(&agent, &id, State::Ask).await;
        assert_eq!(view.title, "Mock · find a plumber and ask me which");
        assert_eq!((view.step, view.steps), (Some(2), Some(3)));
        let ask = view.ask.unwrap();
        assert_eq!(
            (ask.id.as_str(), ask.options.len(), ask.recommend),
            ("a1", 2, Some(0))
        );
        assert_eq!(agent.question(&id).unwrap(), ask);
        let list = agent.list(false);
        assert_eq!((list.counts.ask, list.counts.unread), (1, 1));
        assert_eq!(list.sessions[0].sub, "Which plumber should I book?");
        // Someone following heard that it asked.
        let mut asked = false;
        while let Ok(event) = events.try_recv() {
            asked |= event.asked && event.session == id;
        }
        assert!(asked);

        assert_eq!(agent.answer(&id, "a1", 2).await, Err(Refusal::NoSuchChoice));
        assert_eq!(
            agent.answer(&id, "a2", 0).await,
            Err(Refusal::AlreadyAnswered)
        );
        assert_eq!(
            agent.answer("nope", "a1", 0).await,
            Err(Refusal::NoSuchSession)
        );
        agent.answer(&id, "a1", 1).await.unwrap();
        assert_eq!(
            agent.answer(&id, "a1", 1).await,
            Err(Refusal::AlreadyAnswered)
        );
        let view = until(&agent, &id, State::Done).await;
        let result = view.result.as_deref().unwrap();
        assert!(
            result.contains(r#"You chose: {"choice":1,"title":"Northgate · Wed 13:00"}"#),
            "{result}"
        );
        assert_eq!(
            log(&view),
            [
                "Reading the task.",
                "run_js 1 + 1",
                "asked: Which plumber should I book?",
                "answered: Northgate · Wed 13:00"
            ]
        );
        assert!(
            view.plan
                .unwrap()
                .iter()
                .all(|p| p.state == voice_proto::StepState::Done)
        );
        assert!(agent.list(false).sessions[0].unread);
        assert!(agent.seen(&id));
        assert!(!agent.list(false).sessions[0].unread);
        assert!(!agent.seen("nope"));
    }

    #[tokio::test]
    async fn a_question_answered_in_words() {
        let dir = crate::skills::tests::scratch("agent-words");
        let agent = mock(&dir);
        let id = agent.start("ask me which plumber").await.unwrap();
        until(&agent, &id, State::Ask).await;
        let said = agent.say(&id, "none, find one for Saturday").await.unwrap();
        assert_eq!(
            said,
            Said::Answered {
                question: "Which plumber should I book?".into()
            }
        );
        let view = until(&agent, &id, State::Done).await;
        assert!(
            view.result
                .as_deref()
                .unwrap()
                .contains(r#"You chose: {"text":"none, find one for Saturday"}"#)
        );
        assert!(log(&view).contains(&"answered: none, find one for Saturday"));
    }

    #[tokio::test]
    async fn a_restart_keeps_the_question_and_the_work() {
        let dir = crate::skills::tests::scratch("agent-restart");
        let agent = mock(&dir);
        let asking = agent.start("ask me which plumber").await.unwrap();
        until(&agent, &asking, State::Ask).await;
        agent.halt();
        drop(agent);
        // One that was working when the daemon went.
        let mut working = Session::new("w1".into(), "tidy the notes", 100);
        working.inbox.clear();
        working.add_said(Who::User, "tidy the notes");
        working.plan = Some(store::Plan {
            steps: vec!["Read".into(), "Tidy".into()],
            current: 2,
        });
        working.add_log("load_skill notes", 101);
        store::save(&dir, &working).unwrap();

        let agent = mock(&dir);
        let view = until(&agent, "w1", State::Done).await;
        let result = view.result.unwrap();
        assert!(result.starts_with("You said: tidy the notes\n\n(inkharness restarted while this task was being worked on"), "{result}");
        assert!(
            result.contains("Your plan: 1. Read (done); 2. Tidy (now)."),
            "{result}"
        );
        assert!(
            result.contains("What you did, oldest first: load_skill notes."),
            "{result}"
        );

        // The question waited through it; its answer starts a run that is told what it was.
        let view = agent.view(&asking).unwrap();
        assert_eq!(view.state, State::Ask);
        agent.answer(&asking, "a1", 0).await.unwrap();
        let view = until(&agent, &asking, State::Done).await;
        let result = view.result.unwrap();
        assert!(
            result.contains(r#"The answer: {"choice":0,"title":"Riverside · Thu 08:00"}."#),
            "{result}"
        );
        assert!(result.contains("You asked: \"Which plumber should I book?\" (0. Riverside · Thu 08:00; 1. Northgate · Wed 13:00)."), "{result}");
    }

    /// Answers "heard: …"; the first leg of a session waits for a permit first.
    struct Gated(Arc<Semaphore>);

    #[async_trait]
    impl Llm for Gated {
        async fn respond(&self, request: Request<'_>, out: mpsc::Sender<Piece>) -> Result<Outcome> {
            if request.history.is_empty() {
                self.0.acquire().await?.forget();
            }
            out.send(Piece::Text(format!("heard: {}", request.user)))
                .await?;
            Ok(Outcome::Done)
        }
    }

    #[tokio::test]
    async fn talking_into_a_session_working_or_finished() {
        let dir = crate::skills::tests::scratch("agent-say");
        let gate = Arc::new(Semaphore::new(0));
        let agent = Agent::open(
            &cfg(&dir),
            Box::new(Gated(gate.clone())),
            tools(),
            prompt(),
            "test",
            Default::default(),
        )
        .unwrap();
        let id = agent.start("find a plumber").await.unwrap();
        tokio::task::yield_now().await;
        let said = agent.say(&id, "make it Saturday").await.unwrap();
        assert_eq!(
            said,
            Said::Added {
                title: "Find a plumber".into(),
                now: "you: make it Saturday".into()
            }
        );
        gate.add_permits(1);
        // The first leg ends; the next hears what was said meanwhile.
        let view = until(&agent, &id, State::Done).await;
        assert_eq!(view.result.as_deref(), Some("heard: make it Saturday"));
        // Finished: talking starts it again, and the spoken reply gets the result.
        let said = agent.say(&id, "and the price?").await.unwrap();
        assert_eq!(
            said,
            Said::Resumed {
                title: "Find a plumber".into(),
                result: Some("heard: make it Saturday".into())
            }
        );
        assert_eq!(agent.view(&id).unwrap().result, None, "none while it works");
        let view = until(&agent, &id, State::Done).await;
        assert_eq!(view.result.as_deref(), Some("heard: and the price?"));
        let said: Vec<String> = agent.sessions.lock().unwrap()[&id]
            .session
            .conversation
            .iter()
            .map(|s| s.text.clone())
            .collect();
        assert_eq!(
            said,
            [
                "find a plumber",
                "heard: find a plumber",
                "make it Saturday",
                "heard: make it Saturday",
                "and the price?",
                "heard: and the price?"
            ]
        );
        assert_eq!(agent.say("nope", "x").await, Err(Refusal::NoSuchSession));
    }

    #[tokio::test]
    async fn stop_and_delete() {
        let dir = crate::skills::tests::scratch("agent-stop");
        let gate = Arc::new(Semaphore::new(0));
        let agent = Agent::open(
            &cfg(&dir),
            Box::new(Gated(gate.clone())),
            tools(),
            prompt(),
            "test",
            Default::default(),
        )
        .unwrap();
        let id = agent.start("find a plumber").await.unwrap();
        tokio::task::yield_now().await;
        agent.stop(&id).await.unwrap();
        assert_eq!(agent.stop(&id).await, Err(Refusal::NotRunning));
        gate.add_permits(1);
        tokio::time::sleep(Duration::from_millis(50)).await;
        let view = agent.view(&id).unwrap();
        assert_eq!((view.state, &view.result), (State::Stopped, &None));
        assert_eq!(log(&view), ["stopped"]);
        assert!(dir.join(format!("{id}.json")).exists());
        agent.delete(&id).await.unwrap();
        assert!(!dir.join(format!("{id}.json")).exists());
        assert_eq!(agent.delete(&id).await, Err(Refusal::NoSuchSession));
        assert!(agent.view(&id).is_none() && agent.list(false).sessions.is_empty());
    }

    /// Waits for a permit, then asks a question (or, the first time with `panics`, panics);
    /// answers "heard it" once asked whether it is there.
    struct AsksLate {
        gate: Arc<Semaphore>,
        panics: std::sync::atomic::AtomicBool,
    }

    #[async_trait]
    impl Llm for AsksLate {
        async fn respond(&self, request: Request<'_>, out: mpsc::Sender<Piece>) -> Result<Outcome> {
            if self
                .panics
                .swap(false, std::sync::atomic::Ordering::Relaxed)
            {
                panic!("a backend that panics");
            }
            if request.user.contains("are you there?") {
                out.send(Piece::Text("heard it".into())).await?;
                return Ok(Outcome::Done);
            }
            self.gate.acquire().await?.forget();
            let question = serde_json::json!({"question": "Which?", "options": [{"title": "A"}, {"title": "B"}]});
            let answer = request.tools.call(tools::ASK_TOOL, question).await;
            out.send(Piece::Text(format!("{answer:?}"))).await?;
            Ok(Outcome::Done)
        }
    }

    #[tokio::test]
    async fn a_stopped_run_changes_nothing() {
        let dir = crate::skills::tests::scratch("agent-stopped-run");
        let gate = Arc::new(Semaphore::new(0));
        let llm = AsksLate {
            gate: gate.clone(),
            panics: false.into(),
        };
        let agent = Agent::open(
            &cfg(&dir),
            Box::new(llm),
            tools(),
            prompt(),
            "test",
            Default::default(),
        )
        .unwrap();
        let id = agent.start("pick one").await.unwrap();
        tokio::task::yield_now().await;
        // Stopped while the run, about to ask, can't be cancelled at once.
        let run = agent.sessions.lock().unwrap()[&id].generation;
        agent.stop(&id).await.unwrap();
        let question = Question {
            id: String::new(),
            question: "Which?".into(),
            detail: None,
            options: vec![],
            recommend: None,
            at: 0,
            deadline: None,
        };
        assert!(agent.put_question(&id, run, question).is_err());
        agent.log(&id, run, "a late line");
        gate.add_permits(1);
        tokio::time::sleep(Duration::from_millis(50)).await;
        let view = agent.view(&id).unwrap();
        assert_eq!((view.state, &view.ask), (State::Stopped, &None));
        assert_eq!(log(&view), ["stopped"]);
    }

    #[tokio::test]
    async fn a_run_that_died_is_started_again() {
        let dir = crate::skills::tests::scratch("agent-panic");
        let llm = AsksLate {
            gate: Arc::new(Semaphore::new(0)),
            panics: true.into(),
        };
        let agent = Agent::open(
            &cfg(&dir),
            Box::new(llm),
            tools(),
            prompt(),
            "test",
            Default::default(),
        )
        .unwrap();
        let id = agent.start("pick one").await.unwrap();
        tokio::time::sleep(Duration::from_millis(50)).await;
        assert_eq!(agent.view(&id).unwrap().state, State::Working);
        agent.say(&id, "are you there?").await.unwrap();
        let view = until(&agent, &id, State::Done).await;
        assert_eq!(view.result.as_deref(), Some("heard it"));
    }

    /// Fails as a tool loop does when the model keeps calling tools.
    struct Endless;

    #[async_trait]
    impl Llm for Endless {
        async fn respond(&self, request: Request<'_>, _: mpsc::Sender<Piece>) -> Result<Outcome> {
            Err(TooManyRounds(request.rounds).into())
        }
    }

    #[tokio::test]
    async fn sessions_run_on_agent_llm_when_it_is_set() {
        let reply = format!(
            "data: {}\n\ndata: [DONE]\n\n",
            serde_json::json!({"choices": [{"delta": {"content": "Booked Riverside."}}]})
        );
        let (url, mut bodies) = crate::llm::testserver::serve(vec![reply]).await;
        let dir = crate::skills::tests::scratch("agent-own-model");
        // [llm] is the mock, which would play its own session through.
        let toml = format!(
            "[llm]\nbackend = \"mock\"\n[agent]\ndir = \"{}\"\n[agent.llm]\n\
             backend = \"openai_compat\"\nbase_url = \"{url}\"\n\
             model = \"anthropic/claude-haiku-5.5\"\napi_key_env = \"INKHARNESS_TEST_NO_KEY\"\n",
            dir.display()
        );
        let place = crate::place::Place {
            config: config::Config::from_table(toml::from_str(&toml).unwrap()).unwrap(),
            read: vec![],
            instructions: vec![],
            skill_dirs: vec![],
        };
        let harness = Harness::build(&place, false).await.unwrap();
        let agent = start(&place.config, &harness).await.unwrap().unwrap();
        let id = agent.start("book a plumber").await.unwrap();
        let view = until(&agent, &id, State::Done).await;
        assert_eq!(view.result.as_deref(), Some("Booked Riverside."));
        let body = bodies.recv().await.unwrap();
        assert_eq!(body["model"], "anthropic/claude-haiku-5.5");
        let tools: Vec<&str> = body["tools"]
            .as_array()
            .unwrap()
            .iter()
            .filter_map(|t| t["function"]["name"].as_str())
            .collect();
        assert!(
            tools.contains(&"ask_choice") && tools.contains(&"plan"),
            "{tools:?}"
        );
        assert!(
            body["messages"][0]["content"]
                .as_str()
                .unwrap()
                .starts_with("You are an agent")
        );

        // A model that speaks has to have a voice, as anywhere else.
        let toml = format!(
            "[llm]\nbackend = \"mock\"\n[agent]\ndir = \"{}\"\n[agent.llm]\n\
             backend = \"openai_compat\"\nbase_url = \"u\"\nmodel = \"m\"\noutput = \"audio\"\n",
            dir.display()
        );
        let cfg = config::Config::from_table(toml::from_str(&toml).unwrap()).unwrap();
        let err = start(&cfg, &harness).await.err().unwrap().to_string();
        assert!(err.contains("[agent.llm] has output = \"audio\""), "{err}");
    }

    #[tokio::test]
    async fn too_many_steps_fail() {
        let dir = crate::skills::tests::scratch("agent-endless");
        let agent = Agent::open(
            &cfg(&dir),
            Box::new(Endless),
            tools(),
            prompt(),
            "test",
            Default::default(),
        )
        .unwrap();
        let id = agent.start("count forever").await.unwrap();
        let view = until(&agent, &id, State::Fail).await;
        assert_eq!(
            view.result.as_deref(),
            Some("too many steps: it stopped after 40 model rounds")
        );
        let id = agent.start("a note about fail").await.unwrap();
        let view = until(&agent, &id, State::Fail).await;
        assert!(view.result.unwrap().starts_with("too many steps"));
    }

    #[tokio::test]
    async fn notes_and_failures_from_the_mock() {
        let dir = crate::skills::tests::scratch("agent-states");
        let agent = mock(&dir);
        let note = agent.start("a note on today").await.unwrap();
        let fail = agent.start("this one should fail").await.unwrap();
        let view = until(&agent, &note, State::Note).await;
        assert!(view.result.unwrap().starts_with("# Note: a note on today"));
        let view = until(&agent, &fail, State::Fail).await;
        assert_eq!(
            view.result.as_deref(),
            Some("the mock model failed, as the task asked")
        );
    }

    #[tokio::test]
    async fn old_sessions_are_deleted() {
        let dir = crate::skills::tests::scratch("agent-prune");
        let now = store::now();
        let day = 86_400;
        let old = store::tests::session("old1", State::Done, now - 31 * day);
        let kept = store::tests::session("kept1", State::Done, now - 29 * day);
        let asking = store::tests::session("ask1", State::Ask, now - 90 * day);
        for s in [&old, &kept, &asking] {
            store::save(&dir, s).unwrap();
        }
        let agent = mock(&dir);
        let ids: Vec<String> = agent
            .list(false)
            .sessions
            .into_iter()
            .map(|s| s.id)
            .collect();
        assert_eq!(ids, ["ask1", "kept1"]);
        assert!(!dir.join("old1.json").exists());
        agent.prune(now + 2 * day);
        assert!(agent.view("kept1").is_none() && agent.view("ask1").is_some());
    }

    /// Whether the session's log has a line containing `text`.
    fn logged(view: &SessionView, text: &str) -> bool {
        view.log.iter().any(|l| l.text.contains(text))
    }

    #[tokio::test]
    async fn paused_sessions_pick_up_where_they_were() {
        let dir = crate::skills::tests::scratch("agent-pause");
        let agent = mock(&dir);

        // Asking: paused, the question stays; resumed, it asks again, and the answer starts a
        // new run that is told it was paused.
        let id = agent.start("ask me which plumber").await.unwrap();
        until(&agent, &id, State::Ask).await;
        agent.pause(&id).unwrap();
        let view = agent.view(&id).unwrap();
        assert_eq!(view.state, State::Paused);
        assert!(view.ask.is_some());
        assert_eq!(agent.list(false).counts.ask, 0);
        agent.resume(&id).unwrap();
        let view = until(&agent, &id, State::Ask).await;
        agent.answer(&id, &view.ask.unwrap().id, 1).await.unwrap();
        let view = until(&agent, &id, State::Done).await;
        let result = view.result.unwrap();
        assert!(
            result.contains("paused this task") && result.contains("Northgate"),
            "{result}"
        );

        // Working: resumed, it starts again, told so.
        let id = agent.start("a slow job").await.unwrap();
        until(&agent, &id, State::Working).await;
        agent.pause(&id).unwrap();
        assert_eq!(agent.view(&id).unwrap().state, State::Paused);
        agent.resume(&id).unwrap();
        let view = until(&agent, &id, State::Done).await;
        assert!(view.result.unwrap().contains("paused this task"));

        // Finished: nothing to pause or resume.
        assert_eq!(agent.pause(&id), Err(Refusal::NotRunning));
        assert!(agent.resume(&id).is_err());

        // Words for a paused session pick it up.
        let id = agent.start("another slow job").await.unwrap();
        until(&agent, &id, State::Working).await;
        agent.pause(&id).unwrap();
        agent.say(&id, "carry on please").await.unwrap();
        let view = until(&agent, &id, State::Done).await;
        assert!(view.result.unwrap().contains("carry on please"));
    }

    #[tokio::test]
    async fn a_schedule_runs_records_each_run_and_waits_again() {
        let dir = crate::skills::tests::scratch("agent-schedule");
        let agent = mock(&dir);
        let id = agent.start("schedule a morning brief").await.unwrap();
        let view = until(&agent, &id, State::Scheduled).await;
        let sched = view.schedule.clone().unwrap();
        assert_eq!(
            (sched.at.as_str(), sched.days.as_str()),
            ("07:00", "MTWTF..")
        );
        assert!(sched.wake_board && sched.runs.is_empty() && sched.next > store::now());
        assert_eq!(view.result, None);
        assert!(logged(&view, "Scheduled weekdays at 07:00"));
        let list = agent.list(false);
        assert_eq!((list.counts.planned, list.next_at), (1, Some(sched.next)));
        // It wakes the board once the run should be done: a minute after it starts.
        assert_eq!(list.check_at, Some(sched.next + 60));
        assert_eq!(list.sessions[0].next_at, Some(sched.next));

        // A run: fresh (no Claude Code session to resume), recorded, its result kept.
        agent
            .sessions
            .lock()
            .unwrap()
            .get_mut(&id)
            .unwrap()
            .session
            .claude = Some("old".into());
        agent.run_now(&id).unwrap();
        assert_eq!(agent.sessions.lock().unwrap()[&id].session.claude, None);
        let view = until(&agent, &id, State::Scheduled).await;
        let runs = &view.schedule.as_ref().unwrap().runs;
        assert_eq!((runs.len(), runs[0].state), (1, State::Done));
        let result = view.result.clone().unwrap();
        assert!(result.starts_with("**Brief:**"), "{result}");
        let list = agent.list(false);
        assert_eq!(list.latest.done, view.updated);
        assert!(list.sessions[0].unread);

        // Changed by voice: a leg, not a run; the result stays.
        agent.say(&id, "make it eight").await.unwrap();
        let view = until(&agent, &id, State::Scheduled).await;
        assert_eq!(view.result.as_deref(), Some(result.as_str()));
        assert_eq!(view.schedule.as_ref().unwrap().runs.len(), 1);
        assert!(logged(&view, "You said: make it eight"));

        // Its time comes: the scheduler runs it.
        let next = view.schedule.as_ref().unwrap().next;
        agent.tick(next);
        let view = until(&agent, &id, State::Scheduled).await;
        assert_eq!(view.schedule.as_ref().unwrap().runs.len(), 2);

        // A run missed by more than an hour (the daemon was down) is skipped.
        let next = view.schedule.as_ref().unwrap().next;
        agent.tick(next + 2 * 3600);
        let view = agent.view(&id).unwrap();
        assert_eq!(view.state, State::Scheduled);
        assert_eq!(view.schedule.as_ref().unwrap().runs.len(), 2);
        assert!(logged(&view, "missed the run"));

        // Stopped, it runs no more.
        agent.stop(&id).await.unwrap();
        assert_eq!(agent.view(&id).unwrap().state, State::Stopped);
        assert!(agent.run_now(&id).is_err());
    }

    #[tokio::test]
    async fn a_failed_scheduled_run_keeps_the_schedule() {
        let dir = crate::skills::tests::scratch("agent-schedule-fail");
        let agent = mock(&dir);
        let id = agent
            .start("schedule a brief that will fail")
            .await
            .unwrap();
        until(&agent, &id, State::Scheduled).await;
        agent.run_now(&id).unwrap();
        let view = until(&agent, &id, State::Scheduled).await;
        let runs = &view.schedule.as_ref().unwrap().runs;
        assert_eq!((runs.len(), runs[0].state), (1, State::Fail));
        assert!(view.result.unwrap().contains("failed"));
        // Paused, it waits; resumed, its next run is in the future again.
        agent.pause(&id).unwrap();
        assert_eq!(agent.view(&id).unwrap().state, State::Paused);
        agent.tick(store::now() + 30 * 86_400);
        assert_eq!(agent.view(&id).unwrap().state, State::Paused);
        agent.resume(&id).unwrap();
        let view = agent.view(&id).unwrap();
        assert_eq!(view.state, State::Scheduled);
        assert!(view.schedule.unwrap().next > store::now());
    }

    #[tokio::test]
    async fn a_watch_looks_until_its_condition_holds() {
        let dir = crate::skills::tests::scratch("agent-watch");
        let agent = mock(&dir);
        let id = agent.start("watch the fare to Lisbon").await.unwrap();
        let view = until(&agent, &id, State::Watch).await;
        let watch = view.watch.clone().unwrap();
        assert_eq!(watch.every_minutes, 5);
        let next = watch.next.unwrap();
        assert!(next > store::now() + 250);
        assert_eq!(agent.list(false).check_at, Some(next));

        // A look: a reading, and it waits again.
        agent.tick(next);
        let view = until(&agent, &id, State::Watch).await;
        assert_eq!(view.watch.as_ref().unwrap().values, [712.0]);
        assert_eq!(agent.list(false).sessions[0].spark, [712.0]);
        assert!(logged(&view, "Looked: $712."));
        assert!(!agent.list(false).sessions[0].unread);

        // The condition holds: done, and the board is told.
        let id = agent.start("watch until DROPPED").await.unwrap();
        until(&agent, &id, State::Watch).await;
        agent.run_now(&id).unwrap();
        let view = until(&agent, &id, State::Done).await;
        assert!(view.watch.as_ref().unwrap().met);
        assert_eq!(agent.list(false).latest.done, view.updated);
    }

    #[tokio::test]
    async fn a_question_past_its_deadline() {
        let dir = crate::skills::tests::scratch("agent-deadline");
        let agent = mock(&dir);
        let later = store::now() + 120;

        // The recommendation, by default.
        let id = agent.start("a deadline").await.unwrap();
        let view = until(&agent, &id, State::Ask).await;
        let deadline = view.ask.unwrap().deadline.unwrap();
        assert_eq!(deadline.then, OnDeadline::Recommend);
        agent.tick(later);
        let view = until(&agent, &id, State::Done).await;
        assert!(view.result.unwrap().contains("Riverside"));

        // Giving up.
        let id = agent.start("a deadline, then give up").await.unwrap();
        until(&agent, &id, State::Ask).await;
        agent.tick(later);
        let view = until(&agent, &id, State::Done).await;
        assert!(view.result.unwrap().contains("Nobody answered in time"));

        // Asking again: the same question, asked anew, with a new deadline.
        let id = agent.start("a deadline, ask again").await.unwrap();
        until(&agent, &id, State::Ask).await;
        let mut events = agent.subscribe();
        agent.tick(later);
        let ask = agent.view(&id).unwrap().ask.unwrap();
        assert_eq!((ask.at, ask.deadline.unwrap().at), (later, later + 60));
        let event = events.recv().await.unwrap();
        assert!(event.asked && event.session == id);
        assert_eq!(agent.list(true).latest.ask, later);

        // Paused, nothing happens at the deadline.
        agent.pause(&id).unwrap();
        agent.tick(later + 3600);
        assert_eq!(agent.view(&id).unwrap().state, State::Paused);
    }

    #[tokio::test]
    async fn jobs_filed_by_talk_wait_to_be_started() {
        let dir = crate::skills::tests::scratch("agent-jobs");
        let agent = mock(&dir);
        let id = agent.propose("compare three plumbers", Some("Plumbers"));
        let view = agent.view(&id).unwrap();
        assert_eq!(
            (view.state, view.title.as_str()),
            (State::Proposed, "Plumbers")
        );
        let list = agent.list(false);
        assert_eq!(
            (list.counts.proposed, list.sessions[0].id.as_str()),
            (1, id.as_str())
        );
        assert_eq!(list.sessions[0].sub, "compare three plumbers");
        // Nothing runs it until it's started.
        tokio::time::sleep(Duration::from_millis(50)).await;
        assert_eq!(agent.view(&id).unwrap().state, State::Proposed);
        assert_eq!(agent.approve(&id, None).await.unwrap(), id);
        let view = until(&agent, &id, State::Done).await;
        assert!(view.result.unwrap().contains("compare three plumbers"));
        assert!(agent.approve(&id, None).await.is_err());

        // Said into, it starts with the words.
        let id = agent.propose("find a plumber", None);
        agent.say(&id, "for Saturday").await.unwrap();
        until(&agent, &id, State::Done).await;

        // Talk's tool files one; [talk] tools can leave it out.
        let tools = tools().with_extra(Arc::new(ProposeTool {
            agent: agent.clone(),
        }));
        let names = |t: &ToolSet| t.defs().iter().map(|d| d.name.clone()).collect::<Vec<_>>();
        assert!(names(&tools).contains(&PROPOSE_TOOL.to_string()));
        let narrowed = tools.only(&["run_js".into()]);
        assert!(!names(&narrowed).contains(&PROPOSE_TOOL.to_string()));
        assert!(
            narrowed
                .call(PROPOSE_TOOL, serde_json::json!({"prompt": "x"}))
                .await
                .is_err()
        );
        let before = agent.list(false).counts.proposed;
        tools
            .call(PROPOSE_TOOL, serde_json::json!({"prompt": "book a table"}))
            .await
            .unwrap();
        assert_eq!(agent.list(false).counts.proposed, before + 1);
    }

    #[tokio::test]
    async fn a_session_says_when_it_expects_to_be_done() {
        let dir = crate::skills::tests::scratch("agent-expect");
        let agent = mock(&dir);
        let id = agent
            .start("ask, and expect it to take a while")
            .await
            .unwrap();
        until(&agent, &id, State::Ask).await;
        let at = agent.sessions.lock().unwrap()[&id]
            .session
            .expect_at
            .unwrap();
        assert!(at > store::now() + 25 * 60);
        // Only while it works.
        assert_eq!(agent.list(false).check_at, None);
        let ask = agent.view(&id).unwrap().ask.unwrap();
        agent.answer(&id, &ask.id, 0).await.unwrap();
        until(&agent, &id, State::Done).await;
        assert_eq!(agent.list(false).check_at, None);
    }

    #[tokio::test]
    async fn a_session_draws_a_panel_and_hears_its_choice() {
        let dir = crate::skills::tests::scratch("agent-panel");
        let agent = mock(&dir);
        let id = agent.start("show a panel").await.unwrap();
        let view = until(&agent, &id, State::Done).await;
        let panel = view.panel.unwrap();
        assert_eq!(
            (panel.title.as_str(), panel.blocks.len()),
            ("Lisbon fare", 8)
        );
        assert!(logged(
            &agent.view(&id).unwrap(),
            "showed the panel Lisbon fare"
        ));
        assert_eq!(
            agent.panel_choice(&id, "book", 9).await,
            Err(Refusal::NoSuchChoice)
        );
        assert_eq!(
            agent.panel_choice(&id, "nope", 0).await,
            Err(Refusal::NoSuchChoice)
        );
        agent.panel_choice(&id, "book", 1).await.unwrap();
        let view = until(&agent, &id, State::Done).await;
        let result = view.result.unwrap();
        assert!(
            result.contains("Book it now?") && result.contains("Keep watching"),
            "{result}"
        );
        // What a board says aloud: the result as plain sentences.
        let said = agent.result_to_say(&id).unwrap();
        assert!(!said.contains("**") && !said.contains('\n'), "{said}");
    }
}
