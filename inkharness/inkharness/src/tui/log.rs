//! Where the log goes while the full-screen chat is up: stderr would scribble over the screen.
//! The last LINES lines are kept for `/log`, and each warning or error waits to be shown once in
//! the status line.

use std::collections::VecDeque;
use std::io::Write;
use std::sync::{Arc, Mutex};

use tracing::{Level, Metadata};
use tracing_subscriber::fmt::MakeWriter;

/// Log lines kept.
const LINES: usize = 500;

#[derive(Clone, Default)]
pub struct Log(Arc<Mutex<Kept>>);

#[derive(Default)]
struct Kept {
    lines: VecDeque<String>,
    /// The newest warning or error not shown yet.
    unseen: Option<String>,
}

impl Log {
    /// The lines kept, oldest first.
    pub fn lines(&self) -> Vec<String> {
        self.0.lock().unwrap().lines.iter().cloned().collect()
    }

    /// The newest warning or error since the last call, if any.
    pub fn take_warning(&self) -> Option<String> {
        self.0.lock().unwrap().unseen.take()
    }

    fn push(&self, text: &str, warning: bool) {
        let mut kept = self.0.lock().unwrap();
        for line in text.lines().filter(|l| !l.trim().is_empty()) {
            if kept.lines.len() == LINES {
                kept.lines.pop_front();
            }
            kept.lines.push_back(line.to_string());
            if warning {
                kept.unseen = Some(line.trim().to_string());
            }
        }
    }
}

/// One event's text, kept when it's dropped.
pub struct Writer {
    log: Log,
    warning: bool,
    text: Vec<u8>,
}

impl Write for Writer {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        self.text.extend_from_slice(buf);
        Ok(buf.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl Drop for Writer {
    fn drop(&mut self) {
        self.log
            .push(&String::from_utf8_lossy(&self.text), self.warning);
    }
}

impl<'a> MakeWriter<'a> for Log {
    type Writer = Writer;

    fn make_writer(&'a self) -> Writer {
        Writer {
            log: self.clone(),
            warning: false,
            text: Vec::new(),
        }
    }

    fn make_writer_for(&'a self, meta: &Metadata<'_>) -> Writer {
        Writer {
            log: self.clone(),
            warning: *meta.level() <= Level::WARN,
            text: Vec::new(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lines_are_kept_and_warnings_wait() {
        let log = Log::default();
        for i in 0..LINES + 3 {
            writeln!(log.make_writer(), "line {i}").unwrap();
        }
        let lines = log.lines();
        assert_eq!(lines.len(), LINES);
        assert_eq!(lines[0], "line 3");
        assert_eq!(log.take_warning(), None);
        {
            let mut w = log.make_writer();
            w.warning = true;
            w.write_all(b" WARN the server left\n").unwrap();
        }
        assert_eq!(log.take_warning().as_deref(), Some("WARN the server left"));
        assert_eq!(log.take_warning(), None);
    }
}
