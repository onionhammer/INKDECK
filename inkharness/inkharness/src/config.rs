//! inkharness.toml (layered by place.rs). Every stage names a backend; API keys come from the
//! environment or a `.env` file.

use anyhow::Result;
use serde::Deserialize;

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    /// Where `serve` listens for devices (docs/voice-protocol.md).
    #[serde(default = "default_listen")]
    pub listen: String,
    /// Where `serve` answers HTTP (http.rs). Empty: no HTTP.
    #[serde(default = "default_http")]
    pub http: String,
    /// The key devices encrypt the connection with (they prove they have it without sending
    /// it), and HTTP requests' bearer token. Empty: anyone who can reach the ports may use
    /// them.
    #[serde(default)]
    pub token: String,
    /// Also take devices on protocol 1, which sends the token, the audio and everything else
    /// unencrypted: older board firmware, while it waits for an update.
    #[serde(default)]
    pub plaintext: bool,
    /// Replaces the built-in prompt, for speech and text alike. Unset: one written for replies
    /// that are spoken (devices) or read (the terminal, HTTP).
    pub system_prompt: Option<String>,
    /// Speech to text. Optional when the model hears the recording itself ([llm] input =
    /// "audio"); then, if present, it runs alongside the model for the device's display and
    /// the conversation history.
    pub stt: Option<Stt>,
    pub llm: Llm,
    /// Text to speech. Optional when every model speaks its reply itself (output = "audio").
    pub tts: Option<Tts>,
    pub home_assistant: Option<HomeAssistant>,
    /// A second model the first can hand hard questions to (tools::THINK_TOOL).
    pub think: Option<Think>,
    /// What text turns (the terminal, HTTP) use instead of [llm].
    pub chat: Option<ChatModel>,
    #[serde(default)]
    pub cache: Cache,
    #[serde(default)]
    pub js: Js,
    /// MCP servers whose tools the models get (mcp.rs).
    #[serde(default)]
    pub mcp: Vec<McpServer>,
    #[serde(default)]
    pub skills: Skills,
    /// Web search (search.rs): `[search]` for one backend, `[[search]]` for several, tried in
    /// order until one answers.
    #[serde(default, deserialize_with = "one_or_more")]
    pub search: Vec<Search>,
    /// Agent sessions (agent/): without this table there are none, and only talk turns work.
    pub agent: Option<Agent>,
    #[serde(default)]
    pub talk: Talk,
}

/// [agent]: sessions that live on the daemon and work with no board connected (issue #52).
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Agent {
    /// Where sessions are kept, relative to the config file it is in (place.rs makes it
    /// absolute).
    #[serde(default = "default_agent_dir")]
    pub dir: String,
    /// The name the board shows for where sessions run. Unset: the hostname, uppercased.
    pub place: Option<String>,
    /// The SHA-256 of the PIN, in hex. With it, a connection must unlock before it sees or
    /// does anything with sessions, and HTTP requests send the PIN.
    pub pin_sha256: Option<String>,
    /// How long an unlock ticket lets a new connection back in without the PIN.
    #[serde(default = "default_ticket_minutes")]
    pub ticket_minutes: u64,
    /// Finished sessions older than this are deleted.
    #[serde(default = "default_keep_days")]
    pub keep_days: u64,
    /// Model rounds per run before a session fails with "too many steps".
    #[serde(default = "default_max_rounds")]
    pub max_rounds: usize,
}

/// [talk]: what a talk turn (HOLD BOOT outside the Agent) may use.
#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Talk {
    /// Tool names, or prefixes ending in `*`. The device's tools always stay. Unset: every
    /// tool.
    pub tools: Option<Vec<String>>,
}

pub(crate) fn default_agent_dir() -> String {
    "sessions".into()
}
fn default_ticket_minutes() -> u64 {
    60
}
fn default_keep_days() -> u64 {
    30
}
fn default_max_rounds() -> usize {
    40
}

/// A table, or an array of them.
fn one_or_more<'de, D, T>(d: D) -> Result<Vec<T>, D::Error>
where
    D: serde::Deserializer<'de>,
    T: Deserialize<'de>,
{
    #[derive(Deserialize)]
    #[serde(untagged)]
    enum OneOrMore<T> {
        More(Vec<T>),
        One(T),
    }
    Ok(match OneOrMore::deserialize(d)? {
        OneOrMore::More(all) => all,
        OneOrMore::One(one) => vec![one],
    })
}

/// A web search backend for the web_search tool (search.rs).
#[derive(Debug, Clone, Deserialize)]
#[serde(tag = "backend", rename_all = "snake_case", deny_unknown_fields)]
pub enum Search {
    /// A SearXNG instance's JSON API: `json` must be in `search.formats` in its settings.yml.
    Searxng {
        url: String,
        /// Its engines to ask, comma-separated (unset: the instance's defaults).
        engines: Option<String>,
        /// A language code such as "en" or "en-US" (unset: the instance's default).
        language: Option<String>,
        #[serde(default = "default_search_results")]
        results: usize,
    },
    /// Brave's Search API (an independent index).
    Brave {
        #[serde(default = "default_brave_key_env")]
        api_key_env: String,
        /// Two-letter country the results are for, e.g. "us".
        country: Option<String>,
        #[serde(default = "default_brave_url")]
        base_url: String,
        #[serde(default = "default_search_results")]
        results: usize,
    },
    /// Tavily, a search API for agents: with each search, a short answer written from the
    /// results, and their text.
    Tavily {
        #[serde(default = "default_tavily_key_env")]
        api_key_env: String,
        /// "basic" or "advanced" (slower, better snippets, costs more).
        #[serde(default = "default_tavily_depth")]
        depth: String,
        #[serde(default = "default_tavily_url")]
        base_url: String,
        #[serde(default = "default_search_results")]
        results: usize,
    },
}

impl Search {
    pub fn name(&self) -> &'static str {
        match self {
            Search::Searxng { .. } => "searxng",
            Search::Brave { .. } => "brave",
            Search::Tavily { .. } => "tavily",
        }
    }
}

fn default_search_results() -> usize {
    5
}
fn default_brave_key_env() -> String {
    "BRAVE_API_KEY".into()
}
fn default_brave_url() -> String {
    "https://api.search.brave.com/res/v1".into()
}
fn default_tavily_key_env() -> String {
    "TAVILY_API_KEY".into()
}
fn default_tavily_depth() -> String {
    "basic".into()
}
fn default_tavily_url() -> String {
    "https://api.tavily.com".into()
}

/// Agent Skills (skills.rs): instructions the models load by name when a task calls for them.
#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Skills {
    /// Folders holding one folder per skill, relative to the config file. Empty: no skills.
    #[serde(default)]
    pub dirs: Vec<String>,
}

/// An MCP server: a command speaking MCP on stdin/stdout, or a streamable HTTP endpoint.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct McpServer {
    /// Prefixes its tools' names: tool "HassTurnOn" of server "home" becomes "home_HassTurnOn".
    pub name: String,
    /// Streamable HTTP endpoint, e.g. http://homeassistant.local:8123/api/mcp.
    pub url: Option<String>,
    /// Environment variable holding a bearer token for `url`.
    pub token_env: Option<String>,
    /// A program to start, speaking MCP on stdin/stdout.
    pub command: Option<String>,
    #[serde(default)]
    pub args: Vec<String>,
    #[serde(default)]
    pub env: std::collections::HashMap<String, String>,
    /// Headers sent with every request to `url` (from a .mcp.json's `headers`).
    #[serde(default)]
    pub headers: std::collections::HashMap<String, String>,
    /// Only these tools (the server's names). Unset: all of them.
    pub tools: Option<Vec<String>>,
    /// A call that takes longer fails.
    #[serde(default = "default_mcp_timeout_ms")]
    pub timeout_ms: u64,
}

/// The run_js tool (js.rs): a JavaScript sandbox for arithmetic and for chaining tool calls.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Js {
    #[serde(default = "default_true")]
    pub enabled: bool,
    /// A script is stopped after this long, its tool calls included.
    #[serde(default = "default_js_timeout_ms")]
    pub timeout_ms: u64,
    #[serde(default = "default_js_memory_mb")]
    pub memory_mb: usize,
    /// Home tool calls one script may make.
    #[serde(default = "default_js_tool_calls")]
    pub max_tool_calls: usize,
}

impl Default for Js {
    fn default() -> Self {
        Js {
            enabled: true,
            timeout_ms: default_js_timeout_ms(),
            memory_mb: default_js_memory_mb(),
            max_tool_calls: default_js_tool_calls(),
        }
    }
}

/// [chat]: a model for replies that are read, such as a text model where [llm] hears and speaks.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ChatModel {
    pub llm: Llm,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Think {
    /// Any [llm] backend; typically one that reasons (reasoning = "medium") or a larger model.
    pub llm: Llm,
    /// Said while it thinks, one after another. Synthesised at startup, so they play at once.
    #[serde(default = "default_think_phrases")]
    pub phrases: Vec<String>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Cache {
    /// Synthesised sentences of up to tts::CACHED_CHARS characters kept in memory, so a
    /// sentence said again plays without a request. 0 turns the cache off.
    #[serde(default = "default_tts_entries")]
    pub tts_entries: usize,
    /// Synthesised at startup, besides [think]'s phrases.
    #[serde(default)]
    pub phrases: Vec<String>,
}

impl Default for Cache {
    fn default() -> Self {
        Cache {
            tts_entries: default_tts_entries(),
            phrases: Vec::new(),
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(tag = "backend", rename_all = "snake_case", deny_unknown_fields)]
pub enum Stt {
    /// POST {base_url}/audio/transcriptions when the utterance ends.
    OpenaiCompat {
        base_url: String,
        model: String,
        #[serde(default = "default_openai_key_env")]
        api_key_env: String,
        language: Option<String>,
    },
    /// In-process streaming transducer through sherpa-onnx (build with --features local).
    /// `dir` holds the three model files and tokens.txt.
    #[cfg_attr(not(feature = "local"), allow(dead_code))]
    SherpaStreaming {
        dir: String,
        #[serde(default = "default_encoder")]
        encoder: String,
        #[serde(default = "default_decoder")]
        decoder: String,
        #[serde(default = "default_joiner")]
        joiner: String,
        #[serde(default = "default_threads")]
        threads: i32,
        /// cpu | coreml | cuda
        #[serde(default = "default_provider")]
        provider: String,
    },
    /// Returns `text` for every utterance (testing without a service).
    Mock {
        #[serde(default = "default_mock_text")]
        text: String,
    },
}

#[derive(Debug, Deserialize)]
#[serde(tag = "backend", rename_all = "snake_case", deny_unknown_fields)]
pub enum Llm {
    Anthropic {
        model: String,
        #[serde(default = "default_anthropic_url")]
        base_url: String,
        #[serde(default = "default_anthropic_key_env")]
        api_key_env: String,
        /// low | medium | high | xhigh | max. Thinking depth; low answers soonest.
        #[serde(default = "default_effort")]
        effort: String,
        #[serde(default = "default_max_tokens")]
        max_tokens: u32,
        /// Let the API re-run a declined request on a fallback model. Not every model
        /// supports it (Haiku does not).
        #[serde(default)]
        fallbacks: bool,
    },
    /// POST {base_url}/chat/completions (Ollama, llama.cpp, vLLM, hosted).
    OpenaiCompat {
        base_url: String,
        model: String,
        #[serde(default = "default_openai_key_env")]
        api_key_env: String,
        #[serde(default = "default_max_tokens")]
        max_tokens: u32,
        /// How much the model thinks before answering: off | minimal | low | medium | high |
        /// xhigh. Sent as OpenRouter's `reasoning` parameter; unset sends nothing (the
        /// model's default). Other servers may ignore or reject it.
        reasoning: Option<String>,
        /// What the model is given of the utterance: the transcript from [stt] ("text"), or
        /// the recording itself ("audio", as an input_audio part), for a model that hears.
        #[serde(default)]
        input: Modality,
        /// How the reply comes back: text that [tts] speaks, or the model's own speech
        /// ("audio": modalities text and audio, PCM16, with a transcript for the display).
        #[serde(default)]
        output: Modality,
        /// The model's voice, for output = "audio".
        voice: Option<String>,
        /// Sample rate of the model's PCM16, for output = "audio" (OpenAI's is 24000).
        #[serde(default = "default_tts_rate")]
        audio_rate: u32,
    },
    Mock,
}

/// Text or audio, for what a model takes in or gives back.
#[derive(Debug, Default, Clone, Copy, PartialEq, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Modality {
    #[default]
    Text,
    Audio,
}

impl Llm {
    pub fn input(&self) -> Modality {
        match self {
            Llm::OpenaiCompat { input, .. } => *input,
            _ => Modality::Text,
        }
    }

    pub fn output(&self) -> Modality {
        match self {
            Llm::OpenaiCompat { output, .. } => *output,
            _ => Modality::Text,
        }
    }

    /// The model's name, for the banner and the health check.
    pub fn name(&self) -> &str {
        match self {
            Llm::Anthropic { model, .. } | Llm::OpenaiCompat { model, .. } => model,
            Llm::Mock => "mock",
        }
    }
}

impl Config {
    /// The stages a model needs are there: a transcript for a model that reads, speech for
    /// one that writes.
    pub fn check(&self) -> Result<()> {
        let mut models = vec![("[llm]", &self.llm)];
        if let Some(think) = &self.think {
            models.push(("[think.llm]", &think.llm));
        }
        for (name, llm) in models {
            if llm.input() == Modality::Text && self.stt.is_none() {
                anyhow::bail!(
                    "{name} reads a transcript: add an [stt] section, or set input = \"audio\" \
                     for a model that hears"
                );
            }
            if llm.output() == Modality::Text && self.tts.is_none() {
                anyhow::bail!(
                    "{name} writes its reply: add a [tts] section, or set output = \"audio\" \
                     for a model that speaks"
                );
            }
            if let Llm::OpenaiCompat {
                output: Modality::Audio,
                voice: None,
                ..
            } = llm
            {
                anyhow::bail!("{name} has output = \"audio\": give it a voice");
            }
        }
        Ok(())
    }
}

/// The `reasoning` request parameter for a config value (OpenRouter's format).
pub fn reasoning_param(value: &str) -> Result<serde_json::Value> {
    match value {
        "off" => Ok(serde_json::json!({"enabled": false})),
        "minimal" | "low" | "medium" | "high" | "xhigh" => Ok(serde_json::json!({"effort": value})),
        _ => anyhow::bail!("reasoning = \"{value}\": use off, minimal, low, medium, high or xhigh"),
    }
}

#[derive(Debug, Deserialize)]
#[serde(tag = "backend", rename_all = "snake_case", deny_unknown_fields)]
pub enum Tts {
    /// POST {base_url}/audio/speech with response_format "pcm" (s16le mono).
    OpenaiCompat {
        base_url: String,
        model: String,
        voice: String,
        #[serde(default = "default_openai_key_env")]
        api_key_env: String,
        /// Sample rate of the PCM the service returns.
        #[serde(default = "default_tts_rate")]
        rate: u32,
    },
    /// In-process Pocket TTS through sherpa-onnx (build with --features local). `dir` holds
    /// the model; `voice` is a WAV of the voice to speak in.
    #[cfg_attr(not(feature = "local"), allow(dead_code))]
    SherpaPocket {
        dir: String,
        voice: String,
        #[serde(default = "default_threads")]
        threads: i32,
        #[serde(default = "default_provider")]
        provider: String,
        /// Flow steps per frame: fewer is faster, more is cleaner.
        #[serde(default = "default_pocket_steps")]
        steps: i32,
    },
    /// A tone per sentence.
    Mock,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HomeAssistant {
    pub url: String,
    #[serde(default = "default_ha_token_env")]
    pub token_env: String,
    /// The action play_music calls, as domain.service.
    #[serde(default = "default_play_service")]
    pub play_service: String,
}

fn default_encoder() -> String {
    "encoder.onnx".into()
}
fn default_decoder() -> String {
    "decoder.onnx".into()
}
fn default_joiner() -> String {
    "joiner.onnx".into()
}
fn default_think_phrases() -> Vec<String> {
    [
        "Let me think about that.",
        "Give me a moment to think.",
        "Hmm, let me think.",
    ]
    .map(String::from)
    .to_vec()
}
pub(crate) fn default_mcp_timeout_ms() -> u64 {
    20000
}
fn default_true() -> bool {
    true
}
fn default_js_timeout_ms() -> u64 {
    3000
}
fn default_js_memory_mb() -> usize {
    32
}
fn default_js_tool_calls() -> usize {
    20
}
fn default_tts_entries() -> usize {
    200
}
fn default_threads() -> i32 {
    2
}
fn default_provider() -> String {
    "cpu".into()
}
fn default_pocket_steps() -> i32 {
    5
}
fn default_listen() -> String {
    format!("0.0.0.0:{}", voice_proto::DEFAULT_PORT)
}
fn default_http() -> String {
    format!("127.0.0.1:{}", voice_proto::DEFAULT_PORT + 1)
}
/// For replies a device speaks.
pub fn spoken_prompt() -> String {
    "You are a voice assistant on a small device in someone's home. Your replies are spoken \
     aloud, so answer in one or two short plain sentences with no markdown, lists or emoji. \
     Use the tools to control things in the home; if no tool fits, say so. Only say you did \
     something if a tool call in this conversation did it, and don't offer to do things no \
     tool can do."
        .into()
}
/// For replies that are read: the terminal, HTTP.
pub fn written_prompt() -> String {
    "You are an assistant with tools, answering in a text chat. Keep answers short and plain; \
     use markdown only where it helps. Use the tools to act and to look things up; if no tool \
     fits, say so. Only say you did something if a tool call in this conversation did it."
        .into()
}
fn default_openai_key_env() -> String {
    "OPENAI_API_KEY".into()
}
fn default_anthropic_key_env() -> String {
    "ANTHROPIC_API_KEY".into()
}
fn default_anthropic_url() -> String {
    "https://api.anthropic.com".into()
}
fn default_effort() -> String {
    "low".into()
}
fn default_max_tokens() -> u32 {
    16000
}
fn default_tts_rate() -> u32 {
    24000
}
fn default_mock_text() -> String {
    "what can you do".into()
}
fn default_ha_token_env() -> String {
    "HOME_ASSISTANT_TOKEN".into()
}
fn default_play_service() -> String {
    "music_assistant.play_media".into()
}

impl Config {
    /// The config in `table` (the merged layers).
    pub fn from_table(table: toml::Table) -> Result<Config> {
        Ok(toml::Value::Table(table).try_into()?)
    }
}

/// Values from `.env` files (`NAME=value` lines), for keys the environment doesn't set.
static DOTENV: std::sync::OnceLock<std::collections::HashMap<String, String>> =
    std::sync::OnceLock::new();

/// Reads the `.env` files that exist of `files`; a later file wins over an earlier one, and the
/// environment over them all. Returns the ones it read.
pub fn load_dotenv(files: &[std::path::PathBuf]) -> Vec<std::path::PathBuf> {
    let mut values = std::collections::HashMap::new();
    let mut read = Vec::new();
    for file in files {
        let Ok(text) = std::fs::read_to_string(file) else {
            continue;
        };
        values.extend(parse_dotenv(&text));
        tracing::info!(file = %file.display(), "read keys");
        read.push(file.clone());
    }
    let _ = DOTENV.set(values);
    read
}

/// `var` from the environment or a `.env` file, if it is set and not empty.
pub fn lookup(var: &str) -> Option<String> {
    std::env::var(var)
        .ok()
        .filter(|v| !v.is_empty())
        .or_else(|| {
            DOTENV
                .get()
                .and_then(|values| values.get(var))
                .filter(|v| !v.is_empty())
                .cloned()
        })
}

fn parse_dotenv(text: &str) -> Vec<(String, String)> {
    text.lines()
        .map(str::trim)
        .filter(|line| !line.starts_with('#'))
        .filter_map(|line| line.strip_prefix("export ").unwrap_or(line).split_once('='))
        .map(|(name, value)| {
            let value = value.trim();
            let unquoted = ['"', '\'']
                .iter()
                .find_map(|&q| value.strip_prefix(q).and_then(|v| v.strip_suffix(q)))
                .unwrap_or(value);
            (name.trim().to_string(), unquoted.to_string())
        })
        .collect()
}

/// The key in `var`, from the environment or a `.env` file, or `None` if it is unset or empty
/// (local servers need none).
pub fn env_key(var: &str) -> Option<String> {
    let key = lookup(var);
    if key.is_none() {
        tracing::warn!("{var} is not set: requests go without a key (fine for a local server)");
    }
    key
}

#[cfg(test)]
mod tests {
    fn check(toml: &str) -> Result<(), String> {
        let cfg: super::Config = toml::from_str(toml).map_err(|e| e.to_string())?;
        cfg.check().map_err(|e| e.to_string())
    }

    const HEARS: &str =
        "[llm]\nbackend = \"openai_compat\"\nbase_url = \"u\"\nmodel = \"m\"\ninput = \"audio\"\n";

    #[test]
    fn stages_a_model_needs() {
        let stt = "[stt]\nbackend = \"mock\"\n";
        let tts = "[tts]\nbackend = \"mock\"\n";
        let think = "[think.llm]\nbackend = \"mock\"\n";
        // The three pipelines: transcript in and speech out, recording in, both.
        assert!(check(&format!("{stt}[llm]\nbackend = \"mock\"\n{tts}")).is_ok());
        assert!(check(&format!("{HEARS}{tts}")).is_ok());
        assert!(check(&format!("{HEARS}output = \"audio\"\nvoice = \"alloy\"\n")).is_ok());
        // A model that reads needs [stt]; one that writes needs [tts]; one that speaks, a voice.
        assert!(
            check(&format!("[llm]\nbackend = \"mock\"\n{tts}"))
                .unwrap_err()
                .contains("[stt]")
        );
        assert!(check(HEARS).unwrap_err().contains("[tts]"));
        assert!(
            check(&format!("{HEARS}output = \"audio\"\n"))
                .unwrap_err()
                .contains("voice")
        );
        // So does a thinking model.
        assert!(
            check(&format!("{HEARS}{tts}{think}"))
                .unwrap_err()
                .contains("[think.llm]")
        );
        assert!(check(&format!("{stt}{HEARS}{tts}{think}")).is_ok());
    }

    #[test]
    fn dotenv_lines() {
        let parsed =
            super::parse_dotenv("# note\nA=1\nexport B = \"two words\"\n\nC='x=y'\nnot a pair\n");
        let expect = [("A", "1"), ("B", "two words"), ("C", "x=y")];
        assert_eq!(parsed, expect.map(|(k, v)| (k.to_string(), v.to_string())));
    }
}
