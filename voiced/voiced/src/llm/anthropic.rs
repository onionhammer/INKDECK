//! Claude through the Messages API: raw HTTP with server-sent events (there is no official
//! Rust SDK). The request and stream format; the tool loop is tool_loop.rs.

use anyhow::{Context, Result, anyhow, bail};
use async_trait::async_trait;
use eventsource_stream::Eventsource;
use futures_util::StreamExt;
use serde_json::{Value, json};

use super::tool_loop::{Call, CallResult, ChatApi, Round, Speaker, Stop};
use super::{Request, Role};
use crate::config;
use crate::tools::ToolDef;

const API_VERSION: &str = "2026-10-21";
const FALLBACK_BETA: &str = "server-side-fallback-2026-07-01";

pub struct Anthropic {
    http: reqwest::Client,
    url: String,
    key: String,
    model: String,
    effort: String,
    max_tokens: u32,
    fallbacks: bool,
}

impl Anthropic {
    pub fn new(
        http: reqwest::Client,
        base_url: &str,
        api_key_env: &str,
        model: &str,
        effort: &str,
        max_tokens: u32,
        fallbacks: bool,
    ) -> Result<Self> {
        let key = config::env_key(api_key_env)
            .with_context(|| format!("the anthropic backend needs an API key in ${api_key_env}"))?;
        Ok(Self {
            http,
            url: format!("{}/v1/messages", base_url.trim_end_matches('/')),
            key,
            model: model.to_string(),
            effort: effort.to_string(),
            max_tokens,
            fallbacks,
        })
    }

    fn body(&self, conversation: &Conversation, tools: &[ToolDef]) -> Value {
        let mut body = json!({
            "model": self.model,
            "max_tokens": self.max_tokens,
            "stream": true,
            "system": conversation.system,
            // Thinking can't be switched off on current models; effort sets its depth.
            "output_config": {"effort": self.effort},
            "messages": conversation.messages,
        });
        if !tools.is_empty() {
            let tools: Vec<Value> = tools
                .iter()
                .map(|t| {
                    json!({
                        "name": t.name,
                        "description": t.description,
                        "input_schema": t.schema,
                        // Inputs stream as generated; they are parsed strictly and checked
                        // against the tool's argument type before the tool runs.
                        "eager_input_streaming": true,
                    })
                })
                .collect();
            body["tools"] = Value::Array(tools);
        }
        if self.fallbacks {
            body["fallbacks"] = json!("default");
        }
        body
    }
}

pub struct Conversation {
    system: String,
    messages: Vec<Value>,
}

/// A content block as it accumulates from the stream.
#[derive(Default)]
pub struct Block {
    value: Value,
    /// A tool call's input, as the JSON text streamed so far.
    input_json: String,
}

fn append(block: &mut Value, field: &str, more: &str) {
    let so_far = block[field].as_str().unwrap_or_default();
    block[field] = Value::String(format!("{so_far}{more}"));
}

#[async_trait]
impl ChatApi for Anthropic {
    type Conversation = Conversation;
    /// The round's content blocks: thinking (sent back unchanged, signature and all), text and
    /// tool calls.
    type Said = Vec<Block>;

    fn start(&self, request: &Request<'_>) -> Conversation {
        let mut messages: Vec<Value> = request
            .history
            .iter()
            .map(|turn| {
                let role = match turn.role {
                    Role::User => "user",
                    Role::Assistant => "assistant",
                };
                json!({"role": role, "content": turn.text})
            })
            .collect();
        messages.push(json!({"role": "user", "content": request.user}));
        Conversation {
            system: request.system.to_string(),
            messages,
        }
    }

    async fn round(
        &self,
        conversation: &Conversation,
        tools: &[ToolDef],
        speaker: &mut Speaker,
    ) -> Result<Round<Vec<Block>>> {
        let mut request = self
            .http
            .post(&self.url)
            .header("x-api-key", &self.key)
            .header("anthropic-version", API_VERSION)
            .json(&self.body(conversation, tools));
        if self.fallbacks {
            request = request.header("anthropic-beta", FALLBACK_BETA);
        }
        let response = request.send().await.context("Claude request")?;
        let status = response.status();
        if !status.is_success() {
            bail!(
                "Claude returned {status}: {}",
                response.text().await.unwrap_or_default()
            );
        }

        let mut blocks: Vec<Block> = Vec::new();
        let mut stop_reason = String::new();
        let mut events = response.bytes_stream().eventsource();
        while let Some(event) = events.next().await {
            let event = event.map_err(|e| anyhow!("Claude stream: {e}"))?;
            let Ok(data) = serde_json::from_str::<Value>(&event.data) else {
                continue;
            };
            let index = data["index"].as_u64().unwrap_or(0) as usize;
            match data["type"].as_str().unwrap_or_default() {
                "content_block_start" => {
                    if blocks.len() <= index {
                        blocks.resize_with(index + 1, Block::default);
                    }
                    blocks[index].value = data["content_block"].clone();
                }
                "content_block_delta" => {
                    let Some(block) = blocks.get_mut(index) else {
                        continue;
                    };
                    let delta = &data["delta"];
                    match delta["type"].as_str().unwrap_or_default() {
                        "text_delta" => {
                            let text = delta["text"].as_str().unwrap_or_default();
                            append(&mut block.value, "text", text);
                            speaker.say(text).await?;
                        }
                        "input_json_delta" => {
                            block
                                .input_json
                                .push_str(delta["partial_json"].as_str().unwrap_or_default());
                        }
                        "thinking_delta" => {
                            append(
                                &mut block.value,
                                "thinking",
                                delta["thinking"].as_str().unwrap_or_default(),
                            );
                        }
                        "signature_delta" => block.value["signature"] = delta["signature"].clone(),
                        _ => {}
                    }
                }
                "message_delta" => {
                    if let Some(reason) = data["delta"]["stop_reason"].as_str() {
                        stop_reason = reason.to_string();
                    }
                }
                "error" => bail!("Claude stream error: {}", data["error"]),
                _ => {}
            }
        }

        let calls = blocks
            .iter()
            .filter(|b| b.value["type"] == "tool_use")
            .map(|b| {
                Call::new(
                    b.value["id"].as_str().unwrap_or_default().to_string(),
                    b.value["name"].as_str().unwrap_or_default().to_string(),
                    b.input_json.clone(),
                )
            })
            .collect();
        let stop = match stop_reason.as_str() {
            "tool_use" => Stop::ToolUse,
            "refusal" => Stop::Refused,
            "max_tokens" => Stop::MaxTokens,
            _ => Stop::Done,
        };
        Ok(Round {
            stop,
            calls,
            said: blocks,
        })
    }

    fn add_round(
        &self,
        conversation: &mut Conversation,
        blocks: Vec<Block>,
        calls: &[Call],
        results: &[CallResult],
    ) {
        // The blocks go back as they came, each call's input an object even if the model's
        // wasn't.
        let content: Vec<Value> = blocks
            .into_iter()
            .map(|mut block| {
                if block.value["type"] == "tool_use" {
                    let input = calls
                        .iter()
                        .find(|c| block.value["id"] == c.id.as_str())
                        .and_then(|c| c.input.clone());
                    block.value["input"] = input.unwrap_or_else(|| json!({}));
                }
                block.value
            })
            .collect();
        // All results go back in one user message.
        let results: Vec<Value> = calls
            .iter()
            .zip(results)
            .map(|(call, result)| {
                let mut block =
                    json!({"type": "tool_result", "tool_use_id": call.id, "content": result.text});
                if result.is_error {
                    block["is_error"] = json!(true);
                }
                block
            })
            .collect();
        conversation
            .messages
            .push(json!({"role": "assistant", "content": content}));
        conversation
            .messages
            .push(json!({"role": "user", "content": results}));
    }
}

#[cfg(test)]
mod tests {
    use super::super::Outcome;
    use super::super::testserver::{collect, serve};
    use super::super::tool_loop::ToolLoop;
    use super::*;

    fn sse(events: &[Value]) -> String {
        events
            .iter()
            .map(|e| format!("event: {}\ndata: {e}\n\n", e["type"].as_str().unwrap()))
            .collect()
    }

    fn client(url: &str) -> ToolLoop<Anthropic> {
        ToolLoop(Anthropic {
            http: reqwest::Client::new(),
            url: format!("{url}/v1/messages"),
            key: "test".into(),
            model: "claude-opus-5-5".into(),
            effort: "low".into(),
            max_tokens: 1000,
            fallbacks: true,
        })
    }

    #[tokio::test]
    async fn tool_round_then_answer() {
        let first = sse(&[
            json!({"type": "message_start", "message": {"id": "msg_1"}}),
            json!({"type": "content_block_start", "index": 0, "content_block": {"type": "thinking", "thinking": "", "signature": ""}}),
            json!({"type": "content_block_delta", "index": 0, "delta": {"type": "signature_delta", "signature": "SIG"}}),
            json!({"type": "content_block_stop", "index": 0}),
            json!({"type": "content_block_start", "index": 1, "content_block": {"type": "text", "text": ""}}),
            json!({"type": "content_block_delta", "index": 1, "delta": {"type": "text_delta", "text": "Let me "}}),
            json!({"type": "ping"}),
            json!({"type": "content_block_delta", "index": 1, "delta": {"type": "text_delta", "text": "check."}}),
            json!({"type": "content_block_stop", "index": 1}),
            json!({"type": "content_block_start", "index": 2, "content_block": {"type": "tool_use", "id": "toolu_1", "name": "play_music", "input": {}}}),
            json!({"type": "content_block_delta", "index": 2, "delta": {"type": "input_json_delta", "partial_json": "{\"speaker\": \"media_pl"}}),
            json!({"type": "content_block_delta", "index": 2, "delta": {"type": "input_json_delta", "partial_json": "ayer.kitchen\", \"query\": \"jazz\"}"}}),
            json!({"type": "content_block_stop", "index": 2}),
            json!({"type": "content_block_start", "index": 3, "content_block": {"type": "tool_use", "id": "toolu_2", "name": "list_speakers", "input": {}}}),
            json!({"type": "content_block_delta", "index": 3, "delta": {"type": "input_json_delta", "partial_json": "{\"broken"}}),
            json!({"type": "content_block_stop", "index": 3}),
            json!({"type": "message_delta", "delta": {"stop_reason": "tool_use"}}),
            json!({"type": "message_stop"}),
        ]);
        let second = sse(&[
            json!({"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}}),
            json!({"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "No speakers are set up."}}),
            json!({"type": "content_block_stop", "index": 0}),
            json!({"type": "message_delta", "delta": {"stop_reason": "end_turn"}}),
            json!({"type": "message_stop"}),
        ]);
        let (url, mut bodies) = serve(vec![first, second]).await;
        let (result, text) = collect(&client(&url), "play jazz in the kitchen").await;
        assert_eq!(result.unwrap(), Outcome::Done);
        assert_eq!(text, "Let me check.\nNo speakers are set up.");

        let request1 = bodies.recv().await.unwrap();
        assert_eq!(request1["stream"], true);
        assert_eq!(request1["output_config"]["effort"], "low");
        assert_eq!(request1["fallbacks"], "default");
        assert_eq!(
            request1["messages"][0],
            json!({"role": "user", "content": "play jazz in the kitchen"})
        );

        let request2 = bodies.recv().await.unwrap();
        let assistant = &request2["messages"][1];
        assert_eq!(assistant["role"], "assistant");
        assert_eq!(
            assistant["content"][0],
            json!({"type": "thinking", "thinking": "", "signature": "SIG"})
        );
        assert_eq!(assistant["content"][1]["text"], "Let me check.");
        assert_eq!(
            assistant["content"][2]["input"],
            json!({"speaker": "media_player.kitchen", "query": "jazz"})
        );
        assert_eq!(assistant["content"][3]["input"], json!({}));
        let results = &request2["messages"][2]["content"];
        assert_eq!(results[0]["tool_use_id"], "toolu_1");
        assert_eq!(results[0]["is_error"], true); // no Home Assistant in this test
        assert_eq!(results[1]["tool_use_id"], "toolu_2");
        assert_eq!(
            results[1]["content"],
            json!({"INVALID_JSON": "{\"broken"}).to_string()
        );
    }

    #[tokio::test]
    async fn refusal_and_stream_errors_fail_the_turn() {
        let refusal = sse(&[json!({"type": "message_delta", "delta": {"stop_reason": "refusal"}})]);
        let (url, _bodies) = serve(vec![refusal]).await;
        let (result, _) = collect(&client(&url), "hi").await;
        assert!(result.unwrap_err().to_string().contains("declined"));

        let error = sse(&[
            json!({"type": "error", "error": {"type": "overloaded_error", "message": "busy"}}),
        ]);
        let (url, _bodies) = serve(vec![error]).await;
        let (result, _) = collect(&client(&url), "hi").await;
        assert!(result.unwrap_err().to_string().contains("overloaded_error"));
    }
}
