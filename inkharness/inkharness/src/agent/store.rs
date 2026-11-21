//! A session as it is kept on disk (one JSON file each in `[agent] dir`), and what a board sees
//! of it: a row of the list (`sessions`) and the whole of it (`session`), with the limits the
//! protocol sets on each field (docs/voice-protocol.md, "Agent sessions").

use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use voice_proto::{
    AskOption, AskView, Backend, Cost, Counts, Deadline, Latest, LogLine, OnDeadline, Panel,
    PlanItem, RunView, ScheduleView, SessionList, SessionRow, SessionView, State, StepState, Via,
    WatchView,
};

/// Sessions listed; the rest are counted in `older`.
pub const LISTED: usize = 40;
pub const TITLE_CHARS: usize = 40;
pub const SUB_CHARS: usize = 80;
pub const LOG_CHARS: usize = 120;
/// Log lines a `session` carries.
pub const LOG_SHOWN: usize = 20;
/// Log lines kept on disk.
const LOG_KEPT: usize = 100;
pub const RESULT_BYTES: usize = 8 * 1024;
/// The prompt as a `session` shows it; the session keeps all of it.
const PROMPT_CHARS: usize = 2000;
/// Messages of the conversation kept for the next run, besides the first.
const CONVERSATION_KEPT: usize = 40;
/// A schedule's runs kept, and a watch's readings.
pub const RUNS_KEPT: usize = 10;
pub const VALUES_KEPT: usize = 24;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Session {
    pub id: String,
    pub title: String,
    pub state: State,
    /// What started it, in the user's words.
    pub prompt: String,
    pub started: u64,
    pub updated: u64,
    #[serde(default)]
    pub plan: Option<Plan>,
    /// Oldest first.
    #[serde(default)]
    pub log: Vec<LogLine>,
    #[serde(default)]
    pub result: Option<String>,
    /// The question waiting for an answer.
    #[serde(default)]
    pub ask: Option<Question>,
    /// Finished or asking, and not opened since.
    #[serde(default)]
    pub unread: bool,
    /// What was said, without the tool calls behind it: the history the next run starts from.
    #[serde(default)]
    pub conversation: Vec<Said>,
    /// What the user said that the run hasn't heard yet.
    #[serde(default)]
    pub inbox: Vec<String>,
    /// Questions asked so far: the next one's id is `a<asked + 1>`.
    #[serde(default)]
    pub asked: u32,
    /// What it runs on.
    #[serde(default)]
    pub backend: Backend,
    /// What runs it, lowercase ("claude code", "codex"); None for the api backend.
    #[serde(default)]
    pub program: Option<String>,
    /// Where it runs, when not where the daemon is (a herdr machine).
    #[serde(default)]
    pub place: Option<String>,
    /// The progress (0–100) its program last reported.
    #[serde(default)]
    pub progress: Option<u8>,
    /// The claude backend: Claude Code's own session id, to resume it with.
    #[serde(default)]
    pub claude: Option<String>,
    /// The herdr backend: its terminal, and what the pending question's choices send.
    #[serde(default)]
    pub herdr: Option<HerdrSide>,
    /// Paused: the state it was in, to go back to.
    #[serde(default)]
    pub paused_from: Option<State>,
    /// When it expects to be done, as the model said (`plan`'s expect_minutes).
    #[serde(default)]
    pub expect_at: Option<u64>,
    /// What its model requests have cost, in US dollars, and whether that is notional (a
    /// subscription).
    #[serde(default)]
    pub cost: f64,
    #[serde(default)]
    pub cost_notional: bool,
    /// A schedule: when it runs, and its last runs.
    #[serde(default)]
    pub schedule: Option<Schedule>,
    /// A watch: how often it looks, for what, and what it saw.
    #[serde(default)]
    pub watch: Option<Watch>,
    /// The screen it drew last (`show_panel`).
    #[serde(default)]
    pub panel: Option<Panel>,
}

/// A schedule (the `schedule` tool): a session that runs its prompt at a time of day.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Schedule {
    /// The local time, "07:00".
    pub at: String,
    /// The days it runs, Monday in bit 0.
    pub days: u8,
    /// What each run does.
    pub prompt: String,
    #[serde(default)]
    pub wake_board: bool,
    #[serde(default)]
    pub show_note: bool,
    #[serde(default)]
    pub speak: bool,
    /// The next run (Unix seconds).
    pub next_run: u64,
    /// The last runs, oldest first.
    #[serde(default)]
    pub runs: Vec<Run>,
    /// A run is under way, since then: its end is recorded as a run.
    #[serde(default)]
    pub running_since: Option<u64>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Run {
    pub at: u64,
    pub state: State,
    pub secs: u64,
}

/// A watch (the `watch` tool): a session that looks every so often until a condition holds.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Watch {
    pub every_minutes: u64,
    pub condition: String,
    /// What to do at each look.
    pub prompt: String,
    pub next_look: u64,
    /// Readings, oldest first.
    #[serde(default)]
    pub values: Vec<f64>,
    /// A look is under way, since then.
    #[serde(default)]
    pub looking_since: Option<u64>,
    /// The condition held.
    #[serde(default)]
    pub met: bool,
}

/// What a herdr session keeps of its terminal.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct HerdrSide {
    /// `t7` here, `desktop/t7` on another machine.
    pub address: String,
    /// The terminal's turn when its question was put: an answer after it moved on is stale.
    #[serde(default)]
    pub turn: u64,
    /// What blocked it: permission, question or auth (the program's word).
    #[serde(default)]
    pub kind: Option<String>,
    /// For each of the pending question's options, the keys it presses.
    #[serde(default)]
    pub keys: Vec<Vec<String>>,
    /// Whether its program reports a status (a plain shell doesn't).
    #[serde(default)]
    pub reports: bool,
}

/// The checklist the model keeps with the `plan` tool.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Plan {
    pub steps: Vec<String>,
    /// The step being worked on, from 1; past the last one, all are done.
    pub current: usize,
}

/// A question from `ask_choice`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Question {
    pub id: String,
    pub question: String,
    pub detail: Option<String>,
    pub options: Vec<AskOption>,
    pub recommend: Option<usize>,
    pub at: u64,
    /// What happens if nobody answers in time.
    #[serde(default)]
    pub deadline: Option<QuestionDeadline>,
}

/// A question's deadline, and how long it was (asking again waits as long again).
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct QuestionDeadline {
    pub at: u64,
    pub minutes: u64,
    pub then: OnDeadline,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Said {
    pub role: Who,
    pub text: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Who {
    User,
    Assistant,
}

/// Now, in Unix seconds.
pub fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_secs())
}

impl Session {
    /// A session for `prompt`, working, with a title made from it until the model gives one.
    pub fn new(id: String, prompt: &str, now: u64) -> Session {
        Session {
            id,
            title: title_from(prompt),
            state: State::Working,
            prompt: prompt.trim().to_string(),
            started: now,
            updated: now,
            plan: None,
            log: Vec::new(),
            result: None,
            ask: None,
            unread: false,
            conversation: Vec::new(),
            inbox: vec![prompt.trim().to_string()],
            asked: 0,
            backend: Backend::Api,
            program: None,
            place: None,
            progress: None,
            claude: None,
            herdr: None,
            paused_from: None,
            expect_at: None,
            cost: 0.0,
            cost_notional: false,
            schedule: None,
            watch: None,
            panel: None,
        }
    }

    pub fn add_log(&mut self, text: &str, now: u64) {
        let text = one_line(text, LOG_CHARS);
        if text.is_empty() {
            return;
        }
        self.log.push(LogLine { at: now, text });
        let extra = self.log.len().saturating_sub(LOG_KEPT);
        self.log.drain(..extra);
    }

    /// Adds to the conversation, keeping the first message (the task) and the last
    /// CONVERSATION_KEPT.
    pub fn add_said(&mut self, role: Who, text: &str) {
        let text = text.trim();
        if text.is_empty() {
            return;
        }
        self.conversation.push(Said {
            role,
            text: text.to_string(),
        });
        let extra = self
            .conversation
            .len()
            .saturating_sub(CONVERSATION_KEPT + 1);
        self.conversation.drain(1..1 + extra);
    }

    /// The plan's current step (from 1, at most the number of steps) and its number of steps.
    pub fn step(&self) -> Option<(usize, usize)> {
        let plan = self.plan.as_ref().filter(|p| !p.steps.is_empty())?;
        Some((plan.current.clamp(1, plan.steps.len()), plan.steps.len()))
    }

    /// One line for the list: the question while asking, the step or the last log line while
    /// working, the result's first line when finished.
    pub fn sub(&self) -> String {
        let last_log = || self.log.last().map(|l| l.text.clone()).unwrap_or_default();
        let text = match self.state {
            State::Ask => self
                .ask
                .as_ref()
                .map(|q| q.question.clone())
                .unwrap_or_default(),
            State::Working => match &self.plan {
                Some(plan) if plan.current >= 1 && plan.current <= plan.steps.len() => {
                    plan.steps[plan.current - 1].clone()
                }
                _ => last_log(),
            },
            State::Idle => match self.log.last() {
                Some(line) => line.text.clone(),
                None => "Ready".into(),
            },
            State::Proposed => self.prompt.clone(),
            State::Paused => match &self.ask {
                Some(q) => q.question.clone(),
                None => last_log(),
            },
            State::Scheduled => match &self.result {
                Some(result) => plain(&first_line(result)),
                None => self
                    .schedule
                    .as_ref()
                    .map(|s| s.prompt.clone())
                    .unwrap_or_default(),
            },
            State::Watch => match &self.watch {
                Some(w) => w.condition.clone(),
                None => last_log(),
            },
            State::Done | State::Note | State::Fail | State::Stopped | State::Unknown => {
                match &self.result {
                    Some(result) => plain(&first_line(result)),
                    None => last_log(),
                }
            }
        };
        one_line(&text, SUB_CHARS)
    }

    /// When the board should next hear from it: a schedule's next run, a watch's next look.
    pub fn next_at(&self) -> Option<u64> {
        match self.state {
            State::Scheduled => self.schedule.as_ref().map(|s| s.next_run),
            State::Watch => self.watch.as_ref().map(|w| w.next_look),
            _ => None,
        }
    }

    /// When it expects to be done, while it works.
    pub fn expects(&self, now: u64) -> Option<u64> {
        self.expect_at
            .filter(|at| *at > now && self.state == State::Working)
    }

    /// Who runs it.
    pub fn via(&self) -> Via {
        match self.backend {
            Backend::Herdr => Via::Herdr,
            _ => Via::Inkharness,
        }
    }

    /// Where it runs: its own place (a herdr machine), else the daemon's.
    fn place_or<'a>(&'a self, place: &'a str) -> &'a str {
        self.place.as_deref().unwrap_or(place)
    }

    pub fn row(&self, place: &str, now: u64) -> SessionRow {
        let step = self.step();
        SessionRow {
            id: self.id.clone(),
            title: self.title.clone(),
            state: self.state,
            place: self.place_or(place).to_string(),
            via: self.via(),
            program: self.program.clone(),
            sub: self.sub(),
            step: step.map(|s| s.0),
            steps: step.map(|s| s.1),
            progress: self.progress,
            updated: self.updated,
            unread: self.unread,
            expect_at: self.expects(now),
            next_at: self.next_at(),
            spark: match (&self.watch, self.state) {
                (Some(w), State::Watch | State::Done | State::Paused) => w.values.clone(),
                _ => Vec::new(),
            },
        }
    }

    /// The session in full; `screen`: a herdr session's last lines of screen.
    pub fn view(&self, place: &str, screen: Option<Vec<String>>) -> SessionView {
        let step = self.step();
        let plan = self.plan.as_ref().filter(|p| !p.steps.is_empty()).map(|p| {
            let finished = matches!(self.state, State::Done | State::Note);
            p.steps
                .iter()
                .enumerate()
                .map(|(i, text)| PlanItem {
                    text: text.clone(),
                    state: match (i + 1).cmp(&p.current) {
                        _ if finished => StepState::Done,
                        std::cmp::Ordering::Less => StepState::Done,
                        std::cmp::Ordering::Equal => StepState::Now,
                        std::cmp::Ordering::Greater => StepState::Todo,
                    },
                })
                .collect()
        });
        SessionView {
            id: self.id.clone(),
            title: self.title.clone(),
            state: self.state,
            place: self.place_or(place).to_string(),
            via: self.via(),
            program: self.program.clone(),
            backend: self.backend,
            prompt: cut(&self.prompt, PROMPT_CHARS),
            started: self.started,
            updated: self.updated,
            step: step.map(|s| s.0),
            steps: step.map(|s| s.1),
            plan,
            log: self.log.iter().rev().take(LOG_SHOWN).cloned().collect(),
            result: self.result.clone(),
            ask: self.question(),
            screen,
            expect_at: self.expects(now()),
            cost: (self.cost > 0.0).then_some(Cost {
                usd: self.cost,
                notional: self.cost_notional,
            }),
            schedule: self.schedule.as_ref().map(|s| ScheduleView {
                at: s.at.clone(),
                days: super::schedule::days_text(s.days),
                next: s.next_run,
                prompt: cut(&s.prompt, PROMPT_CHARS),
                wake_board: s.wake_board,
                show_note: s.show_note,
                speak: s.speak,
                runs: s
                    .runs
                    .iter()
                    .rev()
                    .map(|r| RunView {
                        at: r.at,
                        state: r.state,
                        secs: r.secs,
                    })
                    .collect(),
            }),
            watch: self.watch.as_ref().map(|w| WatchView {
                condition: w.condition.clone(),
                every_minutes: w.every_minutes,
                next: (!w.met).then_some(w.next_look),
                values: w.values.clone(),
                met: w.met,
            }),
            panel: self.panel.clone(),
        }
    }

    /// The pending question as the board gets it.
    pub fn question(&self) -> Option<AskView> {
        let q = self.ask.as_ref()?;
        Some(AskView {
            session: self.id.clone(),
            id: q.id.clone(),
            title: self.title.clone(),
            question: q.question.clone(),
            detail: q.detail.clone(),
            options: q.options.clone(),
            recommend: q.recommend,
            at: q.at,
            deadline: q.deadline.map(|d| Deadline {
                at: d.at,
                then: d.then,
            }),
        })
    }

    /// When its last result came, if it is one the board hasn't seen (latest.done): a
    /// finished session's, or a schedule's last run. herdr sessions finish after every task,
    /// so they don't count.
    fn unread_result(&self) -> Option<u64> {
        if !self.unread || self.backend == Backend::Herdr {
            return None;
        }
        let ran = self.state == State::Scheduled
            && self.schedule.as_ref().is_some_and(|s| !s.runs.is_empty());
        (self.state.finished() || ran).then_some(self.updated)
    }
}

/// The list as a board gets it: asking and proposed first (the oldest question first), then
/// working (the most recently active first), idle and paused, then schedules and watches,
/// then the rest, newest first; LISTED of them, the rest counted in `older`. Locked, it is the
/// counts and times alone: no titles, no question.
pub fn list<'a>(
    sessions: impl Iterator<Item = &'a Session>,
    place: &str,
    locked: bool,
    now: u64,
) -> SessionList {
    let mut all: Vec<&Session> = sessions.collect();
    let mut counts = Counts::default();
    let mut latest = Latest::default();
    let (mut check_at, mut next_at): (Option<u64>, Option<u64>) = (None, None);
    let earliest = |a: Option<u64>, b: u64| Some(a.map_or(b, |a| a.min(b)));
    for s in &all {
        match s.state {
            State::Ask => counts.ask += 1,
            State::Working => counts.running += 1,
            State::Idle => counts.idle += 1,
            State::Scheduled | State::Watch => counts.planned += 1,
            State::Proposed => counts.proposed += 1,
            _ => {}
        }
        counts.unread += s.unread as usize;
        if s.state == State::Ask
            && let Some(q) = &s.ask
        {
            latest.ask = latest.ask.max(q.at);
        }
        if let Some(at) = s.unread_result() {
            latest.done = latest.done.max(at);
        }
        if let Some(at) = s.expects(now) {
            check_at = earliest(check_at, at);
        }
        match (s.state, &s.schedule, &s.watch) {
            (State::Scheduled, Some(sched), _) => {
                next_at = earliest(next_at, sched.next_run);
                if sched.wake_board {
                    // Done by then, give or take: as long as the last run took.
                    let took = sched.runs.last().map_or(0, |r| r.secs).max(60);
                    check_at = earliest(check_at, sched.next_run + took);
                }
            }
            (State::Watch, _, Some(watch)) if watch.next_look > now => {
                check_at = earliest(check_at, watch.next_look);
            }
            _ => {}
        }
    }
    let check_at = check_at.filter(|at| *at > now);
    if locked {
        return SessionList {
            locked: true,
            counts,
            check_at,
            next_at,
            latest,
            ask: None,
            sessions: Vec::new(),
            older: 0,
        };
    }
    let ask = all
        .iter()
        .filter(|s| s.state == State::Ask)
        .filter_map(|s| s.question())
        .max_by_key(|q| q.at);
    let group = |s: &Session| match s.state {
        State::Ask | State::Proposed => 0,
        State::Working => 1,
        State::Idle | State::Paused => 2,
        State::Scheduled | State::Watch => 3,
        State::Done | State::Note | State::Fail | State::Stopped | State::Unknown => 4,
    };
    all.sort_by(|a, b| {
        group(a).cmp(&group(b)).then_with(|| match a.state {
            State::Ask => {
                let at = |s: &Session| s.ask.as_ref().map_or(s.updated, |q| q.at);
                at(a).cmp(&at(b))
            }
            _ => b.updated.cmp(&a.updated),
        })
    });
    let older = all.len().saturating_sub(LISTED);
    SessionList {
        locked: false,
        counts,
        check_at,
        next_at,
        latest,
        ask,
        sessions: all.iter().take(LISTED).map(|s| s.row(place, now)).collect(),
        older,
    }
}

/// A title from the prompt: its first line, cut to TITLE_CHARS at a word.
pub fn title_from(prompt: &str) -> String {
    let line = first_line(prompt);
    let mut chars = line.chars();
    let title: String = match chars.next() {
        Some(c) => c.to_uppercase().chain(chars).collect(),
        None => "New session".into(),
    };
    one_line(title.trim_end_matches(['.', '?', '!']), TITLE_CHARS)
}

fn first_line(text: &str) -> String {
    text.lines()
        .map(str::trim)
        .find(|l| !l.is_empty())
        .unwrap_or_default()
        .trim_start_matches('#')
        .trim()
        .to_string()
}

/// A line of Markdown as plain text, for a board that shows it as it is: no heading, list or
/// quote marker, no emphasis or code marks, and links as their text.
pub fn plain(line: &str) -> String {
    let mut line = line.trim().trim_start_matches('#').trim_start();
    for marker in ["- ", "* ", "+ ", "> "] {
        line = line.strip_prefix(marker).unwrap_or(line);
    }
    if let Some((number, rest)) = line.split_once(". ")
        && !number.is_empty()
        && number.bytes().all(|b| b.is_ascii_digit())
    {
        line = rest;
    }
    let mut out = String::with_capacity(line.len());
    let mut rest = line;
    // [text](url) → text
    while let Some(open) = rest.find('[') {
        let link = rest[open..].find("](").and_then(|mid| {
            let close = rest[open + mid..].find(')')?;
            Some((open + mid, open + mid + close))
        });
        match link {
            Some((mid, close)) => {
                out.push_str(&rest[..open]);
                out.push_str(&rest[open + 1..mid]);
                rest = &rest[close + 1..];
            }
            None => {
                out.push_str(&rest[..=open]);
                rest = &rest[open + 1..];
            }
        }
    }
    out.push_str(rest);
    out.replace("**", "").replace("__", "").replace('`', "")
}

/// `text` on one line (runs of whitespace become one space), cut to `max` characters at a
/// word where it can be, with an ellipsis that counts towards `max`.
pub fn one_line(text: &str, max: usize) -> String {
    let joined = text.split_whitespace().collect::<Vec<_>>().join(" ");
    if joined.chars().count() <= max {
        return joined;
    }
    let keep: String = joined.chars().take(max.saturating_sub(1)).collect();
    let at_word = match keep.rfind(' ') {
        Some(i) if i > keep.len() / 2 => &keep[..i],
        _ => &keep,
    };
    format!("{}…", at_word.trim_end())
}

/// `text` cut to `max` characters, with an ellipsis (counted) if anything was cut.
pub fn cut(text: &str, max: usize) -> String {
    if text.chars().count() <= max {
        return text.to_string();
    }
    let keep: String = text.chars().take(max.saturating_sub(1)).collect();
    format!("{keep}…")
}

/// `text` cut to at most `max` bytes, at a character boundary, with an ellipsis if cut.
pub fn cut_bytes(text: &str, max: usize) -> String {
    if text.len() <= max {
        return text.to_string();
    }
    let mut end = max.saturating_sub('…'.len_utf8());
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    format!("{}…", &text[..end])
}

/// A new session id: four letters and digits not yet taken.
pub fn new_id(taken: impl Fn(&str) -> bool) -> String {
    const ALPHABET: &[u8] = b"abcdefghijkmnpqrstuvwxyz23456789";
    loop {
        let mut bytes = [0u8; 4];
        let _ = getrandom::fill(&mut bytes);
        let id: String = bytes
            .iter()
            .map(|b| ALPHABET[*b as usize % ALPHABET.len()] as char)
            .collect();
        if !taken(&id) {
            return id;
        }
    }
}

/// True for an id `new_id` could have made (and so a safe file name).
pub fn valid_id(id: &str) -> bool {
    (1..=16).contains(&id.len()) && id.bytes().all(|b| b.is_ascii_alphanumeric())
}

fn file(dir: &Path, id: &str) -> PathBuf {
    dir.join(format!("{id}.json"))
}

/// Writes the session's file whole: to a temporary file first, then renamed over the old one,
/// so a crash leaves one or the other.
pub fn save(dir: &Path, session: &Session) -> std::io::Result<()> {
    let json = serde_json::to_vec_pretty(session)?;
    let tmp = dir.join(format!(".{}.json.tmp", session.id));
    std::fs::write(&tmp, json)?;
    std::fs::rename(&tmp, file(dir, &session.id))
}

pub fn remove(dir: &Path, id: &str) -> std::io::Result<()> {
    match std::fs::remove_file(file(dir, id)) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(e),
        _ => Ok(()),
    }
}

/// Every session in `dir`. A file that doesn't read is logged and left alone.
pub fn load_all(dir: &Path) -> Vec<Session> {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut sessions = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        let Some(id) = path
            .file_name()
            .and_then(|n| n.to_str())
            .and_then(|n| n.strip_suffix(".json"))
        else {
            continue;
        };
        if !valid_id(id) {
            continue;
        }
        let read = std::fs::read(&path)
            .map_err(|e| e.to_string())
            .and_then(|bytes| serde_json::from_slice::<Session>(&bytes).map_err(|e| e.to_string()));
        match read {
            Ok(session) if session.id == id => sessions.push(session),
            Ok(_) => {
                tracing::warn!(file = %path.display(), "a session file under another id: left alone")
            }
            Err(e) => {
                tracing::warn!(file = %path.display(), "a session file that doesn't read: {e}")
            }
        }
    }
    sessions
}

/// True if the session finished (or was filed by talk and never started) more than
/// `keep_days` ago.
pub fn expired(session: &Session, keep_days: u64, now: u64) -> bool {
    (session.state.finished() || session.state == State::Proposed)
        && session.updated.saturating_add(keep_days * 86_400) < now
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    pub fn session(id: &str, state: State, updated: u64) -> Session {
        let mut s = Session::new(id.into(), &format!("task {id}"), updated);
        s.state = state;
        s.inbox.clear();
        s
    }

    #[test]
    fn the_list_puts_questions_first_then_work_then_the_newest() {
        let mut sessions = [
            session("done1", State::Done, 100),
            session("work1", State::Working, 50),
            session("done2", State::Fail, 300),
            session("work2", State::Working, 60),
            session("ask1", State::Ask, 400),
            session("ask2", State::Ask, 10),
        ];
        // The question asked first comes first, whenever its session last changed.
        sessions[4].ask = Some(Question {
            id: "a1".into(),
            question: "Which plumber should I book? ".repeat(5),
            detail: None,
            options: vec![],
            recommend: None,
            at: 200,
            deadline: None,
        });
        sessions[5].ask = Some(Question {
            at: 300,
            ..sessions[4].ask.clone().unwrap()
        });
        sessions[0].unread = true;
        sessions[4].unread = true;
        // A program in a herdr terminal on another machine, waiting for something to do.
        let mut idle = session("idle1", State::Idle, 500);
        idle.backend = Backend::Herdr;
        idle.place = Some("DESKTOP".into());
        idle.program = Some("codex".into());
        idle.progress = Some(40);
        let sessions: Vec<Session> = sessions.into_iter().chain([idle]).collect();
        let list = list(sessions.iter(), "HOME", false, 1000);
        let ids: Vec<&str> = list.sessions.iter().map(|s| s.id.as_str()).collect();
        assert_eq!(
            ids,
            ["ask1", "ask2", "work2", "work1", "idle1", "done2", "done1"]
        );
        assert_eq!(
            (
                list.counts.ask,
                list.counts.running,
                list.counts.planned,
                list.counts.unread,
                list.counts.idle
            ),
            (2, 2, 0, 2, 1)
        );
        let herdr = &list.sessions[4];
        assert_eq!(
            (
                herdr.place.as_str(),
                herdr.via,
                herdr.program.as_deref(),
                herdr.progress
            ),
            ("DESKTOP", Via::Herdr, Some("codex"), Some(40))
        );
        assert_eq!(herdr.sub, "Ready");
        assert_eq!(
            (list.sessions[0].via, list.sessions[0].program.as_deref()),
            (Via::Inkharness, None)
        );
        assert_eq!(list.older, 0);
        let sub = &list.sessions[0].sub;
        assert!(
            sub.chars().count() <= SUB_CHARS && sub.ends_with('…'),
            "{sub}"
        );
        assert_eq!(list.sessions[0].place, "HOME");

        // Locked: the counts, and nothing that says what they are.
        let locked = super::list(sessions.iter(), "HOME", true, 1000);
        assert!(locked.locked && locked.sessions.is_empty());
        assert_eq!(locked.counts, list.counts);
        // When the newest question came and the newest result: times, not what they are.
        assert_eq!(
            serde_json::to_value(&locked).unwrap(),
            serde_json::json!({"locked": true,
                               "counts": {"ask": 2, "running": 2, "planned": 0, "unread": 2, "idle": 1, "proposed": 0},
                               "check_at": null, "latest": {"ask": 300, "done": 100},
                               "sessions": [], "older": 0})
        );
        // Unlocked, the newest question comes whole.
        assert_eq!(list.ask.as_ref().map(|q| q.session.as_str()), Some("ask2"));
    }

    #[test]
    fn schedules_watches_jobs_and_pauses_in_the_list() {
        let now = 10_000;
        let mut sched = session("sch1", State::Scheduled, 100);
        sched.schedule = Some(Schedule {
            at: "07:00".into(),
            days: super::super::schedule::WEEKDAYS,
            prompt: "brief me".into(),
            wake_board: true,
            show_note: true,
            speak: false,
            next_run: 20_000,
            runs: vec![Run {
                at: 5_000,
                state: State::Done,
                secs: 300,
            }],
            running_since: None,
        });
        sched.unread = true;
        let mut watch = session("wat1", State::Watch, 200);
        watch.watch = Some(Watch {
            every_minutes: 5,
            condition: "the fare under $700".into(),
            prompt: "the fare under $700".into(),
            next_look: 10_300,
            values: vec![760.0, 712.0],
            looking_since: None,
            met: false,
        });
        let mut work = session("wrk1", State::Working, 300);
        work.expect_at = Some(15_000);
        let mut late = session("wrk2", State::Working, 310);
        late.expect_at = Some(9_000); // past: it says nothing about when to look
        let mut paused = session("pau1", State::Paused, 400);
        paused.paused_from = Some(State::Working);
        let job = session("job1", State::Proposed, 50);
        let done = session("don1", State::Done, 600);
        let all = [sched, watch, work, late, paused, job, done];
        let list = list(all.iter(), "HOME", false, now);
        let ids: Vec<&str> = list.sessions.iter().map(|s| s.id.as_str()).collect();
        assert_eq!(
            ids,
            ["job1", "wrk2", "wrk1", "pau1", "wat1", "sch1", "don1"]
        );
        let c = &list.counts;
        assert_eq!((c.running, c.planned, c.proposed), (2, 2, 1));
        // The watch's next look comes first; the schedule wakes the board 300 s after its run.
        assert_eq!((list.check_at, list.next_at), (Some(10_300), Some(20_000)));
        assert_eq!(list.latest.done, 100);
        let row = |id: &str| list.sessions.iter().find(|s| s.id == id).unwrap();
        assert_eq!(row("wat1").spark, [760.0, 712.0]);
        assert_eq!(
            (row("wat1").next_at, row("sch1").next_at),
            (Some(10_300), Some(20_000))
        );
        assert_eq!(
            (row("wrk1").expect_at, row("wrk2").expect_at),
            (Some(15_000), None)
        );
        assert_eq!(row("wat1").sub, "the fare under $700");
        assert_eq!(row("sch1").sub, "brief me");
        // Locked: the times, never what they are.
        let locked = super::list(all.iter(), "HOME", true, now);
        assert_eq!(
            (locked.check_at, locked.next_at, locked.ask),
            (Some(10_300), Some(20_000), None)
        );
        // A job nobody started goes after keep_days, like a finished session.
        assert!(expired(&all[5], 30, 50 + 31 * 86_400) && !expired(&all[0], 30, u64::MAX / 2));
    }

    #[test]
    fn past_forty_they_are_counted() {
        let sessions: Vec<Session> = (0..45)
            .map(|i| session(&format!("s{i}"), State::Done, i))
            .collect();
        let list = list(sessions.iter(), "HOME", false, 1000);
        assert_eq!((list.sessions.len(), list.older), (LISTED, 5));
        assert_eq!(list.sessions[0].id, "s44");
    }

    #[test]
    fn steps_subs_and_the_plan() {
        let mut s = session("k7q2", State::Working, 1);
        assert_eq!(s.sub(), "");
        s.add_log("web_search   plumbers\nopen Saturday", 2);
        assert_eq!(s.sub(), "web_search plumbers open Saturday");
        s.plan = Some(Plan {
            steps: vec!["Find plumbers".into(), "Compare".into(), "Ask".into()],
            current: 2,
        });
        assert_eq!(s.sub(), "Compare");
        assert_eq!(s.step(), Some((2, 3)));
        let view = s.view("HOME", None);
        let states: Vec<StepState> = view.plan.unwrap().iter().map(|p| p.state).collect();
        assert_eq!(states, [StepState::Done, StepState::Now, StepState::Todo]);
        // Done: every step is.
        s.state = State::Done;
        s.result = Some("## Booked\n\nRiverside, Thursday 08:00.".into());
        assert_eq!(s.sub(), "Booked");
        let states: Vec<StepState> = s
            .view("HOME", None)
            .plan
            .unwrap()
            .iter()
            .map(|p| p.state)
            .collect();
        assert_eq!(states, [StepState::Done; 3]);
        // The log goes newest first, LOG_SHOWN of it.
        for i in 0..30 {
            s.add_log(&format!("line {i}"), 10 + i);
        }
        let view = s.view("HOME", None);
        assert_eq!(view.log.len(), LOG_SHOWN);
        assert_eq!(view.log[0].text, "line 29");
    }

    #[test]
    fn plain_lines_for_the_board() {
        assert_eq!(
            plain("**Done:** booked `Riverside`"),
            "Done: booked Riverside"
        );
        assert_eq!(plain("## A heading"), "A heading");
        assert_eq!(
            plain("- [Riverside](https://example.com) at 08:00"),
            "Riverside at 08:00"
        );
        assert_eq!(plain("2. second [not a link"), "second [not a link");
        assert_eq!(plain("run_js 6 * 7"), "run_js 6 * 7");
    }

    #[test]
    fn text_limits() {
        assert_eq!(
            title_from("find a plumber for the kitchen leak this week, one who can come early"),
            "Find a plumber for the kitchen leak…"
        );
        assert_eq!(title_from("  \n\nwhat's on today?"), "What's on today");
        assert_eq!(title_from(""), "New session");
        assert!(one_line(&"x".repeat(200), LOG_CHARS).chars().count() == LOG_CHARS);
        let long = "é".repeat(5000);
        let cut = cut_bytes(&long, RESULT_BYTES);
        assert!(cut.len() <= RESULT_BYTES && cut.ends_with('…'));
        assert_eq!(cut_bytes("short", 10), "short");
        assert!(valid_id(&new_id(|_| false)));
        assert!(!valid_id("../etc") && !valid_id(""));
    }

    #[test]
    fn the_conversation_keeps_its_start() {
        let mut s = session("c", State::Working, 1);
        s.add_said(Who::User, "the task");
        for i in 0..60 {
            s.add_said(Who::Assistant, &format!("reply {i}"));
        }
        assert_eq!(s.conversation.len(), CONVERSATION_KEPT + 1);
        assert_eq!(s.conversation[0].text, "the task");
        assert_eq!(s.conversation.last().unwrap().text, "reply 59");
    }

    #[test]
    fn sessions_persist_and_expire() {
        let dir = crate::skills::tests::scratch("agent-store");
        let mut s = session("k7q2", State::Done, 1000);
        s.result = Some("Booked.".into());
        s.add_log("web_search plumbers", 900);
        save(&dir, &s).unwrap();
        save(&dir, &session("w1", State::Working, 5)).unwrap();
        std::fs::write(dir.join("bad1.json"), "{not json").unwrap();
        std::fs::write(dir.join("notes.txt"), "ignored").unwrap();
        std::fs::write(dir.join("pin-tries.json"), r#"{"wrong":1,"until":0}"#).unwrap();
        let mut loaded = load_all(&dir);
        loaded.sort_by(|a, b| a.id.cmp(&b.id));
        let ids: Vec<&str> = loaded.iter().map(|s| s.id.as_str()).collect();
        assert_eq!(ids, ["k7q2", "w1"]);
        assert_eq!(loaded[0].result.as_deref(), Some("Booked."));
        assert_eq!(loaded[0].log[0].text, "web_search plumbers");

        let day = 86_400;
        assert!(!expired(&loaded[0], 30, 1000 + 30 * day));
        assert!(expired(&loaded[0], 30, 1001 + 30 * day));
        assert!(!expired(&loaded[1], 30, u64::MAX / 2), "working ones stay");
        remove(&dir, "k7q2").unwrap();
        remove(&dir, "k7q2").unwrap();
        assert_eq!(load_all(&dir).len(), 1);
    }
}
