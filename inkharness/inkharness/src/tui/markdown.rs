//! A reply's markdown as terminal lines, wrapped to the width: headings, emphasis, code, lists,
//! quotes, links (their address after them, dimmed), rules and simple tables. Anything else
//! comes through as its text. `wrap` is shared with the rest of the screen.

use pulldown_cmark::{CodeBlockKind, Event, HeadingLevel, Options, Parser, Tag, TagEnd};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use unicode_width::{UnicodeWidthChar, UnicodeWidthStr};

/// Text in a style, before it is wrapped.
pub type Segment = (String, Style);

pub fn code_style() -> Style {
    Style::default().fg(Color::Cyan)
}

pub fn dim() -> Style {
    Style::default().fg(Color::DarkGray)
}

/// `markdown` as lines at most `width` columns wide.
pub fn render(markdown: &str, width: usize) -> Vec<Line<'static>> {
    let mut r = Renderer {
        width: width.max(10),
        ..Renderer::default()
    };
    let options =
        Options::ENABLE_TABLES | Options::ENABLE_STRIKETHROUGH | Options::ENABLE_TASKLISTS;
    for event in Parser::new_ext(markdown, options) {
        r.event(event);
    }
    r.flush();
    r.out
}

#[derive(Default)]
struct Renderer {
    width: usize,
    out: Vec<Line<'static>>,
    /// The block being gathered.
    segs: Vec<Segment>,
    /// Inline styles: each start pushes the style it adds to.
    styles: Vec<Style>,
    /// Open lists: the next number of a numbered one, None for bullets.
    lists: Vec<Option<u64>>,
    /// The current item's marker, for the first line of its text.
    marker: Option<String>,
    quotes: usize,
    /// In a code block: its text so far, and its language.
    code: Option<(String, String)>,
    /// Open links: the address, and where their text starts in `segs`.
    links: Vec<(String, usize)>,
    /// The cells of the table row being gathered.
    cells: Vec<Vec<Segment>>,
    in_head: bool,
}

impl Renderer {
    fn style(&self) -> Style {
        self.styles.last().copied().unwrap_or_default()
    }

    fn push_style(&mut self, add: Style) {
        let style = self.style().patch(add);
        self.styles.push(style);
    }

    fn text(&mut self, text: &str, style: Style) {
        if let Some((code, _)) = &mut self.code {
            code.push_str(text);
        } else {
            self.segs.push((text.to_string(), style));
        }
    }

    /// A blank line before a block, unless it opens the reply or sits in a list.
    fn gap(&mut self) {
        if !self.out.is_empty()
            && self.lists.is_empty()
            && self.out.last().is_some_and(|l| l.width() > 0)
        {
            self.out.push(Line::default());
        }
    }

    /// The prefixes of a block's first line and of the lines after it: the quotes, the list
    /// indent, and the item's marker once.
    fn prefixes(&mut self) -> (Vec<Span<'static>>, Vec<Span<'static>>) {
        let mut first = Vec::new();
        let mut rest = Vec::new();
        for _ in 0..self.quotes {
            first.push(Span::styled("▎ ", dim()));
            rest.push(Span::styled("▎ ", dim()));
        }
        if !self.lists.is_empty() {
            let indent = "  ".repeat(self.lists.len() - 1);
            match self.marker.take() {
                Some(marker) => {
                    let width = marker.width();
                    first.push(Span::raw(format!("{indent}{marker}")));
                    rest.push(Span::raw(format!("{indent}{}", " ".repeat(width))));
                }
                None => {
                    // A later paragraph of the same item lines up with its text.
                    let indent = format!("{indent}  ");
                    first.push(Span::raw(indent.clone()));
                    rest.push(Span::raw(indent));
                }
            }
        }
        (first, rest)
    }

    /// Wraps and writes out the block gathered so far.
    fn flush(&mut self) {
        if self.segs.iter().all(|(t, _)| t.trim().is_empty()) {
            self.segs.clear();
            return;
        }
        let segs = std::mem::take(&mut self.segs);
        let (first, rest) = self.prefixes();
        self.out.extend(wrap(&segs, self.width, &first, &rest));
    }

    fn event(&mut self, event: Event) {
        match event {
            Event::Start(tag) => self.start(tag),
            Event::End(tag) => self.end(tag),
            Event::Text(text) => {
                let style = self.style();
                self.text(&text, style);
            }
            Event::Code(code) => {
                let style = self.style().patch(code_style());
                self.text(&code, style);
            }
            Event::InlineMath(math) | Event::DisplayMath(math) => {
                let style = self.style().patch(code_style());
                self.text(&math, style);
            }
            Event::Html(html) | Event::InlineHtml(html) => {
                let style = self.style().patch(dim());
                self.text(html.trim_end_matches('\n'), style);
            }
            Event::SoftBreak => {
                let style = self.style();
                self.text(" ", style);
            }
            Event::HardBreak => self.text("\n", Style::default()),
            Event::Rule => {
                self.flush();
                self.gap();
                self.out
                    .push(Line::styled("─".repeat(self.width.min(60)), dim()));
            }
            Event::TaskListMarker(done) => {
                let style = self.style();
                self.text(if done { "[x] " } else { "[ ] " }, style);
            }
            Event::FootnoteReference(name) => {
                let style = self.style().patch(dim());
                self.text(&format!("[^{name}]"), style);
            }
        }
    }

    fn start(&mut self, tag: Tag) {
        match tag {
            Tag::Paragraph => {
                if self.lists.is_empty() {
                    self.flush();
                    self.gap();
                } else if !self.segs.is_empty() {
                    self.flush();
                }
            }
            Tag::Heading { level, .. } => {
                self.flush();
                self.gap();
                let style = match level {
                    HeadingLevel::H1 => {
                        Style::default().add_modifier(Modifier::BOLD | Modifier::UNDERLINED)
                    }
                    HeadingLevel::H2 => Style::default().add_modifier(Modifier::BOLD),
                    _ => Style::default().add_modifier(Modifier::BOLD | Modifier::ITALIC),
                };
                self.push_style(style);
            }
            Tag::BlockQuote(_) => {
                self.flush();
                self.gap();
                self.quotes += 1;
                self.push_style(Style::default().add_modifier(Modifier::ITALIC));
            }
            Tag::CodeBlock(kind) => {
                self.flush();
                self.gap();
                let lang = match kind {
                    CodeBlockKind::Fenced(lang) => lang.to_string(),
                    CodeBlockKind::Indented => String::new(),
                };
                self.code = Some((String::new(), lang));
            }
            Tag::List(first) => {
                self.flush();
                if self.lists.is_empty() {
                    self.gap();
                }
                self.lists.push(first);
            }
            Tag::Item => {
                self.flush();
                let marker = match self.lists.last_mut() {
                    Some(Some(n)) => {
                        let marker = format!("{n}. ");
                        *n += 1;
                        marker
                    }
                    _ => "• ".to_string(),
                };
                self.marker = Some(marker);
            }
            Tag::Emphasis => self.push_style(Style::default().add_modifier(Modifier::ITALIC)),
            Tag::Strong => self.push_style(Style::default().add_modifier(Modifier::BOLD)),
            Tag::Strikethrough => {
                self.push_style(Style::default().add_modifier(Modifier::CROSSED_OUT))
            }
            Tag::Link { dest_url, .. } => {
                self.links.push((dest_url.to_string(), self.segs.len()));
                self.push_style(
                    Style::default()
                        .fg(Color::Blue)
                        .add_modifier(Modifier::UNDERLINED),
                );
            }
            Tag::Image { dest_url, .. } => {
                self.links.push((dest_url.to_string(), self.segs.len()));
                self.push_style(dim());
                let style = self.style();
                self.text("[image: ", style);
            }
            Tag::Table(_) => {
                self.flush();
                self.gap();
            }
            Tag::TableHead => {
                self.in_head = true;
                self.cells.clear();
            }
            Tag::TableRow => self.cells.clear(),
            Tag::TableCell if self.in_head => {
                self.push_style(Style::default().add_modifier(Modifier::BOLD));
            }
            _ => {}
        }
    }

    fn end(&mut self, tag: TagEnd) {
        match tag {
            TagEnd::Paragraph => self.flush(),
            TagEnd::Heading(_) => {
                self.flush();
                self.styles.pop();
            }
            TagEnd::BlockQuote(_) => {
                self.flush();
                self.quotes = self.quotes.saturating_sub(1);
                self.styles.pop();
            }
            TagEnd::CodeBlock => {
                if let Some((code, lang)) = self.code.take() {
                    self.code_block(&code, &lang);
                }
            }
            TagEnd::List(_) => {
                self.flush();
                self.lists.pop();
                self.marker = None;
            }
            TagEnd::Item => self.flush(),
            TagEnd::Emphasis | TagEnd::Strong | TagEnd::Strikethrough => {
                self.styles.pop();
            }
            TagEnd::Link => {
                self.styles.pop();
                if let Some((url, from)) = self.links.pop() {
                    let text: String = self.segs[from..].iter().map(|(t, _)| t.as_str()).collect();
                    let plain = url.strip_prefix("mailto:").unwrap_or(&url);
                    if !url.is_empty() && text.trim() != plain {
                        self.segs.push((format!(" ({url})"), dim()));
                    }
                }
            }
            TagEnd::Image => {
                let style = self.style();
                self.text("]", style);
                self.styles.pop();
                if let Some((url, _)) = self.links.pop()
                    && !url.is_empty()
                {
                    self.segs.push((format!(" ({url})"), dim()));
                }
            }
            TagEnd::TableCell => {
                if self.in_head {
                    self.styles.pop();
                }
                self.cells.push(std::mem::take(&mut self.segs));
            }
            TagEnd::TableHead | TagEnd::TableRow => {
                let cells = std::mem::take(&mut self.cells);
                for (i, cell) in cells.into_iter().enumerate() {
                    if i > 0 {
                        self.segs.push((" │ ".into(), dim()));
                    }
                    self.segs.extend(cell);
                }
                self.flush();
                if self.in_head {
                    self.in_head = false;
                    self.out
                        .push(Line::styled("─".repeat(self.width.min(60)), dim()));
                }
            }
            _ => {}
        }
    }

    fn code_block(&mut self, code: &str, lang: &str) {
        let (first, _) = self.prefixes();
        let bar = |prefix: &[Span<'static>]| {
            let mut spans = prefix.to_vec();
            spans.push(Span::styled("│ ", dim()));
            spans
        };
        if !lang.is_empty() {
            let mut spans = first.clone();
            spans.push(Span::styled(format!("╭ {lang}"), dim()));
            self.out.push(Line::from(spans));
        }
        let room = self.width.saturating_sub(width_of(&first) + 2).max(4);
        for line in code.trim_end_matches('\n').split('\n') {
            for chunk in chunks(line, room) {
                let mut spans = bar(&first);
                spans.push(Span::styled(chunk, code_style()));
                self.out.push(Line::from(spans));
            }
        }
    }
}

fn width_of(spans: &[Span]) -> usize {
    spans.iter().map(|s| s.content.width()).sum()
}

/// `text` cut into pieces at most `room` columns wide (one, empty, for an empty line).
pub fn chunks(text: &str, room: usize) -> Vec<String> {
    let mut out = vec![String::new()];
    let mut used = 0;
    for c in text.chars() {
        let w = c.width().unwrap_or(0);
        if used + w > room && used > 0 {
            out.push(String::new());
            used = 0;
        }
        out.last_mut().unwrap().push(c);
        used += w;
    }
    out
}

/// Styled text wrapped at spaces to `width` columns, each line after `first` (the first line)
/// or `rest` (the others). A `\n` starts a new line; a word too long for a line is cut.
pub fn wrap(
    segs: &[Segment],
    width: usize,
    first: &[Span<'static>],
    rest: &[Span<'static>],
) -> Vec<Line<'static>> {
    let mut lines = Vec::new();
    let mut spans: Vec<Span<'static>> = first.to_vec();
    let mut prefix = width_of(first);
    let mut used = prefix;
    let mut empty = true;
    let new_line = |lines: &mut Vec<Line<'static>>, spans: &mut Vec<Span<'static>>| {
        lines.push(Line::from(std::mem::take(spans)));
        spans.extend(rest.iter().cloned());
    };
    let add = |spans: &mut Vec<Span<'static>>, text: &str, style: Style| match spans.last_mut() {
        Some(last) if last.style == style && !text.is_empty() => {
            last.content = format!("{}{text}", last.content).into();
        }
        _ => spans.push(Span::styled(text.to_string(), style)),
    };
    for (text, style) in segs {
        for (i, part) in text.split('\n').enumerate() {
            if i > 0 {
                new_line(&mut lines, &mut spans);
                prefix = width_of(rest);
                used = prefix;
                empty = true;
            }
            for word in part.split_inclusive(' ') {
                let bare = word.trim_end_matches(' ');
                let w = bare.width();
                if !empty && used + w > width {
                    new_line(&mut lines, &mut spans);
                    prefix = width_of(rest);
                    used = prefix;
                    empty = true;
                }
                let room = width.saturating_sub(prefix).max(1);
                if w > room {
                    // Longer than a whole line: cut it.
                    for chunk in chunks(bare, room) {
                        if !empty && used + chunk.width() > width {
                            new_line(&mut lines, &mut spans);
                            used = prefix;
                        }
                        used += chunk.width();
                        add(&mut spans, &chunk, *style);
                        empty = false;
                    }
                } else if !bare.is_empty() {
                    used += w;
                    add(&mut spans, bare, *style);
                    empty = false;
                }
                if word.len() > bare.len() && !empty && used < width {
                    used += 1;
                    add(&mut spans, " ", *style);
                }
            }
        }
    }
    if !empty || lines.is_empty() {
        lines.push(Line::from(spans));
    }
    // Spaces left at the ends of lines.
    for line in &mut lines {
        if let Some(last) = line.spans.last_mut() {
            let trimmed = last.content.trim_end().to_string();
            last.content = trimmed.into();
        }
    }
    lines
}

#[cfg(test)]
mod tests {
    use super::*;

    fn text(lines: &[Line]) -> Vec<String> {
        lines
            .iter()
            .map(|l| l.spans.iter().map(|s| s.content.as_ref()).collect())
            .collect()
    }

    #[test]
    fn blocks_and_inline_styles() {
        let md = "# Title\n\nSome **bold** and `code`.\n\n- one\n- two\n  1. nested\n\n```rust\nlet x = 1;\n```\n\n> quoted\n\nSee [the docs](https://example.com).\n\n---\n";
        let lines = render(md, 40);
        assert_eq!(
            text(&lines),
            [
                "Title",
                "",
                "Some bold and code.",
                "",
                "• one",
                "• two",
                "  1. nested",
                "",
                "╭ rust",
                "│ let x = 1;",
                "",
                "▎ quoted",
                "",
                "See the docs (https://example.com).",
                "",
                "────────────────────────────────────────",
            ]
        );
        assert!(
            lines[0].spans[0]
                .style
                .add_modifier
                .contains(Modifier::BOLD)
        );
        let bold = lines[2].spans.iter().find(|s| s.content == "bold").unwrap();
        assert!(bold.style.add_modifier.contains(Modifier::BOLD));
        let code = lines[2].spans.iter().find(|s| s.content == "code").unwrap();
        assert_eq!(code.style.fg, Some(Color::Cyan));
    }

    #[test]
    fn lines_wrap_to_the_width() {
        let lines = render(
            "- a list item long enough that it has to wrap over lines",
            20,
        );
        assert_eq!(
            text(&lines),
            [
                "• a list item long",
                "  enough that it has",
                "  to wrap over lines"
            ]
        );
        for line in &lines {
            assert!(line.width() <= 20);
        }
        // A word longer than a line is cut.
        let lines = render(&"x".repeat(25), 10);
        assert_eq!(text(&lines), ["xxxxxxxxxx", "xxxxxxxxxx", "xxxxx"]);
        // A link that is its own address isn't repeated; a table's cells are divided.
        let lines = render("<https://a.b>\n\n| a | b |\n|---|---|\n| 1 | 2 |\n", 30);
        assert_eq!(text(&lines)[0], "https://a.b");
        assert!(
            text(&lines).contains(&"1 │ 2".to_string()),
            "{:?}",
            text(&lines)
        );
    }
}
