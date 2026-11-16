//! Any server with OpenAI's chat completions API (Ollama, llama.cpp, vLLM, hosted services,
//! OpenRouter): the request and stream format. The tool loop is tool_loop.rs.

use anyhow::{Context, Result, anyhow, bail};
use async_trait::async_trait;
use eventsource_stream::Eventsource;
use futures_util::StreamExt;
use serde_json::{Value, json};

use super::tool_loop::{Call, CallResult, ChatApi, Round, Speaker, Stop};
use super::{Request, Role};
use crate::config;
use crate::tools::ToolDef;

pub struct OpenAiCompat {
    http: reqwest::Client,
    url: String,
    key: Option<String>,
    model: String,
    max_tokens: u32,
    /// OpenRouter's `reasoning` parameter, sent with every request when set.
    reasoning: Option<Value>,
}

impl OpenAiCompat {
    pub fn new(
        http: reqwest::Client,
        base_url: &str,
        api_key_env: &str,
        model: &str,
        max_tokens: u32,
        reasoning: Option<Value>,
    ) -> Self {
        Self {
            http,
            url: format!("{}/chat/completions", base_url.trim_end_matches('/')),
            key: config::env_key(api_key_env),
            model: model.to_string(),
            max_tokens,
            reasoning,
        }
    }

    fn body(&self, messages: &[Value], tools: &[ToolDef]) -> Value {
        let mut body = json!({
            "model": self.model,
            "stream": true,
            "max_tokens": self.max_tokens,
            "messages": messages,
        });
        if !tools.is_empty() {
            let tools: Vec<Value> = tools
                .iter()
                .map(|t| {
                    json!({"type": "function", "function": {
                        "name": t.name, "description": t.description, "parameters": t.schema,
                    }})
                })
                .collect();
            body["tools"] = Value::Array(tools);
        }
        if let Some(reasoning) = &self.reasoning {
            body["reasoning"] = reasoning.clone();
        }
        body
    }
}

/// A tool call as its pieces stream in.
#[derive(Default, Clone)]
struct Partial {
    id: String,
    name: String,
    arguments: String,
}

#[async_trait]
impl ChatApi for OpenAiCompat {
    /// The messages, system prompt first.
    type Conversation = Vec<Value>;
    /// The round's text, sent back as the assistant message's content.
    type Said = String;

    fn start(&self, request: &Request<'_>) -> Vec<Value> {
        let mut messages = vec![json!({"role": "system", "content": request.system})];
        for turn in request.history {
            let role = match turn.role {
                Role::User => "user",
                Role::Assistant => "assistant",
            };
            messages.push(json!({"role": role, "content": turn.text}));
        }
        messages.push(json!({"role": "user", "content": request.user}));
        messages
    }

    async fn round(
        &self,
        messages: &Vec<Value>,
        tools: &[ToolDef],
        speaker: &mut Speaker,
    ) -> Result<Round<String>> {
        let mut request = self.http.post(&self.url).json(&self.body(messages, tools));
        if let Some(key) = &self.key {
            request = request.bearer_auth(key);
        }
        let response = request.send().await.context("chat request")?;
        let status = response.status();
        if !status.is_success() {
            bail!(
                "chat returned {status}: {}",
                response.text().await.unwrap_or_default()
            );
        }
        let mut text = String::new();
        let mut partials: Vec<Partial> = Vec::new();
        let mut finish = String::new();
        let mut events = response.bytes_stream().eventsource();
        while let Some(event) = events.next().await {
            let event = event.map_err(|e| anyhow!("chat stream: {e}"))?;
            if event.data.trim() == "[DONE]" {
                break;
            }
            let Ok(data) = serde_json::from_str::<Value>(&event.data) else {
                continue;
            };
            if !data["error"].is_null() {
                bail!("chat stream error: {}", data["error"]);
            }
            let choice = &data["choices"][0];
            if let Some(reason) = choice["finish_reason"].as_str() {
                finish = reason.to_string();
            }
            let delta = &choice["delta"];
            if let Some(piece) = delta["content"].as_str().filter(|p| !p.is_empty()) {
                text.push_str(piece);
                speaker.say(piece).await?;
            }
            for part in delta["tool_calls"].as_array().into_iter().flatten() {
                let index = part["index"].as_u64().unwrap_or(0) as usize;
                if partials.len() <= index {
                    partials.resize(index + 1, Partial::default());
                }
                let partial = &mut partials[index];
                if let Some(id) = part["id"].as_str() {
                    partial.id = id.to_string();
                }
                if let Some(name) = part["function"]["name"].as_str() {
                    partial.name.push_str(name);
                }
                if let Some(arguments) = part["function"]["arguments"].as_str() {
                    partial.arguments.push_str(arguments);
                }
            }
        }
        let calls: Vec<Call> = partials
            .into_iter()
            .map(|p| Call::new(p.id, p.name, p.arguments))
            .collect();
        // Servers differ in the finish reason they give with tool calls; the calls decide.
        let stop = if !calls.is_empty() {
            Stop::ToolUse
        } else if finish == "length" {
            Stop::MaxTokens
        } else {
            Stop::Done
        };
        Ok(Round {
            stop,
            calls,
            said: text,
        })
    }

    fn add_round(
        &self,
        messages: &mut Vec<Value>,
        text: String,
        calls: &[Call],
        results: &[CallResult],
    ) {
        let echoed: Vec<Value> = calls
            .iter()
            .map(|c| {
                // Arguments that weren't a JSON object go back as {} (servers reject them).
                let arguments = match &c.input {
                    Some(_) if !c.raw.trim().is_empty() => c.raw.clone(),
                    _ => "{}".to_string(),
                };
                json!({"id": c.id, "type": "function",
                       "function": {"name": c.name, "arguments": arguments}})
            })
            .collect();
        messages.push(json!({"role": "assistant", "content": text, "tool_calls": echoed}));
        for (call, result) in calls.iter().zip(results) {
            let content = if result.is_error {
                format!("Error: {}", result.text)
            } else {
                result.text.clone()
            };
            messages.push(json!({"role": "tool", "tool_call_id": call.id, "content": content}));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::Outcome;
    use super::super::testserver::{collect, serve};
    use super::super::tool_loop::ToolLoop;
    use super::*;

    fn sse(deltas: &[Value]) -> String {
        let mut out: String = deltas
            .iter()
            .map(|d| format!("data: {}\n\n", json!({"choices": [{"delta": d}]})))
            .collect();
        out.push_str("data: [DONE]\n\n");
        out
    }

    #[tokio::test]
    async fn tool_round_then_answer() {
        let first = sse(&[
            json!({"role": "assistant", "content": ""}),
            json!({"tool_calls": [{"index": 0, "id": "call_1", "function": {"name": "list_speakers", "arguments": ""}}]}),
            json!({"tool_calls": [{"index": 0, "function": {"arguments": "{"}}]}),
            json!({"tool_calls": [{"index": 0, "function": {"arguments": "}"}}]}),
        ]);
        let second = sse(&[json!({"content": "Nothing "}), json!({"content": "found."})]);
        let (url, mut bodies) = serve(vec![first, second]).await;
        let llm = ToolLoop(OpenAiCompat::new(
            reqwest::Client::new(),
            &url,
            "VOICED_TEST_NO_SUCH_KEY",
            "m",
            500,
            Some(config::reasoning_param("off").unwrap()),
        ));
        let (result, text) = collect(&llm, "which speakers are there").await;
        assert_eq!(result.unwrap(), Outcome::Done);
        // Nothing was said before the tool round, so no newline.
        assert_eq!(text, "Nothing found.");

        let request1 = bodies.recv().await.unwrap();
        assert_eq!(
            request1["messages"][0],
            json!({"role": "system", "content": "sys"})
        );
        assert_eq!(request1["reasoning"], json!({"enabled": false}));
        let request2 = bodies.recv().await.unwrap();
        assert_eq!(request2["reasoning"], json!({"enabled": false}));
        assert_eq!(
            request2["messages"][2]["tool_calls"][0]["function"]["arguments"],
            "{}"
        );
        assert_eq!(request2["messages"][3]["role"], "tool");
        assert_eq!(request2["messages"][3]["tool_call_id"], "call_1");
        // list_speakers doesn't exist here: the error goes back marked as one.
        assert!(
            request2["messages"][3]["content"]
                .as_str()
                .unwrap()
                .starts_with("Error: ")
        );
    }

    #[tokio::test]
    async fn think_tool_hands_off_without_running() {
        let reply = sse(&[
            json!({"content": "Hmm."}),
            json!({"tool_calls": [{"index": 0, "id": "call_1", "function": {"name": "think_harder", "arguments": "{}"}}]}),
        ]);
        let (url, mut bodies) = serve(vec![reply]).await;
        let llm = ToolLoop(OpenAiCompat::new(
            reqwest::Client::new(),
            &url,
            "VOICED_TEST_NO_SUCH_KEY",
            "m",
            500,
            None,
        ));
        let (result, text) = collect(&llm, "how far is the moon in light seconds").await;
        assert_eq!(result.unwrap(), Outcome::Think { spoke: true });
        assert_eq!(text, "Hmm.");
        assert!(bodies.recv().await.unwrap().get("reasoning").is_none());
        assert!(
            bodies.try_recv().is_err(),
            "one request only: the tool isn't run"
        );
    }
}
