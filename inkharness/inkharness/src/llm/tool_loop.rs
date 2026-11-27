//! The turn every backend shares: ask the model, run the tools it calls, give it the results,
//! and repeat until it answers. A provider supplies only its API ([`ChatApi`]): how the
//! conversation is written, how one streamed round is read, and how a round's tool results go
//! back. [`ToolLoop`] does the rest, the same way for all of them.

use anyhow::{Result, anyhow, bail};
use async_trait::async_trait;
use bytes::Bytes;
use futures_util::future::join_all;
use serde_json::{Value, json};
use tokio::sync::mpsc;

use super::{Llm, Outcome, Piece, Request, TooManyRounds};
use crate::tools::{THINK_TOOL, ToolDef, ToolSet};

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

    /// See [`Llm::hears`].
    fn hears(&self) -> bool {
        false
    }

    /// See [`Llm::speaks`].
    fn speaks(&self) -> Option<u32> {
        None
    }

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

    /// Adds what the user said after a round's tool results, for the next round to read
    /// ([`Request::steer`]).
    fn add_user(&self, conversation: &mut Self::Conversation, text: &str);
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

/// Passes the reply on as it is generated. Text that follows text from an earlier round
/// starts on a new line; blank lines a model opens with are dropped.
pub struct Speaker {
    out: mpsc::Sender<Piece>,
    spoke: bool,
    round_start: bool,
}

impl Speaker {
    fn new(out: mpsc::Sender<Piece>) -> Self {
        Speaker {
            out,
            spoke: false,
            round_start: true,
        }
    }

    /// Text to show and speak.
    pub async fn say(&mut self, piece: &str) -> Result<()> {
        match self.join(piece) {
            Some(text) => self.send(Piece::Text(text)).await,
            None => Ok(()),
        }
    }

    /// The transcript of what the model says in its audio: shown only.
    pub async fn transcript(&mut self, piece: &str) -> Result<()> {
        match self.join(piece) {
            Some(text) => self.send(Piece::Transcript(text)).await,
            None => Ok(()),
        }
    }

    /// The model's own speech.
    pub async fn audio(&mut self, pcm: Bytes) -> Result<()> {
        if pcm.is_empty() {
            return Ok(());
        }
        self.spoke = true;
        self.send(Piece::Audio(pcm)).await
    }

    /// The piece as sent: trimmed at a round's start, after a newline if an earlier round
    /// spoke. None if nothing is left.
    fn join(&mut self, piece: &str) -> Option<String> {
        let piece = if self.round_start {
            piece.trim_start()
        } else {
            piece
        };
        if piece.is_empty() {
            return None;
        }
        let text = if self.round_start && self.spoke {
            format!("\n{piece}")
        } else {
            piece.to_string()
        };
        self.round_start = false;
        self.spoke = true;
        Some(text)
    }

    async fn send(&self, piece: Piece) -> Result<()> {
        self.out
            .send(piece)
            .await
            .map_err(|_| anyhow!("the reply was cancelled"))
    }

    /// A tool call starting.
    pub async fn tool(&mut self, name: &str, input: Option<&Value>) -> Result<()> {
        let input = input.cloned().unwrap_or(Value::Null);
        self.send(Piece::Tool(name.to_string(), input)).await
    }

    fn next_round(&mut self) {
        self.round_start = true;
    }
}

/// Told to the model before the last round a turn allows.
pub const LAST_ROUND: &str = "That is as many tool calls as this turn allows: don't call any \
     more tools. Answer now with what you found and what you did, and say plainly what is \
     left undone.";

/// A backend: a provider's API driven by the shared loop.
pub struct ToolLoop<A>(pub A);

#[async_trait]
impl<A: ChatApi> Llm for ToolLoop<A> {
    fn hears(&self) -> bool {
        self.0.hears()
    }

    fn speaks(&self) -> Option<u32> {
        self.0.speaks()
    }

    async fn respond(&self, request: Request<'_>, out: mpsc::Sender<Piece>) -> Result<Outcome> {
        let api = &self.0;
        let mut conversation = api.start(&request);
        let mut speaker = Speaker::new(out);
        for round in 0..request.rounds {
            if round > 0 && round + 1 == request.rounds {
                // Rather than fail with nothing said, the last round answers with what it has.
                api.add_user(&mut conversation, LAST_ROUND);
            }
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
            for call in &round.calls {
                speaker.tool(&call.name, call.input.as_ref()).await?;
            }
            let done = speaker.out.clone();
            let results = join_all(round.calls.iter().map(|call| {
                let done = done.clone();
                async move {
                    let result = run(request.tools, call).await;
                    // Only shown; a reader that went away ends the turn at the next piece.
                    let _ = done
                        .send(Piece::ToolDone(call.name.clone(), !result.is_error))
                        .await;
                    result
                }
            }))
            .await;
            api.add_round(&mut conversation, round.said, &round.calls, &results);
            for text in request.steer.map(|steer| steer()).unwrap_or_default() {
                api.add_user(&mut conversation, &text);
            }
        }
        Err(TooManyRounds(request.rounds).into())
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

    /// The rounds to play, and the conversation each was sent.
    struct Script(Mutex<Vec<Scripted>>, Mutex<Vec<Vec<String>>>);

    #[async_trait]
    impl ChatApi for Script {
        type Conversation = Vec<String>;
        type Said = ();

        fn start(&self, request: &Request<'_>) -> Vec<String> {
            vec![format!("user: {}", request.user)]
        }

        async fn round(
            &self,
            conversation: &Vec<String>,
            _: &[ToolDef],
            speaker: &mut Speaker,
        ) -> Result<Round<()>> {
            self.1.lock().unwrap().push(conversation.clone());
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

        fn add_user(&self, c: &mut Vec<String>, text: &str) {
            c.push(format!("user: {text}"));
        }
    }

    fn call(name: &str, raw: &str) -> Call {
        Call::new("id".into(), name.into(), raw.into())
    }

    fn script(rounds: Vec<Scripted>) -> ToolLoop<Script> {
        ToolLoop(Script(Mutex::new(rounds), Mutex::default()))
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
    async fn each_call_says_when_it_is_done() {
        let llm = script(vec![
            (
                vec![],
                vec![call("run_js", r#"{"code": "1+1"}"#), call("run_js", "{bad")],
                Stop::ToolUse,
            ),
            (vec!["Done."], vec![], Stop::Done),
        ]);
        let tools = crate::tools::ToolSet::build(
            None,
            &crate::config::Js::default(),
            Default::default(),
            Default::default(),
            reqwest::Client::new(),
        )
        .unwrap();
        let (tx, mut rx) = mpsc::channel(64);
        let request = Request {
            system: "s",
            history: &[],
            user: "q",
            audio: None,
            tools: &tools,
            rounds: 4,
            steer: None,
        };
        let (result, pieces) = tokio::join!(llm.respond(request, tx), async {
            let mut pieces = Vec::new();
            while let Some(piece) = rx.recv().await {
                pieces.push(piece);
            }
            pieces
        });
        result.unwrap();
        let marks: Vec<String> = pieces
            .iter()
            .filter_map(|p| match p {
                Piece::Tool(name, _) => Some(format!("start {name}")),
                Piece::ToolDone(name, ok) => Some(format!("done {name} {ok}")),
                _ => None,
            })
            .collect();
        assert_eq!(marks[..2], ["start run_js", "start run_js"]);
        // The calls run together, so either may finish first.
        let mut done = marks[2..].to_vec();
        done.sort();
        assert_eq!(done, ["done run_js false", "done run_js true"]);
    }

    #[tokio::test]
    async fn the_last_round_is_told_to_answer() {
        let calls = || {
            (
                vec![],
                vec![call("run_js", r#"{"code": "1"}"#)],
                Stop::ToolUse,
            )
        };
        let llm = script(vec![
            calls(),
            calls(),
            (vec!["Did two."], vec![], Stop::Done),
        ]);
        let tools = crate::tools::ToolSet::build(
            None,
            &crate::config::Js::default(),
            Default::default(),
            Default::default(),
            reqwest::Client::new(),
        )
        .unwrap();
        let (tx, mut rx) = mpsc::channel(64);
        let request = Request {
            system: "s",
            history: &[],
            user: "q",
            audio: None,
            tools: &tools,
            rounds: 3,
            steer: None,
        };
        let drain = async { while rx.recv().await.is_some() {} };
        let (result, ()) = tokio::join!(llm.respond(request, tx), drain);
        assert_eq!(result.unwrap(), Outcome::Done);
        let sent = llm.0.1.lock().unwrap();
        let told = |round: &Vec<String>| round.iter().any(|m| m == &format!("user: {LAST_ROUND}"));
        assert!(!told(&sent[1]), "{:?}", sent[1]);
        assert!(told(&sent[2]), "{:?}", sent[2]);
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

        let endless = (0..crate::llm::TALK_ROUNDS)
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

    #[tokio::test]
    async fn a_request_sets_its_rounds_and_hears_what_was_said_meanwhile() {
        let llm = script(vec![
            (
                vec!["Looking."],
                vec![call("run_js", r#"{"code": "1"}"#)],
                Stop::ToolUse,
            ),
            (
                vec![],
                vec![call("run_js", r#"{"code": "2"}"#)],
                Stop::ToolUse,
            ),
            (vec!["Done."], vec![], Stop::Done),
        ]);
        let tools = ToolSet::build(
            None,
            &crate::config::Js::default(),
            Default::default(),
            Default::default(),
            reqwest::Client::new(),
        )
        .unwrap();
        let said = Mutex::new(vec!["make it Saturday".to_string()]);
        let steer = || std::mem::take(&mut *said.lock().unwrap());
        let (tx, mut rx) = mpsc::channel(64);
        let request = Request {
            system: "sys",
            history: &[],
            user: "find a plumber",
            audio: None,
            tools: &tools,
            rounds: 3,
            steer: Some(&steer),
        };
        let drain = async { while rx.recv().await.is_some() {} };
        let (outcome, ()) = tokio::join!(llm.respond(request, tx), drain);
        assert_eq!(outcome.unwrap(), Outcome::Done);
        let seen = llm.0.1.lock().unwrap().clone();
        assert_eq!(
            seen[1],
            [
                "user: find a plumber",
                r#"run_js -> CallResult { text: "=> 1", is_error: false }"#,
                "user: make it Saturday"
            ]
        );
        let steered = seen[2].iter().filter(|m| *m == "user: make it Saturday");
        assert_eq!(steered.count(), 1, "said once: {:?}", seen[2]);
        // The third round is the last of three: it's told to answer.
        assert_eq!(seen[2].last().unwrap(), &format!("user: {LAST_ROUND}"));

        // Three rounds of calls with only three allowed.
        let llm = script(
            (0..3)
                .map(|_| {
                    (
                        vec![],
                        vec![call("run_js", r#"{"code": "1"}"#)],
                        Stop::ToolUse,
                    )
                })
                .collect(),
        );
        let (tx, _rx) = mpsc::channel(64);
        let request = Request {
            system: "sys",
            history: &[],
            user: "q",
            audio: None,
            tools: &tools,
            rounds: 3,
            steer: None,
        };
        let err = llm.respond(request, tx).await.unwrap_err();
        assert!(err.downcast_ref::<TooManyRounds>().is_some(), "{err}");
    }

    #[test]
    fn call_arguments() {
        assert_eq!(call("t", "").input, Some(json!({})));
        assert_eq!(call("t", r#"{"a": 1}"#).input, Some(json!({"a": 1})));
        assert_eq!(call("t", "[1]").input, None);
        assert_eq!(call("t", "{bad").input, None);
    }
}
