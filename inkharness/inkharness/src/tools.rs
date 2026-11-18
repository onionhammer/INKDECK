//! What the model can do. In the home: speakers, through Home Assistant's REST API (only the
//! media_player domain is reachable, whatever the model asks for), and the tools of any MCP
//! servers (mcp.rs). Web search (search.rs). On a board that is talking: the tools it runs
//! itself (device.rs). Then run_js, a JavaScript sandbox that can call all of those (js.rs), the
//! skill tools
//! (skills.rs), and the think tool, which the chat backends hand back to the session instead of
//! running.

use std::sync::Arc;

use anyhow::{Context, Result, bail};
use serde::Deserialize;
use serde_json::{Value, json};

use crate::config;
use crate::device::Device;
use crate::js;
use crate::mcp::Mcp;
use crate::search::{self, Search};
use crate::skills::Skills;

/// The tool that hands a turn to the thinking model ([think] in inkharness.toml).
pub const THINK_TOOL: &str = "think_harder";
/// The JavaScript sandbox ([js] in inkharness.toml).
pub const JS_TOOL: &str = "run_js";

#[derive(Clone)]
pub struct ToolDef {
    pub name: String,
    pub description: String,
    /// JSON Schema of the input object.
    pub schema: Value,
}

#[derive(Clone)]
pub struct ToolSet {
    /// What the model gets: `callable`, run_js, the skill tools, the think tool.
    defs: Vec<ToolDef>,
    /// What a script can call too: Home Assistant's tools, the MCP servers', web search, the
    /// device's.
    callable: Vec<ToolDef>,
    think: bool,
    backends: Backends,
    js: Option<js::Limits>,
    skills: Arc<Skills>,
}

/// What runs the tools a script can call too.
#[derive(Clone)]
struct Backends {
    ha: Option<Arc<HomeAssistant>>,
    mcp: Arc<Mcp>,
    search: Option<Arc<Search>>,
    device: Option<Arc<Device>>,
}

impl Backends {
    /// A Home Assistant, MCP, search or device tool.
    async fn call(&self, name: &str, input: Value) -> Result<String, String> {
        if let Some(device) = self.device.as_deref().filter(|d| d.has(name)) {
            return device.call(name, input).await;
        }
        if let (search::TOOL, Some(search)) = (name, &self.search) {
            return search.call(input).await;
        }
        if let Some(result) = self.mcp.call(name, input.clone()).await {
            return result;
        }
        call_home(self.ha.as_deref(), name, input).await
    }
}

impl ToolSet {
    pub fn build(
        ha: Option<&config::HomeAssistant>,
        js: &config::Js,
        mcp: Arc<Mcp>,
        skills: Arc<Skills>,
        http: reqwest::Client,
    ) -> Result<ToolSet> {
        let ha = match ha {
            None => None,
            Some(cfg) => {
                let token = config::env_key(&cfg.token_env).with_context(|| {
                    format!("[home_assistant] needs a token in ${}", cfg.token_env)
                })?;
                let Some((domain, service)) = cfg.play_service.split_once('.') else {
                    bail!(
                        "play_service must be domain.service, got {:?}",
                        cfg.play_service
                    );
                };
                Some(Arc::new(HomeAssistant {
                    http,
                    url: cfg.url.trim_end_matches('/').to_string(),
                    token,
                    play: (domain.to_string(), service.to_string()),
                }))
            }
        };
        let mut callable = if ha.is_some() {
            definitions()
        } else {
            Vec::new()
        };
        callable.extend(mcp.tools().iter().map(|t| ToolDef {
            name: t.name.clone(),
            description: t.description.clone(),
            schema: t.schema.clone(),
        }));
        let js = js.enabled.then(|| js::Limits {
            timeout: std::time::Duration::from_millis(js.timeout_ms),
            memory_bytes: js.memory_mb << 20,
            max_tool_calls: js.max_tool_calls,
        });
        let mut tools = ToolSet {
            defs: Vec::new(),
            callable,
            think: false,
            backends: Backends {
                ha,
                mcp,
                search: None,
                device: None,
            },
            js,
            skills,
        };
        tools.assemble();
        Ok(tools)
    }

    fn assemble(&mut self) {
        let mut defs = self.callable.clone();
        if let Some(limits) = &self.js {
            let callable: Vec<&str> = self.callable.iter().map(|d| d.name.as_str()).collect();
            defs.push(js_definition(&callable, limits));
        }
        // After run_js, so scripts don't get them: see skills.rs.
        defs.extend(self.skills.defs());
        if self.think {
            defs.push(think_definition());
        }
        self.defs = defs;
    }

    /// The same tools plus the think tool, for the fast model.
    pub fn with_think(&self) -> ToolSet {
        let mut tools = self.clone();
        tools.think = true;
        tools.assemble();
        tools
    }

    /// The same tools plus a connected device's (scripts can call them too).
    pub fn with_device(&self, device: Arc<Device>) -> ToolSet {
        let mut tools = self.clone();
        tools.callable.extend(device.defs().iter().cloned());
        tools.backends.device = Some(device);
        tools.assemble();
        tools
    }

    /// The same tools plus web search.
    pub fn with_search(&self, search: Arc<Search>) -> ToolSet {
        let mut tools = self.clone();
        tools.callable.push(search.def());
        tools.backends.search = Some(search);
        tools.assemble();
        tools
    }

    pub fn defs(&self) -> &[ToolDef] {
        &self.defs
    }

    /// The names a device's tools can't have.
    pub fn names(&self) -> Vec<&str> {
        let mut names: Vec<&str> = self.defs.iter().map(|d| d.name.as_str()).collect();
        names.push(THINK_TOOL);
        names
    }

    /// True if anything in the home besides run_js can be called: Home Assistant or an MCP
    /// tool.
    pub fn has_home(&self) -> bool {
        self.backends.ha.is_some() || !self.backends.mcp.tools().is_empty()
    }

    /// Runs a tool. `Err` is a message for the model (bad input, or the call failed).
    pub async fn call(&self, name: &str, input: Value) -> Result<String, String> {
        match name {
            THINK_TOOL => Err(format!("{THINK_TOOL} is handled by the daemon, not run")),
            JS_TOOL => {
                let Some(limits) = &self.js else {
                    return Err(format!("no tool named {name}"));
                };
                let Some(code) = input["code"].as_str().filter(|c| !c.trim().is_empty()) else {
                    return Err("invalid input: code is required".into());
                };
                // The script gets the home's, search and the device's tools: not run_js itself,
                // the think tool or the skill tools.
                let names: Vec<String> = self.callable.iter().map(|d| d.name.clone()).collect();
                let backends = self.backends.clone();
                let call: js::Caller = Arc::new(move |name, input| {
                    let backends = backends.clone();
                    Box::pin(async move { backends.call(&name, input).await })
                });
                js::run(code.to_string(), limits.clone(), names, call).await
            }
            _ => match self.skills.call(name, input.clone()) {
                Some(result) => result,
                None => self.backends.call(name, input).await,
            },
        }
    }
}

fn think_definition() -> ToolDef {
    ToolDef {
        name: THINK_TOOL.into(),
        description: "Hands this request to a slower model that thinks it through before \
                      answering. Use it when a good answer needs reasoning, planning or facts \
                      you are not sure of. Don't use it for commands, greetings or anything you \
                      can answer well at once. The user hears a short phrase while it thinks, \
                      then its answer, so say nothing yourself before or after calling it."
            .into(),
        schema: json!({"type": "object", "properties": {}, "additionalProperties": false}),
    }
}

async fn call_home(ha: Option<&HomeAssistant>, name: &str, input: Value) -> Result<String, String> {
    let Some(ha) = ha else {
        return Err(format!("no tool named {name}"));
    };
    let result = match name {
        "list_speakers" => ha.list_speakers().await,
        "play_music" => match parse(input) {
            Ok(args) => ha.play_music(args).await,
            Err(e) => return Err(e),
        },
        "media_control" => match parse(input) {
            Ok(args) => ha.media_control(args).await,
            Err(e) => return Err(e),
        },
        _ => return Err(format!("no tool named {name}")),
    };
    result.map_err(|e| format!("{e:#}"))
}

fn js_definition(callable: &[&str], limits: &js::Limits) -> ToolDef {
    let tools = if callable.is_empty() {
        String::new()
    } else {
        format!(
            " It can also do several steps in one call: the other tools ({}) are \
             synchronous functions on `tools` that take the same input object and return the \
             result as a string, e.g. `const s = tools.list_speakers(); \
             tools.play_music({{speaker: \"media_player.kitchen\", query: \"jazz\"}})`. A failed \
             call throws. When a request needs two or more steps, do them in one script: \
             that is one round instead of several, so the user waits less.",
            callable.join(", ")
        )
    };
    ToolDef {
        name: JS_TOOL.into(),
        description: format!(
            "Runs JavaScript in a sandbox and returns what it prints and the value of its last \
             expression. Use it for arithmetic, dates and times, unit conversions and anything \
             you would otherwise work out in your head.{tools} There is no network, file \
             access or timers, and it stops after {} ms. print(...) or console.log(...) adds a \
             line to the output.",
            limits.timeout.as_millis()
        ),
        schema: json!({
            "type": "object",
            "properties": {
                "code": {"type": "string", "description": "The script; its last expression is the result"}
            },
            "required": ["code"],
            "additionalProperties": false
        }),
    }
}

/// The model's input, checked against the tool's argument struct.
fn parse<T: for<'de> Deserialize<'de>>(input: Value) -> Result<T, String> {
    serde_json::from_value(input).map_err(|e| format!("invalid input: {e}"))
}

fn definitions() -> Vec<ToolDef> {
    vec![
        ToolDef {
            name: "list_speakers".into(),
            description: "Lists the speakers and media players in the home: entity id, name \
                          and state. Call this first to find the entity id for a room or \
                          speaker the user names."
                .into(),
            schema: json!({"type": "object", "properties": {}, "additionalProperties": false}),
        },
        ToolDef {
            name: "play_music".into(),
            description: "Plays music on a speaker. query is what to play: an artist, album, \
                          track, playlist or radio station name."
                .into(),
            schema: json!({
                "type": "object",
                "properties": {
                    "speaker": {"type": "string", "description": "Entity id from list_speakers, e.g. media_player.kitchen"},
                    "query": {"type": "string", "description": "What to play"}
                },
                "required": ["speaker", "query"],
                "additionalProperties": false
            }),
        },
        ToolDef {
            name: "media_control".into(),
            description: "Controls playback on a speaker: play, pause, stop, next, previous, \
                          or set_volume (with volume from 0 to 100)."
                .into(),
            schema: json!({
                "type": "object",
                "properties": {
                    "speaker": {"type": "string", "description": "Entity id from list_speakers"},
                    "action": {"type": "string", "enum": ["play", "pause", "stop", "next", "previous", "set_volume"]},
                    "volume": {"type": "integer", "description": "0 to 100; only for set_volume"}
                },
                "required": ["speaker", "action"],
                "additionalProperties": false
            }),
        },
    ]
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct PlayMusic {
    speaker: String,
    query: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct MediaControl {
    speaker: String,
    action: String,
    volume: Option<i64>,
}

struct HomeAssistant {
    http: reqwest::Client,
    url: String,
    token: String,
    play: (String, String),
}

impl HomeAssistant {
    async fn list_speakers(&self) -> Result<String> {
        let response = self
            .http
            .get(format!("{}/api/states", self.url))
            .bearer_auth(&self.token)
            .send()
            .await
            .context("Home Assistant request")?;
        let status = response.status();
        if !status.is_success() {
            bail!("Home Assistant returned {status}");
        }
        let states: Vec<Value> = response.json().await.context("Home Assistant response")?;
        let mut lines = Vec::new();
        for state in &states {
            let Some(id) = state["entity_id"].as_str() else {
                continue;
            };
            if !id.starts_with("media_player.") {
                continue;
            }
            let name = state["attributes"]["friendly_name"].as_str().unwrap_or(id);
            let now = state["state"].as_str().unwrap_or("unknown");
            lines.push(format!("{id} | {name} | {now}"));
        }
        if lines.is_empty() {
            return Ok("No speakers found.".into());
        }
        Ok(lines.join("\n"))
    }

    async fn play_music(&self, args: PlayMusic) -> Result<String> {
        speaker_ok(&args.speaker)?;
        let (domain, service) = &self.play;
        self.service(
            domain,
            service,
            json!({"entity_id": args.speaker, "media_id": args.query}),
        )
        .await?;
        Ok(format!("Asked {} to play {:?}.", args.speaker, args.query))
    }

    async fn media_control(&self, args: MediaControl) -> Result<String> {
        speaker_ok(&args.speaker)?;
        let mut data = json!({"entity_id": args.speaker});
        let service = match args.action.as_str() {
            "play" => "media_play",
            "pause" => "media_pause",
            "stop" => "media_stop",
            "next" => "media_next_track",
            "previous" => "media_previous_track",
            "set_volume" => {
                let Some(volume) = args.volume else {
                    bail!("set_volume needs volume")
                };
                data["volume_level"] = json!(volume.clamp(0, 100) as f64 / 100.0);
                "volume_set"
            }
            other => bail!("unknown action {other:?}"),
        };
        self.service("media_player", service, data).await?;
        Ok(format!("Done: {} on {}.", args.action, args.speaker))
    }

    async fn service(&self, domain: &str, service: &str, data: Value) -> Result<()> {
        let response = self
            .http
            .post(format!("{}/api/services/{domain}/{service}", self.url))
            .bearer_auth(&self.token)
            .json(&data)
            .send()
            .await
            .context("Home Assistant request")?;
        let status = response.status();
        if !status.is_success() {
            bail!(
                "Home Assistant returned {status}: {}",
                response.text().await.unwrap_or_default()
            );
        }
        Ok(())
    }
}

fn speaker_ok(entity: &str) -> Result<()> {
    let name = entity.strip_prefix("media_player.").unwrap_or("");
    if name.is_empty() || !name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
        bail!("{entity:?} is not a media_player entity id; call list_speakers");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_media_players() {
        assert!(speaker_ok("media_player.kitchen_2").is_ok());
        assert!(speaker_ok("light.kitchen").is_err());
        assert!(speaker_ok("media_player.").is_err());
        assert!(speaker_ok("media_player.a/../b").is_err());
    }

    #[test]
    fn input_is_checked() {
        assert!(parse::<PlayMusic>(json!({"speaker": "media_player.a", "query": "jazz"})).is_ok());
        assert!(parse::<PlayMusic>(json!({"speaker": "media_player.a"})).is_err());
        assert!(parse::<PlayMusic>(json!({"speaker": "x", "query": "y", "extra": 1})).is_err());
    }

    /// A Home Assistant stand-in: two media players; records each request line and body.
    async fn fake_home() -> (String, tokio::sync::mpsc::UnboundedReceiver<String>) {
        use tokio::io::{AsyncReadExt, AsyncWriteExt};
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = format!("http://{}", listener.local_addr().unwrap());
        let (tx, rx) = tokio::sync::mpsc::unbounded_channel();
        tokio::spawn(async move {
            loop {
                let (mut socket, _) = listener.accept().await.unwrap();
                let mut buf = Vec::new();
                let (head_end, length) = loop {
                    let mut chunk = [0u8; 4096];
                    let n = socket.read(&mut chunk).await.unwrap();
                    buf.extend_from_slice(&chunk[..n]);
                    let text = String::from_utf8_lossy(&buf).to_string();
                    if let Some(end) = text.find("\r\n\r\n") {
                        let length = text[..end]
                            .lines()
                            .find_map(|l| {
                                l.to_ascii_lowercase()
                                    .strip_prefix("content-length:")
                                    .map(|v| v.trim().parse::<usize>().unwrap())
                            })
                            .unwrap_or(0);
                        break (end + 4, length);
                    }
                };
                while buf.len() < head_end + length {
                    let mut chunk = [0u8; 4096];
                    let n = socket.read(&mut chunk).await.unwrap();
                    buf.extend_from_slice(&chunk[..n]);
                }
                let text = String::from_utf8_lossy(&buf).to_string();
                let line = text
                    .lines()
                    .next()
                    .unwrap_or_default()
                    .rsplit_once(' ')
                    .unwrap()
                    .0
                    .to_string();
                let body = text[head_end..].to_string();
                let reply = if line.starts_with("GET /api/states") {
                    r#"[{"entity_id": "media_player.kitchen", "state": "idle", "attributes": {"friendly_name": "Kitchen"}},
                        {"entity_id": "media_player.bedroom", "state": "playing", "attributes": {"friendly_name": "Bedroom"}}]"#
                } else {
                    "[]"
                };
                tx.send(format!("{line} {body}").trim().to_string())
                    .unwrap();
                let response = format!(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{reply}",
                    reply.len()
                );
                socket.write_all(response.as_bytes()).await.unwrap();
            }
        });
        (url, rx)
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn run_js_calls_the_home_tools() {
        // SAFETY: tests that read this variable don't run alongside others that change it.
        unsafe { std::env::set_var("INKHARNESS_TEST_HA_TOKEN", "t") };
        let (url, mut requests) = fake_home().await;
        let ha = config::HomeAssistant {
            url,
            token_env: "INKHARNESS_TEST_HA_TOKEN".into(),
            play_service: "music_assistant.play_media".into(),
        };
        let tools = ToolSet::build(
            Some(&ha),
            &config::Js::default(),
            Default::default(),
            Default::default(),
            reqwest::Client::new(),
        )
        .unwrap();
        let names: Vec<&str> = tools.defs().iter().map(|d| d.name.as_str()).collect();
        assert_eq!(
            names,
            ["list_speakers", "play_music", "media_control", "run_js"]
        );

        let code = r#"
            const playing = tools.list_speakers().split("\n")
                .filter(l => l.endsWith("| playing")).map(l => l.split(" | ")[0]);
            for (const s of playing) tools.media_control({speaker: s, action: "pause"});
            let refused = "";
            try { tools.media_control({speaker: "light.hall", action: "stop"}) } catch (e) { refused = e.message }
            print(typeof tools.run_js, typeof tools.think_harder);
            ({paused: playing, refused})
        "#;
        let out = tools.call(JS_TOOL, json!({"code": code})).await.unwrap();
        assert!(out.starts_with("undefined undefined\n=> "), "{out}");
        let result: Value = serde_json::from_str(out.split_once("=> ").unwrap().1).unwrap();
        assert_eq!(result["paused"], json!(["media_player.bedroom"]));
        assert!(
            result["refused"]
                .as_str()
                .unwrap()
                .contains("not a media_player"),
            "{result}"
        );

        assert_eq!(requests.recv().await.unwrap(), "GET /api/states");
        assert_eq!(
            requests.recv().await.unwrap(),
            r#"POST /api/services/media_player/media_pause {"entity_id":"media_player.bedroom"}"#
        );
        assert!(
            requests.try_recv().is_err(),
            "the refused call never reached Home Assistant"
        );

        let err = tools.call(JS_TOOL, json!({})).await.unwrap_err();
        assert!(err.contains("code is required"));
    }

    #[tokio::test]
    async fn skill_tools_go_through_the_tool_set() {
        use crate::skills::tests::{scratch, write};
        let dir = scratch("toolset");
        write(
            &dir.join("tidy/SKILL.md"),
            "---\nname: tidy\ndescription: Tidies.\n---\nRead steps.md.\n",
        );
        write(&dir.join("tidy/steps.md"), "Step one.");
        let skills = Arc::new(Skills::load(&[dir]));
        let tools = ToolSet::build(
            None,
            &config::Js::default(),
            Default::default(),
            skills,
            reqwest::Client::new(),
        )
        .unwrap();
        assert!(!tools.has_home());
        let names: Vec<&str> = tools.defs().iter().map(|d| d.name.as_str()).collect();
        assert_eq!(names, ["run_js", "load_skill", "read_skill_file"]);
        // Both models get them: the fast model's set is the same plus think_harder.
        let fast: Vec<String> = tools
            .with_think()
            .defs()
            .iter()
            .map(|d| d.name.clone())
            .collect();
        assert_eq!(
            fast,
            ["run_js", "load_skill", "read_skill_file", THINK_TOOL]
        );

        assert_eq!(
            tools.call("load_skill", json!({"name": "tidy"})).await,
            Ok("Read steps.md.\n\nFiles in this skill (read_skill_file): steps.md".into())
        );
        assert_eq!(
            tools
                .call(
                    "read_skill_file",
                    json!({"skill": "tidy", "path": "steps.md"})
                )
                .await,
            Ok("Step one.".into())
        );
        let err = tools.call("load_skill", json!({"name": "nope"})).await;
        assert_eq!(
            err,
            Err("no skill named \"nope\"; the skills are: tidy".into())
        );
        let err = tools.call("load_skill", json!({"skill": "tidy"})).await;
        assert!(err.unwrap_err().starts_with("invalid input"));
        // Scripts don't get them.
        let out = tools
            .call(JS_TOOL, json!({"code": "typeof tools.load_skill"}))
            .await;
        assert_eq!(out, Ok("=> undefined".into()));
        assert!(!tools.defs()[0].description.contains("load_skill"));
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn the_goodnight_skill_pauses_what_plays() {
        // SAFETY: tests that read this variable don't run alongside others that change it.
        unsafe { std::env::set_var("INKHARNESS_TEST_HA_TOKEN", "t") };
        let (url, mut requests) = fake_home().await;
        let ha = config::HomeAssistant {
            url,
            token_env: "INKHARNESS_TEST_HA_TOKEN".into(),
            play_service: "music_assistant.play_media".into(),
        };
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../skills");
        let skills = Arc::new(Skills::load(&[dir]));
        assert!(skills.prompt().contains("\n- goodnight: Bedtime routine."));
        let tools = ToolSet::build(
            Some(&ha),
            &config::Js::default(),
            Default::default(),
            skills,
            reqwest::Client::new(),
        )
        .unwrap();
        let body = tools
            .call("load_skill", json!({"name": "goodnight"}))
            .await
            .unwrap();
        // The script in the instructions, run as the model is told to.
        let code = body
            .split("```js\n")
            .nth(1)
            .unwrap()
            .split("```")
            .next()
            .unwrap();
        let out = tools.call(JS_TOOL, json!({"code": code})).await;
        assert_eq!(out, Ok("=> 1".into()));
        assert_eq!(requests.recv().await.unwrap(), "GET /api/states");
        assert_eq!(
            requests.recv().await.unwrap(),
            r#"POST /api/services/media_player/media_pause {"entity_id":"media_player.bedroom"}"#
        );
    }

    #[tokio::test]
    async fn run_js_calls_mcp_tools() {
        let mcp = Arc::new(crate::mcp::tests::connected(None).await);
        let tools = ToolSet::build(
            None,
            &config::Js::default(),
            mcp,
            Default::default(),
            reqwest::Client::new(),
        )
        .unwrap();
        assert!(tools.has_home());
        let names: Vec<&str> = tools.defs().iter().map(|d| d.name.as_str()).collect();
        assert!(
            names.contains(&"test_add") && names.last() == Some(&"run_js"),
            "{names:?}"
        );
        assert_eq!(
            tools.call("test_add", json!({"a": 1, "b": 2})).await,
            Ok("3".into())
        );
        let code = "const n = Number(tools.test_add({a: 20, b: 22})); \
                    let e = ''; try { tools.test_broken() } catch (x) { e = x.message } [n, e]";
        let out = tools.call(JS_TOOL, json!({"code": code})).await;
        assert_eq!(out, Ok(r#"=> [42,"it broke"]"#.into()));
    }
}
