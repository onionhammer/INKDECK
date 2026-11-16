//! The turn every backend shares: ask the model, run the tools it calls, give it the results,
//! and repeat until it answers. A provider supplies only its API ([`ChatApi`]): how the
//! conversation is written, how one streamed round is read, and how a round's tool results go
//! back. [`ToolLoop`] does the rest, the same way for all of them.

use anyhow::{Result, anyhow, bail};
use async_trait::async_trait;
use futures_util::future::join_all;
use serde_json::{Value, json};
use tokio::sync::mpsc;

use super::{Llm, Outcome, Request};
use crate::tools::{THINK_TOOL, ToolDef, ToolSet};

/// Tool-call rounds allowed in one turn before giving up.
pub const MAX_ROUNDS: usize = 6;

/// One provider's chat API.
#[async_trait]
pub trait ChatApi: Send + Sync {
    /// The conversation so far, in this API's format.
    type Conversation: Send + Sync;
    /// What the model produced in a round, kept to be sent back with the tool results (the
    /// API's own blocks: text, thinking with its signature, the tool calls).
    type Said: Send;

    /// The conversation at the start of a turn: system prompt, history, the user's words.
    fn start(&self, request: &Request<'_>) -> Self::Conversation;

    /// Sends the conversation and streams the model's next round: its text to `speaker` as it
    /// arrives, its tool calls and why it stopped in the result.
    async fn round(
        &self,
        conversation: &Self::Conversation,
        tools: &[ToolDef],
        speaker: &mut Speaker,
    ) -> Result<Round<Self::Said>>;

    /// Adds the round the model produced and the results of its calls, in order.
    fn add_round(
        &self,
        conversation: &mut Self::Conversation,
        said: Self::Said,
        calls: &[Call],
        results: &[CallResult],
    );
}

/// What one round produced.
pub struct Round<S> {
    pub stop: Stop,
    pub calls: Vec<Call>,
    pub said: S,
}

/// Why the model stopped.
#[derive(Debug, PartialEq)]
pub enum Stop {
    /// It answered.
    Done,
    /// It wants its tool calls run.
    ToolUse,
    /// It hit the token limit; what it said stands.
    MaxTokens,
    /// It declined to answer.
    Refused,
}

/// A tool call as the model wrote it.
#[derive(Debug, Clone)]
pub struct Call {
    pub id: String,
    pub name: String,
    /// The arguments, if they are a JSON object (empty text counts as `{}`).
    pub input: Option<Value>,
    /// The arguments as streamed.
    pub raw: String,
}

impl Call {
    pub fn new(id: String, name: String, raw: String) -> Call {
        let text = if raw.trim().is_empty() {
            "{}"
        } else {
            raw.as_str()
        };
        let input = serde_json::from_str::<Value>(text)
            .ok()
            .filter(Value::is_object);
        Call {
            id,
            name,
            input,
            raw,
        }
    }
}

/// A tool's answer, for the model.
#[derive(Debug, Clone, PartialEq)]
pub struct CallResult {
    pub text: String,
    pub is_error: bool,
}

/// Passes the reply's text on as it is generated. Text that follows text from an earlier
/// round starts on a new line; blank lines a model opens with are dropped.
pub struct Speaker {
    out: mpsc::Sender<String>,
    spoke: bool,
    round_start: bool,
}

impl Speaker {
    fn new(out: mpsc::Sender<String>) -> Self {
        Speaker {
            out,
            spoke: false,
            round_start: true,
        }
    }

    pub async fn say(&mut self, piece: &str) -> Result<()> {
        let piece = if self.round_start {
            piece.trim_start()
        } else {
            piece
        };
        if piece.is_empty() {
            return Ok(());
        }
        let text = if self.round_start && self.spoke {
            format!("\n{piece}")
        } else {
            piece.to_string()
        };
        self.round_start = false;
        self.spoke = true;
        self.out
            .send(text)
            .await
            .map_err(|_| anyhow!("the reply was cancelled"))
    }

    fn next_round(&mut self) {
        self.round_start = true;
    }
}

/// A backend: a provider's API driven by the shared loop.
pub struct ToolLoop<A>(pub A);

#[async_trait]
impl<A: ChatApi> Llm for ToolLoop<A> {
    async fn respond(&self, request: Request<'_>, out: mpsc::Sender<String>) -> Result<Outcome> {
        let api = &self.0;
        let mut conversation = api.start(&request);
        let mut speaker = Speaker::new(out);
        for _ in 0..MAX_ROUNDS {
            speaker.next_round();
            let round = api
                .round(&conversation, request.tools.defs(), &mut speaker)
                .await?;
            match round.stop {
                Stop::Refused => bail!("the model declined to answer"),
                Stop::MaxTokens => {
                    tracing::warn!("reply cut off at max_tokens");
                    return Ok(Outcome::Done);
                }
                Stop::Done => return Ok(Outcome::Done),
                Stop::ToolUse if round.calls.is_empty() => return Ok(Outcome::Done),
                Stop::ToolUse => {}
            }
            if round.calls.iter().any(|c| c.name == THINK_TOOL) {
                return Ok(Outcome::Think {
                    spoke: speaker.spoke,
                });
            }
            let results = join_all(round.calls.iter().map(|c| run(request.tools, c))).await;
            api.add_round(&mut conversation, round.said, &round.calls, &results);
        }
        bail!("the model kept calling tools ({MAX_ROUNDS} rounds)")
    }
}

/// Runs one call. Arguments that aren't a JSON object come back as an error naming them.
async fn run(tools: &ToolSet, call: &Call) -> CallResult {
    let result = match &call.input {
        Some(input) => {
            tracing::info!(tool = %call.name, %input, "tool call");
            tools.call(&call.name, input.clone()).await
        }
        None => Err(json!({"INVALID_JSON": call.raw}).to_string()),
    };
    match result {
        Ok(text) => CallResult {
            text,
            is_error: false,
        },
        Err(text) => {
            tracing::warn!(tool = %call.name, error = %text, "tool failed");
            CallResult {
                text,
                is_error: true,
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;

    use super::super::testserver::collect;
    use super::*;

    /// A scripted API: each round says its pieces and makes its calls; the conversation
    /// records what the loop sent back.
    /// What a scripted round says, the calls it makes, and why it stops.
    type Scripted = (Vec<&'static str>, Vec<Call>, Stop);

    struct Script(Mutex<Vec<Scripted>>);

    #[async_trait]
    impl ChatApi for Script {
        type Conversation = Vec<String>;
        type Said = ();

        fn start(&self, request: &Request<'_>) -> Vec<String> {
            vec![format!("user: {}", request.user)]
        }

        async fn round(
            &self,
            _: &Vec<String>,
            _: &[ToolDef],
            speaker: &mut Speaker,
        ) -> Result<Round<()>> {
            let (pieces, calls, stop) = self.0.lock().unwrap().remove(0);
            for piece in pieces {
                speaker.say(piece).await?;
            }
            Ok(Round {
                stop,
                calls,
                said: (),
            })
        }

        fn add_round(&self, c: &mut Vec<String>, _: (), calls: &[Call], results: &[CallResult]) {
            for (call, result) in calls.iter().zip(results) {
                c.push(format!("{} -> {:?}", call.name, result));
            }
        }
    }

    fn call(name: &str, raw: &str) -> Call {
        Call::new("id".into(), name.into(), raw.into())
    }

    fn script(rounds: Vec<Scripted>) -> ToolLoop<Script> {
        ToolLoop(Script(Mutex::new(rounds)))
    }

    #[tokio::test]
    async fn rounds_join_on_new_lines() {
        let llm = script(vec![
            (
                vec!["\n\n", "Let me ", "check."],
                vec![call("run_js", r#"{"code": "1+1"}"#)],
                Stop::ToolUse,
            ),
            (vec![], vec![call("run_js", "{bad")], Stop::ToolUse),
            (vec!["\n", "It is 2."], vec![], Stop::Done),
        ]);
        let (result, text) = collect(&llm, "one plus one").await;
        assert_eq!(result.unwrap(), Outcome::Done);
        assert_eq!(text, "Let me check.\nIt is 2.");
    }

    #[tokio::test]
    async fn first_text_in_a_later_round_has_no_newline() {
        let llm = script(vec![
            (
                vec![],
                vec![call("run_js", r#"{"code": "2"}"#)],
                Stop::ToolUse,
            ),
            (vec!["Two."], vec![], Stop::Done),
        ]);
        assert_eq!(collect(&llm, "q").await.1, "Two.");
    }

    #[tokio::test]
    async fn think_hands_off_and_stops_end_the_turn() {
        let llm = script(vec![(
            vec!["Hmm."],
            vec![call("think_harder", ""), call("run_js", "{}")],
            Stop::ToolUse,
        )]);
        assert_eq!(
            collect(&llm, "q").await.0.unwrap(),
            Outcome::Think { spoke: true }
        );

        let llm = script(vec![(vec![], vec![], Stop::Refused)]);
        assert!(
            collect(&llm, "q")
                .await
                .0
                .unwrap_err()
                .to_string()
                .contains("declined")
        );

        let llm = script(vec![(
            vec!["Cut"],
            vec![call("run_js", "{}")],
            Stop::MaxTokens,
        )]);
        let (result, text) = collect(&llm, "q").await;
        assert_eq!((result.unwrap(), text.as_str()), (Outcome::Done, "Cut"));

        let endless = (0..MAX_ROUNDS)
            .map(|_| {
                (
                    vec![],
                    vec![call("run_js", r#"{"code": "1"}"#)],
                    Stop::ToolUse,
                )
            })
            .collect();
        assert!(
            collect(&script(endless), "q")
                .await
                .0
                .unwrap_err()
                .to_string()
                .contains("kept calling")
        );
    }

    #[test]
    fn call_arguments() {
        assert_eq!(call("t", "").input, Some(json!({})));
        assert_eq!(call("t", r#"{"a": 1}"#).input, Some(json!({"a": 1})));
        assert_eq!(call("t", "[1]").input, None);
        assert_eq!(call("t", "{bad").input, None);
    }
}
