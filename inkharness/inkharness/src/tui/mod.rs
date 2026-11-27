//! The conversation in the terminal, full-screen: `inkharness` in a terminal (pipes and
//! `--plain` get chat.rs's lines instead). The conversation scrolls above a multi-line input;
//! typing `/` opens a menu of the built-in commands, the command files and the skills; tool
//! calls show as they start and end; the status line has the model, what is connected, the
//! cost and the reply's progress.
//!
//! A turn runs in this task beside the screen, as a future polled with the key events, so
//! stopping a reply drops it (as Ctrl-C does in the plain chat). The log goes to `log::Log`
//! while the screen is up, and MCP servers' stderr nowhere.

mod app;
mod input;
pub mod log;
mod markdown;
mod render;

use std::future::Future;
use std::io::{Stdout, Write};
use std::pin::Pin;
use std::time::Duration;

use anyhow::Result;
use base64::Engine;
use crossterm::event::{
    DisableBracketedPaste, DisableMouseCapture, EnableBracketedPaste, EnableMouseCapture, Event,
    EventStream, KeyboardEnhancementFlags, PopKeyboardEnhancementFlags,
    PushKeyboardEnhancementFlags,
};
use crossterm::terminal::{
    EnterAlternateScreen, LeaveAlternateScreen, disable_raw_mode, enable_raw_mode,
    supports_keyboard_enhancement,
};
use futures_util::StreamExt;
use ratatui::Terminal;
use ratatui::backend::CrosstermBackend;
use tokio::sync::mpsc;

use self::app::{Action, App, Facts, KEYS};
pub use self::log::Log;
use crate::chat::{self, Chat};
use crate::commands;
use crate::harness::Harness;
use crate::llm::Piece;
use crate::meter::{self, Charge};
use crate::place::Place;

type Reply<'a> = Pin<Box<dyn Future<Output = (Result<String>, Vec<Charge>)> + 'a>>;

/// What a key, a paste or a command asks of the loop.
enum Request {
    /// Send this prompt (the input already shows as a turn).
    Start(String),
    Stop,
    Quit,
    Redraw,
}

pub async fn run(chat: &Chat, harness: &Harness, place: &Place, log: Log) -> Result<()> {
    let mut screen = Screen::enter()?;
    let facts = Facts {
        model: chat.model.clone(),
        skills: harness.skills.names().len(),
        servers: harness.mcp.servers().len(),
        tools: chat.tools().defs().len(),
    };
    let mut app = App::new(facts, chat.commands().list());
    let mut intro: Vec<String> = chat::banner(place, harness, chat)
        .lines()
        .map(|l| l.trim().to_string())
        .collect();
    intro.push("/help for the commands and keys.".into());
    app.note(intro);

    let mut events = EventStream::new();
    let mut tick = tokio::time::interval(Duration::from_millis(100));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    // The reply being written, its pieces, and what was typed for it.
    let mut reply: Option<Reply> = None;
    let mut pieces: Option<mpsc::Receiver<Piece>> = None;
    let mut typed = String::new();
    loop {
        if app.dirty {
            app.dirty = false;
            screen.terminal.draw(|f| render::draw(f, &mut app))?;
        }
        let request = tokio::select! {
            event = events.next() => match event {
                None => Some(Request::Quit),
                Some(Err(e)) => return Err(e.into()),
                Some(Ok(event)) => on_event(event, &mut app, chat, harness, &log),
            },
            piece = async { pieces.as_mut().unwrap().recv().await }, if pieces.is_some() => {
                match piece {
                    Some(piece) => app.piece(piece),
                    None => pieces = None,
                }
                None
            }
            (result, charges) = async { reply.as_mut().unwrap().await }, if reply.is_some() => {
                reply = None;
                if let Some(mut rest) = pieces.take() {
                    while let Ok(piece) = rest.try_recv() {
                        app.piece(piece);
                    }
                }
                app.finish(result, charges, &typed);
                None
            }
            _ = tick.tick() => {
                app.tick();
                if let Some(warning) = log.take_warning() {
                    app.flash(warning, true);
                }
                None
            }
        };
        match request {
            None => {}
            Some(Request::Quit) => break,
            Some(Request::Redraw) => {
                screen.terminal.clear()?;
                app.dirty = true;
            }
            Some(Request::Stop) => {
                reply = None;
                pieces = None;
                app.stopped();
            }
            Some(Request::Start(prompt)) => {
                let history = app.history.clone();
                let (tx, rx) = mpsc::channel(64);
                typed = match app.entries.last() {
                    Some(app::Entry::User { typed, .. }) => typed.clone(),
                    _ => prompt.clone(),
                };
                pieces = Some(rx);
                reply = Some(Box::pin(async move {
                    meter::measure(chat.turn(&history, &prompt, tx)).await
                }));
            }
        }
    }
    drop(screen);
    Ok(())
}

fn on_event(
    event: Event,
    app: &mut App,
    chat: &Chat,
    harness: &Harness,
    log: &Log,
) -> Option<Request> {
    match event {
        Event::Key(key) => match app.key(key) {
            Action::None => None,
            Action::Stop => Some(Request::Stop),
            Action::Quit => Some(Request::Quit),
            Action::Redraw => Some(Request::Redraw),
            Action::Submit(text) => submit(app, chat, harness, log, &text),
        },
        Event::Paste(text) => {
            app.paste(&text);
            None
        }
        Event::Mouse(mouse) => {
            app.mouse(mouse);
            None
        }
        Event::Resize(..) => {
            app.dirty = true;
            None
        }
        _ => None,
    }
}

/// The input sent: a built-in runs, a command or a skill expands, anything else goes as
/// written. `//text` sends `/text`.
fn submit(app: &mut App, chat: &Chat, harness: &Harness, log: &Log, text: &str) -> Option<Request> {
    let text = text.trim();
    if text.is_empty() {
        return None;
    }
    if let Some(rest) = text.strip_prefix("//") {
        return start(app, text, format!("/{rest}"), None);
    }
    if let Some((name, args)) = commands::split(text) {
        if let Some(builtin) = commands::builtin(name) {
            if app.busy() && matches!(builtin.name, "clear" | "reset") {
                app.flash("Stop the reply first (Esc)".into(), false);
                return None;
            }
            app.input.take();
            return run_builtin(app, chat, harness, log, builtin.name, args);
        }
        match chat.expand(text) {
            Err(e) => {
                app.input.take();
                app.error(e);
                return None;
            }
            Ok(Some(expanded)) => return start(app, text, expanded.prompt, Some(expanded.source)),
            Ok(None) if is_name(name) => {
                app.note(unknown(app, name));
                return None;
            }
            Ok(None) => {}
        }
    }
    start(app, text, text.to_string(), None)
}

/// A turn starts, unless one is running.
fn start(app: &mut App, typed: &str, prompt: String, source: Option<String>) -> Option<Request> {
    if app.busy() {
        app.flash("A reply is being written: Esc stops it".into(), false);
        return None;
    }
    app.input.take();
    app.start(typed.to_string(), prompt.clone(), source);
    Some(Request::Start(prompt))
}

/// Could be a command's name (not a path or a word with punctuation).
fn is_name(name: &str) -> bool {
    name.chars()
        .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-' || c == ':')
}

/// What /name gets when nothing has that name.
fn unknown(app: &App, name: &str) -> Vec<String> {
    let close: Vec<String> = app
        .commands
        .iter()
        .filter(|c| {
            c.name.contains(name)
                || name.contains(&c.name)
                || c.name.chars().take(2).eq(name.chars().take(2))
        })
        .take(4)
        .map(|c| format!("/{}", c.name))
        .collect();
    let mut line = format!("No command /{name}.");
    if !close.is_empty() {
        line.push_str(&format!(" Did you mean {}?", close.join(", ")));
    }
    vec![
        line,
        format!("/help lists the commands; //{name} sends /{name} as written."),
    ]
}

fn run_builtin(
    app: &mut App,
    chat: &Chat,
    harness: &Harness,
    log: &Log,
    name: &str,
    args: &str,
) -> Option<Request> {
    match name {
        "quit" | "exit" => return Some(Request::Quit),
        "clear" | "reset" => app.clear(),
        "help" => app.note(chat::help_lines(chat, KEYS)),
        "tools" => app.note(chat::tool_lines(chat)),
        "skills" => app.note(chat::skill_lines(harness)),
        "mcp" => app.note(chat::mcp_lines(harness)),
        "cost" => {
            let mut lines: Vec<String> = app
                .costs
                .iter()
                .map(|(typed, cost, unpriced)| {
                    format!(
                        "{:<20} {}",
                        chat::cost_text(*cost, *unpriced),
                        chat::cut(typed.lines().next().unwrap_or(""), 50)
                    )
                })
                .collect();
            let total: f64 = app.costs.iter().map(|(_, cost, _)| cost).sum();
            let unpriced: usize = app.costs.iter().map(|(_, _, n)| n).sum();
            lines.push(format!(
                "{} turns, {} in all (as the responses stated it)",
                app.costs.len(),
                chat::cost_text(total, unpriced)
            ));
            app.note(lines);
        }
        "copy" => match app.last_reply().map(str::to_string) {
            Some(text) => {
                copy(&text);
                app.flash(
                    "Copied the last reply (if the terminal takes OSC 52)".into(),
                    false,
                );
            }
            None => app.flash("No reply to copy yet".into(), false),
        },
        "save" => {
            let file = if args.is_empty() {
                format!(
                    "inkharness-{}.md",
                    chrono::Local::now().format("%Y%m%d-%H%M%S")
                )
            } else {
                args.to_string()
            };
            match std::fs::write(&file, app.markdown()) {
                Ok(()) => app.note(vec![format!("Saved the conversation to {file}.")]),
                Err(e) => app.error(format!("can't write {file}: {e}")),
            }
        }
        "log" => {
            let lines = log.lines();
            let start = lines.len().saturating_sub(40);
            if lines.is_empty() {
                app.note(vec!["Nothing logged yet (-v logs more).".into()]);
            } else {
                app.note(lines[start..].to_vec());
            }
        }
        other => app.note(vec![format!("/{other} isn't here yet.")]),
    }
    None
}

/// Puts `text` on the clipboard by OSC 52, which most terminals take.
fn copy(text: &str) {
    let encoded = base64::engine::general_purpose::STANDARD.encode(text);
    let mut out = std::io::stdout();
    let _ = write!(out, "\x1b]52;c;{encoded}\x07");
    let _ = out.flush();
}

/// The terminal in full-screen mode while this lives, and as it was after (a panic too).
struct Screen {
    terminal: Terminal<CrosstermBackend<Stdout>>,
    enhanced: bool,
}

impl Screen {
    fn enter() -> Result<Screen> {
        enable_raw_mode()?;
        let mut out = std::io::stdout();
        crossterm::execute!(
            out,
            EnterAlternateScreen,
            EnableBracketedPaste,
            EnableMouseCapture
        )?;
        // Shift-Enter is only told apart from Enter with the kitty keyboard protocol.
        let enhanced = matches!(supports_keyboard_enhancement(), Ok(true));
        if enhanced {
            crossterm::execute!(
                out,
                PushKeyboardEnhancementFlags(KeyboardEnhancementFlags::DISAMBIGUATE_ESCAPE_CODES)
            )?;
        }
        let previous = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            restore(true);
            previous(info);
        }));
        let mut terminal = Terminal::new(CrosstermBackend::new(out))?;
        terminal.clear()?;
        Ok(Screen { terminal, enhanced })
    }
}

fn restore(enhanced: bool) {
    let mut out = std::io::stdout();
    if enhanced {
        let _ = crossterm::execute!(out, PopKeyboardEnhancementFlags);
    }
    let _ = crossterm::execute!(
        out,
        DisableMouseCapture,
        DisableBracketedPaste,
        LeaveAlternateScreen,
        crossterm::cursor::Show
    );
    let _ = disable_raw_mode();
}

impl Drop for Screen {
    fn drop(&mut self) {
        restore(self.enhanced);
    }
}
