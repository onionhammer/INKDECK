//! A session as it is kept on disk (one JSON file each in `[agent] dir`), and what a board sees
//! of it: a row of the list (`sessions`) and the whole of it (`session`), with the limits the
//! protocol sets on each field (docs/voice-protocol.md, "Agent sessions").

use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use voice_proto::{
    AskOption, AskView, Counts, LogLine, PlanItem, SessionList, SessionRow, SessionView, State,
    StepState,
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
            _ => match &self.result {
                Some(result) => first_line(result),
                None => last_log(),
            },
        };
        one_line(&text, SUB_CHARS)
    }

    pub fn row(&self, place: &str) -> SessionRow {
        let step = self.step();
        SessionRow {
            id: self.id.clone(),
            title: self.title.clone(),
            state: self.state,
            place: place.to_string(),
            sub: self.sub(),
            step: step.map(|s| s.0),
            steps: step.map(|s| s.1),
            progress: None,
            updated: self.updated,
            unread: self.unread,
        }
    }

    pub fn view(&self, place: &str) -> SessionView {
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
            place: place.to_string(),
            prompt: cut(&self.prompt, PROMPT_CHARS),
            started: self.started,
            updated: self.updated,
            step: step.map(|s| s.0),
            steps: step.map(|s| s.1),
            plan,
            log: self.log.iter().rev().take(LOG_SHOWN).cloned().collect(),
            result: self.result.clone(),
            ask: self.question(),
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
        })
    }
}

/// The list as a board gets it: asking first (the oldest question first), then working (the
/// most recently active first), then the rest, newest first; LISTED of them, the rest counted
/// in `older`. Locked, it is the counts alone.
pub fn list<'a>(
    sessions: impl Iterator<Item = &'a Session>,
    place: &str,
    locked: bool,
) -> SessionList {
    let mut all: Vec<&Session> = sessions.collect();
    let mut counts = Counts::default();
    for s in &all {
        match s.state {
            State::Ask => counts.ask += 1,
            State::Working => counts.running += 1,
            _ => {}
        }
        counts.unread += s.unread as usize;
    }
    if locked {
        return SessionList {
            locked: true,
            counts,
            check_at: None,
            sessions: Vec::new(),
            older: 0,
        };
    }
    let group = |s: &Session| match s.state {
        State::Ask => 0,
        State::Working => 1,
        _ => 2,
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
        check_at: None,
        sessions: all.iter().take(LISTED).map(|s| s.row(place)).collect(),
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

/// True if the session finished more than `keep_days` ago.
pub fn expired(session: &Session, keep_days: u64, now: u64) -> bool {
    session.state.finished() && session.updated.saturating_add(keep_days * 86_400) < now
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
        });
        sessions[5].ask = Some(Question {
            at: 300,
            ..sessions[4].ask.clone().unwrap()
        });
        sessions[0].unread = true;
        sessions[4].unread = true;
        let list = list(sessions.iter(), "HOME", false);
        let ids: Vec<&str> = list.sessions.iter().map(|s| s.id.as_str()).collect();
        assert_eq!(ids, ["ask1", "ask2", "work2", "work1", "done2", "done1"]);
        assert_eq!(
            (
                list.counts.ask,
                list.counts.running,
                list.counts.planned,
                list.counts.unread
            ),
            (2, 2, 0, 2)
        );
        assert_eq!(list.older, 0);
        let sub = &list.sessions[0].sub;
        assert!(
            sub.chars().count() <= SUB_CHARS && sub.ends_with('…'),
            "{sub}"
        );
        assert_eq!(list.sessions[0].place, "HOME");

        // Locked: the counts, and nothing that says what they are.
        let locked = super::list(sessions.iter(), "HOME", true);
        assert!(locked.locked && locked.sessions.is_empty());
        assert_eq!(locked.counts, list.counts);
        assert_eq!(
            serde_json::to_value(&locked).unwrap(),
            serde_json::json!({"locked": true, "counts": {"ask": 2, "running": 2, "planned": 0, "unread": 2},
                               "check_at": null, "sessions": [], "older": 0})
        );
    }

    #[test]
    fn past_forty_they_are_counted() {
        let sessions: Vec<Session> = (0..45)
            .map(|i| session(&format!("s{i}"), State::Done, i))
            .collect();
        let list = list(sessions.iter(), "HOME", false);
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
        let view = s.view("HOME");
        let states: Vec<StepState> = view.plan.unwrap().iter().map(|p| p.state).collect();
        assert_eq!(states, [StepState::Done, StepState::Now, StepState::Todo]);
        // Done: every step is.
        s.state = State::Done;
        s.result = Some("## Booked\n\nRiverside, Thursday 08:00.".into());
        assert_eq!(s.sub(), "Booked");
        let states: Vec<StepState> = s
            .view("HOME")
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
        let view = s.view("HOME");
        assert_eq!(view.log.len(), LOG_SHOWN);
        assert_eq!(view.log[0].text, "line 29");
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
