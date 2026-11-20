//! inkharness: an assistant with tools, skills and MCP servers, started in any folder.
//!
//! - `inkharness`: a conversation in the terminal (or `inkharness <question>` for one answer).
//! - `inkharness serve`: the daemon. INKDECK boards talk to it over TCP
//!   (docs/voice-protocol.md); programs send text over HTTP (http.rs).
//! - `inkharness init`: a starter config in the global config folder.
//!
//! What it uses comes from the folder it starts in and the global config folder (place.rs):
//! `inkharness.toml`, `AGENTS.md`, skills folders, `.mcp.json`, `.env`.

mod agent;
mod chat;
mod config;
mod device;
mod harness;
mod http;
mod js;
mod llm;
#[cfg(feature = "local")]
mod local;
mod mcp;
mod meter;
mod place;
mod search;
mod sentence;
mod serve;
mod session;
mod skills;
mod stt;
mod tools;
mod tts;

use std::path::PathBuf;

use anyhow::{Context, Result, bail};
use clap::{ArgAction, Parser, Subcommand};

#[derive(Parser)]
#[command(
    name = "inkharness",
    version,
    about = "An assistant with tools, skills and MCP servers: a chat in the terminal, or the daemon INKDECK boards talk to",
    long_about = "An assistant with tools, skills and MCP servers: a chat in the terminal, or the \
                  daemon INKDECK boards talk to.\n\nIt reads inkharness.toml, AGENTS.md, skills/ \
                  (and .agents/skills/, .claude/skills/), .mcp.json and .env from the folder it \
                  starts in, over the same files in the global config folder \
                  (~/.config/inkharness, or $INKHARNESS_HOME).",
    args_conflicts_with_subcommands = true
)]
struct Cli {
    #[command(subcommand)]
    command: Option<Command>,
    /// Ask this, print the answer and leave (without it: a conversation).
    question: Vec<String>,
    /// Use this file in place of ./inkharness.toml (still over the global one).
    #[arg(short, long, global = true, value_name = "FILE")]
    config: Option<PathBuf>,
    /// Start as if in this folder.
    #[arg(short = 'C', long = "dir", global = true, value_name = "DIR")]
    dir: Option<PathBuf>,
    /// Log more: -v each step, -vv everything.
    #[arg(short, long, action = ArgAction::Count, global = true)]
    verbose: u8,
}

#[derive(Subcommand)]
enum Command {
    /// Run as a daemon: boards talk to it over TCP, programs over HTTP.
    Serve {
        /// Where devices connect (overrides `listen`).
        #[arg(long, value_name = "ADDR")]
        listen: Option<String>,
        /// Where HTTP is answered (overrides `http`; "" turns it off).
        #[arg(long, value_name = "ADDR")]
        http: Option<String>,
    },
    /// Write a starter inkharness.toml to the global config folder.
    Init {
        /// Write it in this folder instead.
        #[arg(long)]
        here: bool,
        /// Replace one that's already there.
        #[arg(long)]
        force: bool,
    },
}

#[tokio::main]
async fn main() {
    if let Err(e) = run().await {
        eprintln!("inkharness: {e:#}");
        std::process::exit(1);
    }
}

async fn run() -> Result<()> {
    let cli = Cli::parse();
    let serving = matches!(cli.command, Some(Command::Serve { .. }));
    let level = match (cli.verbose, serving) {
        (0, false) => "warn",
        (0, true) | (1, _) => "info",
        _ => "info,inkharness=debug",
    };
    let log = tracing_subscriber::fmt()
        .with_writer(std::io::stderr)
        .with_ansi(std::io::IsTerminal::is_terminal(&std::io::stderr()))
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| level.into()),
        );
    if serving {
        log.init();
    } else {
        // Beside a conversation: the message, without the time and the module.
        log.without_time().with_target(false).init();
    }

    if let Some(dir) = &cli.dir {
        std::env::set_current_dir(dir).with_context(|| format!("going to {}", dir.display()))?;
    }
    if let Some(Command::Init { here, force }) = cli.command {
        return init(here, force);
    }

    let here = std::env::current_dir()?;
    let mut place = place::load(&here, cli.config.as_deref())?;
    for file in &place.read {
        tracing::info!(file = %file.display(), "read");
    }
    if let Some(Command::Serve { listen, http }) = cli.command {
        if let Some(listen) = listen {
            place.config.listen = listen;
        }
        if let Some(http) = http {
            place.config.http = http;
        }
        let harness = harness::Harness::build(&place, true).await?;
        return serve::run(place, harness).await;
    }

    let harness = harness::Harness::build(&place, cli.verbose > 0).await?;
    let chat = chat::Chat::build(&place.config, &harness)?;
    if cli.question.is_empty() {
        chat::repl(&chat, &chat::banner(&place, &harness, &chat)).await
    } else {
        chat::once(&chat, &cli.question.join(" ")).await
    }
}

/// Writes STARTER where the next start will find it.
fn init(here: bool, force: bool) -> Result<()> {
    let dir = if here {
        std::env::current_dir()?
    } else {
        place::global_dir().context("no home folder to put the global config in")?
    };
    let file = dir.join(place::CONFIG_FILE);
    if file.exists() && !force {
        bail!("{} is already there (--force replaces it)", file.display());
    }
    std::fs::create_dir_all(&dir).with_context(|| format!("making {}", dir.display()))?;
    std::fs::write(&file, STARTER).with_context(|| format!("writing {}", file.display()))?;
    println!("wrote {}", file.display());
    println!(
        "Put the key it names in your environment or in {}, then run inkharness in any folder.",
        dir.join(".env").display()
    );
    Ok(())
}

const STARTER: &str = r#"# inkharness settings. A folder's own inkharness.toml is layered over the global one
# (~/.config/inkharness/inkharness.toml): its tables merge in, its other values replace.
# Keys come from the environment or a .env file beside this one, never from this file.

# The model, for the chat and the HTTP API, and for boards once there's speech in and out.
[llm]
backend = "anthropic"
model = "claude-opus-5-5"
effort = "low"                  # thinking depth: low answers soonest
api_key_env = "ANTHROPIC_API_KEY"
fallbacks = true

# Or any server with OpenAI's /chat/completions (OpenRouter, Ollama, llama.cpp, vLLM):
# [llm]
# backend = "openai_compat"
# base_url = "https://openrouter.ai/api/v1"
# model = "..."
# api_key_env = "OPENROUTER_API_KEY"

# For boards (inkharness serve): speech to text and text to speech, or a model that hears and
# speaks. inkharness.toml.example in the repo has every section; setup_openrouter.py writes one.
# token = ""                    # boards encrypt with it (voice_token in SD:/inkdeck.ini), HTTP as a bearer
# listen = "0.0.0.0:7391"       # boards
# http = "127.0.0.1:7392"       # the HTTP API; "" turns it off

# Web search: a SearXNG of your own, or Brave or Tavily with a key (the repo's
# inkharness.toml.example has each). [[search]] tables are tried in order.
# [search]
# backend = "searxng"
# url = "http://searxng.local:8080"

# MCP servers also come from a folder's .mcp.json, skills from its skills/ folder, and
# instructions from its AGENTS.md.
"#;
