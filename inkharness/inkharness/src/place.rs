//! Where inkharness starts. It reads what it finds in the folder it starts in and in the
//! global config folder (`~/.config/inkharness`, or `$INKHARNESS_HOME`):
//!
//! - `inkharness.toml`: the folder's file is layered over the global one. Tables merge key by
//!   key; anything else (a value, an array) replaces. A stage that names another `backend`
//!   replaces the global stage whole. Relative paths in a file are relative to its folder.
//! - `.env`: keys, the folder's over the global one's; the environment wins over both.
//! - `AGENTS.md` (or `AGENT.md`, or `CLAUDE.md`): instructions, added to the system prompt,
//!   the global file's first.
//! - Skills: `skills/`, `.agents/skills/` and `.claude/skills/` in the folder, the global
//!   `skills/`, and `[skills] dirs`. Of two skills with one name, the folder's is kept.
//! - Slash commands: `commands/`, `.agents/commands/` and `.claude/commands/`, the global
//!   `commands/`, and `[commands] dirs`, found the same way.
//! - `.mcp.json`: MCP servers in the format other agents use (`mcpServers`), with `${VAR}`
//!   and `${VAR:-default}` taken from the environment and `.env`. They join `[[mcp]]`; of two
//!   servers with one name, the folder's file wins.

use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use serde::Deserialize;

use crate::config::{self, Config, McpServer};

pub const CONFIG_FILE: &str = "inkharness.toml";
/// The first of these a folder holds is its instructions.
const INSTRUCTION_FILES: [&str; 3] = ["AGENTS.md", "AGENT.md", "CLAUDE.md"];
/// Folders that hold skills, in the folder it starts in.
const SKILL_DIRS: [&str; 3] = ["skills", ".agents/skills", ".claude/skills"];
/// Folders that hold slash commands, in the folder it starts in.
const COMMAND_DIRS: [&str; 3] = ["commands", ".agents/commands", ".claude/commands"];
const MCP_FILE: &str = ".mcp.json";
/// An instructions file is cut at this size: all of it goes into every request.
const MAX_INSTRUCTIONS: usize = 64 * 1024;

pub struct Place {
    pub config: Config,
    /// Every file it read, in order (for the banner and the log).
    pub read: Vec<PathBuf>,
    /// The instructions files and their text, the global one first.
    pub instructions: Vec<(PathBuf, String)>,
    /// Folders to look for skills in, the first to be kept first.
    pub skill_dirs: Vec<PathBuf>,
    /// Folders to look for slash commands in, the same way.
    pub command_dirs: Vec<PathBuf>,
}

/// The global config folder: `$INKHARNESS_HOME`, else `$XDG_CONFIG_HOME/inkharness`, else
/// `~/.config/inkharness`.
pub fn global_dir() -> Option<PathBuf> {
    let var = |name| std::env::var_os(name).filter(|v| !v.is_empty());
    if let Some(home) = var("INKHARNESS_HOME") {
        return Some(home.into());
    }
    if let Some(xdg) = var("XDG_CONFIG_HOME") {
        return Some(PathBuf::from(xdg).join("inkharness"));
    }
    std::env::home_dir().map(|home| home.join(".config").join("inkharness"))
}

/// What `dir` and the global folder hold. `explicit` takes the folder file's place.
pub fn load(dir: &Path, explicit: Option<&Path>) -> Result<Place> {
    let global = global_dir();
    // The global folder itself is just the global folder.
    let dirs: Vec<&Path> = match &global {
        Some(g) if !same(g, dir) => vec![g.as_path(), dir],
        _ => vec![dir],
    };

    let mut layers: Vec<PathBuf> = Vec::new();
    if let Some(g) = global.as_ref().filter(|g| !same(g, dir)) {
        layers.push(g.join(CONFIG_FILE));
    }
    match explicit {
        Some(file) if !file.is_file() => bail!("{} isn't there", file.display()),
        Some(file) => layers.push(file.to_path_buf()),
        None => layers.push(dir.join(CONFIG_FILE)),
    }
    layers.retain(|f| f.is_file());
    if layers.is_empty() {
        bail!(
            "no {CONFIG_FILE} here or in {}: `inkharness init` writes a starter one",
            global.map_or("the global config folder".into(), |g| g
                .display()
                .to_string())
        );
    }

    // Keys first: .mcp.json and the backends read them.
    let mut envs: Vec<PathBuf> = dirs.iter().map(|d| d.join(".env")).collect();
    for layer in &layers {
        envs.insert(envs.len() - 1, layer.with_file_name(".env"));
    }
    dedup(&mut envs);
    let mut read = config::load_dotenv(&envs);

    let mut merged = toml::Table::new();
    for layer in &layers {
        let text = std::fs::read_to_string(layer)
            .with_context(|| format!("reading {}", layer.display()))?;
        let mut table: toml::Table =
            toml::from_str(&text).with_context(|| format!("parsing {}", layer.display()))?;
        resolve_paths(&mut table, layer.parent().unwrap_or(Path::new(".")));
        merge(&mut merged, table);
        read.push(layer.clone());
    }
    let names = || {
        layers
            .iter()
            .map(|l| l.display().to_string())
            .collect::<Vec<_>>()
            .join(" over ")
    };
    if !merged.contains_key("llm") {
        bail!("{} names no model: add an [llm] section", names());
    }
    let mut config = Config::from_table(merged).with_context(|| format!("in {}", names()))?;

    for file in dirs
        .iter()
        .map(|d| d.join(MCP_FILE))
        .filter(|f| f.is_file())
    {
        for server in read_mcp_json(&file)? {
            if let Some(i) = config.mcp.iter().position(|s| s.name == server.name) {
                tracing::info!(server = %server.name, file = %file.display(), "replaces an earlier MCP server of that name");
                config.mcp.remove(i);
            }
            config.mcp.push(server);
        }
        read.push(file);
    }

    let mut instructions = Vec::new();
    for d in &dirs {
        let Some(file) = INSTRUCTION_FILES
            .iter()
            .map(|n| d.join(n))
            .find(|f| f.is_file())
        else {
            continue;
        };
        let mut text = std::fs::read_to_string(&file)
            .with_context(|| format!("reading {}", file.display()))?;
        if text.len() > MAX_INSTRUCTIONS {
            tracing::warn!(file = %file.display(), "over {} KB; the rest is left out", MAX_INSTRUCTIONS / 1024);
            let mut cut = MAX_INSTRUCTIONS;
            while !text.is_char_boundary(cut) {
                cut -= 1;
            }
            text.truncate(cut);
        }
        if !text.trim().is_empty() {
            read.push(file.clone());
            instructions.push((file, text.trim().to_string()));
        }
    }

    let global = global.as_ref().filter(|g| !same(g, dir));
    let skill_dirs = folders(&config.skills.dirs, &SKILL_DIRS, dir, global, "skills");
    let command_dirs = folders(
        &config.commands.dirs,
        &COMMAND_DIRS,
        dir,
        global,
        "commands",
    );

    Ok(Place {
        config,
        read,
        instructions,
        skill_dirs,
        command_dirs,
    })
}

/// Where to look for skills or commands: the config's folders, then the ones of `names` this
/// folder has, then the global folder's `global_name`, each once.
fn folders(
    configured: &[String],
    names: &[&str],
    dir: &Path,
    global: Option<&PathBuf>,
    global_name: &str,
) -> Vec<PathBuf> {
    let mut found: Vec<PathBuf> = configured.iter().map(PathBuf::from).collect();
    found.extend(names.iter().map(|s| dir.join(s)).filter(|d| d.is_dir()));
    if let Some(g) = global {
        found.extend(Some(g.join(global_name)).filter(|d| d.is_dir()));
    }
    dedup(&mut found);
    found
}

fn same(a: &Path, b: &Path) -> bool {
    match (a.canonicalize(), b.canonicalize()) {
        (Ok(a), Ok(b)) => a == b,
        _ => a == b,
    }
}

/// Drops later duplicates of a path (the same file by another name too).
fn dedup(paths: &mut Vec<PathBuf>) {
    let mut seen: Vec<PathBuf> = Vec::new();
    paths.retain(|p| {
        let key = p.canonicalize().unwrap_or_else(|_| p.clone());
        if seen.contains(&key) {
            false
        } else {
            seen.push(key);
            true
        }
    });
}

/// `from` over `into`: tables merge key by key, except a stage with another backend, which
/// replaces (its fields wouldn't fit the other backend's).
fn merge(into: &mut toml::Table, from: toml::Table) {
    for (key, value) in from {
        match (into.get_mut(&key), value) {
            (Some(toml::Value::Table(old)), toml::Value::Table(new))
                if new
                    .get("backend")
                    .is_none_or(|b| old.get("backend") == Some(b)) =>
            {
                merge(old, new)
            }
            (_, value) => {
                into.insert(key, value);
            }
        }
    }
}

/// Makes the paths in a layer absolute, from its folder: skill folders, and the in-process
/// models' folders and voice. `~/` is the home folder.
fn resolve_paths(table: &mut toml::Table, base: &Path) {
    let fix = |value: &mut toml::Value| {
        if let toml::Value::String(s) = value {
            *s = resolve(base, s).to_string_lossy().into_owned();
        }
    };
    for key in ["skills", "commands"] {
        if let Some(toml::Value::Table(section)) = table.get_mut(key)
            && let Some(toml::Value::Array(dirs)) = section.get_mut("dirs")
        {
            dirs.iter_mut().for_each(fix);
        }
    }
    // The sessions' folder, from the file whose [agent] table is used, and the claude
    // backend's.
    if let Some(toml::Value::Table(agent)) = table.get_mut("agent") {
        let dir = agent
            .entry("dir")
            .or_insert_with(|| crate::config::default_agent_dir().into());
        fix(dir);
        if let Some(toml::Value::Table(claude)) = agent.get_mut("claude") {
            let dir = claude
                .entry("dir")
                .or_insert_with(|| crate::config::default_claude_dir().into());
            fix(dir);
        }
    }
    for stage in ["stt", "tts"] {
        let Some(toml::Value::Table(stage)) = table.get_mut(stage) else {
            continue;
        };
        // An `openai_compat` voice is a name, not a file.
        let keys: &[&str] = match stage.get("backend").and_then(|b| b.as_str()) {
            Some("sherpa_streaming") => &["dir"],
            Some("sherpa_pocket") => &["dir", "voice"],
            _ => &[],
        };
        for key in keys {
            if let Some(value) = stage.get_mut(*key) {
                fix(value);
            }
        }
    }
}

fn resolve(base: &Path, path: &str) -> PathBuf {
    if let (Some(rest), Some(home)) = (path.strip_prefix("~/"), std::env::home_dir()) {
        return home.join(rest);
    }
    base.join(path)
}

/// A .mcp.json, as Claude Code, Cursor and others write it.
#[derive(Deserialize)]
struct McpJson {
    #[serde(rename = "mcpServers", default)]
    servers: BTreeMap<String, McpJsonServer>,
}

#[derive(Deserialize)]
struct McpJsonServer {
    /// stdio (the default with a command), http (with a url), sse.
    #[serde(rename = "type")]
    kind: Option<String>,
    command: Option<String>,
    #[serde(default)]
    args: Vec<String>,
    #[serde(default)]
    env: HashMap<String, String>,
    url: Option<String>,
    #[serde(default)]
    headers: HashMap<String, String>,
    #[serde(default)]
    disabled: bool,
    /// inkharness's own: an allowlist, a timeout and whether talk gets its tools, as in
    /// `[[mcp]]`.
    tools: Option<Vec<String>>,
    timeout_ms: Option<u64>,
    talk: Option<bool>,
}

/// The servers in a .mcp.json. One that can't be used (a variable not set) is logged and left
/// out; a file that isn't JSON stops the start.
fn read_mcp_json(file: &Path) -> Result<Vec<McpServer>> {
    let text =
        std::fs::read_to_string(file).with_context(|| format!("reading {}", file.display()))?;
    let parsed: McpJson =
        serde_json::from_str(&text).with_context(|| format!("parsing {}", file.display()))?;
    let mut servers = Vec::new();
    for (name, server) in parsed.servers {
        if server.disabled {
            continue;
        }
        match mcp_server(&name, server) {
            Ok(server) => servers.push(server),
            Err(e) => {
                tracing::error!(server = %name, file = %file.display(), "MCP server left out: {e:#}")
            }
        }
    }
    Ok(servers)
}

fn mcp_server(name: &str, s: McpJsonServer) -> Result<McpServer> {
    let kind = s
        .kind
        .as_deref()
        .unwrap_or(if s.url.is_some() { "http" } else { "stdio" });
    let (command, url) = match kind {
        "stdio" => (
            Some(expand(s.command.as_deref().context("no command")?)?),
            None,
        ),
        "http" | "streamable-http" | "streamableHttp" => {
            (None, Some(expand(s.url.as_deref().context("no url")?)?))
        }
        "sse" => {
            tracing::warn!(
                server = name,
                "SSE isn't supported; trying it as streamable HTTP"
            );
            (None, Some(expand(s.url.as_deref().context("no url")?)?))
        }
        other => bail!("type \"{other}\" isn't supported: stdio or http"),
    };
    let map = |values: HashMap<String, String>| -> Result<HashMap<String, String>> {
        values
            .into_iter()
            .map(|(k, v)| Ok((k, expand(&v)?)))
            .collect()
    };
    Ok(McpServer {
        name: name.to_string(),
        url,
        token_env: None,
        command,
        args: s.args.iter().map(|a| expand(a)).collect::<Result<_>>()?,
        env: map(s.env)?,
        headers: map(s.headers)?,
        tools: s.tools,
        timeout_ms: s.timeout_ms.unwrap_or_else(config::default_mcp_timeout_ms),
        talk: s.talk.unwrap_or(true),
    })
}

/// `${VAR}` and `${VAR:-default}` from the environment and `.env`.
fn expand(text: &str) -> Result<String> {
    let mut out = String::new();
    let mut rest = text;
    while let Some(start) = rest.find("${") {
        out.push_str(&rest[..start]);
        let after = &rest[start + 2..];
        let end = after
            .find('}')
            .with_context(|| format!("no closing }} in {text:?}"))?;
        let (var, default) = match after[..end].split_once(":-") {
            Some((var, default)) => (var, Some(default)),
            None => (&after[..end], None),
        };
        match (config::lookup(var), default) {
            (Some(value), _) => out.push_str(&value),
            (None, Some(default)) => out.push_str(default),
            (None, None) => bail!("${var} isn't set"),
        }
        rest = &after[end + 1..];
    }
    out.push_str(rest);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn table(text: &str) -> toml::Table {
        toml::from_str(text).unwrap()
    }

    #[test]
    fn layers_merge_tables_and_replace_the_rest() {
        let mut into = table(
            "token = \"a\"\n[llm]\nbackend = \"anthropic\"\nmodel = \"m\"\neffort = \"low\"\n[skills]\ndirs = [\"x\"]\n",
        );
        merge(
            &mut into,
            table("token = \"b\"\n[llm]\nmodel = \"n\"\n[skills]\ndirs = [\"y\"]\n"),
        );
        assert_eq!(
            into,
            table(
                "token = \"b\"\n[llm]\nbackend = \"anthropic\"\nmodel = \"n\"\neffort = \"low\"\n[skills]\ndirs = [\"y\"]\n"
            )
        );
        // Another backend: the global stage's fields don't carry over.
        merge(
            &mut into,
            table("[llm]\nbackend = \"openai_compat\"\nbase_url = \"u\"\nmodel = \"o\"\n"),
        );
        assert_eq!(
            into["llm"],
            toml::Value::Table(table(
                "backend = \"openai_compat\"\nbase_url = \"u\"\nmodel = \"o\"\n"
            ))
        );
    }

    #[test]
    fn paths_resolve_from_their_file() {
        let mut t = table(
            "[skills]\ndirs = [\"skills\", \"/abs\"]\n[tts]\nbackend = \"sherpa_pocket\"\ndir = \"m\"\nvoice = \"v.wav\"\n[stt]\nbackend = \"openai_compat\"\nbase_url = \"u\"\nmodel = \"m\"\n",
        );
        resolve_paths(&mut t, Path::new("/base"));
        assert_eq!(t["skills"]["dirs"][0].as_str(), Some("/base/skills"));
        assert_eq!(t["skills"]["dirs"][1].as_str(), Some("/abs"));
        assert_eq!(t["tts"]["dir"].as_str(), Some("/base/m"));
        assert_eq!(t["tts"]["voice"].as_str(), Some("/base/v.wav"));
        // A hosted voice is a name.
        let mut t = table("[tts]\nbackend = \"openai_compat\"\nvoice = \"alloy\"\n");
        resolve_paths(&mut t, Path::new("/base"));
        assert_eq!(t["tts"]["voice"].as_str(), Some("alloy"));
        // The sessions' folder, given or not.
        let mut t = table("[agent]\nplace = \"HOME\"\n");
        resolve_paths(&mut t, Path::new("/base"));
        assert_eq!(t["agent"]["dir"].as_str(), Some("/base/sessions"));
        let mut t = table("[agent]\n[agent.claude]\nmodel = \"haiku\"\n");
        resolve_paths(&mut t, Path::new("/base"));
        assert_eq!(t["agent"]["claude"]["dir"].as_str(), Some("/base/work"));
        let mut t = table("[agent]\ndir = \"~/s\"\n");
        resolve_paths(&mut t, Path::new("/base"));
        assert!(!t["agent"]["dir"].as_str().unwrap().starts_with("/base"));
    }

    #[test]
    fn variables_expand() {
        unsafe { std::env::set_var("INKHARNESS_TEST_EXPAND", "secret") };
        assert_eq!(
            expand("Bearer ${INKHARNESS_TEST_EXPAND}").unwrap(),
            "Bearer secret"
        );
        assert_eq!(
            expand("${INKHARNESS_TEST_UNSET:-fallback}/x").unwrap(),
            "fallback/x"
        );
        assert!(expand("${INKHARNESS_TEST_UNSET}").is_err());
        assert!(expand("${OPEN").is_err());
        assert_eq!(expand("plain").unwrap(), "plain");
    }

    #[test]
    fn mcp_json_servers() {
        unsafe { std::env::set_var("INKHARNESS_TEST_MCP_TOKEN", "t0k") };
        let dir = crate::skills::tests::scratch("mcp-json");
        let file = dir.join(".mcp.json");
        crate::skills::tests::write(
            &file,
            r#"{"mcpServers": {
                "files": {"command": "npx", "args": ["-y", "server", "${INKHARNESS_TEST_UNSET:-.}"], "env": {"K": "v"}},
                "remote": {"type": "http", "url": "https://example.com/mcp", "headers": {"Authorization": "Bearer ${INKHARNESS_TEST_MCP_TOKEN}"}},
                "off": {"command": "x", "disabled": true},
                "broken": {"command": "${INKHARNESS_TEST_UNSET}"}
            }}"#,
        );
        let servers = read_mcp_json(&file).unwrap();
        let names: Vec<_> = servers.iter().map(|s| s.name.as_str()).collect();
        assert_eq!(names, ["files", "remote"]);
        assert_eq!(servers[0].command.as_deref(), Some("npx"));
        assert_eq!(servers[0].args, ["-y", "server", "."]);
        assert_eq!(servers[1].url.as_deref(), Some("https://example.com/mcp"));
        assert_eq!(servers[1].headers["Authorization"], "Bearer t0k");
    }

    #[test]
    fn a_folder_over_the_global_one() {
        let root = crate::skills::tests::scratch("place");
        let (global, here) = (root.join("global"), root.join("here"));
        let write = crate::skills::tests::write;
        write(
            &global.join(CONFIG_FILE),
            "token = \"g\"\n[llm]\nbackend = \"mock\"\n",
        );
        write(&global.join("AGENTS.md"), "Global rules.");
        write(
            &global.join("skills/a/SKILL.md"),
            "---\nname: a\ndescription: d\n---\n",
        );
        write(&here.join(CONFIG_FILE), "listen = \"127.0.0.1:1\"\n");
        write(&here.join("AGENT.md"), "Folder rules.");
        write(
            &here.join(".claude/skills/b/SKILL.md"),
            "---\nname: b\ndescription: d\n---\n",
        );
        write(&here.join("commands/play.md"), "Add $ARGUMENTS");
        write(&here.join(".claude/commands/other.md"), "x");
        write(&global.join("commands/g.md"), "x");
        write(
            &here.join(MCP_FILE),
            r#"{"mcpServers": {"m": {"command": "true"}}}"#,
        );
        unsafe { std::env::set_var("INKHARNESS_HOME", &global) };
        let place = load(&here, None).unwrap();
        unsafe { std::env::remove_var("INKHARNESS_HOME") };
        assert_eq!(place.config.token, "g");
        assert_eq!(place.config.listen, "127.0.0.1:1");
        assert!(matches!(place.config.llm, config::Llm::Mock));
        let texts: Vec<_> = place.instructions.iter().map(|(_, t)| t.as_str()).collect();
        assert_eq!(texts, ["Global rules.", "Folder rules."]);
        assert_eq!(
            place.skill_dirs,
            [here.join(".claude/skills"), global.join("skills")]
        );
        assert_eq!(
            place.command_dirs,
            [
                here.join("commands"),
                here.join(".claude/commands"),
                global.join("commands")
            ]
        );
        assert_eq!(place.config.mcp.len(), 1);
        // No config anywhere: says how to get one.
        unsafe { std::env::set_var("INKHARNESS_HOME", root.join("none")) };
        let err = load(&root, None).err().unwrap().to_string();
        unsafe { std::env::remove_var("INKHARNESS_HOME") };
        assert!(err.contains("inkharness init"), "{err}");
    }
}
