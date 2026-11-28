//! `[talk] delegate`: the voice model hands a task of many steps to a helper and tells the user
//! what came of it. The helper is the text turns' model ([chat.llm], else [llm]) with their
//! rounds, the turn's tools (the board's included) and the skills, and works inside the turn:
//! the user waits, and hears how it goes. Each tool it calls shows on the board (a `status`,
//! sent again every [`KEEPALIVE`] so the board doesn't give up on the turn), and from
//! [`FIRST_UPDATE`] on, at most every [`UPDATE_GAP`] when it has done something new, the voice
//! model says in a sentence what it's doing. Its report is the tool's result, which the voice
//! model tells the user.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::time::Duration;

use async_trait::async_trait;
use futures_util::FutureExt;
use futures_util::future::{BoxFuture, Shared};
use serde_json::{Value, json};
use tokio::sync::mpsc;
use tokio::time::Instant;

use crate::llm::{self, Llm, Outcome, Piece, Turn};
use crate::tools::{Extra, ToolDef, ToolSet};

/// The tool the voice model calls.
pub const TOOL: &str = "delegate";

/// Rounds of its own tool calls after which the voice model, not having delegated, is reminded
/// once ([`Nudge`]).
pub const NUDGE_AFTER: usize = 2;
/// The reminder.
pub const NUDGE: &str = "(A note from the system, not the user: don't answer it or mention it.) \
     You have made several tool calls yourself. Unless you can answer now, call the delegate \
     tool at once, saying nothing first, with the whole task as the user asked it; don't tell \
     the user anything you found until its report comes back.";

/// The board's status goes again after this long without a new one: it gives up on a turn
/// after 45 s with nothing from the daemon (until the reply's audio starts).
pub const KEEPALIVE: Duration = Duration::from_secs(15);
/// A task that's over this quickly isn't talked about.
pub const FIRST_UPDATE: Duration = Duration::from_secs(8);
/// The least time between two updates.
pub const UPDATE_GAP: Duration = Duration::from_secs(20);
/// The most updates one task gets.
pub const MAX_UPDATES: usize = 4;
/// A helper still working after this long is stopped.
pub const TIME_LIMIT: Duration = Duration::from_secs(300);
/// What the voice model reads before the report: a model told to answer in a sentence or two
/// kept only the last part of a two-part answer (2026-11-28).
const REPORT_HEAD: &str = "The helper's report. Say it to the user nearly as it is, keeping \
     every part that answers what they asked (all of the names, numbers and results) and \
     anything it couldn't do:";
/// The longest report the voice model gets back.
const REPORT_CHARS: usize = 4000;
/// Steps an update hears about (the latest).
const UPDATE_STEPS: usize = 8;
/// How much of the helper's latest writing an update hears.
const NOTE_CHARS: usize = 300;
/// An update still being said when the task ends gets this long to finish.
const UPDATE_FINISH: Duration = Duration::from_secs(10);

/// The user's words this turn: the transcript, when there is speech-to-text ("" when not).
pub type Heard = Shared<BoxFuture<'static, Result<String, String>>>;

/// What tasks are handed to, built once for the daemon.
pub struct Helper {
    /// Does the work: [chat.llm], else [llm].
    pub worker: Box<dyn Llm>,
    /// The worker's system prompt ([`worker_prompt`] and what follows every prompt).
    pub system: String,
    /// Model rounds a task gets ([chat] max_rounds).
    pub rounds: usize,
    /// Says the updates: the voice model, [llm].
    pub voice: Box<dyn Llm>,
}

/// The tool for one turn.
pub struct DelegateTool {
    pub helper: Arc<Helper>,
    /// What the worker gets: the turn's tools, without this one or the think tool.
    pub tools: ToolSet,
    /// The conversation before this turn.
    pub history: Vec<Turn>,
    pub heard: Heard,
    /// The turn's reply: the worker's tool calls (shown as the status) and the updates go
    /// there, between what the voice model says.
    pub out: mpsc::Sender<Piece>,
    /// Set once it's called, for the turn's [`Nudge`].
    pub used: Arc<AtomicBool>,
}

/// Reminds the voice model, once, after [`NUDGE_AFTER`] rounds of its own tool calls without the
/// delegate tool: gpt-audio-mini often made a search or three itself first, and once gave up
/// after four (2026-11-28). Quick requests (a round or two) never see it.
pub struct Nudge {
    rounds: AtomicUsize,
    used: Arc<AtomicBool>,
}

impl Nudge {
    pub fn new(used: Arc<AtomicBool>) -> Nudge {
        Nudge {
            rounds: AtomicUsize::new(0),
            used,
        }
    }

    /// For [`llm::Request::steer`]: what joins the conversation after a round's tool results.
    pub fn after_round(&self) -> Vec<String> {
        let rounds = self.rounds.fetch_add(1, Ordering::Relaxed) + 1;
        if rounds == NUDGE_AFTER && !self.used.load(Ordering::Relaxed) {
            tracing::info!(rounds, "reminding the voice model to delegate");
            vec![NUDGE.to_string()]
        } else {
            Vec::new()
        }
    }
}

/// The base of the worker's system prompt.
pub fn worker_prompt() -> String {
    "You are a helper working on a task for a voice assistant, which tells the user what you \
     did. Do the whole task with the tools, making the calls that don't need each other's \
     results together, and follow a skill when one covers it. The task was written by a model \
     that may not know today's date: where it and the user's own words differ, the user's \
     words win. Don't ask questions: nobody can answer before you finish, so do what is \
     sensible and say what you assumed. End with a report the voice assistant can read out as \
     it is: two to four short plain sentences, no markdown, that answer every part of the task, \
     and anything left undone or that went wrong. Only say you did something if a tool call \
     did it."
        .into()
}

/// Opens the voice model's system prompt: the tool's description alone, and then this at the
/// prompt's end, didn't make gpt-audio-mini use it (it made four web searches and two scripts
/// itself, 2026-11-28).
pub const VOICE_RULE: &str = "Rule: if a request needs more than two tool calls, or a skill, you \
     must not make those calls yourself. Say only one short sentence first (\"Let me \
     check.\"), without repeating the request back, then call the delegate tool, before any \
     other tool, once with the whole task. Say that sentence once a turn, not before each \
     call, and until you answer say nothing about what your own tool calls returned. When the \
     report comes back, tell the user every part of it that answers what they asked, in a few \
     short sentences. Do only quick things yourself: one or two tool calls (the ones that \
     don't need each other's results together), or what you can answer at once.";

/// The voice model's base prompt with [`VOICE_RULE`] first: `custom` (`system_prompt`), or the
/// spoken prompt without its own advice on several tool calls, which the rule replaces.
pub fn voice_prompt(custom: Option<&str>) -> String {
    match custom {
        Some(custom) => format!("{VOICE_RULE}\n\n{custom}"),
        None => format!("{VOICE_RULE}\n\n{}", crate::config::spoken_base()),
    }
}

/// The system prompt of an update.
const UPDATE_SYSTEM: &str = "You are the voice of an assistant working on what the user asked \
     for, while they wait. In one short spoken sentence of at most 15 words, say in the first \
     person what you're doing now (\"I'm checking the prices now.\"), in plain words: no tool \
     names, IDs or search terms. The steps are what you looked for, not what you found: never \
     take a search for a result, and say you found something (a name, a number) only if your \
     notes say so. It must tell them something they haven't heard yet. Don't greet them, ask \
     anything, or say you're done.";

pub fn definition() -> ToolDef {
    ToolDef {
        name: TOOL.into(),
        description: "Hands a task of several steps to a helper: a model that works through it \
                      with the same tools and skills, and more steps than you have, while the \
                      user hears how it's going. Use it for anything that takes more than two \
                      or three tool calls or a skill's procedure: setting something up, \
                      comparing, cleaning up, a search that needs following up. Don't use it \
                      for one quick action or a question you can answer, and don't load a \
                      skill first: the helper reads the ones it needs. Before calling it, say \
                      one short sentence (\"Let me check.\"). Write the task so \
                      it stands alone: what the user wants, with the names, dates and details \
                      from the conversation. It returns the helper's report: tell the user \
                      every part of it that answers what they asked."
            .into(),
        schema: json!({
            "type": "object",
            "properties": {
                "task": {"type": "string", "description": "The whole task, standing alone."},
            },
            "required": ["task"],
            "additionalProperties": false,
        }),
    }
}

#[async_trait]
impl Extra for DelegateTool {
    fn defs(&self) -> Vec<ToolDef> {
        vec![definition()]
    }

    async fn call(&self, name: &str, input: &Value) -> Option<Result<String, String>> {
        if name != TOOL {
            return None;
        }
        let Some(task) = input["task"]
            .as_str()
            .map(str::trim)
            .filter(|t| !t.is_empty())
        else {
            return Some(Err("invalid input: task is required".into()));
        };
        self.used.store(true, Ordering::Relaxed);
        Some(self.run(task).await)
    }
}

/// What the worker has done so far.
#[derive(Default)]
struct Progress {
    /// Each tool call, as the board shows it.
    steps: Vec<String>,
    /// The latest call, to show again.
    last: Option<(String, Value)>,
    /// What it wrote.
    text: String,
    failed: usize,
}

impl DelegateTool {
    async fn run(&self, task: &str) -> Result<String, String> {
        let heard = self.heard.clone().await.unwrap_or_default();
        let now = chrono::Local::now().format("%A %-d %B %Y, %H:%M (UTC%:z)");
        let user = brief(task, &heard, &now.to_string());
        tracing::info!(task, "delegating");
        let started = Instant::now();
        let (tx, mut rx) = mpsc::channel::<Piece>(64);
        let request = llm::Request {
            system: &self.helper.system,
            history: &self.history,
            user: &user,
            audio: None,
            tools: &self.tools,
            rounds: self.helper.rounds,
            steer: None,
        };
        let work = tokio::time::timeout(TIME_LIMIT, self.helper.worker.respond(request, tx));
        tokio::pin!(work);

        let mut progress = Progress::default();
        let mut pace = Pace::default();
        let mut said: Vec<String> = Vec::new();
        let mut update: Option<BoxFuture<'_, String>> = None;
        let mut shown = Instant::now();
        let mut tick = tokio::time::interval(Duration::from_secs(1));
        let result = loop {
            tokio::select! {
                biased;
                result = &mut work => break result,
                Some(piece) = rx.recv() => {
                    if self.take(piece, &mut progress).await {
                        shown = Instant::now();
                    }
                }
                text = async { update.as_mut().unwrap().await }, if update.is_some() => {
                    update = None;
                    said.push(text);
                    // What was said cleared the status: the step being worked on shows again.
                    if let Some((name, input)) = progress.last.clone() {
                        let _ = self.out.send(Piece::Tool(name, input)).await;
                        shown = Instant::now();
                    }
                }
                _ = tick.tick() => {
                    if shown.elapsed() >= KEEPALIVE {
                        let (name, input) = progress.last.clone().unwrap_or_else(|| {
                            (TOOL.to_string(), json!({"task": task}))
                        });
                        let _ = self.out.send(Piece::Tool(name, input)).await;
                        shown = Instant::now();
                    }
                    let steps = progress.steps.len();
                    if update.is_none() && pace.due(started.elapsed(), steps) {
                        let before = pace.steps;
                        pace.start(started.elapsed(), steps);
                        let prompt =
                            update_prompt(task, &progress.steps, before, &progress.text, &said);
                        update = Some(self.update(prompt).boxed());
                    }
                }
            }
        };
        while let Ok(piece) = rx.try_recv() {
            self.take(piece, &mut progress).await;
        }
        if let Some(update) = update
            && let Ok(text) = tokio::time::timeout(UPDATE_FINISH, update).await
        {
            said.push(text);
        }
        if !said.is_empty() {
            // What the voice model says next starts on a line of its own.
            let _ = self.out.send(self.newline()).await;
        }
        let secs = started.elapsed().as_secs_f32();
        let steps = progress.steps.len();
        let updates = said.len();
        match result {
            Ok(Ok(Outcome::Done)) => {
                let report = report(&progress);
                tracing::info!(
                    secs,
                    steps,
                    failed = progress.failed,
                    updates,
                    report = %crate::chat::cut(&report, 300),
                    "delegated task done"
                );
                Ok(format!("{REPORT_HEAD}\n\n{report}"))
            }
            Ok(Ok(Outcome::Think { .. })) => Err("the helper asked to think harder".into()),
            Ok(Err(e)) => {
                tracing::warn!(secs, steps, updates, "the helper failed: {e:#}");
                Err(format!("the helper stopped: {e:#}. {}", so_far(&progress)))
            }
            Err(_) => {
                tracing::warn!(secs, steps, updates, "the helper ran out of time");
                Err(format!(
                    "the helper was stopped after {} minutes. {}",
                    TIME_LIMIT.as_secs() / 60,
                    so_far(&progress)
                ))
            }
        }
    }

    /// Takes in a piece of the worker's reply: a tool call goes on to the board (true then).
    async fn take(&self, piece: Piece, progress: &mut Progress) -> bool {
        match piece {
            Piece::Tool(name, input) => {
                progress.steps.push(crate::session::doing(&name, &input));
                progress.last = Some((name.clone(), input.clone()));
                let _ = self.out.send(Piece::Tool(name, input)).await;
                true
            }
            Piece::ToolDone(_, ok) => {
                progress.failed += usize::from(!ok);
                false
            }
            Piece::Text(text) | Piece::Transcript(text) => {
                progress.text.push_str(&text);
                false
            }
            Piece::Audio(_) => false,
        }
    }

    /// Has the voice model say how it's going: its speech goes to the turn's reply, on a line of
    /// its own. Returns what it said.
    async fn update(&self, prompt: String) -> String {
        let (tx, mut rx) = mpsc::channel::<Piece>(64);
        let none = ToolSet::none();
        let request = llm::Request {
            system: UPDATE_SYSTEM,
            history: &[],
            user: &prompt,
            audio: None,
            tools: &none,
            rounds: 1,
            steer: None,
        };
        let forward = async {
            let mut said = String::new();
            let mut first = true;
            while let Some(piece) = rx.recv().await {
                match &piece {
                    Piece::Text(text) | Piece::Transcript(text) => said.push_str(text),
                    Piece::Audio(_) => {}
                    Piece::Tool(..) | Piece::ToolDone(..) => continue,
                }
                if first {
                    first = false;
                    let _ = self.out.send(self.newline()).await;
                }
                let _ = self.out.send(piece).await;
            }
            said
        };
        let (result, said) = tokio::join!(self.helper.voice.respond(request, tx), forward);
        match result {
            Ok(_) => tracing::info!(update = said.trim(), "delegated task: an update"),
            Err(e) => tracing::warn!("delegated task: the update failed: {e:#}"),
        }
        said.trim().to_string()
    }

    /// A line break in the reply: shown only, for a voice that speaks, or ending a sentence
    /// for text-to-speech.
    fn newline(&self) -> Piece {
        if self.helper.voice.speaks().is_some() {
            Piece::Transcript("\n".into())
        } else {
            Piece::Text("\n".into())
        }
    }
}

/// When an update is due: never before [`FIRST_UPDATE`], then no sooner than [`UPDATE_GAP`] after
/// the last, only once the worker has done something since, and [`MAX_UPDATES`] at most.
#[derive(Default)]
struct Pace {
    said: usize,
    /// When the last one began (time since the task started).
    at: Option<Duration>,
    /// The steps there were then.
    steps: usize,
}

impl Pace {
    fn due(&self, elapsed: Duration, steps: usize) -> bool {
        self.said < MAX_UPDATES
            && steps > self.steps
            && elapsed >= FIRST_UPDATE
            && self.at.is_none_or(|at| elapsed >= at + UPDATE_GAP)
    }

    fn start(&mut self, elapsed: Duration, steps: usize) {
        self.said += 1;
        self.at = Some(elapsed);
        self.steps = steps;
    }
}

/// The worker's message: the task, the user's own words when there are any, and the time.
fn brief(task: &str, heard: &str, now: &str) -> String {
    let mut out = task.to_string();
    let heard = heard.trim();
    if !heard.is_empty() {
        out.push_str(&format!("\n\n(The user's own words: \"{heard}\")"));
    }
    out.push_str(&format!("\n\nIt's {now}, local time."));
    out
}

/// What an update is told: the task, the steps taken by the last update (the first `before`)
/// and since, the worker's latest writing, and the updates the user has heard.
fn update_prompt(
    task: &str,
    steps: &[String],
    before: usize,
    text: &str,
    said: &[String],
) -> String {
    let mut prompt = format!("The user's request: {task}\n");
    let before = before.min(steps.len());
    if before > 0 {
        prompt.push_str(&format!(
            "\nSteps you had taken by the last update: {}\n",
            steps[..before].join("; ")
        ));
    }
    prompt
        .push_str("\nSteps since then, oldest first (what you looked for, not what you found):\n");
    let new = &steps[before..];
    let skip = new.len().saturating_sub(UPDATE_STEPS);
    for step in &new[skip..] {
        prompt.push_str(&format!("- {step}\n"));
    }
    let note = text.trim();
    if note.is_empty() {
        prompt.push_str("\nYour notes: none yet, so you have found nothing to tell.\n");
    } else {
        let start = note
            .char_indices()
            .rev()
            .nth(NOTE_CHARS - 1)
            .map_or(0, |(i, _)| i);
        prompt.push_str(&format!(
            "\nYour latest notes, the only findings you may mention: {}\n",
            &note[start..]
        ));
    }
    if !said.is_empty() {
        prompt.push_str(&format!(
            "\nThe user has heard these updates (say something else): {}\n",
            said.join(" ")
        ));
    }
    prompt
}

/// The tool's result: what the worker wrote, or its steps if it wrote nothing.
fn report(progress: &Progress) -> String {
    let text = progress.text.trim();
    if text.is_empty() {
        return format!("The helper finished without a report. {}", steps(progress));
    }
    crate::chat::cut(text, REPORT_CHARS)
}

/// What the worker wrote and did, for an error.
fn so_far(progress: &Progress) -> String {
    let text = progress.text.trim();
    let mut out = steps(progress);
    if !text.is_empty() {
        out.push_str(&format!(
            " What it wrote: {}",
            crate::chat::cut(text, REPORT_CHARS)
        ));
    }
    out
}

fn steps(progress: &Progress) -> String {
    if progress.steps.is_empty() {
        "It called no tools.".into()
    } else {
        format!("Its steps: {}.", progress.steps.join("; "))
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;

    use super::*;

    #[test]
    fn updates_wait_for_news_and_space_out() {
        let mut pace = Pace::default();
        let s = Duration::from_secs;
        assert!(!pace.due(s(5), 3), "too soon");
        assert!(!pace.due(s(9), 0), "nothing done yet");
        assert!(pace.due(s(9), 1));
        pace.start(s(9), 1);
        assert!(!pace.due(s(20), 4), "too soon after the last");
        assert!(!pace.due(s(40), 1), "nothing new");
        assert!(pace.due(s(29), 2));
        for i in 1..MAX_UPDATES {
            pace.start(s(30 * i as u64), i + 1);
        }
        assert!(!pace.due(s(1000), 99), "enough said");
    }

    #[test]
    fn the_nudge_comes_once_after_two_rounds_without_delegating() {
        let nudge = Nudge::new(Arc::default());
        assert!(nudge.after_round().is_empty());
        assert_eq!(nudge.after_round(), [NUDGE]);
        assert!(nudge.after_round().is_empty());
        assert!(nudge.after_round().is_empty());
        let used = Arc::new(AtomicBool::new(false));
        let nudge = Nudge::new(used.clone());
        assert!(nudge.after_round().is_empty());
        used.store(true, Ordering::Relaxed);
        assert!(nudge.after_round().is_empty(), "it delegated in round 2");
    }

    #[test]
    fn the_update_hears_the_latest() {
        let steps: Vec<String> = (1..=10).map(|i| format!("step {i}")).collect();
        let note = "x".repeat(500) + " now the streams";
        let first = update_prompt("set up the games", &steps, 0, &note, &[]);
        assert!(first.starts_with("The user's request: set up the games\n\nSteps since then"));
        assert!(!first.contains("- step 2\n") && first.contains("- step 3\n"));
        assert!(first.contains("- step 10\n"));
        assert!(first.contains("the only findings you may mention: "));
        assert!(first.contains("now the streams\n") && !first.contains(&"x".repeat(300)));
        let next = update_prompt("set up the games", &steps, 9, "", &["Looking now.".into()]);
        assert!(next.contains("\nSteps you had taken by the last update: step 1; step 2;"));
        assert!(next.contains(
            "step 9\n\nSteps since then, oldest first (what you looked for, not what you \
             found):\n- step 10\n\nYour notes: none yet"
        ));
        assert!(
            next.ends_with("The user has heard these updates (say something else): Looking now.\n")
        );
        assert_eq!(brief("t", " ", "Sunday"), "t\n\nIt's Sunday, local time.");
        assert_eq!(
            brief("t", "add the games", "Sunday"),
            "t\n\n(The user's own words: \"add the games\")\n\nIt's Sunday, local time."
        );
    }

    /// A worker that calls `web_search` every 10 s, `calls` times, then reports.
    struct Worker {
        calls: usize,
        /// What it was asked.
        asked: Arc<Mutex<Option<String>>>,
    }

    #[async_trait]
    impl Llm for Worker {
        async fn respond(
            &self,
            request: llm::Request<'_>,
            out: mpsc::Sender<Piece>,
        ) -> anyhow::Result<Outcome> {
            *self.asked.lock().unwrap() = Some(request.user.to_string());
            assert!(request.tools.defs().iter().all(|d| d.name != TOOL));
            assert_eq!(request.rounds, 20);
            for i in 0..self.calls {
                out.send(Piece::Tool(
                    "web_search".into(),
                    json!({"query": format!("q{i}")}),
                ))
                .await?;
                tokio::time::sleep(Duration::from_secs(10)).await;
                out.send(Piece::ToolDone("web_search".into(), true)).await?;
                out.send(Piece::Text(format!("Looked up q{i}. "))).await?;
            }
            out.send(Piece::Text("Set up both games.".into())).await?;
            Ok(Outcome::Done)
        }
    }

    /// A voice that says "Update n.", keeping what each update was told.
    struct Voice(Arc<Mutex<Vec<String>>>);

    #[async_trait]
    impl Llm for Voice {
        async fn respond(
            &self,
            request: llm::Request<'_>,
            out: mpsc::Sender<Piece>,
        ) -> anyhow::Result<Outcome> {
            assert!(request.tools.defs().is_empty());
            assert_eq!(request.system, UPDATE_SYSTEM);
            self.0.lock().unwrap().push(request.user.to_string());
            let n = self.0.lock().unwrap().len();
            out.send(Piece::Transcript(format!("Update {n}."))).await?;
            out.send(Piece::Audio(vec![1, 0].into())).await?;
            Ok(Outcome::Done)
        }

        fn speaks(&self) -> Option<u32> {
            Some(24000)
        }
    }

    fn tool(worker: Worker, voice: Voice) -> (DelegateTool, mpsc::Receiver<Piece>) {
        let (out, rx) = mpsc::channel(256);
        let heard: BoxFuture<'static, Result<String, String>> =
            async { Ok("add today's games".to_string()) }.boxed();
        let tool = DelegateTool {
            helper: Arc::new(Helper {
                worker: Box::new(worker),
                system: worker_prompt(),
                rounds: 20,
                voice: Box::new(voice),
            }),
            tools: ToolSet::none(),
            history: Vec::new(),
            heard: heard.shared(),
            out,
            used: Arc::default(),
        };
        (tool, rx)
    }

    fn pieces(rx: &mut mpsc::Receiver<Piece>) -> Vec<Piece> {
        let mut all = Vec::new();
        while let Ok(piece) = rx.try_recv() {
            all.push(piece);
        }
        all
    }

    #[tokio::test(start_paused = true)]
    async fn a_long_task_shows_its_steps_and_says_how_it_goes() {
        let asked = Arc::new(Mutex::new(None));
        let told = Arc::new(Mutex::new(Vec::new()));
        let (tool, mut rx) = tool(
            Worker {
                calls: 6,
                asked: asked.clone(),
            },
            Voice(told.clone()),
        );
        let result = tool
            .call(TOOL, &json!({"task": "Set up today's games."}))
            .await;
        assert_eq!(
            result,
            Some(Ok(format!(
                "{REPORT_HEAD}\n\nLooked up q0. Looked up q1. Looked up q2. Looked up q3. \
                 Looked up q4. Looked up q5. Set up both games."
            )))
        );
        let asked = asked.lock().unwrap().clone().unwrap();
        assert!(
            asked.starts_with(
                "Set up today's games.\n\n(The user's own words: \"add today's games\")\n\nIt's "
            ),
            "{asked}"
        );
        let all = pieces(&mut rx);
        let shown: Vec<&Piece> = all
            .iter()
            .filter(|p| matches!(p, Piece::Tool(..)))
            .collect();
        // Each step, and the one being worked on again after each update (no keepalive: a
        // step comes every 10 s).
        assert_eq!(shown.len(), 6 + 3, "{shown:?}");
        assert_eq!(
            shown[0],
            &Piece::Tool("web_search".into(), json!({"query": "q0"}))
        );
        let i = all
            .iter()
            .position(|p| p == &Piece::Transcript("Update 1.".into()))
            .unwrap();
        assert_eq!(all[i + 1], Piece::Audio(vec![1, 0].into()));
        assert_eq!(
            all[i + 2],
            Piece::Tool("web_search".into(), json!({"query": "q0"}))
        );
        // The second update hears what's new and what was said.
        let told = told.lock().unwrap().clone();
        assert!(
            told[1].contains("Steps you had taken by the last update: Searching the web: q0\n"),
            "{}",
            told[1]
        );
        assert!(told[1].contains("- Searching the web: q2\n"), "{}", told[1]);
        assert!(
            told[1].ends_with("(say something else): Update 1.\n"),
            "{}",
            told[1]
        );
        // 60 s of work: updates at 8 s or so (after the first step), 28 s and 48 s.
        let said: Vec<&Piece> = all
            .iter()
            .filter(|p| matches!(p, Piece::Transcript(t) if t.starts_with("Update")))
            .collect();
        assert_eq!(said.len(), 3, "{all:?}");
        // Each on a line of its own, and a newline for what the voice model says next.
        let i = all
            .iter()
            .position(|p| p == &Piece::Transcript("Update 1.".into()))
            .unwrap();
        assert_eq!(all[i - 1], Piece::Transcript("\n".into()));
        assert_eq!(all.last(), Some(&Piece::Transcript("\n".into())));
    }

    #[tokio::test(start_paused = true)]
    async fn a_quick_task_is_quiet_and_a_bare_call_fails() {
        let (tool, mut rx) = tool(
            Worker {
                calls: 0,
                asked: Arc::default(),
            },
            Voice(Arc::default()),
        );
        assert_eq!(
            tool.call(TOOL, &json!({"task": "  "})).await,
            Some(Err("invalid input: task is required".into()))
        );
        assert_eq!(tool.call("other", &json!({})).await, None);
        assert!(
            !tool.used.load(Ordering::Relaxed),
            "a call to another tool isn't one"
        );
        let result = tool.call(TOOL, &json!({"task": "Set it up."})).await;
        assert!(tool.used.load(Ordering::Relaxed));
        assert_eq!(
            result,
            Some(Ok(format!("{REPORT_HEAD}\n\nSet up both games.")))
        );
        assert!(pieces(&mut rx).is_empty(), "nothing shown or said");
    }
}
