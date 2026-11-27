//! The full-screen chat's state, and what keys and the model's pieces do to it. Nothing here
//! touches the terminal or the model: mod.rs runs the turns and the built-ins, render.rs draws.

use std::time::{Duration, Instant};

use crossterm::event::{KeyCode, KeyEvent, KeyEventKind, KeyModifiers, MouseEvent, MouseEventKind};
use ratatui::text::Line;
use serde_json::Value;

use super::input::Input;
use super::render;
use crate::chat;
use crate::commands::Listed;
use crate::llm::{Piece, Turn};
use crate::meter::{self, Charge};

/// How long a message stays in the status line.
const FLASH: Duration = Duration::from_secs(6);
/// A second Ctrl-C within this long leaves.
const QUIT_AGAIN: Duration = Duration::from_secs(2);
/// Lines the mouse wheel and Shift-↑/↓ move.
const WHEEL: usize = 3;
/// Rows the command menu shows at most.
pub const MENU_ROWS: usize = 8;

/// The keys, as /help lists them.
pub const KEYS: &[&str] = &[
    "Enter sends · Shift-Enter, Alt-Enter or Ctrl-J: a new line · ↑/↓: earlier inputs",
    "/ opens the menu: ↑/↓ pick, Tab completes, Enter runs, Esc closes",
    "Esc or Ctrl-C stops a reply · Ctrl-C twice or Ctrl-D on an empty input leaves",
    "PgUp/PgDn, Shift-↑/↓ or the wheel scroll · Ctrl-Home/End: top and bottom · Ctrl-L redraws",
    "Shift-drag selects text in most terminals; /copy copies the last reply",
];

/// What the status line says about the place.
pub struct Facts {
    pub model: String,
    pub skills: usize,
    pub servers: usize,
    pub tools: usize,
}

pub enum Entry {
    /// What was typed, and where its prompt came from if it was a command.
    User {
        typed: String,
        source: Option<String>,
    },
    Assistant {
        text: String,
    },
    Tool {
        name: String,
        args: String,
        started: Instant,
        /// Whether it worked and how long it took, once it's done.
        done: Option<(bool, Duration)>,
    },
    Note {
        lines: Vec<String>,
    },
    Error {
        text: String,
    },
}

/// A reply being written.
struct Turning {
    since: Instant,
    /// What the model was sent, for the history.
    prompt: String,
    /// Where this turn's entries start.
    first: usize,
}

#[derive(Debug, PartialEq)]
pub enum Action {
    None,
    /// The input, to send or run: mod.rs decides, and takes it from the input if it does.
    Submit(String),
    Stop,
    Quit,
    Redraw,
}

pub struct App {
    pub entries: Vec<Entry>,
    /// Each entry's lines at a width (and a running tool's spinner frame), while it doesn't
    /// change.
    #[allow(clippy::type_complexity)]
    cache: Vec<Option<(usize, Option<usize>, Vec<Line<'static>>)>>,
    pub input: Input,
    pub history: Vec<Turn>,
    /// Each finished turn: what was typed, its stated cost, and its requests that stated none.
    pub costs: Vec<(String, f64, usize)>,
    pub commands: Vec<Listed>,
    pub facts: Facts,
    /// Lines scrolled up from the bottom; 0 follows the reply.
    pub scroll: usize,
    /// Something new arrived below while scrolled up.
    pub unseen: bool,
    /// The conversation's height at the last draw.
    pub page: usize,
    pub selected: usize,
    /// The input the menu was closed at: it stays closed until the input changes.
    menu_closed: Option<String>,
    turning: Option<Turning>,
    quit_armed: Option<Instant>,
    flash: Option<(String, Instant, bool)>,
    pub dirty: bool,
}

impl App {
    pub fn new(facts: Facts, commands: Vec<Listed>) -> App {
        App {
            entries: Vec::new(),
            cache: Vec::new(),
            input: Input::default(),
            history: Vec::new(),
            costs: Vec::new(),
            commands,
            facts,
            scroll: 0,
            unseen: false,
            page: 20,
            selected: 0,
            menu_closed: None,
            turning: None,
            quit_armed: None,
            flash: None,
            dirty: true,
        }
    }

    pub fn busy(&self) -> bool {
        self.turning.is_some()
    }

    /// How long the reply has been running.
    pub fn running_for(&self) -> Option<Duration> {
        self.turning.as_ref().map(|t| t.since.elapsed())
    }

    fn push(&mut self, entry: Entry) {
        self.entries.push(entry);
        self.cache.push(None);
        self.changed();
    }

    /// Something in the conversation changed.
    fn changed(&mut self) {
        if self.scroll > 0 {
            self.unseen = true;
        }
        self.dirty = true;
    }

    fn touch(&mut self, i: usize) {
        self.cache[i] = None;
        self.changed();
    }

    pub fn note(&mut self, lines: Vec<String>) {
        self.push(Entry::Note { lines });
    }

    pub fn error(&mut self, text: String) {
        self.push(Entry::Error { text });
    }

    /// A message in the status line for a while; a warning stands out.
    pub fn flash(&mut self, text: String, warning: bool) {
        self.flash = Some((text, Instant::now(), warning));
        self.dirty = true;
    }

    pub fn flashing(&self) -> Option<(&str, bool)> {
        self.flash
            .as_ref()
            .filter(|(_, at, _)| at.elapsed() < FLASH)
            .map(|(text, _, warning)| (text.as_str(), *warning))
    }

    /// Every 100 ms: the spinner turns, and a flash ends.
    pub fn tick(&mut self) {
        if self.busy() {
            self.dirty = true;
        }
        if self
            .flash
            .as_ref()
            .is_some_and(|(_, at, _)| at.elapsed() >= FLASH)
        {
            self.flash = None;
            self.dirty = true;
        }
    }

    /// A new conversation: the screen and the history emptied, the costs kept.
    pub fn clear(&mut self) {
        self.entries.clear();
        self.cache.clear();
        self.history.clear();
        self.scroll = 0;
        self.unseen = false;
        self.note(vec!["A new conversation.".into()]);
    }

    /// The entry's lines at `width`, drawn again only when it, the width or (for a tool still
    /// running) the spinner changes.
    pub fn lines(&mut self, i: usize, width: usize) -> &[Line<'static>] {
        let running = matches!(self.entries[i], Entry::Tool { done: None, .. }) && self.busy();
        let spin = running.then(|| render::spinner(self.running_for().unwrap_or_default()));
        let fresh = matches!(&self.cache[i], Some((w, s, _)) if *w == width && *s == spin);
        if !fresh {
            let prev = i.checked_sub(1).map(|p| &self.entries[p]);
            let lines = render::entry_lines(&self.entries[i], prev, width, spin);
            self.cache[i] = Some((width, spin, lines));
        }
        &self.cache[i].as_ref().unwrap().2
    }

    // --- turns ----------------------------------------------------------------------------

    /// A turn starts: `typed` shows, `prompt` goes to the model.
    pub fn start(&mut self, typed: String, prompt: String, source: Option<String>) {
        self.push(Entry::User { typed, source });
        self.turning = Some(Turning {
            since: Instant::now(),
            prompt,
            first: self.entries.len(),
        });
        self.scroll = 0;
        self.unseen = false;
    }

    pub fn piece(&mut self, piece: Piece) {
        match piece {
            Piece::Text(text) | Piece::Transcript(text) => {
                if text.is_empty() {
                    return;
                }
                let last = self.entries.len().wrapping_sub(1);
                if let Some(Entry::Assistant { text: so_far }) = self.entries.last_mut() {
                    so_far.push_str(&text);
                    self.touch(last);
                } else {
                    let text = text.trim_start_matches('\n').to_string();
                    if !text.is_empty() {
                        self.push(Entry::Assistant { text });
                    }
                }
            }
            Piece::Tool(name, input) => self.push(Entry::Tool {
                name,
                args: args_text(&input),
                started: Instant::now(),
                done: None,
            }),
            Piece::ToolDone(done_name, ok) => {
                let first = self.turning.as_ref().map_or(0, |t| t.first);
                let found = self.entries[first.min(self.entries.len())..]
                    .iter()
                    .position(
                        |e| matches!(e, Entry::Tool { name, done: None, .. } if *name == done_name),
                    )
                    .map(|i| i + first);
                if let Some(i) = found {
                    if let Entry::Tool { started, done, .. } = &mut self.entries[i] {
                        *done = Some((ok, started.elapsed()));
                    }
                    self.touch(i);
                }
            }
            Piece::Audio(_) => {}
        }
    }

    /// The turn ended: its reply joins the history, or its error shows.
    pub fn finish(&mut self, result: anyhow::Result<String>, charges: Vec<Charge>, typed: &str) {
        let Some(turning) = self.turning.take() else {
            return;
        };
        let (cost, unpriced) = meter::total(&charges);
        self.costs.push((typed.to_string(), cost, unpriced));
        match result {
            Ok(reply) => {
                if reply.trim().is_empty() && turning.first == self.entries.len() {
                    self.note(vec!["(no reply)".into()]);
                }
                chat::remember(&mut self.history, &turning.prompt, &reply);
            }
            Err(e) => self.error(format!("{e:#}")),
        }
        self.dirty = true;
    }

    /// The reply was stopped: what it wrote stays on the screen, out of the history.
    pub fn stopped(&mut self) {
        if self.turning.take().is_some() {
            self.note(vec!["(stopped)".into()]);
        }
    }

    // --- the menu --------------------------------------------------------------------------

    /// The commands the input could be the start of: names that start with it, then names
    /// that hold it. Empty unless a name is being typed.
    pub fn menu(&self) -> Vec<&Listed> {
        let text = self.input.text();
        let Some(query) = text.strip_prefix('/') else {
            return Vec::new();
        };
        if query.starts_with('/')
            || query.contains(char::is_whitespace)
            || self.menu_closed.as_deref() == Some(text.as_str())
        {
            return Vec::new();
        }
        let starts = self.commands.iter().filter(|c| c.name.starts_with(query));
        let holds = self
            .commands
            .iter()
            .filter(|c| !c.name.starts_with(query) && c.name.contains(query));
        starts.chain(holds).collect()
    }

    /// The menu's choice, if it is open.
    fn chosen(&self) -> Option<Listed> {
        let menu = self.menu();
        menu.get(self.selected.min(menu.len().saturating_sub(1)))
            .map(|c| (*c).clone())
    }

    // --- keys ------------------------------------------------------------------------------

    pub fn key(&mut self, key: KeyEvent) -> Action {
        if key.kind == KeyEventKind::Release {
            return Action::None;
        }
        let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
        let alt = key.modifiers.contains(KeyModifiers::ALT);
        let shift = key.modifiers.contains(KeyModifiers::SHIFT);
        let before = self.input.text();
        if !(ctrl && key.code == KeyCode::Char('c')) {
            self.quit_armed = None;
        }
        self.dirty = true;
        let action = match key.code {
            KeyCode::Char('c') if ctrl => {
                if self.busy() {
                    Action::Stop
                } else if !self.input.is_empty() {
                    self.input.set("");
                    Action::None
                } else if self.quit_armed.is_some_and(|at| at.elapsed() < QUIT_AGAIN) {
                    Action::Quit
                } else {
                    self.quit_armed = Some(Instant::now());
                    self.flash("Ctrl-C again to leave".into(), false);
                    Action::None
                }
            }
            KeyCode::Char('d') if ctrl => {
                if self.input.is_empty() {
                    Action::Quit
                } else {
                    self.input.delete();
                    Action::None
                }
            }
            KeyCode::Char('l') if ctrl => Action::Redraw,
            KeyCode::Esc => {
                if !self.menu().is_empty() {
                    self.menu_closed = Some(before.clone());
                    Action::None
                } else if self.busy() {
                    Action::Stop
                } else {
                    self.bottom();
                    Action::None
                }
            }
            KeyCode::Enter if shift || alt => {
                self.input.newline();
                Action::None
            }
            KeyCode::Char('j') if ctrl => {
                self.input.newline();
                Action::None
            }
            KeyCode::Enter => match self.chosen() {
                Some(chosen)
                    if !chosen.hint.is_empty() && before != format!("/{}", chosen.name) =>
                {
                    self.input.set(&format!("/{} ", chosen.name));
                    Action::None
                }
                Some(chosen) => {
                    self.input.set(&format!("/{}", chosen.name));
                    Action::Submit(self.input.text())
                }
                None => Action::Submit(before.clone()),
            },
            KeyCode::Tab => {
                if let Some(chosen) = self.chosen() {
                    self.input.set(&format!("/{} ", chosen.name));
                }
                Action::None
            }
            KeyCode::Up if shift => {
                self.scroll_up(1);
                Action::None
            }
            KeyCode::Down if shift => {
                self.scroll_down(1);
                Action::None
            }
            KeyCode::Up => {
                let n = self.menu().len();
                if n > 0 {
                    self.selected = (self.selected.min(n - 1) + n - 1) % n;
                } else {
                    self.input.up();
                }
                Action::None
            }
            KeyCode::Down => {
                let n = self.menu().len();
                if n > 0 {
                    self.selected = (self.selected.min(n - 1) + 1) % n;
                } else {
                    self.input.down();
                }
                Action::None
            }
            KeyCode::PageUp => {
                self.scroll_up(self.page.saturating_sub(2).max(1));
                Action::None
            }
            KeyCode::PageDown => {
                self.scroll_down(self.page.saturating_sub(2).max(1));
                Action::None
            }
            KeyCode::Home if ctrl => {
                self.scroll = usize::MAX;
                Action::None
            }
            KeyCode::End if ctrl => {
                self.bottom();
                Action::None
            }
            KeyCode::Backspace if ctrl || alt => {
                self.input.delete_word();
                Action::None
            }
            KeyCode::Backspace => {
                self.input.backspace();
                Action::None
            }
            KeyCode::Delete => {
                self.input.delete();
                Action::None
            }
            KeyCode::Left if ctrl || alt => {
                self.input.word_left();
                Action::None
            }
            KeyCode::Right if ctrl || alt => {
                self.input.word_right();
                Action::None
            }
            KeyCode::Left => {
                self.input.left();
                Action::None
            }
            KeyCode::Right => {
                self.input.right();
                Action::None
            }
            KeyCode::Home => {
                self.input.home();
                Action::None
            }
            KeyCode::End => {
                self.input.end();
                Action::None
            }
            KeyCode::Char(c) if ctrl => {
                match c {
                    'a' => self.input.home(),
                    'e' => self.input.end(),
                    'u' => self.input.kill_back(),
                    'k' => self.input.kill_forward(),
                    'w' => self.input.delete_word(),
                    _ => {}
                }
                Action::None
            }
            KeyCode::Char(c) if alt => {
                match c {
                    'b' => self.input.word_left(),
                    'f' => self.input.word_right(),
                    _ => {}
                }
                Action::None
            }
            KeyCode::Char(c) => {
                self.input.insert(c);
                Action::None
            }
            _ => Action::None,
        };
        if self.input.text() != before {
            self.selected = 0;
            self.menu_closed = None;
        }
        action
    }

    pub fn paste(&mut self, text: &str) {
        self.input.paste(text);
        self.selected = 0;
        self.menu_closed = None;
        self.dirty = true;
    }

    pub fn mouse(&mut self, mouse: MouseEvent) {
        match mouse.kind {
            MouseEventKind::ScrollUp => self.scroll_up(WHEEL),
            MouseEventKind::ScrollDown => self.scroll_down(WHEEL),
            _ => {}
        }
    }

    fn scroll_up(&mut self, lines: usize) {
        self.scroll = self.scroll.saturating_add(lines);
        self.dirty = true;
    }

    fn scroll_down(&mut self, lines: usize) {
        self.scroll = self.scroll.saturating_sub(lines);
        if self.scroll == 0 {
            self.unseen = false;
        }
        self.dirty = true;
    }

    fn bottom(&mut self) {
        self.scroll = 0;
        self.unseen = false;
        self.dirty = true;
    }

    /// The last reply's text.
    pub fn last_reply(&self) -> Option<&str> {
        self.entries.iter().rev().find_map(|e| match e {
            Entry::Assistant { text } => Some(text.as_str()),
            _ => None,
        })
    }

    /// The conversation as markdown, for /save.
    pub fn markdown(&self) -> String {
        let mut out = String::new();
        for entry in &self.entries {
            match entry {
                Entry::User { typed, source } => {
                    out.push_str(&format!("\n## You\n\n{typed}\n"));
                    if let Some(source) = source {
                        out.push_str(&format!("\n*(from {source})*\n"));
                    }
                }
                Entry::Assistant { text } => out.push_str(&format!("\n{}\n", text.trim())),
                Entry::Tool {
                    name, args, done, ..
                } => {
                    let mark = match done {
                        Some((true, _)) => "✓",
                        Some((false, _)) => "✗",
                        None => "…",
                    };
                    out.push_str(&format!("\n- `{name} {args}` {mark}\n"));
                }
                Entry::Note { .. } => {}
                Entry::Error { text } => out.push_str(&format!("\n> error: {text}\n")),
            }
        }
        out.trim_start().to_string()
    }
}

/// A tool's input as `key=value` words.
pub fn args_text(input: &Value) -> String {
    match input {
        Value::Object(map) => map
            .iter()
            .map(|(k, v)| match v {
                Value::String(s) if s.contains(char::is_whitespace) || s.is_empty() => {
                    format!("{k}={s:?}")
                }
                Value::String(s) => format!("{k}={s}"),
                other => format!("{k}={other}"),
            })
            .collect::<Vec<_>>()
            .join(" "),
        Value::Null => String::new(),
        other => other.to_string(),
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::commands::Kind;

    fn listed(name: &str, hint: &str) -> Listed {
        Listed {
            name: name.into(),
            description: format!("does {name}"),
            hint: hint.into(),
            kind: Kind::File,
        }
    }

    pub fn app() -> App {
        App::new(
            Facts {
                model: "mock".into(),
                skills: 1,
                servers: 0,
                tools: 3,
            },
            vec![
                listed("clear", ""),
                listed("goodnight", "[request]"),
                listed("play", "<album>"),
                listed("help", ""),
            ],
        )
    }

    fn press(app: &mut App, code: KeyCode) -> Action {
        app.key(KeyEvent::new(code, KeyModifiers::NONE))
    }

    fn typed(app: &mut App, text: &str) {
        for c in text.chars() {
            press(app, KeyCode::Char(c));
        }
    }

    #[test]
    fn the_menu_filters_completes_and_runs() {
        let mut app = app();
        typed(&mut app, "/");
        assert_eq!(app.menu().len(), 4);
        typed(&mut app, "p");
        // Names that start with it, then names that hold it.
        let names: Vec<&str> = app.menu().iter().map(|c| c.name.as_str()).collect();
        assert_eq!(names, ["play", "help"]);
        // Enter on one that takes arguments completes it; the menu closes at the space.
        assert_eq!(press(&mut app, KeyCode::Enter), Action::None);
        assert_eq!(app.input.text(), "/play ");
        assert!(app.menu().is_empty());
        typed(&mut app, "Coltrane");
        assert_eq!(
            press(&mut app, KeyCode::Enter),
            Action::Submit("/play Coltrane".into())
        );

        // ↓ then Enter on one that takes nothing runs it.
        let mut app = super::tests::app();
        typed(&mut app, "/");
        press(&mut app, KeyCode::Down);
        press(&mut app, KeyCode::Up);
        press(&mut app, KeyCode::Up);
        assert_eq!(app.chosen().unwrap().name, "help");
        assert_eq!(
            press(&mut app, KeyCode::Enter),
            Action::Submit("/help".into())
        );

        // Tab completes; Esc closes until the input changes.
        let mut app = super::tests::app();
        typed(&mut app, "/go");
        press(&mut app, KeyCode::Tab);
        assert_eq!(app.input.text(), "/goodnight ");
        app.input.set("/go");
        assert!(!app.menu().is_empty());
        press(&mut app, KeyCode::Esc);
        assert!(app.menu().is_empty());
        typed(&mut app, "x");
        assert!(app.menu().is_empty(), "no command holds gox");
        press(&mut app, KeyCode::Backspace);
        assert!(!app.menu().is_empty());
        // A path isn't a name being typed.
        app.input.set("/etc/hosts");
        assert!(app.menu().is_empty());
    }

    #[test]
    fn keys_edit_send_stop_and_leave() {
        let mut app = app();
        typed(&mut app, "one");
        app.key(KeyEvent::new(KeyCode::Enter, KeyModifiers::SHIFT));
        typed(&mut app, "two");
        assert_eq!(app.input.text(), "one\ntwo");
        assert_eq!(
            press(&mut app, KeyCode::Enter),
            Action::Submit("one\ntwo".into())
        );
        let ctrl = |c| KeyEvent::new(KeyCode::Char(c), KeyModifiers::CONTROL);
        // Ctrl-C empties the input, then asks again before leaving.
        assert_eq!(app.key(ctrl('c')), Action::None);
        assert!(app.input.is_empty());
        assert_eq!(app.key(ctrl('c')), Action::None);
        assert!(app.flashing().is_some());
        assert_eq!(app.key(ctrl('c')), Action::Quit);
        // While a reply runs, Ctrl-C and Esc stop it.
        app.start("hi".into(), "hi".into(), None);
        assert_eq!(app.key(ctrl('c')), Action::Stop);
        assert_eq!(press(&mut app, KeyCode::Esc), Action::Stop);
        app.stopped();
        assert!(!app.busy());
        assert_eq!(app.key(ctrl('d')), Action::Quit);
    }

    #[test]
    fn pieces_build_the_turn() {
        let mut app = app();
        app.start(
            "/play Coltrane".into(),
            "Play this: Coltrane".into(),
            Some("play.md".into()),
        );
        app.piece(Piece::Text("Looking".into()));
        app.piece(Piece::Text(" it up.".into()));
        app.piece(Piece::Tool(
            "play_music".into(),
            serde_json::json!({"speaker": "media_player.kitchen", "query": "Coltrane"}),
        ));
        app.piece(Piece::Tool(
            "web_search".into(),
            serde_json::json!({"query": "a b"}),
        ));
        app.piece(Piece::ToolDone("web_search".into(), false));
        app.piece(Piece::ToolDone("play_music".into(), true));
        app.piece(Piece::Text("\nFound it.".into()));
        app.finish(
            Ok("Looking it up.\nFound it.".into()),
            vec![],
            "/play Coltrane",
        );

        assert_eq!(app.entries.len(), 5);
        assert!(matches!(&app.entries[1], Entry::Assistant { text } if text == "Looking it up."));
        assert!(matches!(
            &app.entries[2],
            Entry::Tool { args, done: Some((true, _)), .. } if args == "query=Coltrane speaker=media_player.kitchen"
        ));
        assert!(
            matches!(&app.entries[3], Entry::Tool { done: Some((false, _)), args, .. } if args == "query=\"a b\"")
        );
        assert!(matches!(&app.entries[4], Entry::Assistant { text } if text == "Found it."));
        // The history has what the model was sent.
        assert_eq!(app.history.len(), 2);
        assert_eq!(app.history[0].text, "Play this: Coltrane");
        assert_eq!(app.costs.len(), 1);
        assert_eq!(app.last_reply(), Some("Found it."));
        let md = app.markdown();
        assert!(
            md.starts_with("## You\n\n/play Coltrane\n\n*(from play.md)*"),
            "{md}"
        );
        assert!(md.contains("- `web_search query=\"a b\"` ✗"), "{md}");

        app.clear();
        assert!(app.history.is_empty());
        assert_eq!(app.entries.len(), 1);
    }

    #[test]
    fn scrolling_marks_what_comes_below() {
        let mut app = app();
        app.scroll_up(5);
        app.note(vec!["new".into()]);
        assert!(app.unseen);
        app.start("x".into(), "x".into(), None);
        assert_eq!(app.scroll, 0, "sending goes back to the bottom");
        assert!(!app.unseen);
    }
}
