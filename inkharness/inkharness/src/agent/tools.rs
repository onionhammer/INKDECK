//! The tools only sessions get: `ask_choice`, which puts a question to the user and waits for
//! the answer, `plan`, the checklist a board shows as a meter, and `schedule` and `watch`,
//! which turn a session into one that runs at a time of day or looks every so often. And the
//! one talk gets: `propose_job`, which files a job for someone behind the PIN to start.

use std::sync::Arc;

use async_trait::async_trait;
use serde::Deserialize;
use serde_json::{Value, json};
use voice_proto::{AskOption, Backend, OnDeadline};

use super::store::{self, LOG_CHARS, Question, TITLE_CHARS, cut, one_line};
use super::{Agent, schedule};
use crate::tools::{Extra, ToolDef};

pub const ASK_TOOL: &str = "ask_choice";
pub const PLAN_TOOL: &str = "plan";
pub const SCHEDULE_TOOL: &str = "schedule";
pub const WATCH_TOOL: &str = "watch";
pub const PROPOSE_TOOL: &str = "propose_job";

/// Tools that show for themselves (the plan, the question, the schedule): kept out of the log.
pub fn shows_itself(name: &str) -> bool {
    [ASK_TOOL, PLAN_TOOL, SCHEDULE_TOOL, WATCH_TOOL].contains(&name)
}

const MIN_OPTIONS: usize = 2;
const MAX_OPTIONS: usize = 6;
const OPTION_TITLE_CHARS: usize = 32;
const OPTION_RIGHT_CHARS: usize = 12;
const OPTION_DETAIL_CHARS: usize = 400;
const QUESTION_CHARS: usize = 200;
const DETAIL_CHARS: usize = 400;
const MAX_STEPS: usize = 12;
const STEP_CHARS: usize = 60;
const PROMPT_CHARS: usize = 2000;
const CONDITION_CHARS: usize = 120;
/// How often a watch may look at most: each look is a model run (a claude run costs more).
const WATCH_FLOOR_API: u64 = 5;
const WATCH_FLOOR_CLAUDE: u64 = 30;
/// The longest a question's deadline can be: a week.
const MAX_DEADLINE_MINUTES: u64 = 7 * 24 * 60;

/// How a question was answered.
#[derive(Debug, Clone, PartialEq)]
pub enum Answer {
    /// One of the options, by key.
    Choice { choice: usize, title: String },
    /// In words, by voice or over HTTP: none of the options, or something more.
    Text(String),
}

impl Answer {
    /// What the model gets back from ask_choice.
    pub fn for_model(&self) -> String {
        match self {
            Answer::Choice { choice, title } => json!({"choice": choice, "title": title}),
            Answer::Text(text) => json!({"text": text}),
        }
        .to_string()
    }

    /// For the log.
    pub fn short(&self) -> &str {
        match self {
            Answer::Choice { title, .. } => title,
            Answer::Text(text) => text,
        }
    }
}

/// One session's ask_choice and plan.
pub struct SessionTools {
    pub agent: Arc<Agent>,
    pub id: String,
    /// The run they belong to (Live::generation).
    pub run: u64,
}

#[async_trait]
impl Extra for SessionTools {
    fn defs(&self) -> Vec<ToolDef> {
        let mut defs = vec![
            plan_definition(),
            ask_definition(),
            schedule_definition(),
            watch_definition(),
        ];
        if self.agent.herdr.get().is_some_and(|h| h.runs()) {
            defs.push(run_definition());
        }
        defs
    }

    async fn call(&self, name: &str, input: &Value) -> Option<Result<String, String>> {
        Some(match name {
            super::herdr::RUN_TOOL => match self.agent.herdr.get().filter(|h| h.runs()) {
                Some(herdr) => herdr.run_tool(&self.agent, &self.id, input).await,
                None => return None,
            },
            ASK_TOOL => self.ask(input).await,
            PLAN_TOOL => self.plan(input),
            SCHEDULE_TOOL => self.schedule(input),
            WATCH_TOOL => self.watch(input),
            _ => return None,
        })
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct AskInput {
    question: String,
    detail: Option<String>,
    options: Vec<OptionInput>,
    recommend: Option<usize>,
    deadline_minutes: Option<u64>,
    then: Option<OnDeadline>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct OptionInput {
    title: String,
    right: Option<String>,
    detail: Option<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct PlanInput {
    steps: Vec<String>,
    current: Option<usize>,
    title: Option<String>,
    expect_minutes: Option<u64>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ScheduleInput {
    at: Option<String>,
    days: Option<Value>,
    prompt: Option<String>,
    #[serde(default)]
    wake_board: bool,
    #[serde(default)]
    show_note: bool,
    #[serde(default)]
    speak: bool,
    #[serde(default)]
    cancel: bool,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct WatchInput {
    every_minutes: Option<u64>,
    condition: Option<String>,
    prompt: Option<String>,
    value: Option<f64>,
    #[serde(default)]
    met: bool,
}

/// Text that is there: trimmed, empty as None.
fn given(text: Option<String>) -> Option<String> {
    text.map(|t| t.trim().to_string()).filter(|t| !t.is_empty())
}

impl SessionTools {
    async fn ask(&self, input: &Value) -> Result<String, String> {
        let input: AskInput =
            serde_json::from_value(input.clone()).map_err(|e| format!("invalid input: {e}"))?;
        let question = one_line(&input.question, QUESTION_CHARS);
        if question.is_empty() {
            return Err("invalid input: the question is empty".into());
        }
        if !(MIN_OPTIONS..=MAX_OPTIONS).contains(&input.options.len()) {
            return Err(format!(
                "give {MIN_OPTIONS} to {MAX_OPTIONS} options (the board adds \"something else\" itself)"
            ));
        }
        let mut options = Vec::new();
        for option in input.options {
            let title = one_line(&option.title, OPTION_TITLE_CHARS);
            if title.is_empty() {
                return Err("invalid input: every option needs a title".into());
            }
            options.push(AskOption {
                title,
                right: given(option.right).map(|r| one_line(&r, OPTION_RIGHT_CHARS)),
                detail: given(option.detail).map(|d| cut(&d, OPTION_DETAIL_CHARS)),
            });
        }
        let recommend = input.recommend.filter(|r| *r < options.len());
        let deadline = match input.deadline_minutes {
            Some(0) => return Err("invalid input: deadline_minutes is at least 1".into()),
            Some(minutes) => Some(store::QuestionDeadline {
                at: 0, // from when it is asked
                minutes: minutes.min(MAX_DEADLINE_MINUTES),
                then: input.then.unwrap_or(match recommend {
                    Some(_) => OnDeadline::Recommend,
                    None => OnDeadline::GiveUp,
                }),
            }),
            None if input.then.is_some() => {
                return Err("invalid input: then needs deadline_minutes".into());
            }
            None => None,
        };
        let question = Question {
            id: String::new(), // the agent numbers it
            question,
            detail: given(input.detail).map(|d| cut(&d, DETAIL_CHARS)),
            recommend,
            options,
            at: 0,
            deadline,
        };
        let answer = self.agent.put_question(&self.id, self.run, question)?;
        match answer.await {
            Ok(answer) => Ok(answer.for_model()),
            Err(_) => Err("the question was withdrawn".into()),
        }
    }

    fn plan(&self, input: &Value) -> Result<String, String> {
        let input: PlanInput =
            serde_json::from_value(input.clone()).map_err(|e| format!("invalid input: {e}"))?;
        let steps: Vec<String> = input
            .steps
            .iter()
            .map(|s| one_line(s, STEP_CHARS))
            .filter(|s| !s.is_empty())
            .take(MAX_STEPS)
            .collect();
        if steps.is_empty() {
            return Err("invalid input: give at least one step".into());
        }
        let n = steps.len();
        let current = input.current.unwrap_or(1).max(1);
        let title = given(input.title).map(|t| one_line(&t, TITLE_CHARS));
        self.agent.set_plan(
            &self.id,
            self.run,
            steps,
            current,
            title,
            input.expect_minutes,
        )?;
        Ok(if current > n {
            format!("Plan saved: all {n} steps done.")
        } else {
            format!("Plan saved: step {current} of {n}.")
        })
    }
}

impl SessionTools {
    fn schedule(&self, input: &Value) -> Result<String, String> {
        let input: ScheduleInput =
            serde_json::from_value(input.clone()).map_err(|e| format!("invalid input: {e}"))?;
        if input.cancel {
            return Ok(if self.agent.cancel_schedule(&self.id, self.run)? {
                "The schedule is cancelled. End with a line saying so.".into()
            } else {
                "This task wasn't a schedule.".into()
            });
        }
        let at = schedule::parse_at(input.at.as_deref().ok_or("invalid input: at is required")?)?;
        let days = schedule::parse_days(input.days.as_ref())?;
        let prompt = given(input.prompt)
            .map(|p| cut(&p, PROMPT_CHARS))
            .ok_or("invalid input: prompt is required: what each run does")?;
        let next_run = schedule::next_run(at, days, store::now());
        let sched = store::Schedule {
            at: schedule::at_text(at),
            days,
            prompt,
            wake_board: input.wake_board,
            show_note: input.show_note,
            speak: input.speak,
            next_run,
            runs: Vec::new(),
            running_since: None,
        };
        let words = format!(
            "{} at {}, next {}",
            schedule::days_words(days),
            sched.at,
            schedule::local_text(next_run)
        );
        self.agent.set_schedule(&self.id, self.run, sched)?;
        Ok(format!(
            "Scheduled {words}. Each run does the prompt on its own. End now with one line \
             saying so."
        ))
    }

    fn watch(&self, input: &Value) -> Result<String, String> {
        let input: WatchInput =
            serde_json::from_value(input.clone()).map_err(|e| format!("invalid input: {e}"))?;
        let setting_up = input.every_minutes.is_some() || input.condition.is_some();
        if !setting_up && self.agent.is_watch(&self.id) {
            self.agent
                .watch_reading(&self.id, self.run, input.value, input.met)?;
            return Ok(if input.met {
                "Noted: the condition holds, so the watch ends. End with what you found: the \
                 user is told."
                    .into()
            } else {
                "Noted. End with one line on what you found.".into()
            });
        }
        let floor = match self.agent.backend_of(&self.id) {
            Some(Backend::Claude) => WATCH_FLOOR_CLAUDE,
            _ => WATCH_FLOOR_API,
        };
        let every = input
            .every_minutes
            .ok_or("invalid input: every_minutes is required")?;
        if every < floor {
            return Err(format!(
                "invalid input: every_minutes is at least {floor} (each look is a model run)"
            ));
        }
        let condition = given(input.condition)
            .map(|c| one_line(&c, CONDITION_CHARS))
            .ok_or("invalid input: condition is required: what to watch for")?;
        let prompt = given(input.prompt)
            .map(|p| cut(&p, PROMPT_CHARS))
            .unwrap_or_else(|| condition.clone());
        let next_look = store::now() + every * 60;
        self.agent.set_watch(
            &self.id,
            self.run,
            store::Watch {
                every_minutes: every,
                condition: condition.clone(),
                prompt,
                next_look,
                values: Vec::new(),
                looking_since: None,
                met: input.met,
            },
        )?;
        Ok(format!(
            "Watching every {every} minutes until: {condition}. The first look is at {}. If \
             you have a reading now, call watch again with value. End now with one line \
             saying so.",
            schedule::local_text(next_look)
        ))
    }
}

/// Talk's one way into the Agent: files a job that waits on the list until someone behind the
/// PIN starts it. Talk sees nothing of the sessions, this one included.
pub struct ProposeTool {
    pub agent: Arc<Agent>,
}

#[async_trait]
impl Extra for ProposeTool {
    fn defs(&self) -> Vec<ToolDef> {
        vec![propose_definition()]
    }

    async fn call(&self, name: &str, input: &Value) -> Option<Result<String, String>> {
        if name != PROPOSE_TOOL {
            return None;
        }
        let prompt = input["prompt"].as_str().map(str::trim).unwrap_or_default();
        if prompt.is_empty() {
            return Some(Err("invalid input: prompt is required".into()));
        }
        self.agent
            .propose(&cut(prompt, PROMPT_CHARS), input["title"].as_str());
        Some(Ok(
            "Filed. It waits on the Agent list until someone starts it there. Say so in a few \
             words; you can't see or change it."
                .into(),
        ))
    }
}

/// A call as a log line: the tool's name and a short form of its input (its text values, or
/// the JSON), as in `web_search plumbers open Saturday`.
pub fn short_call(name: &str, input: &Value) -> String {
    let args = match input {
        Value::Object(map) => {
            let words: Vec<String> = map
                .values()
                .filter_map(|v| match v {
                    Value::String(s) => Some(s.clone()),
                    Value::Number(n) => Some(n.to_string()),
                    Value::Bool(b) => Some(b.to_string()),
                    _ => None,
                })
                .collect();
            if words.is_empty() && !map.is_empty() {
                input.to_string()
            } else {
                words.join(" ")
            }
        }
        other => other.to_string(),
    };
    one_line(&format!("{name} {args}"), LOG_CHARS)
}

fn plan_definition() -> ToolDef {
    ToolDef {
        name: PLAN_TOOL.into(),
        description: format!(
            "Sets or updates this task's plan: a short checklist the user sees as a meter and a \
             list of steps. Call it first for any job with more than one step, and again as \
             you move on (current is the step you are working on, from 1; one past the last \
             when all are done). Steps of a few words each, at most {MAX_STEPS}. With the first \
             plan, give the task a title of at most {TITLE_CHARS} characters, such as \
             \"Plumber · kitchen leak\"."
        ),
        schema: json!({
            "type": "object",
            "properties": {
                "steps": {"type": "array", "items": {"type": "string"}, "description": "The steps, in order"},
                "current": {"type": "integer", "minimum": 1, "description": "The step being worked on now, from 1"},
                "title": {"type": "string", "description": "A short title for the task"},
                "expect_minutes": {"type": "integer", "minimum": 1, "description": "For a long job: how many minutes from now you expect to be done. The board checks back then"}
            },
            "required": ["steps"],
            "additionalProperties": false
        }),
    }
}

fn run_definition() -> ToolDef {
    ToolDef {
        name: super::herdr::RUN_TOOL.into(),
        description: "Starts a program in a new herdr terminal, here or on another herdr \
            machine, and gives it a task: a coding agent (Claude Code, Codex) or any command. \
            It runs on its own as a session of its own, which the user follows on their board; \
            this returns its id at once, so say where it started and finish."
            .into(),
        schema: json!({
            "type": "object",
            "properties": {
                "program": {"type": "string", "description": "A template name from [herdr] templates (such as claude or codex), or a command line"},
                "task": {"type": "string", "description": "What it should do, typed into it once it is ready"},
                "machine": {"type": "string", "description": "The herdr machine to start it on; leave it out for this one"},
                "dir": {"type": "string", "description": "The folder to start it in"}
            },
            "required": ["program", "task"],
            "additionalProperties": false
        }),
    }
}

fn ask_definition() -> ToolDef {
    ToolDef {
        name: ASK_TOOL.into(),
        description: format!(
            "Asks the user to choose, and waits for the answer: it shows on their board and \
             can take minutes or hours. Use it only when the choice is genuinely theirs (which \
             plumber to book, which flight to buy), not for things you can decide or the task \
             already asked for. Give {MIN_OPTIONS} to {MAX_OPTIONS} options; the board adds \
             \"something else\" itself. Returns {{\"choice\": n, \"title\": \"…\"}} when they \
             pick an option (n from 0), or {{\"text\": \"…\"}} when they answer in words. One \
             question at a time."
        ),
        schema: json!({
            "type": "object",
            "properties": {
                "question": {"type": "string", "description": "The question, in one short line"},
                "detail": {"type": "string", "description": "A sentence or two of context"},
                "options": {
                    "type": "array",
                    "minItems": MIN_OPTIONS,
                    "maxItems": MAX_OPTIONS,
                    "items": {
                        "type": "object",
                        "properties": {
                            "title": {"type": "string", "description": format!("At most {OPTION_TITLE_CHARS} characters")},
                            "right": {"type": "string", "description": format!("A short value shown at the right, such as a price or a time; at most {OPTION_RIGHT_CHARS} characters")},
                            "detail": {"type": "string", "description": format!("What the user should know to choose it; at most {OPTION_DETAIL_CHARS} characters")}
                        },
                        "required": ["title"],
                        "additionalProperties": false
                    }
                },
                "recommend": {"type": "integer", "minimum": 0, "description": "The option you recommend, from 0"},
                "deadline_minutes": {"type": "integer", "minimum": 1, "description": "When the choice can't wait: minutes to wait for an answer"},
                "then": {"type": "string", "enum": ["ask_again", "recommend", "give_up"], "description": "What happens at the deadline: ask again, take your recommendation (the default with one), or give up (the default without)"}
            },
            "required": ["question", "options"],
            "additionalProperties": false
        }),
    }
}

fn schedule_definition() -> ToolDef {
    ToolDef {
        name: SCHEDULE_TOOL.into(),
        description: "Makes this task a schedule: from now on it runs the prompt at a time of \
            day, on the days given, each run on its own, and the user sees each result on their \
            board. Use it when they ask for something every day or every week (\"every weekday \
            at seven, brief me\"). Call it again to change it, or with cancel to end it. Then \
            end this turn with one line saying what was scheduled."
            .into(),
        schema: json!({
            "type": "object",
            "properties": {
                "at": {"type": "string", "description": "The time of day, 24-hour, such as \"07:00\""},
                "days": {"description": "daily (the default), weekdays, weekends, or a list such as [\"mon\", \"thu\"]",
                         "anyOf": [{"type": "string"}, {"type": "array", "items": {"type": "string"}}]},
                "prompt": {"type": "string", "description": "What each run does, as a task in its own right: \"Brief me on today's calendar, the weather and the news\""},
                "wake_board": {"type": "boolean", "description": "Wake the board to show the result when it's ready (a morning brief)"},
                "show_note": {"type": "boolean", "description": "Show the result on the board as a note"},
                "speak": {"type": "boolean", "description": "Say the result aloud when the board is on USB"},
                "cancel": {"type": "boolean", "description": "End the schedule"}
            },
            "additionalProperties": false
        }),
    }
}

fn watch_definition() -> ToolDef {
    ToolDef {
        name: WATCH_TOOL.into(),
        description: format!(
            "Makes this task a watch: it looks again every so often until a condition holds \
             (a fare under $700, a parcel delivered), then the user is told. Set it up with \
             every_minutes (at least {WATCH_FLOOR_API}) and condition. At each look, call it \
             with value (a number to chart, such as the price) or with met: true once the \
             condition holds."
        ),
        schema: json!({
            "type": "object",
            "properties": {
                "every_minutes": {"type": "integer", "minimum": WATCH_FLOOR_API, "description": "How often to look"},
                "condition": {"type": "string", "description": "What it waits for, in a few words: \"the fare to Lisbon under $700\""},
                "prompt": {"type": "string", "description": "What to do at each look, if more than checking the condition"},
                "value": {"type": "number", "description": "At a look: the reading, for the chart"},
                "met": {"type": "boolean", "description": "At a look: the condition holds now"}
            },
            "additionalProperties": false
        }),
    }
}

fn propose_definition() -> ToolDef {
    ToolDef {
        name: PROPOSE_TOOL.into(),
        description: "Files a job for the agent, which works through longer tasks on its own: \
            research, comparisons, bookings, anything with many steps. You can't run it or see \
            it: it waits on the board's Agent list until the user starts it there. Use it when \
            they ask for something like that, or say \"make that a job\"."
            .into(),
        schema: json!({
            "type": "object",
            "properties": {
                "prompt": {"type": "string", "description": "The job, in full, as the agent should get it"},
                "title": {"type": "string", "description": format!("A short title, at most {TITLE_CHARS} characters")}
            },
            "required": ["prompt"],
            "additionalProperties": false
        }),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn calls_as_log_lines() {
        assert_eq!(
            short_call("web_search", &json!({"query": "plumbers open Saturday"})),
            "web_search plumbers open Saturday"
        );
        assert_eq!(
            short_call(
                "run_js",
                &json!({"code": "const a = 1;\nconst b = 2;\na + b"})
            ),
            "run_js const a = 1; const b = 2; a + b"
        );
        assert_eq!(short_call("list_speakers", &json!({})), "list_speakers");
        assert_eq!(
            short_call("home_set", &json!({"targets": ["a", "b"]})),
            r#"home_set {"targets":["a","b"]}"#
        );
        assert!(
            short_call("x", &json!({"q": "y".repeat(500)}))
                .chars()
                .count()
                <= LOG_CHARS
        );
    }

    #[test]
    fn answers_for_the_model() {
        let choice = Answer::Choice {
            choice: 1,
            title: "Northgate".into(),
        };
        assert_eq!(choice.for_model(), r#"{"choice":1,"title":"Northgate"}"#);
        assert_eq!(
            Answer::Text("none, find one for Saturday".into()).for_model(),
            r#"{"text":"none, find one for Saturday"}"#
        );
    }
}
