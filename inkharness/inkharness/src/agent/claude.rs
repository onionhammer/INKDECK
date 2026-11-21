//! The claude backend: a session is a headless Claude Code run (`claude -p`, stream-json in and
//! out) in a working folder of its own. The prompt goes in as a user message, and so do later
//! words said into the session, while it runs. Its tool calls go to the session's log, its
//! TodoWrite becomes the plan, and its final result the session's result. It reaches
//! inkharness's ask_choice and plan through the MCP bridge (bridge.rs).
//!
//! Credentials follow the Agent SDK: the run gets the daemon's environment unchanged, so Claude
//! Code uses ANTHROPIC_API_KEY when it is set (the daemon's .env counts) and the subscription
//! it is logged in with otherwise.
//!
//! Claude Code's own session id is kept, so a run cut off by a daemon restart, or a finished
//! session talked into, carries on with `--resume`.

use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{Result, anyhow};
use serde_json::{Value, json};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::process::{Child, ChildStdin, Command};
use tokio::sync::Notify;
use voice_proto::State;

use super::store::{self, Who};
use super::tools::{self, ASK_TOOL, PANEL_TOOL, PLAN_TOOL, SCHEDULE_TOOL, WATCH_TOOL};
use super::{Agent, bridge};
use crate::config;
use crate::meter;

/// The program a claude session says it runs.
pub const PROGRAM: &str = "claude code";
/// How long a stopped run's process has after SIGTERM before SIGKILL.
const STOP_GRACE: Duration = Duration::from_secs(5);
/// What Claude Code's MCP tool calls may take: a question can wait days for its answer.
const MCP_TOOL_TIMEOUT_MS: &str = "604800000";

/// The claude backend, as configured.
#[derive(Clone)]
pub struct Claude {
    pub command: Vec<String>,
    pub model: String,
    pub permission_mode: String,
    pub allowed_tools: Vec<String>,
    pub disallowed_tools: Vec<String>,
    /// Each session works in `<dir>/<id>`.
    pub dir: PathBuf,
    pub max_turns: u32,
    /// The run's whole environment (child_env).
    pub env: Vec<(OsString, OsString)>,
    /// This binary, which serves the MCP bridge (None: the run gets no inkharness tools).
    pub mcp: Option<PathBuf>,
}

/// The run's environment: the daemon's as it is, plus ANTHROPIC_API_KEY when only the daemon's
/// .env has it (`dotenv_key`), and a long MCP tool timeout unless one is set.
pub fn child_env(
    process: impl Iterator<Item = (OsString, OsString)>,
    dotenv_key: Option<String>,
) -> Vec<(OsString, OsString)> {
    let mut env: Vec<(OsString, OsString)> = process.collect();
    let has = |env: &[(OsString, OsString)], name: &str| env.iter().any(|(k, _)| k == name);
    if !has(&env, "ANTHROPIC_API_KEY")
        && let Some(key) = dotenv_key
    {
        env.push(("ANTHROPIC_API_KEY".into(), key.into()));
    }
    if !has(&env, "MCP_TOOL_TIMEOUT") {
        env.push(("MCP_TOOL_TIMEOUT".into(), MCP_TOOL_TIMEOUT_MS.into()));
    }
    env
}

impl Claude {
    pub fn new(cfg: &config::Claude) -> Claude {
        Claude {
            command: cfg.command.clone(),
            model: cfg.model.clone(),
            permission_mode: cfg.permission_mode.clone(),
            allowed_tools: cfg.allowed_tools.clone(),
            disallowed_tools: cfg.disallowed_tools.clone(),
            dir: PathBuf::from(&cfg.dir),
            max_turns: cfg.max_turns,
            env: child_env(
                std::env::vars_os(),
                config::lookup("ANTHROPIC_API_KEY")
                    .filter(|_| std::env::var_os("ANTHROPIC_API_KEY").is_none()),
            ),
            mcp: std::env::current_exe().ok(),
        }
    }

    /// What the runs pay with: "ANTHROPIC_API_KEY" or "subscription".
    pub fn credential(&self) -> &'static str {
        let set = self
            .env
            .iter()
            .any(|(k, v)| k == "ANTHROPIC_API_KEY" && !v.is_empty());
        if set {
            "ANTHROPIC_API_KEY"
        } else {
            "subscription"
        }
    }

    /// Checks that the command runs (`--version`): its answer.
    pub async fn check(&self) -> Result<String> {
        let (program, args) = self
            .command
            .split_first()
            .ok_or_else(|| anyhow!("[agent.claude] command is empty"))?;
        let output = tokio::time::timeout(
            Duration::from_secs(30),
            Command::new(program)
                .args(args)
                .arg("--version")
                .env_clear()
                .envs(self.env.iter().cloned())
                .stdin(Stdio::null())
                .output(),
        )
        .await
        .map_err(|_| anyhow!("{program} --version took over 30 s"))??;
        if !output.status.success() {
            return Err(anyhow!(
                "{program} --version: {}",
                String::from_utf8_lossy(&output.stderr).trim()
            ));
        }
        Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
    }

    /// The arguments after the command.
    pub fn args(
        &self,
        mcp_config: Option<&Path>,
        resume: Option<&str>,
        prompt: &str,
    ) -> Vec<String> {
        let mut args: Vec<String> = [
            "-p",
            "--output-format",
            "stream-json",
            "--input-format",
            "stream-json",
            "--verbose",
            // Nobody is at this terminal: what would ask for permission is denied.
            "--permission-prompts",
            "none",
            "--permission-mode",
        ]
        .map(String::from)
        .to_vec();
        args.push(self.permission_mode.clone());
        args.push("--max-turns".into());
        args.push(self.max_turns.to_string());
        if !self.model.is_empty() {
            args.extend(["--model".into(), self.model.clone()]);
        }
        let mut allowed = self.allowed_tools.clone();
        if let Some(file) = mcp_config {
            args.extend(["--mcp-config".into(), file.display().to_string()]);
            allowed.push(format!("mcp__{}", bridge::SERVER));
        }
        if !allowed.is_empty() {
            args.push("--allowedTools".into());
            args.extend(allowed);
        }
        if !self.disallowed_tools.is_empty() {
            args.push("--disallowedTools".into());
            args.extend(self.disallowed_tools.iter().cloned());
        }
        args.extend(["--append-system-prompt".into(), prompt.to_string()]);
        if let Some(id) = resume {
            args.extend(["--resume".into(), id.to_string()]);
        }
        args
    }
}

/// What a claude run is told on top of Claude Code's own prompt.
pub fn prompt() -> String {
    format!(
        "You are working through this task on your own, for someone who checks on it now and \
         then on a small black-and-white e-paper screen. Nobody reads this terminal.\n\
         - For a job with more than one step, keep a todo list (TodoWrite): it shows on their \
         screen as the plan. mcp__{s}__{PLAN_TOOL} sets a short title for the task.\n\
         - When a choice is genuinely theirs (which plumber to book, which flight to buy), call \
         mcp__{s}__{ASK_TOOL} and wait for the answer: it reaches them on their board, and \
         nobody would see a question asked in text. Decide everything else yourself.\n\
         - For something every day or every week (\"every weekday at seven, brief me\"), call \
         mcp__{s}__{SCHEDULE_TOOL}; to keep an eye on something until a condition holds (a \
         fare under $700), call mcp__{s}__{WATCH_TOOL}. Don't wait or loop yourself.\n\
         - For something they keep an eye on, mcp__{s}__{PANEL_TOOL} draws a simple screen on \
         their board.\n\
         - Keep what you write between tool calls to a few words.\n\
         - End with the result in Markdown, short and to the point as it should read on that \
         screen. If it is something to keep and read later (a brief, a summary, a recipe), \
         start it with a `# ` heading.",
        s = bridge::SERVER
    )
}

/// A user message in stream-json.
fn user_message(text: &str) -> String {
    let message = json!({
        "type": "user",
        "message": {"role": "user", "content": [{"type": "text", "text": text}]},
        "parent_tool_use_id": null,
    });
    format!("{message}\n")
}

/// What a line of Claude Code's stream said that the run acts on.
enum Line {
    Nothing,
    /// The final result of a turn: its text, whether it failed, what it cost.
    Result {
        ok: bool,
        text: String,
    },
}

impl Agent {
    /// A claude session's run: one Claude Code process, fed the session's words until it has
    /// answered them all. `run` is its generation; `wake` says words are waiting or it was
    /// stopped.
    pub(super) async fn run_claude(self: Arc<Self>, id: String, run: u64, wake: Arc<Notify>) {
        let Some(claude) = self.claude.clone() else {
            self.fail(
                &id,
                run,
                &anyhow!("the claude backend is off (the daemon's log says why)"),
            );
            return;
        };
        let Some((first, resume)) = self.claude_begin(&id, run) else {
            return;
        };
        let work = claude.dir.join(&id);
        if let Err(e) = std::fs::create_dir_all(&work) {
            self.fail(&id, run, &anyhow!("making {}: {e}", work.display()));
            return;
        }
        let mcp_config = self.mcp_config(&claude, &id, run);
        let args = claude.args(mcp_config.as_deref(), resume.as_deref(), &prompt());
        let (program, command_args) = claude.command.split_first().expect("checked at startup");
        let spawned = Command::new(program)
            .args(command_args)
            .args(&args)
            .current_dir(&work)
            .env_clear()
            .envs(claude.env.iter().cloned())
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .kill_on_drop(true)
            .spawn();
        let mut child = match spawned {
            Ok(child) => child,
            Err(e) => {
                self.fail(&id, run, &anyhow!("couldn't start {program}: {e}"));
                return;
            }
        };
        tracing::info!(session = %id, resume = resume.is_some(), credential = claude.credential(), "claude run");
        let mut stdin = child.stdin.take();
        let mut lines = BufReader::new(child.stdout.take().expect("piped")).lines();
        let stderr = tokio::spawn(tail(child.stderr.take().expect("piped")));

        let mut sent = 0usize;
        let mut answered = 0usize;
        if !write(&mut stdin, &first).await {
            sent = usize::MAX; // nothing more can go in
        } else {
            sent += 1;
        }
        let mut last_result = None;
        let outcome = meter::measure(async {
            loop {
                tokio::select! {
                    line = lines.next_line() => {
                        let Ok(Some(line)) = line else {
                            return Err(None); // it ended
                        };
                        match self.claude_line(&id, run, &line) {
                            Line::Nothing => {}
                            Line::Result { ok: false, text } => return Err(Some(text)),
                            Line::Result { ok: true, text } => {
                                answered += 1;
                                last_result = Some(text);
                                if answered < sent {
                                    continue; // more words already went in
                                }
                                let more = self.hear(&id, run);
                                if more.is_empty() {
                                    return Ok(());
                                }
                                for text in more {
                                    if write(&mut stdin, &text).await {
                                        sent += 1;
                                    }
                                }
                            }
                        }
                    }
                    () = wake.notified() => {
                        if !self.owns(&id, run) {
                            return Err(Some(String::new())); // stopped or deleted
                        }
                        for text in self.hear(&id, run) {
                            if write(&mut stdin, &text).await {
                                sent += 1;
                            }
                        }
                    }
                }
            }
        })
        .await;
        let (outcome, charges) = outcome;
        let (cost, _) = meter::total(&charges);
        let notional = claude.credential() == "subscription";
        self.add_cost(&id, run, cost, notional);
        tracing::info!(session = %id, cost, notional, "claude run ended (on a subscription the cost is notional)");
        match outcome {
            Ok(()) => {
                drop(stdin.take()); // it exits once its input ends
                let _ = tokio::time::timeout(STOP_GRACE, child.wait()).await;
                if self.end_claude(&id, run, last_result.unwrap_or_default()) {
                    // Words came just as it finished: a new run, resumed, hears them.
                    self.rerun(&id, run);
                }
            }
            Err(Some(text)) if !self.owns(&id, run) => {
                let _ = text;
                terminate(&mut child).await;
            }
            Err(Some(text)) => {
                terminate(&mut child).await;
                self.fail(
                    &id,
                    run,
                    &anyhow!(
                        "{}",
                        if text.is_empty() {
                            "Claude Code stopped".into()
                        } else {
                            text
                        }
                    ),
                );
            }
            Err(None) if last_result.is_some() && self.owns(&id, run) => {
                // It ended on its own after answering: its last answer is the result.
                let _ = child.wait().await;
                if self.end_claude(&id, run, last_result.unwrap_or_default()) {
                    self.rerun(&id, run);
                }
            }
            Err(None) => {
                let status = child.wait().await.ok();
                let said = stderr.await.unwrap_or_default();
                if self.owns(&id, run) {
                    let code = status
                        .and_then(|s| s.code())
                        .map_or("a signal".to_string(), |c| format!("code {c}"));
                    let message = match said.trim() {
                        "" => format!("Claude Code ended ({code}) before it answered"),
                        said => format!("Claude Code ended ({code}): {said}"),
                    };
                    self.fail(&id, run, &anyhow!(message));
                }
            }
        }
    }

    /// The run's first message and the Claude Code session to resume, moving what waited into
    /// the conversation. A fresh run (nothing to resume) is given everything said so far.
    fn claude_begin(&self, id: &str, run: u64) -> Option<(String, Option<String>)> {
        let mut sessions = self.sessions.lock().unwrap();
        let live =
            super::own(&mut sessions, id, run).filter(|l| l.session.state == State::Working)?;
        let s = &mut live.session;
        let waiting = std::mem::take(&mut s.inbox);
        let resume = s.claude.clone();
        let earlier: Vec<String> = match resume {
            Some(_) => Vec::new(),
            None => s
                .conversation
                .iter()
                .filter(|m| m.role == Who::User)
                .map(|m| m.text.clone())
                .collect(),
        };
        for text in &waiting {
            s.add_said(Who::User, text);
        }
        self.save(s);
        let first: Vec<String> = earlier.into_iter().chain(waiting).collect();
        let first = match first.join("\n\n") {
            text if text.trim().is_empty() => "Carry on.".to_string(),
            text => text,
        };
        Some((first, resume))
    }

    /// Writes the run's MCP config (its key to the bridge) beside the session's file, readable
    /// only by the daemon's user. None if the run gets no inkharness tools.
    fn mcp_config(&self, claude: &Claude, id: &str, run: u64) -> Option<PathBuf> {
        let (exe, bridge) = (claude.mcp.as_ref()?, self.bridge.get()?);
        let key = bridge.issue(id, run);
        let config = json!({"mcpServers": {bridge::SERVER: {
            "command": exe,
            "args": ["mcp-bridge"],
            "env": {bridge::ADDR_VAR: bridge.addr.to_string(), bridge::KEY_VAR: key},
        }}});
        let file = self.dir.join(format!("{id}.mcp.json"));
        let written = write_private(&file, config.to_string().as_bytes());
        match written {
            Ok(()) => Some(file),
            Err(e) => {
                tracing::warn!(session = %id, "couldn't write its MCP config: {e}");
                None
            }
        }
    }

    /// True while `run` is still the session's run.
    fn owns(&self, id: &str, run: u64) -> bool {
        let mut sessions = self.sessions.lock().unwrap();
        super::own(&mut sessions, id, run).is_some()
    }

    /// Acts on one line of the run's stream-json.
    fn claude_line(&self, id: &str, run: u64, line: &str) -> Line {
        let Ok(message) = serde_json::from_str::<Value>(line) else {
            return Line::Nothing;
        };
        match message["type"].as_str().unwrap_or_default() {
            "system" if message["subtype"] == "init" => {
                self.keep_claude_id(id, run, &message);
                let tools_ok = message["mcp_servers"].as_array().is_none_or(|servers| {
                    servers
                        .iter()
                        .all(|s| s["name"] != bridge::SERVER || s["status"] == "connected")
                });
                if !tools_ok {
                    tracing::warn!(session = %id, "inkharness's tools didn't connect to the claude run");
                    self.log(id, run, "inkharness tools not connected");
                }
                Line::Nothing
            }
            "assistant" => {
                let blocks = message["message"]["content"]
                    .as_array()
                    .cloned()
                    .unwrap_or_default();
                let calls = blocks.iter().any(|b| b["type"] == "tool_use");
                for block in &blocks {
                    match block["type"].as_str().unwrap_or_default() {
                        // Text beside tool calls is a line of work; without, the answer.
                        "text" if calls => {
                            let text = block["text"].as_str().unwrap_or_default();
                            if !text.trim().is_empty() {
                                self.log(id, run, &store::plain(text));
                            }
                        }
                        "tool_use" => self.claude_call(id, run, block),
                        _ => {}
                    }
                }
                Line::Nothing
            }
            "result" => {
                self.keep_claude_id(id, run, &message);
                meter::record(
                    "claude",
                    message["session_id"].as_str(),
                    message["total_cost_usd"].as_f64(),
                );
                let ok = message["subtype"] == "success" && message["is_error"] != true;
                let text = match message["result"].as_str() {
                    Some(text) if !text.trim().is_empty() => text.to_string(),
                    _ if ok => String::new(),
                    _ => format!(
                        "Claude Code: {}",
                        message["subtype"]
                            .as_str()
                            .unwrap_or("error")
                            .replace('_', " ")
                    ),
                };
                Line::Result { ok, text }
            }
            _ => Line::Nothing,
        }
    }

    /// A tool call: TodoWrite is the plan; inkharness's own show for themselves; the rest go to
    /// the log.
    fn claude_call(&self, id: &str, run: u64, block: &Value) {
        let name = block["name"].as_str().unwrap_or_default();
        let input = &block["input"];
        if name == "TodoWrite" {
            let todos = input["todos"].as_array().cloned().unwrap_or_default();
            let steps: Vec<String> = todos
                .iter()
                .filter_map(|t| t["content"].as_str())
                .map(|t| store::one_line(t, 60))
                .collect();
            let state = |t: &Value| t["status"].as_str().unwrap_or_default().to_string();
            let current = todos
                .iter()
                .position(|t| state(t) == "in_progress")
                .or_else(|| todos.iter().position(|t| state(t) == "pending"))
                .map_or(steps.len() + 1, |i| i + 1);
            if !steps.is_empty() {
                let _ = self.set_plan(id, run, steps, current, None, None);
            }
            return;
        }
        let own = format!("mcp__{}__", bridge::SERVER);
        match name.strip_prefix(&own) {
            Some(name) if tools::shows_itself(name) => {}
            Some(other) => self.log(id, run, &tools::short_call(other, input)),
            None => self.log(id, run, &tools::short_call(name, input)),
        }
    }

    /// Keeps Claude Code's session id, to resume with.
    fn keep_claude_id(&self, id: &str, run: u64, message: &Value) {
        let Some(claude_id) = message["session_id"].as_str() else {
            return;
        };
        let mut sessions = self.sessions.lock().unwrap();
        if let Some(live) = super::own(&mut sessions, id, run)
            && live.session.claude.as_deref() != Some(claude_id)
        {
            live.session.claude = Some(claude_id.to_string());
            self.save(&live.session);
        }
    }

    /// The run answered everything said to it: the session is done (or a note). True if more
    /// words came in meanwhile.
    fn end_claude(&self, id: &str, run: u64, result: String) -> bool {
        self.end_leg(
            id,
            run,
            super::Written {
                all: result.clone(),
                result,
            },
        )
    }

    /// Starts the session's next run, if `run` is still its run.
    fn rerun(self: &Arc<Self>, id: &str, run: u64) {
        let mut sessions = self.sessions.lock().unwrap();
        if let Some(live) = super::own(&mut sessions, id, run) {
            self.spawn_run(live);
        }
    }
}

/// Writes a user message; false once the run's input has closed.
async fn write(stdin: &mut Option<ChildStdin>, text: &str) -> bool {
    let Some(pipe) = stdin.as_mut() else {
        return false;
    };
    let ok =
        pipe.write_all(user_message(text).as_bytes()).await.is_ok() && pipe.flush().await.is_ok();
    if !ok {
        *stdin = None;
    }
    ok
}

/// Ends a run's process: SIGTERM, then SIGKILL after STOP_GRACE.
async fn terminate(child: &mut Child) {
    if let Some(pid) = child.id() {
        let _ = Command::new("kill")
            .args(["-TERM", &pid.to_string()])
            .stderr(Stdio::null())
            .status()
            .await;
        if tokio::time::timeout(STOP_GRACE, child.wait()).await.is_ok() {
            return;
        }
    }
    let _ = child.kill().await;
}

/// The last few lines a run wrote to stderr.
async fn tail(stderr: tokio::process::ChildStderr) -> String {
    let mut lines = BufReader::new(stderr).lines();
    let mut last: Vec<String> = Vec::new();
    while let Ok(Some(line)) = lines.next_line().await {
        last.push(line);
        if last.len() > 8 {
            last.remove(0);
        }
    }
    store::cut(&last.join(" / "), 600)
}

/// Writes a file only its owner can read.
fn write_private(file: &Path, bytes: &[u8]) -> std::io::Result<()> {
    let mut options = std::fs::OpenOptions::new();
    options.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    use std::io::Write;
    options.open(file)?.write_all(bytes)
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;

    use super::*;
    use crate::agent::store::{Session, Who};
    use crate::agent::tests::{cfg, tools, until};
    use crate::agent::{Backends, Live, mock};
    use voice_proto::Backend;

    /// Claude Code as a script: it replays what a real run prints, steered by words in the
    /// first message ("ask", "fail", "wait", "hang"), and answers later messages with
    /// "heard: …". The ask round trip goes to the bridge as the MCP server would.
    const FAKE: &str = r##"
echo "$*" > args.txt
mcp=""; resume=""
while [ $# -gt 0 ]; do
  case "$1" in
    --mcp-config) mcp="$2"; shift ;;
    --resume) resume="$2"; shift ;;
  esac
  shift
done
text_of() { printf '%s' "$1" | sed -n 's/.*"text":"\([^"]*\)".*/\1/p'; }
result() { printf '{"type":"result","subtype":"success","is_error":false,"result":"%s","session_id":"sess-1","total_cost_usd":0.02}\n' "$1"; }
read -r first
printf '{"type":"system","subtype":"init","session_id":"sess-1","mcp_servers":[{"name":"inkharness","status":"connected"}],"model":"claude-test"}\n'
case "$first" in
  *"please fail"*)
    printf '{"type":"result","subtype":"error_max_turns","is_error":true,"session_id":"sess-1","total_cost_usd":0.01}\n' ;;
  *"ask me"*)
    printf '{"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"TodoWrite","input":{"todos":[{"content":"Find plumbers","status":"completed"},{"content":"Compare them","status":"in_progress"},{"content":"Book one","status":"pending"}]}}]},"session_id":"sess-1"}\n'
    printf '{"type":"assistant","message":{"content":[{"type":"text","text":"Searching."},{"type":"tool_use","id":"t2","name":"WebSearch","input":{"query":"plumbers open Saturday"}}]},"session_id":"sess-1"}\n'
    printf '{"type":"assistant","message":{"content":[{"type":"tool_use","id":"t3","name":"mcp__inkharness__ask_choice","input":{}}]},"session_id":"sess-1"}\n'
    addr=$(sed -n 's/.*"INKHARNESS_BRIDGE":"\([^"]*\)".*/\1/p' "$mcp")
    key=$(sed -n 's/.*"INKHARNESS_BRIDGE_KEY":"\([^"]*\)".*/\1/p' "$mcp")
    exec 3<>"/dev/tcp/${addr%:*}/${addr##*:}"
    printf '{"key":"%s","tool":"ask_choice","input":{"question":"Which plumber?","options":[{"title":"Riverside","right":"$145"},{"title":"Northgate"}]}}\n' "$key" >&3
    read -r answer <&3
    exec 3<&-
    esc=$(printf '%s' "$answer" | sed 's/\\/\\\\/g; s/"/\\"/g')
    result "Booked: $esc" ;;
  *"wait for me"*)
    read -r second
    result "first done"
    result "heard: $(text_of "$second")" ;;
  *"hang around"*)
    echo $$ > pid
    while read -r line; do :; done
    exit 0 ;;
  *)
    result "key=${ANTHROPIC_API_KEY:-none} resume=${resume:-none}" ;;
esac
while read -r line; do result "heard: $(text_of "$line")"; done
"##;

    fn fake(dir: &std::path::Path, env: Vec<(OsString, OsString)>) -> Claude {
        Claude {
            command: vec!["bash".into(), "-c".into(), FAKE.into(), "claude".into()],
            model: String::new(),
            permission_mode: "default".into(),
            allowed_tools: vec!["Read".into()],
            disallowed_tools: vec![],
            dir: dir.join("work"),
            max_turns: 40,
            env,
            mcp: Some(PathBuf::from("/unused/inkharness")),
        }
    }

    fn env(key: Option<&str>) -> Vec<(OsString, OsString)> {
        let mut env: Vec<(OsString, OsString)> = std::env::vars_os()
            .filter(|(k, _)| k != "ANTHROPIC_API_KEY")
            .collect();
        if let Some(key) = key {
            env.push(("ANTHROPIC_API_KEY".into(), key.into()));
        }
        env
    }

    fn agent(name: &str, key: Option<&str>) -> (Arc<Agent>, std::path::PathBuf) {
        let dir = crate::skills::tests::scratch(name);
        let backends = Backends {
            default: Backend::Claude,
            claude: Some(fake(&dir, env(key))),
        };
        let agent = Agent::open(
            &cfg(&dir),
            Box::new(mock::MockAgent),
            tools(),
            "sys".into(),
            "test",
            backends,
        )
        .unwrap();
        (agent, dir)
    }

    #[test]
    fn credentials_as_the_agent_sdk_takes_them() {
        let process = |key: Option<&str>| env(key).into_iter();
        // A key in the environment goes through as it is; .env's isn't needed.
        let with = child_env(process(Some("sk-env")), Some("sk-dotenv".into()));
        let key = |env: &[(OsString, OsString)]| {
            env.iter()
                .find(|(k, _)| k == "ANTHROPIC_API_KEY")
                .map(|(_, v)| v.clone())
        };
        assert_eq!(key(&with), Some("sk-env".into()));
        // Only in .env: added. In neither: none, and Claude Code uses the subscription.
        assert_eq!(
            key(&child_env(process(None), Some("sk-dotenv".into()))),
            Some("sk-dotenv".into())
        );
        let none = child_env(process(None), None);
        assert_eq!(key(&none), None);
        assert!(none.iter().any(|(k, _)| k == "MCP_TOOL_TIMEOUT"));
        let mut claude = fake(std::path::Path::new("/tmp"), none);
        assert_eq!(claude.credential(), "subscription");
        claude.env = with;
        assert_eq!(claude.credential(), "ANTHROPIC_API_KEY");
    }

    #[test]
    fn the_command_line() {
        let mut claude = fake(std::path::Path::new("/tmp"), vec![]);
        claude.model = "haiku".into();
        claude.disallowed_tools = vec!["Bash".into()];
        let args = claude.args(
            Some(std::path::Path::new("/s/k7q2.mcp.json")),
            Some("sess-1"),
            "be brief",
        );
        let line = args.join(" ");
        for part in [
            "-p --output-format stream-json --input-format stream-json --verbose",
            "--permission-prompts none --permission-mode default --max-turns 40 --model haiku",
            "--mcp-config /s/k7q2.mcp.json --allowedTools Read mcp__inkharness --disallowedTools Bash",
            "--append-system-prompt be brief --resume sess-1",
        ] {
            assert!(line.contains(part), "{line}");
        }
        let args = claude.args(None, None, "x");
        assert!(
            !args.contains(&"--resume".to_string()) && !args.contains(&"--mcp-config".to_string())
        );
    }

    #[tokio::test]
    async fn a_run_plans_logs_asks_and_finishes() {
        let (agent, dir) = agent("claude-ask", Some("sk-test"));
        let id = agent.start("find a plumber and ask me").await.unwrap();
        let view = until(&agent, &id, State::Ask).await;
        assert_eq!(
            (view.backend, view.program.as_deref()),
            (Backend::Claude, Some(PROGRAM))
        );
        assert_eq!((view.step, view.steps), (Some(2), Some(3)));
        let log: Vec<&str> = view.log.iter().rev().map(|l| l.text.as_str()).collect();
        assert_eq!(
            log,
            [
                "Searching.",
                "WebSearch plumbers open Saturday",
                "asked: Which plumber?"
            ]
        );
        let ask = view.ask.unwrap();
        assert_eq!(ask.options[0].right.as_deref(), Some("$145"));
        agent.answer(&id, &ask.id, 1).await.unwrap();
        let view = until(&agent, &id, State::Done).await;
        assert_eq!(
            view.result.as_deref(),
            Some(r#"Booked: {"text":"{\"choice\":1,\"title\":\"Northgate\"}"}"#)
        );
        let sessions = agent.sessions.lock().unwrap();
        assert_eq!(sessions[&id].session.claude.as_deref(), Some("sess-1"));
        drop(sessions);
        // Its MCP config was only for the daemon's user, and its folder its own.
        let config = dir.join(format!("{id}.mcp.json"));
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                std::fs::metadata(&config).unwrap().permissions().mode() & 0o777,
                0o600
            );
        }
        let args = std::fs::read_to_string(dir.join("work").join(&id).join("args.txt")).unwrap();
        assert!(
            args.contains("--append-system-prompt") && args.contains("mcp__inkharness"),
            "{args}"
        );
    }

    #[tokio::test]
    async fn the_key_or_the_subscription_reaches_the_run() {
        for (key, shown) in [(Some("sk-test"), "key=sk-test"), (None, "key=none")] {
            let (agent, _) = agent(&format!("claude-key-{shown}"), key);
            let id = agent.start("say hi").await.unwrap();
            let view = until(&agent, &id, State::Done).await;
            assert_eq!(view.result.unwrap(), format!("{shown} resume=none"));
        }
    }

    #[tokio::test]
    async fn an_error_result_fails_it() {
        let (agent, _) = agent("claude-fail", None);
        let id = agent.start("this will please fail").await.unwrap();
        let view = until(&agent, &id, State::Fail).await;
        assert_eq!(view.result.as_deref(), Some("Claude Code: error max turns"));
    }

    #[tokio::test]
    async fn words_said_while_it_works_get_their_turn() {
        let (agent, _) = agent("claude-say", None);
        let id = agent.start("wait for me").await.unwrap();
        crate::agent::herdr::tests_soon(|| {
            agent.sessions.lock().unwrap()[&id].session.claude.is_some()
        })
        .await;
        agent.say(&id, "saturday").await.unwrap();
        let view = until(&agent, &id, State::Done).await;
        assert_eq!(view.result.as_deref(), Some("heard: saturday"));
        // Talked into once finished: resumed.
        agent.say(&id, "thanks").await.unwrap();
        let view = until(&agent, &id, State::Done).await;
        assert_eq!(view.result.as_deref(), Some("key=none resume=sess-1"));
    }

    #[tokio::test]
    async fn a_restart_resumes_the_claude_session() {
        let dir = crate::skills::tests::scratch("claude-resume");
        let mut s = Session::new("w1".into(), "tidy the notes", 100);
        s.inbox.clear();
        s.add_said(Who::User, "tidy the notes");
        s.backend = Backend::Claude;
        s.program = Some(PROGRAM.into());
        s.claude = Some("sess-9".into());
        store::save(&dir, &s).unwrap();
        let backends = Backends {
            default: Backend::Api,
            claude: Some(fake(&dir, env(None))),
        };
        let agent = Agent::open(
            &cfg(&dir),
            Box::new(mock::MockAgent),
            tools(),
            "sys".into(),
            "test",
            backends,
        )
        .unwrap();
        let view = until(&agent, "w1", State::Done).await;
        assert_eq!(view.result.as_deref(), Some("key=none resume=sess-9"));
    }

    #[tokio::test]
    async fn stop_ends_the_process() {
        let (agent, dir) = agent("claude-stop", None);
        let id = agent.start("hang around").await.unwrap();
        let pid_file = dir.join("work").join(&id).join("pid");
        crate::agent::herdr::tests_soon(|| pid_file.exists()).await;
        let pid = std::fs::read_to_string(&pid_file)
            .unwrap()
            .trim()
            .to_string();
        agent.stop(&id).await.unwrap();
        assert_eq!(agent.view(&id).unwrap().state, State::Stopped);
        let alive = || {
            std::process::Command::new("kill")
                .args(["-0", &pid])
                .stderr(Stdio::null())
                .status()
                .unwrap()
                .success()
        };
        crate::agent::herdr::tests_soon(|| !alive()).await;
        assert_eq!(
            agent.view(&id).unwrap().state,
            State::Stopped,
            "and stays stopped"
        );
    }

    #[tokio::test]
    async fn the_mcp_server_relays_to_the_session() {
        let (agent, _) = agent("claude-mcp", None);
        let mut s = Session::new("m1".into(), "x", 1);
        s.inbox.clear();
        s.backend = Backend::Claude;
        agent
            .sessions
            .lock()
            .unwrap()
            .insert("m1".into(), Live::new(s));
        let bridge = agent.bridge.get().unwrap().clone();
        let key = bridge.issue("m1", 0);
        let addr = bridge.addr.to_string();
        let (ours, theirs) = tokio::io::duplex(1 << 16);
        let (server_in, server_out) = tokio::io::split(theirs);
        let server = tokio::spawn({
            let (addr, key) = (addr.clone(), key.clone());
            async move { bridge::mcp_serve(server_in, server_out, &addr, &key).await }
        });
        let (reader, mut writer) = tokio::io::split(ours);
        let mut replies = BufReader::new(reader).lines();
        let ask = |id: u64, method: &str, params: Value| {
            let line = format!(
                "{}\n",
                json!({"jsonrpc": "2.0", "id": id, "method": method, "params": params})
            );
            async move { line }
        };
        for (id, method, params) in [
            (
                1,
                "initialize",
                json!({"protocolVersion": "2025-06-18", "capabilities": {}}),
            ),
            (2, "tools/list", json!({})),
            (
                3,
                "tools/call",
                json!({"name": "plan", "arguments": {"steps": ["Read", "Write"], "current": 2, "title": "Notes"}}),
            ),
        ] {
            writer
                .write_all(ask(id, method, params).await.as_bytes())
                .await
                .unwrap();
        }
        writer
            .write_all(b"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n")
            .await
            .unwrap();
        let mut got: HashMap<u64, Value> = HashMap::new();
        while got.len() < 3 {
            let line = replies.next_line().await.unwrap().unwrap();
            let reply: Value = serde_json::from_str(&line).unwrap();
            got.insert(reply["id"].as_u64().unwrap(), reply);
        }
        assert_eq!(got[&1]["result"]["protocolVersion"], "2025-06-18");
        let names: Vec<&str> = got[&2]["result"]["tools"]
            .as_array()
            .unwrap()
            .iter()
            .map(|t| t["name"].as_str().unwrap())
            .collect();
        assert_eq!(
            names,
            ["plan", "ask_choice", "schedule", "watch", "show_panel"]
        );
        assert!(got[&2]["result"]["tools"][1]["inputSchema"]["properties"]["options"].is_object());
        assert_eq!(
            got[&3]["result"]["content"][0]["text"],
            "Plan saved: step 2 of 2."
        );
        let view = agent.view("m1").unwrap();
        assert_eq!((view.title.as_str(), view.step), ("Notes", Some(2)));

        // ask_choice waits for the answer.
        writer.write_all(ask(4, "tools/call", json!({"name": "ask_choice", "arguments": {"question": "Which?", "options": [{"title": "A"}, {"title": "B"}]}})).await.as_bytes()).await.unwrap();
        until(&agent, "m1", State::Ask).await;
        agent.answer("m1", "a1", 0).await.unwrap();
        let line = replies.next_line().await.unwrap().unwrap();
        let reply: Value = serde_json::from_str(&line).unwrap();
        assert_eq!(
            reply["result"]["content"][0]["text"],
            r#"{"choice":0,"title":"A"}"#
        );
        assert_eq!(reply["result"]["isError"], false);
        writer.shutdown().await.unwrap();
        server.await.unwrap().unwrap();

        // Another run's key: its tools have ended.
        let (ours, theirs) = tokio::io::duplex(1 << 16);
        let (server_in, server_out) = tokio::io::split(theirs);
        tokio::spawn(async move { bridge::mcp_serve(server_in, server_out, &addr, "stale").await });
        let (reader, mut writer) = tokio::io::split(ours);
        writer
            .write_all(
                ask(
                    1,
                    "tools/call",
                    json!({"name": "plan", "arguments": {"steps": ["x"]}}),
                )
                .await
                .as_bytes(),
            )
            .await
            .unwrap();
        let line = BufReader::new(reader)
            .lines()
            .next_line()
            .await
            .unwrap()
            .unwrap();
        let reply: Value = serde_json::from_str(&line).unwrap();
        assert_eq!(reply["result"]["isError"], true);
        assert_eq!(
            reply["result"]["content"][0]["text"],
            "this run's tools have ended"
        );
    }
}
