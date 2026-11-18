//! Text in, text out: the conversation in the terminal, one question from the command line, and
//! the HTTP API's turns (http.rs). The tools, skills and instructions a device gets, with [llm]
//! (or [chat.llm] if there is one). A model that speaks still speaks (some, such as OpenAI's
//! audio models, only answer text with audio): its transcript is the reply, and the audio goes
//! nowhere.

use std::io::{IsTerminal, Write};

use anyhow::Result;
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::sync::mpsc;

use crate::config::Config;
use crate::harness::Harness;
use crate::llm::{self, Llm, Outcome, Piece, Role, Turn};
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
        let model = cfg.chat.as_ref().map_or(&cfg.llm, |chat| &chat.llm);
        Ok(Chat {
            system: harness.prompt(cfg, false),
            llm: llm::build(model, harness.http.clone())?,
            tools: if think.is_some() {
                harness.tools.with_think()
            } else {
                harness.tools.clone()
            },
            think,
            model: model.name().to_string(),
        })
    }

    pub fn tools(&self) -> &ToolSet {
        &self.tools
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
            };
            let outcome = self.llm.respond(request(&self.tools), tx.clone()).await?;
            if let (Outcome::Think { .. }, Some((think, tools))) = (outcome, &self.think) {
                tracing::info!("handing the turn to the thinking model");
                let _ = tx.send(Piece::Tool(THINK_TOOL.into())).await;
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

const HELP: &str = "\
/reset   start a new conversation
/tools   the tools the model has
/quit    leave (or Ctrl-D)
Ctrl-C stops a reply.";

/// The conversation: a line is a turn. Prompts and the banner only when a person is typing;
/// from a pipe each line is a turn, answered in order.
pub async fn repl(chat: &Chat, banner: &str) -> Result<()> {
    let typing = std::io::stdin().is_terminal();
    if typing {
        eprintln!("{banner}\n  /help for commands, Ctrl-D to quit\n");
    }
    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    let mut history: Vec<Turn> = Vec::new();
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
        match line {
            "" => continue,
            "/quit" | "/exit" => return Ok(()),
            "/reset" | "/clear" => {
                history.clear();
                eprintln!("(a new conversation)");
                continue;
            }
            "/tools" => {
                for def in chat.tools().defs() {
                    let summary = def.description.lines().next().unwrap_or_default();
                    eprintln!("  {}  {}", def.name, cut(summary, 90));
                }
                continue;
            }
            "/help" => {
                eprintln!("{HELP}");
                continue;
            }
            _ => {}
        }
        let mut printer = Printer::new();
        let (tx, mut rx) = mpsc::channel(64);
        let turn = chat.turn(&history, line, tx);
        let print = async {
            while let Some(piece) = rx.recv().await {
                printer.piece(piece);
            }
        };
        let finished = tokio::select! {
            (result, ()) = async { tokio::join!(turn, print) } => Some(result),
            _ = tokio::signal::ctrl_c() => None,
        };
        printer.end();
        match finished {
            Some(Ok(reply)) => remember(&mut history, line, &reply),
            Some(Err(e)) => eprintln!("{}", printer.dim(&format!("error: {e:#}"))),
            None => eprintln!("{}", printer.dim("(stopped)")),
        }
    }
}

/// One question: prints the answer and returns.
pub async fn once(chat: &Chat, question: &str) -> Result<()> {
    let mut printer = Printer::new();
    let (tx, mut rx) = mpsc::channel(64);
    let print = async {
        while let Some(piece) = rx.recv().await {
            printer.piece(piece);
        }
    };
    let (result, ()) = tokio::join!(chat.turn(&[], question, tx), print);
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
            Piece::Tool(name) => {
                if !self.line_start {
                    println!();
                    self.line_start = true;
                }
                eprintln!("{}", self.dim(&format!("· {name}")));
            }
            Piece::Audio(_) => {}
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

fn cut(text: &str, max: usize) -> String {
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
