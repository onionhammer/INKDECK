//! Slash commands ([commands] in inkharness.toml, and the folders place.rs finds): prompts kept
//! as markdown files and typed as `/name arguments` in a text turn (the terminal, HTTP). Spoken
//! turns never expand them.
//!
//! A command is `<name>.md`, as Claude Code writes them: an optional frontmatter block with
//! `description` and `argument-hint` (other keys, such as `allowed-tools` and `model`, are
//! read and ignored), then the prompt. `$ARGUMENTS` in the prompt is what followed the name,
//! and `$1` to `$9` its words ("quoted words" count as one); a prompt with neither gets the
//! arguments after it. A file in a subfolder is namespaced: `frontend/component.md` is
//! `/frontend:component`. Claude Code's `` !`shell` `` and `@file` stay as they are written:
//! a folder's files don't run commands.
//!
//! Every skill is a command too: `/goodnight` puts the goodnight skill's instructions in
//! the prompt with the request, which saves the model a load_skill round. A command file wins
//! over a skill of the same name.
//!
//! The folders are scanned at startup, and a command's file is read again each time it's used,
//! so editing one needs no restart; adding one does. The built-in commands (BUILTINS) belong
//! to the terminal, and a file can't take their names.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use crate::skills::{self, Skills};

/// Command files beyond this many are left out.
const MAX_COMMANDS: usize = 100;
/// A command file over this size is left out: all of it goes into the turn.
const MAX_FILE: u64 = 64 * 1024;
/// How deep the namespaces go below a commands folder.
const MAX_DEPTH: usize = 3;

/// A command of the terminal's own.
pub struct Builtin {
    pub name: &'static str,
    /// What it takes, as the menu shows it ("" for nothing).
    pub args: &'static str,
    pub about: &'static str,
}

/// The terminal's own commands, in the order `/help` lists them. `/reset` and `/exit` are
/// other names for `/clear` and `/quit`.
pub const BUILTINS: &[Builtin] = &[
    Builtin {
        name: "help",
        args: "",
        about: "keys, commands and skills",
    },
    Builtin {
        name: "clear",
        args: "",
        about: "start a new conversation",
    },
    Builtin {
        name: "reset",
        args: "",
        about: "the same as /clear",
    },
    Builtin {
        name: "tools",
        args: "",
        about: "the tools the model has",
    },
    Builtin {
        name: "skills",
        args: "",
        about: "the skills, each also a /command",
    },
    Builtin {
        name: "mcp",
        args: "",
        about: "the MCP servers and their tools",
    },
    Builtin {
        name: "cost",
        args: "",
        about: "what each turn cost, and in all",
    },
    Builtin {
        name: "copy",
        args: "",
        about: "copy the last reply",
    },
    Builtin {
        name: "save",
        args: "[file]",
        about: "save the conversation as markdown",
    },
    Builtin {
        name: "log",
        args: "",
        about: "recent log lines",
    },
    Builtin {
        name: "quit",
        args: "",
        about: "leave",
    },
    Builtin {
        name: "exit",
        args: "",
        about: "the same as /quit",
    },
];

pub fn builtin(name: &str) -> Option<&'static Builtin> {
    BUILTINS.iter().find(|b| b.name == name)
}

struct Command {
    name: String,
    description: String,
    hint: String,
    file: PathBuf,
    /// The file as the folder names it ("music/play.md"), to say where a prompt came from.
    shown: String,
}

/// The command files and the skills, as slash commands.
pub struct Commands {
    commands: Vec<Command>,
    skills: Arc<Skills>,
}

/// What a command is, for the menu.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Builtin,
    File,
    Skill,
}

/// One line of the menu or of /help.
#[derive(Debug, Clone, PartialEq)]
pub struct Listed {
    pub name: String,
    pub description: String,
    pub hint: String,
    pub kind: Kind,
}

/// A command typed, as the model gets it.
#[derive(Debug, Clone, PartialEq)]
pub struct Expanded {
    pub name: String,
    pub prompt: String,
    /// Where the prompt came from: the command's file, or "the goodnight skill".
    pub source: String,
}

impl Commands {
    /// Scans `dirs`, each holding command files and folders of them. Problems are logged and
    /// leave that command out.
    pub fn load(dirs: &[PathBuf], skills: Arc<Skills>) -> Commands {
        let mut commands = Commands {
            commands: Vec::new(),
            skills,
        };
        for dir in dirs {
            let mut files = Vec::new();
            walk(dir, 0, &mut files);
            files.sort();
            for file in files {
                match load_command(dir, &file) {
                    Ok(command) => commands.add(command),
                    Err(e) => {
                        tracing::warn!(command = %file.display(), "command left out: {e}")
                    }
                }
            }
        }
        if !commands.commands.is_empty() {
            let names: Vec<&str> = commands.commands.iter().map(|c| c.name.as_str()).collect();
            tracing::info!(commands = %names.join(", "), "commands loaded");
        }
        commands
    }

    fn add(&mut self, command: Command) {
        if builtin(&command.name).is_some() {
            tracing::warn!(command = %command.file.display(), "/{} is built in; left out", command.name);
        } else if self.commands.iter().any(|c| c.name == command.name) {
            tracing::warn!(command = %command.file.display(), "a command named {} is already loaded; keeping the first", command.name);
        } else if self.commands.len() >= MAX_COMMANDS {
            tracing::warn!(command = %command.name, "more than {MAX_COMMANDS} commands; left out");
        } else {
            self.commands.push(command);
        }
    }

    /// The command files' names.
    pub fn names(&self) -> Vec<&str> {
        self.commands.iter().map(|c| c.name.as_str()).collect()
    }

    /// The built-ins, the command files, then the skills a file doesn't shadow.
    pub fn list(&self) -> Vec<Listed> {
        let mut all: Vec<Listed> = BUILTINS
            .iter()
            .map(|b| Listed {
                name: b.name.into(),
                description: b.about.into(),
                hint: b.args.into(),
                kind: Kind::Builtin,
            })
            .collect();
        all.extend(self.commands.iter().map(|c| Listed {
            name: c.name.clone(),
            description: c.description.clone(),
            hint: c.hint.clone(),
            kind: Kind::File,
        }));
        for (name, description) in self.skills.described() {
            if !all.iter().any(|l| l.name == name) {
                all.push(Listed {
                    name: name.into(),
                    description: description.into(),
                    hint: "[request]".into(),
                    kind: Kind::Skill,
                });
            }
        }
        all
    }

    /// `text` as the model gets it, if it starts with a command file's or a skill's name: None
    /// for anything else (a built-in, an unknown name, a path such as /etc/hosts, plain text),
    /// which goes to the model as written. Err: the command's file can't be read now.
    pub fn expand(&self, text: &str) -> Option<Result<Expanded, String>> {
        let (name, args) = split(text)?;
        if let Some(command) = self.commands.iter().find(|c| c.name == name) {
            return Some(read_body(&command.file).map(|body| Expanded {
                name: name.to_string(),
                prompt: fill(&body, args),
                source: command.shown.clone(),
            }));
        }
        if !self.skills.names().contains(&name) {
            return None;
        }
        Some(self.skills.instructions(name).map(|body| {
            let mut prompt = format!("The user ran /{name}. Follow the {name} skill:\n\n{body}");
            if !args.is_empty() {
                prompt.push_str(&format!("\n\nTheir request: {args}"));
            }
            Expanded {
                name: name.to_string(),
                prompt,
                source: format!("the {name} skill"),
            }
        }))
    }
}

/// `/name rest` → the name and the rest, trimmed. None unless `text` starts with a slash and a
/// name.
pub fn split(text: &str) -> Option<(&str, &str)> {
    let rest = text.trim_start().strip_prefix('/')?;
    let end = rest.find(char::is_whitespace).unwrap_or(rest.len());
    let name = &rest[..end];
    if name.is_empty() || name.starts_with('/') {
        return None;
    }
    Some((name, rest[end..].trim()))
}

/// The `.md` files under `dir`, hidden ones left out.
fn walk(dir: &Path, depth: usize, out: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        if depth == 0 {
            tracing::warn!(dir = %dir.display(), "commands folder can't be read");
        }
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path
            .file_name()
            .is_some_and(|n| n.to_string_lossy().starts_with('.'))
        {
            continue;
        }
        if path.is_dir() {
            if depth + 1 < MAX_DEPTH {
                walk(&path, depth + 1, out);
            }
        } else if path.extension().is_some_and(|e| e == "md") {
            out.push(path);
        }
    }
}

/// Reads and checks one command file under `dir`.
fn load_command(dir: &Path, file: &Path) -> Result<Command, String> {
    let relative = file.strip_prefix(dir).map_err(|_| "outside its folder")?;
    let shown = relative.to_string_lossy().replace('\\', "/");
    let parts: Vec<String> = relative
        .with_extension("")
        .components()
        .map(|c| c.as_os_str().to_string_lossy().into_owned())
        .collect();
    for part in &parts {
        skills::check_name(part)?;
    }
    let name = parts.join(":");
    let size = std::fs::metadata(file)
        .map_err(|e| format!("can't read it: {e}"))?
        .len();
    if size > MAX_FILE {
        return Err(format!("over {} KB", MAX_FILE / 1024));
    }
    let text = std::fs::read_to_string(file).map_err(|e| format!("can't read it: {e}"))?;
    let (fields, body) = parts_of(&text)?;
    if body.trim().is_empty() {
        return Err("no prompt".into());
    }
    for key in fields.keys() {
        if !matches!(key.as_str(), "description" | "argument-hint") {
            tracing::debug!(command = %name, "{key} is for Claude Code; ignored");
        }
    }
    let description = fields
        .get("description")
        .map(|d| d.split_whitespace().collect::<Vec<_>>().join(" "))
        .filter(|d| !d.is_empty())
        .unwrap_or_else(|| {
            let first = body.lines().find(|l| !l.trim().is_empty()).unwrap_or("");
            first.trim().trim_start_matches('#').trim().to_string()
        });
    Ok(Command {
        name,
        description,
        hint: fields.get("argument-hint").cloned().unwrap_or_default(),
        file: file.to_path_buf(),
        shown,
    })
}

/// The frontmatter's fields, if it has any, and the prompt.
fn parts_of(text: &str) -> Result<(std::collections::HashMap<String, String>, &str), String> {
    let starts = text
        .strip_prefix('\u{feff}')
        .unwrap_or(text)
        .lines()
        .next()
        .is_some_and(|l| l.trim_end() == "---");
    if starts {
        skills::frontmatter(text)
    } else {
        Ok((Default::default(), text))
    }
}

/// A command's prompt, read now.
fn read_body(file: &Path) -> Result<String, String> {
    let text =
        std::fs::read_to_string(file).map_err(|e| format!("can't read {}: {e}", file.display()))?;
    let (_, body) = parts_of(&text)?;
    Ok(body.trim().to_string())
}

/// The prompt with `$ARGUMENTS` and `$1`…`$9` filled in, or the arguments after it if it has
/// neither.
fn fill(body: &str, args: &str) -> String {
    let words = words(args);
    let mut out = String::with_capacity(body.len() + args.len());
    let mut used = false;
    let mut rest = body;
    while let Some(i) = rest.find('$') {
        out.push_str(&rest[..i]);
        let after = &rest[i + 1..];
        if let Some(tail) = after.strip_prefix("ARGUMENTS") {
            out.push_str(args);
            used = true;
            rest = tail;
            continue;
        }
        let mut chars = after.chars();
        match (chars.next(), chars.next()) {
            (Some(d @ '1'..='9'), next) if !next.is_some_and(|c| c.is_ascii_digit()) => {
                let n = d as usize - '1' as usize;
                out.push_str(words.get(n).map_or("", String::as_str));
                used = true;
                rest = &after[1..];
            }
            _ => {
                out.push('$');
                rest = after;
            }
        }
    }
    out.push_str(rest);
    if !used && !args.is_empty() {
        out.push_str(&format!("\n\nARGUMENTS: {args}"));
    }
    out
}

/// The words of `args`: split at spaces, with "double quotes" holding a word together.
fn words(args: &str) -> Vec<String> {
    let (mut words, mut word, mut quoted) = (Vec::new(), String::new(), false);
    for c in args.chars() {
        match c {
            '"' => quoted = !quoted,
            c if c.is_whitespace() && !quoted => {
                if !word.is_empty() {
                    words.push(std::mem::take(&mut word));
                }
            }
            c => word.push(c),
        }
    }
    if !word.is_empty() {
        words.push(word);
    }
    words
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::skills::tests::{scratch, write};

    fn goodnight() -> Arc<Skills> {
        let dir = scratch("commands-skills");
        write(
            &dir.join("goodnight/SKILL.md"),
            "---\nname: goodnight\ndescription: Bedtime routine.\n---\nPause the speakers.",
        );
        write(
            &dir.join("play/SKILL.md"),
            "---\nname: play\ndescription: The skill version.\n---\nShadowed.",
        );
        Arc::new(Skills::load(&[dir]))
    }

    #[test]
    fn files_load_from_each_folder() {
        let here = scratch("commands-here");
        let global = scratch("commands-global");
        write(
            &here.join("play.md"),
            "---\ndescription: Play music\nargument-hint: <album>\nallowed-tools: Bash\nmodel: haiku\n---\nPlay this: $ARGUMENTS",
        );
        write(&here.join("plain.md"), "# Say hello\nHello there.");
        write(&here.join("frontend/component.md"), "Make $1 in $2.");
        write(&here.join("Bad_Name.md"), "x");
        write(&here.join("empty.md"), "---\ndescription: d\n---\n\n");
        write(&here.join("help.md"), "Not the built-in.");
        write(&here.join("notes.txt"), "not a command");
        write(&here.join(".hidden.md"), "x");
        write(&here.join("big.md"), &"x".repeat(MAX_FILE as usize + 1));
        // The same name in a later folder: the first is kept.
        write(&global.join("play.md"), "The global one.");
        write(&global.join("only-global.md"), "From the global folder.");

        let commands = Commands::load(&[here.clone(), global, here.join("missing")], goodnight());
        assert_eq!(
            commands.names(),
            ["frontend:component", "plain", "play", "only-global"]
        );
        let list = commands.list();
        let add = list.iter().find(|l| l.name == "play").unwrap();
        assert_eq!(add.description, "Play music");
        assert_eq!(add.hint, "<album>");
        assert_eq!(add.kind, Kind::File);
        let plain = list.iter().find(|l| l.name == "plain").unwrap();
        assert_eq!(plain.description, "Say hello");
        // The built-ins come first; a skill a file shadows isn't listed twice.
        assert_eq!(list[0].kind, Kind::Builtin);
        assert_eq!(list.iter().filter(|l| l.name == "play").count(), 1);
        let skill = list.iter().find(|l| l.name == "goodnight").unwrap();
        assert_eq!(skill.kind, Kind::Skill);
    }

    #[test]
    fn commands_expand() {
        let dir = scratch("commands-expand");
        write(
            &dir.join("play.md"),
            "---\ndescription: Play music\n---\nPlay this: $ARGUMENTS\n\nCheck $1 first. Costs $5, not $10.",
        );
        write(&dir.join("frontend/component.md"), "Make $1 in $2.");
        write(&dir.join("bare.md"), "Do the thing.");
        let commands = Commands::load(std::slice::from_ref(&dir), goodnight());
        let expand = |text: &str| commands.expand(text).unwrap().unwrap();

        let e = expand("/play Miles Davis");
        assert_eq!(e.name, "play");
        assert_eq!(e.source, "play.md");
        assert_eq!(
            e.prompt,
            "Play this: Miles Davis\n\nCheck Miles first. Costs , not $10."
        );
        assert_eq!(
            expand("/frontend:component \"Nav bar\" header").prompt,
            "Make Nav bar in header."
        );
        assert_eq!(expand("/bare").prompt, "Do the thing.");
        assert_eq!(
            expand("  /bare now please").prompt,
            "Do the thing.\n\nARGUMENTS: now please"
        );
        // A file is read again when it's used.
        write(&dir.join("bare.md"), "Do the other thing.");
        assert_eq!(expand("/bare").prompt, "Do the other thing.");

        // A skill: its instructions, then the request.
        let e = expand("/goodnight not the bedroom");
        assert_eq!(e.source, "the goodnight skill");
        assert_eq!(
            e.prompt,
            "The user ran /goodnight. Follow the goodnight skill:\n\nPause the speakers.\n\n\
             Their request: not the bedroom"
        );

        // Not commands: these go to the model as written.
        for text in [
            "hello",
            "/help",
            "/missing x",
            "/etc/hosts",
            "//play",
            "/",
            " / x",
        ] {
            assert!(commands.expand(text).is_none(), "{text}");
        }
        // A file that went away.
        std::fs::remove_file(dir.join("bare.md")).unwrap();
        assert!(commands.expand("/bare").unwrap().is_err());
    }

    #[test]
    fn words_and_splits() {
        assert_eq!(words(" a  \"b c\" d"), ["a", "b c", "d"]);
        assert_eq!(split("/play  Miles Davis "), Some(("play", "Miles Davis")));
        assert_eq!(split("/x\nmore lines"), Some(("x", "more lines")));
        assert_eq!(split("x"), None);
        assert_eq!(split("//x"), None);
    }
}
