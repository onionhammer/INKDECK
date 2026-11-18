//! Cuts streamed LLM text into sentences, so speech synthesis can start on the first one
//! while the rest is still being generated.

/// A sentence shorter than this waits for the next one ("Sure." alone makes a choppy clip).
const MIN_CHARS: usize = 12;

#[derive(Default)]
pub struct Splitter {
    pending: String,
}

impl Splitter {
    pub fn new() -> Self {
        Self::default()
    }

    /// Adds text; returns the sentences it completed.
    pub fn push(&mut self, delta: &str) -> Vec<String> {
        self.pending.push_str(delta);
        let mut out = Vec::new();
        let mut from = 0; // where the search for the next boundary starts
        while let Some(end) = boundary(&self.pending, from) {
            if self.pending[..end].trim().chars().count() < MIN_CHARS {
                from = end;
                continue;
            }
            let rest = self.pending.split_off(end);
            let sentence = std::mem::replace(&mut self.pending, rest);
            out.push(sentence.trim().to_string());
            from = 0;
        }
        out
    }

    /// What is left when the text ends.
    pub fn finish(&mut self) -> Option<String> {
        let rest = std::mem::take(&mut self.pending);
        let rest = rest.trim();
        (!rest.is_empty()).then(|| rest.to_string())
    }
}

/// Byte offset just past the first sentence end at or after `from`: a newline, or . ! ? followed
/// by whitespace. A terminator at the very end of the text doesn't count yet ("3." may become
/// "3.5").
fn boundary(text: &str, from: usize) -> Option<usize> {
    let mut chars = text[from..].char_indices().peekable();
    while let Some((i, c)) = chars.next() {
        let after = from + i + c.len_utf8();
        if c == '\n' {
            return Some(after);
        }
        if matches!(c, '.' | '!' | '?') && chars.peek().is_some_and(|(_, n)| n.is_whitespace()) {
            return Some(after);
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(deltas: &[&str]) -> Vec<String> {
        let mut splitter = Splitter::new();
        let mut out = Vec::new();
        for d in deltas {
            out.extend(splitter.push(d));
        }
        out.extend(splitter.finish());
        out
    }

    #[test]
    fn splits_at_sentence_ends() {
        assert_eq!(
            run(&[
                "Playing jazz in the kit",
                "chen. Anything else you ",
                "need? Bye"
            ]),
            [
                "Playing jazz in the kitchen.",
                "Anything else you need?",
                "Bye"
            ]
        );
    }

    #[test]
    fn short_sentences_join_the_next() {
        assert_eq!(
            run(&["Sure. I turned the lights off. Done"]),
            ["Sure. I turned the lights off.", "Done"]
        );
    }

    #[test]
    fn decimals_and_trailing_dots_wait() {
        assert_eq!(
            run(&["It is 21.", "5 degrees outside. "]),
            ["It is 21.5 degrees outside."]
        );
    }

    #[test]
    fn newline_is_a_boundary() {
        assert_eq!(
            run(&["Let me check that\nIt is on"]),
            ["Let me check that", "It is on"]
        );
    }
}
