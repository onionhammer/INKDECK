//! The text being typed: lines of it, a cursor, and the inputs sent before (Up and Down go
//! through them from the first and last line). The cursor's column counts characters.

/// Earlier inputs kept for Up.
const HISTORY: usize = 200;

#[derive(Debug)]
pub struct Input {
    lines: Vec<String>,
    row: usize,
    col: usize,
    sent: Vec<String>,
    /// Which earlier input is showing, and what was being typed before Up.
    browsing: Option<(usize, String)>,
}

impl Default for Input {
    fn default() -> Self {
        Input {
            lines: vec![String::new()],
            row: 0,
            col: 0,
            sent: Vec::new(),
            browsing: None,
        }
    }
}

impl Input {
    pub fn text(&self) -> String {
        self.lines.join("\n")
    }

    pub fn lines(&self) -> &[String] {
        &self.lines
    }

    pub fn is_empty(&self) -> bool {
        self.lines.len() == 1 && self.lines[0].is_empty()
    }

    /// The cursor: its line, and its column in characters.
    pub fn cursor(&self) -> (usize, usize) {
        (self.row, self.col)
    }

    /// Replaces the text, the cursor at its end.
    pub fn set(&mut self, text: &str) {
        self.lines = text.split('\n').map(str::to_string).collect();
        self.row = self.lines.len() - 1;
        self.col = self.lines[self.row].chars().count();
        self.browsing = None;
    }

    /// The text, now sent: kept for Up, and the input emptied.
    pub fn take(&mut self) -> String {
        let text = self.text();
        if !text.trim().is_empty() && self.sent.last() != Some(&text) {
            self.sent.push(text.clone());
            if self.sent.len() > HISTORY {
                self.sent.remove(0);
            }
        }
        self.set("");
        text
    }

    fn line(&mut self) -> &mut String {
        &mut self.lines[self.row]
    }

    /// The byte offset of character `col` in the current line.
    fn byte(&self, col: usize) -> usize {
        let line = &self.lines[self.row];
        line.char_indices().nth(col).map_or(line.len(), |(i, _)| i)
    }

    fn len(&self) -> usize {
        self.lines[self.row].chars().count()
    }

    pub fn insert(&mut self, c: char) {
        if c == '\n' {
            return self.newline();
        }
        let at = self.byte(self.col);
        self.line().insert(at, c);
        self.col += 1;
        self.browsing = None;
    }

    /// Pasted text: its line ends as newlines, other control characters dropped.
    pub fn paste(&mut self, text: &str) {
        let text = text.replace("\r\n", "\n").replace('\r', "\n");
        for c in text.chars() {
            if c == '\n' || c == '\t' || !c.is_control() {
                self.insert(if c == '\t' { ' ' } else { c });
            }
        }
    }

    pub fn newline(&mut self) {
        let at = self.byte(self.col);
        let rest = self.line().split_off(at);
        self.row += 1;
        self.lines.insert(self.row, rest);
        self.col = 0;
        self.browsing = None;
    }

    pub fn backspace(&mut self) {
        if self.col > 0 {
            let (from, to) = (self.byte(self.col - 1), self.byte(self.col));
            self.line().replace_range(from..to, "");
            self.col -= 1;
        } else if self.row > 0 {
            let line = self.lines.remove(self.row);
            self.row -= 1;
            self.col = self.len();
            self.line().push_str(&line);
        }
        self.browsing = None;
    }

    pub fn delete(&mut self) {
        if self.col < self.len() {
            let (from, to) = (self.byte(self.col), self.byte(self.col + 1));
            self.line().replace_range(from..to, "");
        } else if self.row + 1 < self.lines.len() {
            let next = self.lines.remove(self.row + 1);
            self.line().push_str(&next);
        }
        self.browsing = None;
    }

    pub fn left(&mut self) {
        if self.col > 0 {
            self.col -= 1;
        } else if self.row > 0 {
            self.row -= 1;
            self.col = self.len();
        }
    }

    pub fn right(&mut self) {
        if self.col < self.len() {
            self.col += 1;
        } else if self.row + 1 < self.lines.len() {
            self.row += 1;
            self.col = 0;
        }
    }

    fn chars(&self) -> Vec<char> {
        self.lines[self.row].chars().collect()
    }

    /// The start of the word before the cursor.
    fn word_start(&self) -> usize {
        let chars = self.chars();
        let mut col = self.col;
        while col > 0 && chars[col - 1].is_whitespace() {
            col -= 1;
        }
        while col > 0 && !chars[col - 1].is_whitespace() {
            col -= 1;
        }
        col
    }

    pub fn word_left(&mut self) {
        if self.col == 0 {
            return self.left();
        }
        self.col = self.word_start();
    }

    pub fn word_right(&mut self) {
        let chars = self.chars();
        if self.col == chars.len() {
            return self.right();
        }
        let mut col = self.col;
        while col < chars.len() && chars[col].is_whitespace() {
            col += 1;
        }
        while col < chars.len() && !chars[col].is_whitespace() {
            col += 1;
        }
        self.col = col;
    }

    pub fn home(&mut self) {
        self.col = 0;
    }

    pub fn end(&mut self) {
        self.col = self.len();
    }

    /// Ctrl-W: the word before the cursor.
    pub fn delete_word(&mut self) {
        if self.col == 0 {
            return self.backspace();
        }
        let start = self.word_start();
        let (from, to) = (self.byte(start), self.byte(self.col));
        self.line().replace_range(from..to, "");
        self.col = start;
        self.browsing = None;
    }

    /// Ctrl-U: the line up to the cursor.
    pub fn kill_back(&mut self) {
        let to = self.byte(self.col);
        self.line().replace_range(..to, "");
        self.col = 0;
        self.browsing = None;
    }

    /// Ctrl-K: the line after the cursor.
    pub fn kill_forward(&mut self) {
        let from = self.byte(self.col);
        self.line().truncate(from);
        self.browsing = None;
    }

    /// Up: the line above, or from the first line the input sent before. False if there was
    /// nowhere to go.
    pub fn up(&mut self) -> bool {
        if self.row > 0 {
            self.row -= 1;
            self.col = self.col.min(self.len());
            return true;
        }
        let next = match &self.browsing {
            Some((i, _)) if *i > 0 => *i - 1,
            Some(_) => return false,
            None if self.sent.is_empty() => return false,
            None => self.sent.len() - 1,
        };
        let draft = match self.browsing.take() {
            Some((_, draft)) => draft,
            None => self.text(),
        };
        self.set(&self.sent[next].clone());
        self.browsing = Some((next, draft));
        true
    }

    /// Down: the line below, or from the last line the next input sent (and then what was
    /// being typed).
    pub fn down(&mut self) -> bool {
        if self.row + 1 < self.lines.len() {
            self.row += 1;
            self.col = self.col.min(self.len());
            return true;
        }
        let Some((i, draft)) = self.browsing.take() else {
            return false;
        };
        if i + 1 < self.sent.len() {
            self.set(&self.sent[i + 1].clone());
            self.browsing = Some((i + 1, draft));
        } else {
            self.set(&draft);
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn typed(text: &str) -> Input {
        let mut input = Input::default();
        for c in text.chars() {
            input.insert(c);
        }
        input
    }

    #[test]
    fn editing() {
        let mut input = typed("héllo world");
        assert_eq!(input.cursor(), (0, 11));
        input.word_left();
        assert_eq!(input.cursor(), (0, 6));
        input.backspace();
        assert_eq!(input.text(), "hélloworld");
        input.home();
        input.delete();
        assert_eq!(input.text(), "élloworld");
        input.end();
        input.delete_word();
        assert_eq!(input.text(), "");
        assert!(input.is_empty());

        let mut input = typed("one two");
        input.word_left();
        input.newline();
        assert_eq!(input.lines(), ["one ", "two"]);
        assert_eq!(input.cursor(), (1, 0));
        input.backspace();
        assert_eq!(input.text(), "one two");
        input.kill_forward();
        assert_eq!(input.text(), "one ");
        input.kill_back();
        assert!(input.is_empty());

        let mut input = Input::default();
        input.paste("a\r\nb\tc\u{7}");
        assert_eq!(input.lines(), ["a", "b c"]);
    }

    #[test]
    fn up_and_down_go_through_lines_then_what_was_sent() {
        let mut input = typed("first");
        assert_eq!(input.take(), "first");
        input.paste("second\nlines");
        input.take();
        input.paste("draft");
        assert!(input.up());
        assert_eq!(input.text(), "second\nlines");
        // In a sent input of two lines, Up moves up a line first.
        assert!(input.up());
        assert_eq!(input.cursor().0, 0);
        assert!(input.up());
        assert_eq!(input.text(), "first");
        assert!(!input.up());
        assert!(input.down());
        assert_eq!(input.text(), "second\nlines");
        assert!(input.down());
        assert_eq!(input.text(), "draft");
        assert!(!input.down());
    }
}
