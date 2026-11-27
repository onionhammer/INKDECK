//! Text in, text out: the conversation in the terminal, one question from the command line, and
//! the HTTP API's turns (http.rs). The tools, skills and instructions a device gets, with [llm]
//! (or [chat.llm] if there is one). A model that speaks still speaks (some, such as OpenAI's
//! audio models, only answer text with audio): its transcript is the reply, and the audio goes
//! nowhere.

use std::io::{IsTerminal, Write};
use std::sync::Arc;

use anyhow::Result;
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::sync::mpsc;

use crate::commands::{self, Commands, Expanded, Kind};
use crate::config::Config;
use crate::harness::Harness;
use crate::llm::{self, Llm, Outcome, Piece, Role, Turn};
use crate::meter;
use crate::place::Place;
use crate::tools::{THINK_TOOL, ToolSet};

/// Earlier turns kept as context (a user turn and its reply count as two).
const HISTORY_TURNS: usize = 40;

pub struct Chat {
    system: String,
    llm: Box<dyn Llm>,
    /// The home's tools, and the think tool if there is a thinking model.
    tools: ToolSet,
    /// The thinking model and the tools it gets.
    think: Option<(Box<dyn Llm>, ToolSet)>,
    /// Slash commands: the command files, and the skills.
    commands: Arc<Commands>,
    /// Model rounds a turn gets ([chat] max_rounds).
    rounds: usize,
    pub model: String,
}

impl Chat {
    pub fn build(cfg: &Config, harness: &Harness) -> Result<Chat> {
        let think = match &cfg.think {
            Some(think) => Some((
                llm::build(&think.llm, harness.http.clone())?,
                harness.tools.clone(),
            )),
            None => None,
        };
        let model = cfg.chat_model();
        Ok(Chat {
            system: harness.prompt(cfg, false),
            llm: llm::build(model, harness.http.clone())?,
            tools: if think.is_some() {
                harness.tools.with_think()
            } else {
                harness.tools.clone()
            },
            think,
            commands: harness.commands.clone(),
            rounds: cfg.chat_rounds(),
            model: model.name().to_string(),
        })
    }

    pub fn tools(&self) -> &ToolSet {
        &self.tools
    }

    pub fn commands(&self) -> &Commands {
        &self.commands
    }

    /// The prompt for `text` when it starts with a command file's or a skill's name
    /// (commands.rs); None for anything else, which goes to the model as written.
    pub fn expand(&self, text: &str) -> Result<Option<Expanded>, String> {
        self.commands.expand(text).transpose()
    }

    /// One turn. The reply goes to `out` as it is written (with the tools it calls); returns
    /// all of its text. The caller adds the turn to the history ([`remember`]).
    pub async fn turn(
        &self,
        history: &[Turn],
        user: &str,
        out: mpsc::Sender<Piece>,
    ) -> Result<String> {
        let (tx, mut rx) = mpsc::channel::<Piece>(64);
        let model = async move {
            let request = |tools| llm::Request {
                system: &self.system,
                history,
                user,
                audio: None,
                tools,
                rounds: self.rounds,
                steer: None,
            };
            let outcome = self.llm.respond(request(&self.tools), tx.clone()).await?;
            if let (Outcome::Think { .. }, Some((think, tools))) = (outcome, &self.think) {
                tracing::info!("handing the turn to the thinking model");
                let _ = tx
                    .send(Piece::Tool(THINK_TOOL.into(), serde_json::Value::Null))
                    .await;
                think.respond(request(tools), tx).await?;
            }
            anyhow::Ok(())
        };
        let collect = async {
            let mut reply = String::new();
            while let Some(piece) = rx.recv().await {
                if let Piece::Text(text) | Piece::Transcript(text) = &piece {
                    reply.push_str(text);
                }
                let _ = out.send(piece).await;
            }
            reply
        };
        let (result, reply) = tokio::join!(model, collect);
        result?;
        Ok(reply)
    }
}

/// Adds a finished turn to the history, keeping the last HISTORY_TURNS.
pub fn remember(history: &mut Vec<Turn>, user: &str, reply: &str) {
    if reply.trim().is_empty() {
        return; // a user turn with no reply would break the alternation
    }
    for (role, text) in [(Role::User, user), (Role::Assistant, reply.trim())] {
        history.push(Turn {
            role,
            text: text.to_string(),
            audio: None,
        });
    }
    let extra = history.len().saturating_sub(HISTORY_TURNS);
    history.drain(..extra);
}

/// What it picked up here, for the top of a conversation.
pub fn banner(place: &Place, harness: &Harness, chat: &Chat) -> String {
    let mut lines = vec![format!(
        "inkharness {} · {}",
        env!("CARGO_PKG_VERSION"),
        chat.model
    )];
    let read: Vec<String> = place.read.iter().map(|f| short(f)).collect();
    if !read.is_empty() {
        lines.push(format!("  read    {}", read.join(", ")));
    }
    let skills = harness.skills.names();
    if !skills.is_empty() {
        lines.push(format!("  skills  {}", skills.join(", ")));
    }
    let servers: Vec<String> = harness
        .mcp
        .servers()
        .iter()
        .map(|(name, tools)| format!("{name} ({tools})"))
        .collect();
    if !servers.is_empty() {
        lines.push(format!("  mcp     {}", servers.join(", ")));
    }
    lines.push(format!("  tools   {}", chat.tools().defs().len()));
    lines.join("\n")
}

/// A path from here (`./`) or from home (`~/`) when it is under one of them.
fn short(path: &std::path::Path) -> String {
    let here = std::env::current_dir().ok();
    if let Some(rest) = here.as_ref().and_then(|h| path.strip_prefix(h).ok()) {
        return rest.display().to_string();
    }
    if let Some(rest) = std::env::home_dir()
        .as_ref()
        .and_then(|h| path.strip_prefix(h).ok())
    {
        return format!("~/{}", rest.display());
    }
    path.display().to_string()
}

/// The lines `/tools` shows: each tool and the first line of what it does.
pub fn tool_lines(chat: &Chat) -> Vec<String> {
    chat.tools()
        .defs()
        .iter()
        .map(|def| {
            let summary = def.description.lines().next().unwrap_or_default();
            format!("{}  {}", def.name, cut(summary, 90))
        })
        .collect()
}

/// The lines `/skills` shows.
pub fn skill_lines(harness: &Harness) -> Vec<String> {
    let skills = harness.skills.described();
    if skills.is_empty() {
        return vec!["No skills here (skills/, .agents/skills/, .claude/skills/).".into()];
    }
    skills
        .iter()
        .map(|(name, description)| format!("/{name}  {}", cut(description, 90)))
        .collect()
}

/// The lines `/mcp` shows.
pub fn mcp_lines(harness: &Harness) -> Vec<String> {
    let servers = harness.mcp.servers();
    if servers.is_empty() {
        return vec!["No MCP servers (.mcp.json, [[mcp]]).".into()];
    }
    servers
        .iter()
        .map(|(name, tools)| format!("{name}  {tools} tools, named {name}_…"))
        .collect()
}

/// The lines `/help` shows: `keys`, then every command.
pub fn help_lines(chat: &Chat, keys: &[&str]) -> Vec<String> {
    let mut lines: Vec<String> = keys.iter().map(|k| k.to_string()).collect();
    let listed = chat.commands().list();
    let column = listed
        .iter()
        .map(|l| l.name.chars().count() + l.hint.chars().count() + 2)
        .max()
        .unwrap_or(0)
        .min(32);
    let mut kind = None;
    for listed in listed {
        if Some(listed.kind) != kind {
            kind = Some(listed.kind);
            lines.push(String::new());
            lines.push(
                match listed.kind {
                    Kind::Builtin => "Commands",
                    Kind::File => "Command files",
                    Kind::Skill => "Skills",
                }
                .into(),
            );
        }
        let name = if listed.hint.is_empty() {
            format!("/{}", listed.name)
        } else {
            format!("/{} {}", listed.name, listed.hint)
        };
        lines.push(format!(
            "  {name:<column$}  {}",
            cut(&listed.description, 70)
        ));
    }
    lines
}

/// What a list of turn costs says: "$0.0021", with how many requests stated none.
pub fn cost_text(cost: f64, unpriced: usize) -> String {
    // An empty sum of floats is -0.0.
    let cost = if cost == 0.0 { 0.0 } else { cost };
    match unpriced {
        0 => format!("${cost:.4}"),
        n => format!("${cost:.4} (+{n} unpriced)"),
    }
}

const PLAIN_KEYS: &[&str] = &["A line is a turn. Ctrl-C stops a reply, Ctrl-D leaves."];

/// The conversation in plain lines: a line is a turn. For pipes, `--plain`, and terminals the
/// full-screen chat (tui/) isn't used in. Prompts and the banner only when a person is typing;
/// from a pipe each line is a turn, answered in order.
pub async fn plain(chat: &Chat, harness: &Harness, banner: &str) -> Result<()> {
    let typing = std::io::stdin().is_terminal();
    if typing {
        eprintln!("{banner}\n  /help for commands, Ctrl-D to quit\n");
    }
    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    let mut history: Vec<Turn> = Vec::new();
    let mut costs: Vec<f64> = Vec::new();
    let note = |lines: Vec<String>| {
        for line in lines {
            eprintln!("  {line}");
        }
    };
    loop {
        if typing {
            print!("› ");
            std::io::stdout().flush()?;
        }
        let line = tokio::select! {
            line = lines.next_line() => line?,
            _ = tokio::signal::ctrl_c() => None,
        };
        let Some(line) = line else {
            if typing {
                println!();
            }
            return Ok(());
        };
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        if let Some((name, _)) = commands::split(line)
            && let Some(builtin) = commands::builtin(name)
        {
            match builtin.name {
                "quit" | "exit" => return Ok(()),
                "clear" | "reset" => {
                    history.clear();
                    eprintln!("(a new conversation)");
                }
                "tools" => note(tool_lines(chat)),
                "skills" => note(skill_lines(harness)),
                "mcp" => note(mcp_lines(harness)),
                "help" => note(help_lines(chat, PLAIN_KEYS)),
                "cost" => {
                    let total: f64 = costs.iter().sum();
                    eprintln!("  {} turns, {}", costs.len(), cost_text(total, 0));
                }
                other => eprintln!("  /{other} is in the full-screen chat"),
            }
            continue;
        }
        let printer_dim = Printer::new();
        let expanded = match chat.expand(line) {
            Ok(expanded) => expanded,
            Err(e) => {
                eprintln!("{}", printer_dim.dim(&format!("error: {e}")));
                continue;
            }
        };
        let prompt = expanded.as_ref().map_or(line, |e| e.prompt.as_str());
        if let Some(e) = &expanded
            && typing
        {
            eprintln!("{}", printer_dim.dim(&format!("↳ {}", e.source)));
        }
        let mut printer = Printer::new();
        let (tx, mut rx) = mpsc::channel(64);
        let turn = meter::measure(chat.turn(&history, prompt, tx));
        let print = async {
            while let Some(piece) = rx.recv().await {
                printer.piece(piece);
            }
        };
        let finished = tokio::select! {
            ((result, charges), ()) = async { tokio::join!(turn, print) } => Some((result, charges)),
            _ = tokio::signal::ctrl_c() => None,
        };
        printer.end();
        match finished {
            Some((Ok(reply), charges)) => {
                costs.push(meter::total(&charges).0);
                remember(&mut history, prompt, &reply);
            }
            Some((Err(e), _)) => eprintln!("{}", printer.dim(&format!("error: {e:#}"))),
            None => eprintln!("{}", printer.dim("(stopped)")),
        }
    }
}

/// One question: prints the answer and returns. A command expands as in a conversation.
pub async fn once(chat: &Chat, question: &str) -> Result<()> {
    let expanded = chat.expand(question).map_err(anyhow::Error::msg)?;
    let prompt = expanded.as_ref().map_or(question, |e| e.prompt.as_str());
    let mut printer = Printer::new();
    let (tx, mut rx) = mpsc::channel(64);
    let print = async {
        while let Some(piece) = rx.recv().await {
            printer.piece(piece);
        }
    };
    let (result, ()) = tokio::join!(chat.turn(&[], prompt, tx), print);
    printer.end();
    result.map(|_| ())
}

/// The reply on stdout as it comes; the tools it calls on stderr, dimmed in a terminal.
struct Printer {
    line_start: bool,
    ansi: bool,
}

impl Printer {
    fn new() -> Self {
        Printer {
            line_start: true,
            ansi: std::io::stderr().is_terminal(),
        }
    }

    fn piece(&mut self, piece: Piece) {
        match piece {
            Piece::Text(text) | Piece::Transcript(text) => {
                print!("{text}");
                let _ = std::io::stdout().flush();
                if !text.is_empty() {
                    self.line_start = text.ends_with('\n');
                }
            }
            Piece::Tool(name, _) => {
                if !self.line_start {
                    println!();
                    self.line_start = true;
                }
                eprintln!("{}", self.dim(&format!("· {name}")));
            }
            Piece::Audio(_) | Piece::ToolDone(..) => {}
        }
    }

    fn end(&mut self) {
        if !self.line_start {
            println!();
            self.line_start = true;
        }
    }

    fn dim(&self, text: &str) -> String {
        if self.ansi {
            format!("\x1b[2m{text}\x1b[0m")
        } else {
            text.to_string()
        }
    }
}

pub fn cut(text: &str, max: usize) -> String {
    match text.char_indices().nth(max) {
        Some((i, _)) => format!("{}…", &text[..i]),
        None => text.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn a_turn_streams_and_is_remembered() {
        let cfg =
            Config::from_table(toml::from_str("[llm]\nbackend = \"mock\"\n").unwrap()).unwrap();
        let place = Place {
            config: cfg,
            read: vec![],
            instructions: vec![(std::path::PathBuf::from("AGENTS.md"), "Be brief.".into())],
            skill_dirs: vec![],
            command_dirs: vec![],
        };
        let harness = Harness::build(&place, false).await.unwrap();
        assert!(
            harness
                .prompt(&place.config, false)
                .ends_with("Instructions from AGENTS.md:\n\nBe brief.")
        );
        let chat = Chat::build(&place.config, &harness).unwrap();
        let (tx, mut rx) = mpsc::channel(64);
        let pieces = async {
            let mut n = 0;
            while rx.recv().await.is_some() {
                n += 1;
            }
            n
        };
        let (reply, n) = tokio::join!(chat.turn(&[], "hello", tx), pieces);
        let reply = reply.unwrap();
        assert!(reply.starts_with("You said: hello."), "{reply}");
        assert!(n > 1);
        let mut history = Vec::new();
        remember(&mut history, "hello", &reply);
        remember(&mut history, "nothing back", " ");
        assert_eq!(history.len(), 2);
        for i in 0..HISTORY_TURNS {
            remember(&mut history, &i.to_string(), "ok");
        }
        assert_eq!(history.len(), HISTORY_TURNS);
        assert!(matches!(history[0].role, Role::User));
    }
}
