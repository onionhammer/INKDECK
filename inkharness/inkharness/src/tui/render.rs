//! Drawing the full-screen chat: the conversation (scrolled from the bottom), the command menu
//! over it, the input box and the status line.

use std::time::Duration;

use ratatui::Frame;
use ratatui::layout::{Alignment, Constraint, Layout, Position, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Borders, Clear, Paragraph};
use unicode_width::UnicodeWidthStr;

use super::app::{App, Entry, MENU_ROWS};
use super::markdown::{self, chunks, dim, wrap};
use crate::chat;
use crate::commands::{Kind, Listed};

/// The input box grows to this many lines, then scrolls.
const INPUT_ROWS: usize = 6;
const SPINNER: [&str; 10] = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];

/// The spinner's frame for a reply running this long.
pub fn spinner(running: Duration) -> usize {
    (running.as_millis() / 100) as usize % SPINNER.len()
}

pub fn draw(frame: &mut Frame, app: &mut App) {
    let area = frame.area();
    let (rows, cursor) = input_rows(app, area.width.saturating_sub(4) as usize);
    let input_height = rows.len().clamp(1, INPUT_ROWS) as u16 + 2;
    let [talk, input, status] = Layout::vertical([
        Constraint::Min(1),
        Constraint::Length(input_height),
        Constraint::Length(1),
    ])
    .areas(area);
    conversation(frame, app, talk);
    input_box(frame, app, input, rows, cursor);
    menu(frame, app, talk, input);
    status_line(frame, app, status);
}

/// Whether a blank line goes between `prev` and `entry`: around a turn's start, and around
/// notes and errors.
fn gap(prev: Option<&Entry>, entry: &Entry) -> bool {
    match (prev, entry) {
        (None, _) => false,
        (Some(Entry::User { .. }), _) | (_, Entry::User { .. }) => true,
        (_, Entry::Note { .. } | Entry::Error { .. }) => true,
        (Some(Entry::Note { .. } | Entry::Error { .. }), _) => true,
        _ => false,
    }
}

/// An entry's lines at `width`. `spin`: the spinner's frame, for a tool still running.
pub fn entry_lines(
    entry: &Entry,
    prev: Option<&Entry>,
    width: usize,
    spin: Option<usize>,
) -> Vec<Line<'static>> {
    let mut lines = Vec::new();
    if gap(prev, entry) {
        lines.push(Line::default());
    }
    let pad = || vec![Span::raw("  ")];
    match entry {
        Entry::User { typed, source } => {
            let style = Style::default().add_modifier(Modifier::BOLD);
            let first = vec![Span::styled(
                "› ",
                Style::default()
                    .fg(Color::Cyan)
                    .add_modifier(Modifier::BOLD),
            )];
            lines.extend(wrap(&[(typed.clone(), style)], width, &first, &pad()));
            if let Some(source) = source {
                lines.push(Line::styled(format!("  ↳ {source}"), dim()));
            }
        }
        Entry::Assistant { text } => {
            for mut line in markdown::render(text, width.saturating_sub(2)) {
                line.spans.insert(0, Span::raw("  "));
                lines.push(line);
            }
        }
        Entry::Tool {
            name, args, done, ..
        } => {
            let (mark, style) = match (done, spin) {
                (Some((true, _)), _) => ("✓", Style::default().fg(Color::Green)),
                (Some((false, _)), _) => ("✗", Style::default().fg(Color::Red)),
                (None, Some(frame)) => (SPINNER[frame], Style::default().fg(Color::Yellow)),
                (None, None) => ("·", dim()),
            };
            let took = match done {
                Some((_, took)) => format!("  {:.1}s", took.as_secs_f32()),
                None => String::new(),
            };
            let fixed = 4 + name.width() + took.width();
            let room = width.saturating_sub(fixed);
            let args = if args.is_empty() || room < 4 {
                String::new()
            } else {
                format!(" {}", chat::cut(args, room.saturating_sub(2)))
            };
            lines.push(Line::from(vec![
                Span::raw("  "),
                Span::styled(mark, style),
                Span::raw(" "),
                Span::raw(name.clone()),
                Span::styled(args, dim()),
                Span::styled(took, dim()),
            ]));
        }
        Entry::Note { lines: notes } => {
            for note in notes {
                lines.extend(wrap(&[(note.clone(), dim())], width, &pad(), &pad()));
            }
        }
        Entry::Error { text } => {
            let red = Style::default().fg(Color::Red);
            let first = vec![Span::raw("  "), Span::styled("error: ", red)];
            lines.extend(wrap(&[(text.clone(), red)], width, &first, &pad()));
        }
    }
    lines
}

fn conversation(frame: &mut Frame, app: &mut App, area: Rect) {
    let (width, height) = (area.width as usize, area.height as usize);
    app.page = height;
    let counts: Vec<usize> = (0..app.entries.len())
        .map(|i| app.lines(i, width).len())
        .collect();
    let total: usize = counts.iter().sum();
    app.scroll = app.scroll.min(total.saturating_sub(height));
    if app.scroll == 0 {
        app.unseen = false;
    }
    let end = total - app.scroll;
    let start = end.saturating_sub(height);
    let mut visible = Vec::with_capacity(height);
    let mut at = 0;
    for (i, count) in counts.iter().enumerate() {
        if at + count > start && at < end {
            for (j, line) in app.lines(i, width).iter().enumerate() {
                if (start..end).contains(&(at + j)) {
                    visible.push(line.clone());
                }
            }
        }
        at += count;
    }
    frame.render_widget(Paragraph::new(visible), area);

    if app.scroll > 0 && area.height > 1 {
        let text = if app.unseen {
            " ↓ new below · Ctrl-End ".to_string()
        } else {
            format!(" ↓ {} lines below · Ctrl-End ", app.scroll)
        };
        let w = (text.width() as u16).min(area.width);
        let hint = Rect::new(area.right() - w, area.bottom() - 1, w, 1);
        frame.render_widget(
            Paragraph::new(text).style(Style::default().add_modifier(Modifier::REVERSED)),
            hint,
        );
    }
}

/// The input's lines as they show, wrapped to `width`, and where the cursor is among them.
fn input_rows(app: &App, width: usize) -> (Vec<String>, (usize, usize)) {
    let width = width.max(4);
    let (row, col) = app.input.cursor();
    let mut rows = Vec::new();
    let mut cursor = (0, 0);
    for (i, line) in app.input.lines().iter().enumerate() {
        let pieces = chunks(line, width);
        if i == row {
            // The piece the cursor's character is in, and its column there.
            let mut left = col;
            let mut found = false;
            for (k, piece) in pieces.iter().enumerate() {
                let n = piece.chars().count();
                let last = k + 1 == pieces.len();
                if left < n || (last && left == n) {
                    let before: String = piece.chars().take(left).collect();
                    let x = before.width();
                    cursor = if x >= width {
                        (rows.len() + k + 1, 0)
                    } else {
                        (rows.len() + k, x)
                    };
                    found = true;
                    break;
                }
                left -= n;
            }
            if !found {
                cursor = (rows.len() + pieces.len() - 1, 0);
            }
        }
        rows.extend(pieces);
    }
    if cursor.0 >= rows.len() {
        rows.push(String::new());
    }
    (rows, cursor)
}

fn input_box(frame: &mut Frame, app: &App, area: Rect, rows: Vec<String>, cursor: (usize, usize)) {
    let border = if app.busy() {
        dim()
    } else {
        Style::default().fg(Color::Cyan)
    };
    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(border);
    let inner = block.inner(area);
    frame.render_widget(block, area);
    let shown = inner.height as usize;
    let skip = (cursor.0 + 1).saturating_sub(shown);
    let lines: Vec<Line> = if app.input.is_empty() {
        let hint = if app.busy() {
            "Writing… Esc stops it"
        } else {
            "Ask anything, or / for commands"
        };
        vec![Line::from(vec![
            Span::styled("› ", Style::default().fg(Color::Cyan)),
            Span::styled(hint, dim()),
        ])]
    } else {
        rows.iter()
            .enumerate()
            .skip(skip)
            .take(shown)
            .map(|(i, row)| {
                let prompt = if i == 0 { "› " } else { "  " };
                Line::from(vec![
                    Span::styled(prompt, Style::default().fg(Color::Cyan)),
                    Span::raw(row.clone()),
                ])
            })
            .collect()
    };
    frame.render_widget(Paragraph::new(lines), inner);
    let (row, col) = cursor;
    let y = inner.y + (row - skip) as u16;
    let x = inner.x + 2 + col as u16;
    if y < inner.bottom() && x < inner.right() {
        frame.set_cursor_position(Position::new(x, y));
    }
}

fn menu(frame: &mut Frame, app: &App, talk: Rect, input: Rect) {
    let items = app.menu();
    if items.is_empty() {
        return;
    }
    let selected = app.selected.min(items.len() - 1);
    let rows = items.len().min(MENU_ROWS);
    let height = (rows as u16 + 2).min(talk.height);
    if height < 3 {
        return;
    }
    let width = input.width.min(90);
    let area = Rect::new(input.x, input.y.saturating_sub(height), width, height);
    let first = (selected + 1).saturating_sub(rows);
    let inner_width = width.saturating_sub(2) as usize;
    let label = |item: &Listed| {
        let hint = if item.hint.is_empty() {
            String::new()
        } else {
            format!(" {}", item.hint)
        };
        (format!(" /{}", item.name), hint)
    };
    // The descriptions line up after the longest name shown.
    let column = items
        .iter()
        .skip(first)
        .take(rows)
        .map(|item| {
            let (name, hint) = label(item);
            name.width() + hint.width()
        })
        .max()
        .unwrap_or(0)
        .min(inner_width / 2);
    let lines: Vec<Line> = items
        .iter()
        .enumerate()
        .skip(first)
        .take(rows)
        .map(|(i, item)| {
            let kind = match item.kind {
                Kind::Builtin => "",
                Kind::File => "command",
                Kind::Skill => "skill",
            };
            let (name, hint) = label(item);
            let gap = column.saturating_sub(name.width() + hint.width()) + 2;
            let used = name.width() + hint.width() + gap + kind.width() + 1;
            let about = chat::cut(&item.description, inner_width.saturating_sub(used + 1));
            let pad = inner_width.saturating_sub(used + about.width());
            let mut line = Line::from(vec![
                Span::styled(name, Style::default().add_modifier(Modifier::BOLD)),
                Span::styled(hint, dim()),
                Span::raw(" ".repeat(gap)),
                Span::raw(about),
                Span::raw(" ".repeat(pad)),
                Span::styled(kind, dim()),
                Span::raw(" "),
            ]);
            if i == selected {
                line = line.style(Style::default().bg(Color::DarkGray).fg(Color::White));
            }
            line
        })
        .collect();
    let title = if items.len() > rows {
        format!(" {} of {} ", selected + 1, items.len())
    } else {
        String::new()
    };
    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(dim())
        .title(Line::styled(title, dim()).alignment(Alignment::Right));
    frame.render_widget(Clear, area);
    frame.render_widget(Paragraph::new(lines).block(block), area);
}

fn status_line(frame: &mut Frame, app: &App, area: Rect) {
    let f = &app.facts;
    let mut left = vec![Span::styled(
        format!(
            " {} · {} skills · {} MCP · {} tools",
            f.model, f.skills, f.servers, f.tools
        ),
        dim(),
    )];
    if let Some((text, warning)) = app.flashing() {
        let style = if warning {
            Style::default().fg(Color::Yellow)
        } else {
            Style::default()
        };
        left.push(Span::raw("  "));
        left.push(Span::styled(text.to_string(), style));
    }
    let total: f64 = app.costs.iter().map(|(_, cost, _)| cost).sum();
    let right = match app.running_for() {
        Some(running) => Line::from(vec![
            Span::styled(
                SPINNER[spinner(running)],
                Style::default().fg(Color::Yellow),
            ),
            Span::raw(format!(" {:.0}s ", running.as_secs_f32())),
            Span::styled("· Esc stops ", dim()),
        ]),
        None if !app.costs.is_empty() => Line::styled(
            format!("{} in all · /cost ", chat::cost_text(total, 0)),
            dim(),
        ),
        None => Line::styled("/help ", dim()),
    };
    let right_width = right.width() as u16;
    let [left_area, right_area] =
        Layout::horizontal([Constraint::Min(1), Constraint::Length(right_width)]).areas(area);
    frame.render_widget(Paragraph::new(Line::from(left)), left_area);
    frame.render_widget(
        Paragraph::new(right).alignment(Alignment::Right),
        right_area,
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::llm::Piece;
    use ratatui::Terminal;
    use ratatui::backend::TestBackend;

    fn screen(app: &mut App, width: u16, height: u16) -> String {
        let mut terminal = Terminal::new(TestBackend::new(width, height)).unwrap();
        terminal.draw(|f| draw(f, app)).unwrap();
        let buffer = terminal.backend().buffer().clone();
        (0..height)
            .map(|y| {
                (0..width)
                    .map(|x| buffer[(x, y)].symbol().to_string())
                    .collect::<String>()
                    .trim_end()
                    .to_string()
            })
            .collect::<Vec<_>>()
            .join("\n")
    }

    #[test]
    fn a_turn_on_the_screen() {
        let mut app = crate::tui::app::tests::app();
        app.start("/play Coltrane".into(), "p".into(), Some("play.md".into()));
        app.piece(Piece::Text("Looking **now**.".into()));
        app.piece(Piece::Tool(
            "play_music".into(),
            serde_json::json!({"query": "Coltrane"}),
        ));
        let running = screen(&mut app, 60, 14);
        assert!(running.contains("› /play Coltrane"), "{running}");
        assert!(running.contains("↳ play.md"), "{running}");
        assert!(running.contains("  Looking now."), "{running}");
        assert!(running.contains("play_music query=Coltrane"), "{running}");
        assert!(running.contains("Esc stops"), "{running}");
        assert!(
            running.contains("mock · 1 skills · 0 MCP · 3 tools"),
            "{running}"
        );

        app.piece(Piece::ToolDone("play_music".into(), true));
        app.finish(Ok("done".into()), vec![], "/play Coltrane");
        let done = screen(&mut app, 60, 14);
        assert!(done.contains("✓ play_music"), "{done}");
        assert!(done.contains("$0.0000 in all"), "{done}");
    }

    #[test]
    fn the_menu_shows_over_the_conversation() {
        let mut app = crate::tui::app::tests::app();
        for c in "/go".chars() {
            app.key(crossterm::event::KeyEvent::new(
                crossterm::event::KeyCode::Char(c),
                crossterm::event::KeyModifiers::NONE,
            ));
        }
        let shown = screen(&mut app, 60, 14);
        assert!(
            shown.contains("/goodnight [request]  does goodnight"),
            "{shown}"
        );
        assert!(shown.contains("› /go"), "{shown}");
    }

    #[test]
    fn long_input_wraps_and_the_cursor_follows() {
        let mut app = crate::tui::app::tests::app();
        app.input.set(&"x".repeat(25));
        let (rows, cursor) = input_rows(&app, 10);
        assert_eq!(rows, ["xxxxxxxxxx", "xxxxxxxxxx", "xxxxx"]);
        assert_eq!(cursor, (2, 5));
        app.input.set(&"y".repeat(10));
        let (rows, cursor) = input_rows(&app, 10);
        assert_eq!(rows, ["yyyyyyyyyy", ""]);
        assert_eq!(cursor, (1, 0));
    }

    #[test]
    fn scrolled_up_the_conversation_says_what_is_below() {
        let mut app = crate::tui::app::tests::app();
        for i in 0..40 {
            app.note(vec![format!("note {i}")]);
        }
        let bottom = screen(&mut app, 50, 12);
        assert!(bottom.contains("note 39"), "{bottom}");
        app.scroll = 10;
        let up = screen(&mut app, 50, 12);
        assert!(!up.contains("note 39"), "{up}");
        assert!(up.contains("lines below"), "{up}");
    }
}
