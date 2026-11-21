//! The api backend's own tools (`[agent.tools]`): files in the session's work folder,
//! `http_fetch`, and `shell` when the policy allows it. The claude backend has Claude Code's own
//! (Read, Write, Bash, WebFetch), and herdr sessions are programs with theirs.

use std::net::IpAddr;
use std::path::{Component, Path, PathBuf};
use std::process::Stdio;
use std::time::Duration;

use serde::Deserialize;
use serde_json::{Value, json};
use tokio::process::Command;

use crate::config;
use crate::tools::ToolDef;

pub const READ_TOOL: &str = "read_file";
pub const WRITE_TOOL: &str = "write_file";
pub const LIST_TOOL: &str = "list_files";
pub const FETCH_TOOL: &str = "http_fetch";
pub const SHELL_TOOL: &str = "shell";

/// What the model gets back at most from a read, a fetch or a command.
const READ_BYTES: usize = 16 * 1024;
const FETCH_BYTES: usize = 16 * 1024;
const SHELL_BYTES: usize = 8 * 1024;
/// The most a write puts in a file at once.
const WRITE_BYTES: usize = 256 * 1024;
const LISTED: usize = 200;
const FETCH_TIMEOUT: Duration = Duration::from_secs(20);
const SHELL_MAX_S: u64 = 600;

/// The tools `cfg` allows, for an api session.
pub fn definitions(cfg: &config::AgentTools) -> Vec<ToolDef> {
    let mut defs = Vec::new();
    if cfg.files {
        defs.extend([read_definition(), write_definition(), list_definition()]);
    }
    if cfg.http == config::Policy::Allow {
        defs.push(fetch_definition());
    }
    if cfg.shell == config::Policy::Allow {
        defs.push(shell_definition(cfg.shell_timeout_s));
    }
    defs
}

/// Runs one of them in `root`, the session's work folder; None if `name` isn't one `cfg` allows.
pub async fn call(
    cfg: &config::AgentTools,
    http: &reqwest::Client,
    root: &Path,
    name: &str,
    input: &Value,
) -> Option<Result<String, String>> {
    Some(match name {
        READ_TOOL | WRITE_TOOL | LIST_TOOL if !cfg.files => return None,
        READ_TOOL => read(root, input).await,
        WRITE_TOOL => write(root, input).await,
        LIST_TOOL => list(root, input).await,
        FETCH_TOOL if cfg.http == config::Policy::Allow => {
            fetch(http, input, cfg.http_private).await
        }
        SHELL_TOOL if cfg.shell == config::Policy::Allow => {
            shell(root, input, cfg.shell_timeout_s).await
        }
        _ => return None,
    })
}

/// `rel` under `root`, refusing an absolute path or one that climbs out with `..`.
fn under(root: &Path, rel: &str) -> Result<PathBuf, String> {
    let rel = rel.trim();
    let path = Path::new(rel);
    if path.is_absolute() {
        return Err("give a path relative to the work folder".into());
    }
    let mut out = root.to_path_buf();
    for part in path.components() {
        match part {
            Component::Normal(p) => out.push(p),
            Component::CurDir => {}
            _ => return Err("the path leaves the work folder".into()),
        }
    }
    Ok(out)
}

/// The path, if it is still under `root` once links are followed (it must exist).
fn inside(root: &Path, path: &Path) -> Result<PathBuf, String> {
    let root = root
        .canonicalize()
        .map_err(|e| format!("the work folder: {e}"))?;
    let real = path
        .canonicalize()
        .map_err(|_| "no such file".to_string())?;
    if real.starts_with(&root) {
        Ok(real)
    } else {
        Err("the path leaves the work folder".into())
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct PathInput {
    #[serde(default)]
    path: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct WriteInput {
    path: String,
    text: String,
    #[serde(default)]
    append: bool,
}

fn parse<T: for<'de> Deserialize<'de>>(input: &Value) -> Result<T, String> {
    serde_json::from_value(input.clone()).map_err(|e| format!("invalid input: {e}"))
}

async fn read(root: &Path, input: &Value) -> Result<String, String> {
    let input: PathInput = parse(input)?;
    let path = inside(root, &under(root, &input.path)?)?;
    let bytes = tokio::fs::read(&path).await.map_err(|e| e.to_string())?;
    let text = String::from_utf8_lossy(&bytes);
    Ok(cap(&text, READ_BYTES))
}

async fn write(root: &Path, input: &Value) -> Result<String, String> {
    let input: WriteInput = parse(input)?;
    if input.path.trim().is_empty() {
        return Err("invalid input: path is required".into());
    }
    if input.text.len() > WRITE_BYTES {
        return Err(format!("at most {WRITE_BYTES} bytes at a time"));
    }
    let path = under(root, &input.path)?;
    let parent = path.parent().ok_or("invalid path")?;
    tokio::fs::create_dir_all(parent)
        .await
        .map_err(|e| e.to_string())?;
    inside(root, parent)?;
    if path.is_symlink() {
        return Err("won't write through a link".into());
    }
    let result = if input.append {
        use tokio::io::AsyncWriteExt;
        let mut file = tokio::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(&path)
            .await
            .map_err(|e| e.to_string())?;
        file.write_all(input.text.as_bytes()).await
    } else {
        tokio::fs::write(&path, input.text.as_bytes()).await
    };
    result.map_err(|e| e.to_string())?;
    Ok(format!(
        "{} {} bytes to {}",
        if input.append { "Added" } else { "Wrote" },
        input.text.len(),
        input.path.trim()
    ))
}

async fn list(root: &Path, input: &Value) -> Result<String, String> {
    let input: PathInput = parse(input)?;
    tokio::fs::create_dir_all(root)
        .await
        .map_err(|e| e.to_string())?;
    let dir = inside(root, &under(root, &input.path)?)?;
    let mut entries = tokio::fs::read_dir(&dir).await.map_err(|e| e.to_string())?;
    let mut lines = Vec::new();
    while let Some(entry) = entries.next_entry().await.map_err(|e| e.to_string())? {
        let name = entry.file_name().to_string_lossy().to_string();
        let meta = entry.metadata().await.ok();
        lines.push(match meta {
            Some(m) if m.is_dir() => format!("{name}/"),
            Some(m) => format!("{name}  {} bytes", m.len()),
            None => name,
        });
    }
    lines.sort();
    let more = lines.len().saturating_sub(LISTED);
    lines.truncate(LISTED);
    if more > 0 {
        lines.push(format!("… and {more} more"));
    }
    Ok(if lines.is_empty() {
        "(empty)".into()
    } else {
        lines.join("\n")
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct FetchInput {
    url: String,
    method: Option<String>,
    body: Option<String>,
    #[serde(default)]
    headers: std::collections::HashMap<String, String>,
}

/// Loopback, private, link-local, the tailnet's CGNAT range and the like: what a fetch may not
/// reach unless `http_private` is set (the daemon's own HTTP, Home Assistant, the router).
pub fn private(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(v4) => {
            let [a, b, ..] = v4.octets();
            v4.is_loopback()
                || v4.is_private()
                || v4.is_link_local()
                || v4.is_unspecified()
                || v4.is_broadcast()
                || v4.is_multicast()
                || (a == 100 && (64..128).contains(&b))
        }
        IpAddr::V6(v6) => {
            if let Some(v4) = v6.to_ipv4_mapped() {
                return private(IpAddr::V4(v4));
            }
            let first = v6.segments()[0];
            v6.is_loopback()
                || v6.is_unspecified()
                || v6.is_multicast()
                || (first & 0xfe00) == 0xfc00 // unique local
                || (first & 0xffc0) == 0xfe80 // link-local
        }
    }
}

async fn fetch(
    http: &reqwest::Client,
    input: &Value,
    allow_private: bool,
) -> Result<String, String> {
    let input: FetchInput = parse(input)?;
    let url = reqwest::Url::parse(input.url.trim()).map_err(|e| format!("invalid url: {e}"))?;
    if !matches!(url.scheme(), "http" | "https") {
        return Err("only http and https".into());
    }
    let host = url.host_str().ok_or("the url has no host")?.to_string();
    let port = url.port_or_known_default().unwrap_or(443);
    if !allow_private {
        let bare = host.trim_start_matches('[').trim_end_matches(']');
        let addrs: Vec<IpAddr> = match bare.parse::<IpAddr>() {
            Ok(ip) => vec![ip],
            Err(_) => tokio::net::lookup_host((bare, port))
                .await
                .map_err(|e| format!("couldn't find {host}: {e}"))?
                .map(|a| a.ip())
                .collect(),
        };
        if addrs.is_empty() || addrs.into_iter().any(private) {
            return Err(format!(
                "{host} is on a private network: not allowed ([agent.tools] http_private)"
            ));
        }
    }
    let method = input
        .method
        .as_deref()
        .unwrap_or("GET")
        .to_uppercase()
        .parse::<reqwest::Method>()
        .map_err(|_| "invalid method".to_string())?;
    let mut request = http.request(method, url.clone()).timeout(FETCH_TIMEOUT);
    for (k, v) in &input.headers {
        request = request.header(k, v);
    }
    if let Some(body) = input.body {
        request = request.body(body);
    }
    let response = request
        .send()
        .await
        .map_err(|e| format!("fetch failed: {e}"))?;
    let status = response.status();
    if status.is_redirection() {
        let to = response
            .headers()
            .get(reqwest::header::LOCATION)
            .and_then(|l| l.to_str().ok())
            .unwrap_or("?");
        return Ok(format!(
            "{status}: it redirects to {to} (fetch that, if it's where you meant to go)"
        ));
    }
    let kind = response
        .headers()
        .get(reqwest::header::CONTENT_TYPE)
        .and_then(|c| c.to_str().ok())
        .unwrap_or("")
        .to_string();
    let body = response
        .text()
        .await
        .map_err(|e| format!("reading it: {e}"))?;
    let text = if kind.contains("html") {
        strip_html(&body)
    } else {
        body
    };
    Ok(format!("{status} {kind}\n\n{}", cap(&text, FETCH_BYTES)))
}

/// A page's text: no scripts, styles or tags, the common entities decoded, blank runs folded.
pub fn strip_html(html: &str) -> String {
    let mut out = String::with_capacity(html.len() / 2);
    let lower = html.to_lowercase();
    let mut i = 0;
    while i < html.len() {
        let rest = &lower[i..];
        if rest.starts_with("<script") || rest.starts_with("<style") {
            let close = if rest.starts_with("<script") {
                "</script>"
            } else {
                "</style>"
            };
            i += rest.find(close).map_or(rest.len(), |e| e + close.len());
            continue;
        }
        if rest.starts_with('<') {
            let end = rest.find('>').map_or(rest.len(), |e| e + 1);
            let tag = &rest[..end];
            if [
                "<br", "<p", "</p", "<div", "</div", "<li", "<h", "</h", "<tr", "</tr",
            ]
            .iter()
            .any(|t| tag.starts_with(t))
            {
                out.push('\n');
            }
            i += end;
            continue;
        }
        let next = rest.find('<').unwrap_or(rest.len());
        out.push_str(&html[i..i + next]);
        i += next;
    }
    let out = out
        .replace("&nbsp;", " ")
        .replace("&amp;", "&")
        .replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&#39;", "'");
    let mut lines: Vec<String> = Vec::new();
    for line in out.lines() {
        let line = line.split_whitespace().collect::<Vec<_>>().join(" ");
        if !line.is_empty() || lines.last().is_some_and(|l| !l.is_empty()) {
            lines.push(line);
        }
    }
    lines.join("\n").trim().to_string()
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ShellInput {
    command: String,
    timeout_s: Option<u64>,
}

async fn shell(root: &Path, input: &Value, default_timeout: u64) -> Result<String, String> {
    let input: ShellInput = parse(input)?;
    if input.command.trim().is_empty() {
        return Err("invalid input: command is required".into());
    }
    tokio::fs::create_dir_all(root)
        .await
        .map_err(|e| e.to_string())?;
    let timeout = input
        .timeout_s
        .unwrap_or(default_timeout)
        .clamp(1, SHELL_MAX_S);
    let child = Command::new("sh")
        .arg("-c")
        .arg(&input.command)
        .current_dir(root)
        .env_clear()
        .env(
            "PATH",
            std::env::var("PATH").unwrap_or_else(|_| "/usr/bin:/bin".into()),
        )
        .env("HOME", root)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true)
        .spawn()
        .map_err(|e| format!("couldn't start sh: {e}"))?;
    match tokio::time::timeout(Duration::from_secs(timeout), child.wait_with_output()).await {
        Err(_) => Err(format!("it took longer than {timeout} s and was ended")),
        Ok(Err(e)) => Err(e.to_string()),
        Ok(Ok(out)) => {
            let mut text = String::from_utf8_lossy(&out.stdout).to_string();
            let err = String::from_utf8_lossy(&out.stderr);
            if !err.trim().is_empty() {
                text.push_str("\n[stderr]\n");
                text.push_str(&err);
            }
            let code = out
                .status
                .code()
                .map_or("a signal".to_string(), |c| c.to_string());
            Ok(format!("exit {code}\n{}", tail(&text, SHELL_BYTES)))
        }
    }
}

/// At most `max` bytes of `text`, from the start, at a character boundary.
fn cap(text: &str, max: usize) -> String {
    if text.len() <= max {
        return text.to_string();
    }
    let mut end = max;
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    format!("{}\n… ({} bytes more)", &text[..end], text.len() - end)
}

/// The last `max` bytes of `text`.
fn tail(text: &str, max: usize) -> String {
    if text.len() <= max {
        return text.to_string();
    }
    let mut start = text.len() - max;
    while !text.is_char_boundary(start) {
        start += 1;
    }
    format!("(… {start} bytes before)\n{}", &text[start..])
}

fn read_definition() -> ToolDef {
    ToolDef {
        name: READ_TOOL.into(),
        description: format!(
            "Reads a file in this task's work folder (its own, kept until the task is \
             deleted). Up to {READ_BYTES} bytes."
        ),
        schema: json!({
            "type": "object",
            "properties": {"path": {"type": "string", "description": "Relative to the work folder"}},
            "required": ["path"],
            "additionalProperties": false
        }),
    }
}

fn write_definition() -> ToolDef {
    ToolDef {
        name: WRITE_TOOL.into(),
        description: "Writes a file in this task's work folder (folders are made as needed), \
            or adds to the end of one: notes, drafts, data to keep between steps."
            .into(),
        schema: json!({
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Relative to the work folder"},
                "text": {"type": "string"},
                "append": {"type": "boolean", "description": "Add to the end instead of replacing"}
            },
            "required": ["path", "text"],
            "additionalProperties": false
        }),
    }
}

fn list_definition() -> ToolDef {
    ToolDef {
        name: LIST_TOOL.into(),
        description: "Lists a folder in this task's work folder (the work folder itself \
            without a path)."
            .into(),
        schema: json!({
            "type": "object",
            "properties": {"path": {"type": "string", "description": "Relative to the work folder"}},
            "additionalProperties": false
        }),
    }
}

fn fetch_definition() -> ToolDef {
    ToolDef {
        name: FETCH_TOOL.into(),
        description: format!(
            "Fetches a URL on the internet: a page (as text), an API's JSON. GET unless \
             another method is given. Up to {FETCH_BYTES} bytes back; redirects are reported, \
             not followed."
        ),
        schema: json!({
            "type": "object",
            "properties": {
                "url": {"type": "string"},
                "method": {"type": "string", "description": "GET, POST, PUT, DELETE…"},
                "body": {"type": "string"},
                "headers": {"type": "object", "additionalProperties": {"type": "string"}}
            },
            "required": ["url"],
            "additionalProperties": false
        }),
    }
}

fn shell_definition(timeout: u64) -> ToolDef {
    ToolDef {
        name: SHELL_TOOL.into(),
        description: format!(
            "Runs a shell command (sh -c) in this task's work folder on the home server, and \
             returns its exit code and output. It ends after {timeout} s unless timeout_s says \
             otherwise (at most {SHELL_MAX_S})."
        ),
        schema: json!({
            "type": "object",
            "properties": {
                "command": {"type": "string"},
                "timeout_s": {"type": "integer", "minimum": 1}
            },
            "required": ["command"],
            "additionalProperties": false
        }),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cfg(shell: config::Policy) -> config::AgentTools {
        config::AgentTools {
            shell,
            ..Default::default()
        }
    }

    #[tokio::test]
    async fn files_stay_in_the_work_folder() {
        let root = crate::skills::tests::scratch("work-files").join("work");
        let http = reqwest::Client::new();
        let c = cfg(config::Policy::Deny);
        let run = |name: &'static str, input: Value| {
            let (c, http, root) = (&c, &http, &root);
            async move { call(c, http, root, name, &input).await.unwrap() }
        };
        assert_eq!(run(LIST_TOOL, json!({})).await.unwrap(), "(empty)");
        run(WRITE_TOOL, json!({"path": "notes/a.md", "text": "one"}))
            .await
            .unwrap();
        run(
            WRITE_TOOL,
            json!({"path": "notes/a.md", "text": " two", "append": true}),
        )
        .await
        .unwrap();
        assert_eq!(
            run(READ_TOOL, json!({"path": "notes/a.md"})).await.unwrap(),
            "one two"
        );
        assert_eq!(run(LIST_TOOL, json!({})).await.unwrap(), "notes/");
        for escape in ["../x", "/etc/passwd", "notes/../../x"] {
            assert!(
                run(READ_TOOL, json!({"path": escape})).await.is_err(),
                "{escape}"
            );
            assert!(
                run(WRITE_TOOL, json!({"path": escape, "text": "x"}))
                    .await
                    .is_err(),
                "{escape}"
            );
        }
        // A link out of the folder isn't followed.
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink("/etc", root.join("etc")).unwrap();
            assert!(run(READ_TOOL, json!({"path": "etc/hosts"})).await.is_err());
            assert!(
                run(WRITE_TOOL, json!({"path": "etc/x", "text": "x"}))
                    .await
                    .is_err()
            );
        }
    }

    #[tokio::test]
    async fn the_shell_only_when_allowed() {
        let root = crate::skills::tests::scratch("work-shell").join("work");
        let http = reqwest::Client::new();
        let denied = cfg(config::Policy::Deny);
        assert!(!definitions(&denied).iter().any(|d| d.name == SHELL_TOOL));
        assert!(
            call(
                &denied,
                &http,
                &root,
                SHELL_TOOL,
                &json!({"command": "echo hi"})
            )
            .await
            .is_none()
        );
        let allowed = cfg(config::Policy::Allow);
        assert!(definitions(&allowed).iter().any(|d| d.name == SHELL_TOOL));
        let out = call(
            &allowed,
            &http,
            &root,
            SHELL_TOOL,
            &json!({"command": "echo hi; pwd"}),
        )
        .await
        .unwrap()
        .unwrap();
        assert!(
            out.starts_with("exit 0\nhi\n") && out.contains("work"),
            "{out}"
        );
        let slow = call(
            &allowed,
            &http,
            &root,
            SHELL_TOOL,
            &json!({"command": "sleep 5", "timeout_s": 1}),
        )
        .await
        .unwrap();
        assert!(slow.unwrap_err().contains("longer than 1 s"));
    }

    #[tokio::test]
    async fn fetches_stay_off_private_networks() {
        let http = reqwest::Client::new();
        for url in [
            "http://127.0.0.1:7392/sessions",
            "http://localhost/",
            "http://192.168.1.1/",
            "http://[::1]/",
            "http://100.101.102.103/",
            "file:///etc/passwd",
        ] {
            assert!(
                fetch(&http, &json!({"url": url}), false).await.is_err(),
                "{url}"
            );
        }
        // Allowed, a local server answers, its HTML as text.
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        tokio::spawn(async move {
            let app = axum::Router::new().route(
                "/",
                axum::routing::get(|| async {
                    axum::response::Html(
                        "<html><style>p{}</style><body><h1>Fares</h1><p>TAP &amp; Iberia</p>\
                         <script>x()</script></body></html>",
                    )
                }),
            );
            axum::serve(listener, app).await.unwrap();
        });
        let page = fetch(&http, &json!({"url": format!("http://{addr}/")}), true)
            .await
            .unwrap();
        assert!(page.starts_with("200 OK text/html"), "{page}");
        assert!(page.ends_with("Fares\n\nTAP & Iberia"), "{page}");
        assert!(private("10.0.0.1".parse().unwrap()) && !private("1.1.1.1".parse().unwrap()));
    }
}
