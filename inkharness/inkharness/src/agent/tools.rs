//! The tools only sessions get: `ask_choice`, which puts a question to the user and waits for
//! the answer, and `plan`, the checklist a board shows as a meter.

use std::sync::Arc;

use async_trait::async_trait;
use serde::Deserialize;
use serde_json::{Value, json};
use voice_proto::AskOption;

use super::Agent;
use super::store::{LOG_CHARS, Question, TITLE_CHARS, cut, one_line};
use crate::tools::{Extra, ToolDef};

pub const ASK_TOOL: &str = "ask_choice";
pub const PLAN_TOOL: &str = "plan";

const MIN_OPTIONS: usize = 2;
const MAX_OPTIONS: usize = 6;
const OPTION_TITLE_CHARS: usize = 32;
const OPTION_RIGHT_CHARS: usize = 12;
const OPTION_DETAIL_CHARS: usize = 400;
const QUESTION_CHARS: usize = 200;
const DETAIL_CHARS: usize = 400;
const MAX_STEPS: usize = 12;
const STEP_CHARS: usize = 60;

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
}

#[async_trait]
impl Extra for SessionTools {
    fn defs(&self) -> Vec<ToolDef> {
        vec![plan_definition(), ask_definition()]
    }

    async fn call(&self, name: &str, input: &Value) -> Option<Result<String, String>> {
        Some(match name {
            ASK_TOOL => self.ask(input).await,
            PLAN_TOOL => self.plan(input),
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
        let question = Question {
            id: String::new(), // the agent numbers it
            question,
            detail: given(input.detail).map(|d| cut(&d, DETAIL_CHARS)),
            recommend: input.recommend.filter(|r| *r < options.len()),
            options,
            at: 0,
        };
        let answer = self.agent.put_question(&self.id, question)?;
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
        self.agent.set_plan(&self.id, steps, current, title)?;
        Ok(if current > n {
            format!("Plan saved: all {n} steps done.")
        } else {
            format!("Plan saved: step {current} of {n}.")
        })
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
                "title": {"type": "string", "description": "A short title for the task"}
            },
            "required": ["steps"],
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
                "recommend": {"type": "integer", "minimum": 0, "description": "The option you recommend, from 0"}
            },
            "required": ["question", "options"],
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
