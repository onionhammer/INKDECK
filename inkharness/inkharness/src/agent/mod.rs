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

mod lock;
mod mock;
pub mod store;
mod tools;

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, Weak};
use std::time::Duration;

use anyhow::{Context, Result};
use tokio::sync::{broadcast, mpsc, oneshot};
use tokio::task::AbortHandle;
use voice_proto::{AgentHello, AskView, SessionList, SessionView, State};

use crate::config;
use crate::llm::{self, Llm, Piece, Role, TooManyRounds, Turn};
use crate::meter;
use crate::tools::ToolSet;
pub use lock::Unlock;
#[cfg(test)]
pub use lock::sha256_hex;
use store::{Question, Session, Who};
pub use tools::Answer;
use tools::SessionTools;

/// How often finished sessions past `keep_days` are looked for.
const PRUNE_EVERY: Duration = Duration::from_secs(3600);

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
}

/// A session and what is working on it.
struct Live {
    session: Session,
    run: Option<AbortHandle>,
    /// The ask_choice call waiting for its answer, when the run that asked is still going.
    waiting: Option<oneshot::Sender<Answer>>,
}

/// A session changed (or went): what a board connection that follows the list listens for.
#[derive(Debug, Clone)]
pub struct Event {
    pub session: String,
    /// It has just asked a question.
    pub asked: bool,
}

/// Why a request about a session can't be done; the message a board or HTTP gets.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Refusal {
    NoSuchSession,
    AlreadyAnswered,
    NoSuchChoice,
    NotRunning,
}

impl std::fmt::Display for Refusal {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Refusal::NoSuchSession => "no such session",
            Refusal::AlreadyAnswered => "already answered",
            Refusal::NoSuchChoice => "no such choice",
            Refusal::NotRunning => "not running",
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
}

/// The session model for `cfg`: the mock plays a whole session (mock.rs).
pub fn model(cfg: &config::Llm, http: reqwest::Client) -> Result<Box<dyn Llm>> {
    Ok(match cfg {
        config::Llm::Mock => Box::new(mock::MockAgent),
        other => llm::build(other, http)?,
    })
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
    }
}

/// What a leg wrote: all of it (for the conversation), and its last stretch of text (the result).
struct Written {
    all: String,
    result: String,
}

impl Agent {
    /// Opens the sessions in `[agent] dir`: deletes the ones past `keep_days`, starts the
    /// working ones again, and leaves the asking ones waiting.
    pub fn open(
        cfg: &config::Agent,
        llm: Box<dyn Llm>,
        tools: ToolSet,
        system: String,
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
            let live = Live {
                session,
                run: None,
                waiting: None,
            };
            sessions.insert(live.session.id.clone(), live);
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
        });

        let mut resumed = 0;
        {
            let mut sessions = agent.sessions.lock().unwrap();
            for live in sessions.values_mut() {
                if live.session.state == State::Working {
                    let note = resume_note(&live.session, None);
                    live.session.inbox.push(note);
                    agent.save(&live.session);
                    agent.spawn_run(live);
                    resumed += 1;
                }
            }
            tracing::info!(
                dir = %agent.dir.display(),
                place = %agent.place,
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
        Ok(agent)
    }

    pub fn hello(&self, unlocked: bool) -> AgentHello {
        AgentHello {
            place: self.place.clone(),
            pin: self.lock.has_pin(),
            unlocked,
        }
    }

    pub fn has_pin(&self) -> bool {
        self.lock.has_pin()
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
        store::list(sessions.values().map(|l| &l.session), &self.place, locked)
    }

    pub fn view(&self, id: &str) -> Option<SessionView> {
        let sessions = self.sessions.lock().unwrap();
        sessions.get(id).map(|l| l.session.view(&self.place))
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

    /// Starts a session for `text`; its id.
    pub fn start(self: &Arc<Self>, text: &str) -> String {
        let mut sessions = self.sessions.lock().unwrap();
        let id = store::new_id(|id| sessions.contains_key(id));
        let session = Session::new(id.clone(), text, store::now());
        tracing::info!(session = %id, title = %session.title, "a new session");
        self.save(&session);
        let mut live = Live {
            session,
            run: None,
            waiting: None,
        };
        self.spawn_run(&mut live);
        sessions.insert(id.clone(), live);
        self.notify(&id, false);
        id
    }

    /// Talks into a session: answers its question in words, joins its work, or starts it again
    /// if it had finished.
    pub fn say(self: &Arc<Self>, id: &str, text: &str) -> Result<Said, Refusal> {
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
                if live.run.is_none() {
                    self.spawn_run(live);
                }
                Said::Added {
                    title: live.session.title.clone(),
                    now: live.session.sub(),
                }
            }
            _ => {
                let said = Said::Resumed {
                    title: live.session.title.clone(),
                    result: live.session.result.clone(),
                };
                let s = &mut live.session;
                s.inbox.push(text.to_string());
                s.add_log(&format!("you: {text}"), now);
                s.state = State::Working;
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

    /// Answers a session's question `ask` with its option `choice`.
    pub fn answer(self: &Arc<Self>, id: &str, ask: &str, choice: usize) -> Result<(), Refusal> {
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
            let note = resume_note(&live.session, Some((&question, &answer)));
            live.session.inbox.push(note);
            if live.run.is_none() {
                self.spawn_run(live);
            }
        }
    }

    /// Stops a working or asking session.
    pub fn stop(&self, id: &str) -> Result<(), Refusal> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = sessions.get_mut(id).ok_or(Refusal::NoSuchSession)?;
        if !matches!(live.session.state, State::Working | State::Ask) {
            return Err(Refusal::NotRunning);
        }
        if let Some(run) = live.run.take() {
            run.abort();
        }
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

    /// Deletes a session for good, stopping it first.
    pub fn delete(&self, id: &str) -> Result<(), Refusal> {
        let live = self
            .sessions
            .lock()
            .unwrap()
            .remove(id)
            .ok_or(Refusal::NoSuchSession)?;
        if let Some(run) = live.run {
            run.abort();
        }
        if let Err(e) = store::remove(&self.dir, id) {
            tracing::warn!(session = %id, "couldn't delete its file: {e}");
        }
        tracing::info!(session = %id, "deleted");
        self.notify(id, false);
        Ok(())
    }

    /// Deletes the sessions that finished more than `keep_days` ago.
    fn prune(&self, now: u64) {
        let mut sessions = self.sessions.lock().unwrap();
        let old: Vec<String> = sessions
            .values()
            .filter(|l| l.run.is_none() && store::expired(&l.session, self.keep_days, now))
            .map(|l| l.session.id.clone())
            .collect();
        for id in old {
            sessions.remove(&id);
            let _ = store::remove(&self.dir, &id);
            self.notify(&id, false);
        }
    }

    // What the runs and their tools do.

    fn spawn_run(self: &Arc<Self>, live: &mut Live) {
        let (agent, id) = (self.clone(), live.session.id.clone());
        live.run = Some(tokio::spawn(agent.run(id)).abort_handle());
    }

    async fn run(self: Arc<Self>, id: String) {
        loop {
            let Some((history, user)) = self.begin_leg(&id) else {
                return;
            };
            let tools = self.tools.with_extra(Arc::new(SessionTools {
                agent: self.clone(),
                id: id.clone(),
            }));
            let steer = {
                let (agent, id) = (self.clone(), id.clone());
                move || agent.hear(&id)
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
            let leg = async { tokio::join!(self.llm.respond(request, tx), self.follow(&id, rx)) };
            let ((outcome, written), charges) = meter::measure(leg).await;
            let (cost, unpriced) = meter::total(&charges);
            tracing::info!(
                session = %id,
                ok = outcome.is_ok(),
                cost,
                unpriced,
                requests = %meter::ids(&charges),
                "session leg"
            );
            let again = match outcome {
                Ok(_) => self.end_leg(&id, written),
                Err(e) => {
                    self.fail(&id, &e);
                    false
                }
            };
            if !again {
                return;
            }
        }
    }

    /// What the next leg hears: the conversation so far, and the user's words since its last
    /// answer (what was waiting moves into the conversation). None if the session has gone or
    /// stopped.
    fn begin_leg(&self, id: &str) -> Option<(Vec<Turn>, String)> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = sessions
            .get_mut(id)
            .filter(|l| l.session.state == State::Working)?;
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
    fn hear(&self, id: &str) -> Vec<String> {
        let mut sessions = self.sessions.lock().unwrap();
        let Some(live) = sessions.get_mut(id) else {
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
    async fn follow(&self, id: &str, mut rx: mpsc::Receiver<Piece>) -> Written {
        let (mut all, mut stretch, mut last) = (String::new(), String::new(), String::new());
        while let Some(piece) = rx.recv().await {
            match piece {
                Piece::Text(text) | Piece::Transcript(text) => {
                    all.push_str(&text);
                    stretch.push_str(&text);
                }
                Piece::Tool(name, input) => {
                    if !stretch.trim().is_empty() {
                        self.log(id, &store::plain(&stretch));
                        last = std::mem::take(&mut stretch);
                    }
                    stretch.clear();
                    // The plan and the question show for themselves.
                    if name != tools::ASK_TOOL && name != tools::PLAN_TOOL {
                        self.log(id, &tools::short_call(&name, &input));
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
    fn end_leg(&self, id: &str, written: Written) -> bool {
        let mut sessions = self.sessions.lock().unwrap();
        let Some(live) = sessions.get_mut(id) else {
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

    fn fail(&self, id: &str, error: &anyhow::Error) {
        let mut sessions = self.sessions.lock().unwrap();
        let Some(live) = sessions.get_mut(id) else {
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
        mut question: Question,
    ) -> Result<oneshot::Receiver<Answer>, String> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = sessions.get_mut(id).ok_or("the session has gone")?;
        let s = &mut live.session;
        if s.ask.is_some() {
            return Err("a question is already waiting for its answer: ask one at a time".into());
        }
        let now = store::now();
        s.asked += 1;
        question.id = format!("a{}", s.asked);
        question.at = now;
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
        steps: Vec<String>,
        current: usize,
        title: Option<String>,
    ) -> Result<(), String> {
        let mut sessions = self.sessions.lock().unwrap();
        let live = sessions.get_mut(id).ok_or("the session has gone")?;
        let s = &mut live.session;
        s.plan = Some(store::Plan { steps, current });
        if let Some(title) = title {
            s.title = title;
        }
        s.updated = store::now();
        self.save(s);
        self.notify(id, false);
        Ok(())
    }

    /// Adds a line to the session's log.
    fn log(&self, id: &str, text: &str) {
        let mut sessions = self.sessions.lock().unwrap();
        let Some(live) = sessions.get_mut(id) else {
            return;
        };
        let now = store::now();
        live.session.add_log(text, now);
        live.session.updated = now;
        self.save(&live.session);
        self.notify(id, false);
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

/// What a new run is told when the daemon restarted under the session: that the tool results
/// are gone, its plan and what it did, and the answer to its question if that is what it was
/// waiting for.
fn resume_note(s: &Session, answered: Option<(&Question, &Answer)>) -> String {
    let mut note = format!(
        "(inkharness restarted while this task {}, so the results of the tool calls before \
         that are gone.",
        if answered.is_some() {
            "waited for an answer"
        } else {
            "was being worked on"
        }
    );
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

    pub fn cfg(dir: &Path) -> config::Agent {
        config::Agent {
            dir: dir.display().to_string(),
            place: Some("HOME".into()),
            pin_sha256: None,
            ticket_minutes: 60,
            keep_days: 30,
            max_rounds: 40,
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
        Agent::open(&cfg(dir), Box::new(mock::MockAgent), tools(), prompt()).unwrap()
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
        let id = agent.start("find a plumber and ask me which");
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

        assert_eq!(agent.answer(&id, "a1", 2), Err(Refusal::NoSuchChoice));
        assert_eq!(agent.answer(&id, "a2", 0), Err(Refusal::AlreadyAnswered));
        assert_eq!(agent.answer("nope", "a1", 0), Err(Refusal::NoSuchSession));
        agent.answer(&id, "a1", 1).unwrap();
        assert_eq!(agent.answer(&id, "a1", 1), Err(Refusal::AlreadyAnswered));
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
        let id = agent.start("ask me which plumber");
        until(&agent, &id, State::Ask).await;
        let said = agent.say(&id, "none, find one for Saturday").unwrap();
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
        let asking = agent.start("ask me which plumber");
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
        agent.answer(&asking, "a1", 0).unwrap();
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
        let agent =
            Agent::open(&cfg(&dir), Box::new(Gated(gate.clone())), tools(), prompt()).unwrap();
        let id = agent.start("find a plumber");
        tokio::task::yield_now().await;
        let said = agent.say(&id, "make it Saturday").unwrap();
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
        let said = agent.say(&id, "and the price?").unwrap();
        assert_eq!(
            said,
            Said::Resumed {
                title: "Find a plumber".into(),
                result: Some("heard: make it Saturday".into())
            }
        );
        let view = until(&agent, &id, State::Done).await;
        assert_eq!(view.result.as_deref(), Some("heard: and the price?"));
        let sessions = agent.sessions.lock().unwrap();
        let said: Vec<&str> = sessions[&id]
            .session
            .conversation
            .iter()
            .map(|s| s.text.as_str())
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
        drop(sessions);
        assert_eq!(agent.say("nope", "x"), Err(Refusal::NoSuchSession));
    }

    #[tokio::test]
    async fn stop_and_delete() {
        let dir = crate::skills::tests::scratch("agent-stop");
        let gate = Arc::new(Semaphore::new(0));
        let agent =
            Agent::open(&cfg(&dir), Box::new(Gated(gate.clone())), tools(), prompt()).unwrap();
        let id = agent.start("find a plumber");
        tokio::task::yield_now().await;
        agent.stop(&id).unwrap();
        assert_eq!(agent.stop(&id), Err(Refusal::NotRunning));
        gate.add_permits(1);
        tokio::time::sleep(Duration::from_millis(50)).await;
        let view = agent.view(&id).unwrap();
        assert_eq!((view.state, &view.result), (State::Stopped, &None));
        assert_eq!(log(&view), ["stopped"]);
        assert!(dir.join(format!("{id}.json")).exists());
        agent.delete(&id).unwrap();
        assert!(!dir.join(format!("{id}.json")).exists());
        assert_eq!(agent.delete(&id), Err(Refusal::NoSuchSession));
        assert!(agent.view(&id).is_none() && agent.list(false).sessions.is_empty());
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
    async fn too_many_steps_fail() {
        let dir = crate::skills::tests::scratch("agent-endless");
        let agent = Agent::open(&cfg(&dir), Box::new(Endless), tools(), prompt()).unwrap();
        let id = agent.start("count forever");
        let view = until(&agent, &id, State::Fail).await;
        assert_eq!(
            view.result.as_deref(),
            Some("too many steps: it stopped after 40 model rounds")
        );
        let id = agent.start("a note about fail");
        let view = until(&agent, &id, State::Fail).await;
        assert!(view.result.unwrap().starts_with("too many steps"));
    }

    #[tokio::test]
    async fn notes_and_failures_from_the_mock() {
        let dir = crate::skills::tests::scratch("agent-states");
        let agent = mock(&dir);
        let note = agent.start("a note on today");
        let fail = agent.start("this one should fail");
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
}
