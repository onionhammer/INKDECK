//! Any server with OpenAI's chat completions API (Ollama, llama.cpp, vLLM, hosted services,
//! OpenRouter): the request and stream format. The tool loop is tool_loop.rs.
//!
//! A model that hears gets the recording as an `input_audio` part instead of a transcript; one
//! that speaks is asked for `modalities: ["text", "audio"]` and streams PCM16 with a transcript
//! (`delta.audio.data`, `delta.audio.transcript`).

use anyhow::{Context, Result, anyhow, bail};
use async_trait::async_trait;
use base64::Engine;
use eventsource_stream::Eventsource;
use futures_util::StreamExt;
use serde_json::{Value, json};

use super::tool_loop::{Call, CallResult, ChatApi, Round, Speaker, Stop};
use super::{Recording, Request, Role};
use crate::config;
use crate::meter;
use crate::tools::ToolDef;

/// How a model that speaks is asked to.
pub struct Speech {
    pub voice: String,
    /// The sample rate of the PCM16 it returns.
    pub rate: u32,
}

pub struct OpenAiCompat {
    http: reqwest::Client,
    url: String,
    key: Option<String>,
    model: String,
    max_tokens: u32,
    /// OpenRouter's `reasoning` parameter, sent with every request when set.
    reasoning: Option<Value>,
    /// The model takes the recording rather than a transcript.
    pub hears: bool,
    /// The model speaks its reply.
    pub speech: Option<Speech>,
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
            hears: false,
            speech: None,
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
        if let Some(speech) = &self.speech {
            body["modalities"] = json!(["text", "audio"]);
            body["audio"] = json!({"voice": speech.voice, "format": "pcm16"});
        }
        body
    }

    /// A user message's content: the recording for a model that hears, else the text.
    fn user_content(&self, text: &str, audio: Option<&Recording>) -> Value {
        match audio {
            Some(recording) if self.hears => json!([{
                "type": "input_audio",
                "input_audio": {"data": recording.wav_base64, "format": "wav"},
            }]),
            _ => json!(text),
        }
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

    fn hears(&self) -> bool {
        self.hears
    }

    fn speaks(&self) -> Option<u32> {
        self.speech.as_ref().map(|s| s.rate)
    }

    fn start(&self, request: &Request<'_>) -> Vec<Value> {
        let mut messages = vec![json!({"role": "system", "content": request.system})];
        for turn in request.history {
            let message = match turn.role {
                Role::User => json!({"role": "user", "content": match &turn.audio {
                    Some(audio) if self.hears && turn.text.is_empty() =>
                        self.user_content("", Some(audio)),
                    _ => json!(turn.readable()),
                }}),
                Role::Assistant => json!({"role": "assistant", "content": turn.text}),
            };
            messages.push(message);
        }
        messages.push(json!({
            "role": "user",
            "content": self.user_content(request.user, request.audio),
        }));
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
        // Recorded however the round ends: a failed or cancelled one may still be billed.
        let mut record = Record::default();
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
            if record.id.is_none() {
                record.id = data["id"].as_str().map(str::to_string);
            }
            if let Some(cost) = data["usage"]["cost"].as_f64() {
                record.cost = Some(cost);
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
            let audio = &delta["audio"];
            if let Some(piece) = audio["transcript"].as_str().filter(|p| !p.is_empty()) {
                text.push_str(piece);
                speaker.transcript(piece).await?;
            }
            if let Some(data) = audio["data"].as_str().filter(|d| !d.is_empty()) {
                let pcm = base64::engine::general_purpose::STANDARD
                    .decode(data)
                    .context("the model's audio")?;
                speaker.audio(pcm.into()).await?;
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

/// A round's request, recorded with the turn's charges when the round ends, however it ends.
#[derive(Default)]
struct Record {
    id: Option<String>,
    cost: Option<f64>,
}

impl Drop for Record {
    fn drop(&mut self) {
        meter::record("llm", self.id.as_deref(), self.cost);
    }
}

#[cfg(test)]
mod tests {
    use super::super::Outcome;
    use super::super::testserver::{collect, collect_all, serve};
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
    async fn hears_the_recording_and_speaks_the_reply() {
        let pcm = base64::engine::general_purpose::STANDARD.encode([1u8, 0, 2, 0, 3]);
        let reply = format!(
            "data: {}\n\ndata: {}\n\ndata: {}\n\ndata: [DONE]\n\n",
            json!({"id": "gen-1", "choices": [{"delta": {"audio": {"transcript": "Paused."}}}]}),
            json!({"id": "gen-1", "choices": [{"delta": {"audio": {"data": pcm}}}]}),
            json!({"id": "gen-1", "choices": [], "usage": {"cost": 0.0002}}),
        );
        let (url, mut bodies) = serve(vec![reply]).await;
        let mut api = OpenAiCompat::new(
            reqwest::Client::new(),
            &url,
            "VOICED_TEST_NO_SUCH_KEY",
            "m",
            500,
            None,
        );
        api.hears = true;
        api.speech = Some(Speech {
            voice: "alloy".into(),
            rate: 24000,
        });
        let llm = ToolLoop(api);
        assert!(super::super::Llm::hears(&llm));
        assert_eq!(super::super::Llm::speaks(&llm), Some(24000));
        let recording = Recording::new(&[0, 0, 1, 0], 16000);
        let ((result, text, audio), charges) =
            meter::measure(collect_all(&llm, "", Some(&recording))).await;
        assert_eq!(result.unwrap(), Outcome::Done);
        assert_eq!(text, "Paused.");
        assert_eq!(audio, [1, 0, 2, 0, 3]);
        assert_eq!(
            charges,
            [meter::Charge {
                stage: "llm",
                id: Some("gen-1".into()),
                cost: Some(0.0002)
            }]
        );

        let body = bodies.recv().await.unwrap();
        assert_eq!(body["modalities"], json!(["text", "audio"]));
        assert_eq!(body["audio"], json!({"voice": "alloy", "format": "pcm16"}));
        let part = &body["messages"][1]["content"][0];
        assert_eq!(part["type"], "input_audio");
        assert_eq!(part["input_audio"]["format"], "wav");
        assert_eq!(part["input_audio"]["data"], recording.wav_base64.as_str());
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
