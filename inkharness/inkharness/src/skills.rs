//! Agent Skills ([skills] in voiced.toml): folders of instructions the models load when a task
//! calls for one, so the system prompt carries only a line per skill.
//!
//! A skill is a folder holding `SKILL.md`: a frontmatter block between `---` lines with at least
//! `name` and `description`, then the instructions in markdown. It may hold other files
//! (reference notes, scripts for run_js). The models see the names and descriptions in the
//! system prompt and get two tools: load_skill (the instructions, with the list of the skill's
//! other files) and read_skill_file (one of those files). Reads stay inside the skill's folder.
//!
//! The folders are scanned at startup. A skill that can't be used is logged and left out; of
//! two with the same name the first is kept. Instructions and files are read from disk when the
//! model asks, so editing them needs no restart; adding a skill or changing a description does.
//!
//! run_js scripts can't call these tools: a script can't act on instructions, and the model can
//! pass a skill's script to run_js itself.

use std::collections::HashMap;
use std::io::Read;
use std::path::{Component, Path, PathBuf};

use serde::Deserialize;
use serde_json::{Value, json};

use crate::tools::ToolDef;

pub const LOAD_TOOL: &str = "load_skill";
pub const READ_TOOL: &str = "read_skill_file";
/// The file that makes a folder a skill.
const SKILL_FILE: &str = "SKILL.md";
/// Skills beyond this many are left out: each one lengthens the system prompt.
const MAX_SKILLS: usize = 32;
/// The Agent Skills spec's limit for a name.
const MAX_NAME: usize = 64;
/// A description is cut to this many characters in the system prompt.
const MAX_DESCRIPTION: usize = 250;
/// SKILL.md and the files read_skill_file returns are cut at this size.
const MAX_FILE: usize = 16 * 1024;
/// load_skill lists at most this many of a skill's other files.
const MAX_LISTED: usize = 50;
/// How deep the file listing goes below the skill's folder.
const MAX_DEPTH: usize = 4;
/// The listing stops looking after this many files.
const MAX_WALKED: usize = 1000;

pub struct Skill {
    pub name: String,
    /// One line, cut to MAX_DESCRIPTION characters.
    pub description: String,
    /// The skill's folder, canonical: every file read must resolve inside it.
    root: PathBuf,
}

#[derive(Default)]
pub struct Skills {
    skills: Vec<Skill>,
}

impl Skills {
    /// Scans `dirs`: each holds one folder per skill, or is a skill's folder itself. Problems
    /// are logged and leave that skill out.
    pub fn load(dirs: &[PathBuf]) -> Skills {
        let mut skills = Skills::default();
        for dir in dirs {
            let folders = if dir.join(SKILL_FILE).is_file() {
                vec![dir.clone()]
            } else {
                match std::fs::read_dir(dir) {
                    Ok(entries) => {
                        let mut folders: Vec<PathBuf> = entries
                            .flatten()
                            .map(|e| e.path())
                            .filter(|p| p.is_dir() && !hidden(p))
                            .collect();
                        folders.sort();
                        folders
                    }
                    Err(e) => {
                        tracing::warn!(dir = %dir.display(), "skills folder can't be read: {e}");
                        continue;
                    }
                }
            };
            for folder in folders {
                if !folder.join(SKILL_FILE).is_file() {
                    tracing::debug!(folder = %folder.display(), "no {SKILL_FILE}; not a skill");
                    continue;
                }
                match load_skill(&folder) {
                    Ok(skill) => skills.add(skill),
                    Err(e) => tracing::warn!(skill = %folder.display(), "skill left out: {e}"),
                }
            }
        }
        if !skills.skills.is_empty() {
            let names: Vec<&str> = skills.skills.iter().map(|s| s.name.as_str()).collect();
            tracing::info!(skills = %names.join(", "), "skills loaded");
        }
        skills
    }

    fn add(&mut self, skill: Skill) {
        if self.skills.iter().any(|s| s.name == skill.name) {
            tracing::warn!(skill = %skill.root.display(), "a skill named {} is already loaded; keeping the first", skill.name);
        } else if self.skills.len() >= MAX_SKILLS {
            tracing::warn!(skill = %skill.name, "more than {MAX_SKILLS} skills; left out");
        } else {
            self.skills.push(skill);
        }
    }

    /// Appended to the system prompt: a line per skill. Empty without skills.
    pub fn prompt(&self) -> String {
        if self.skills.is_empty() {
            return String::new();
        }
        let mut text = format!(
            "\n\nSkills: when a request matches one, call {LOAD_TOOL} with its name first and \
             follow what it says."
        );
        for skill in &self.skills {
            text.push_str(&format!("\n- {}: {}", skill.name, skill.description));
        }
        text
    }

    /// The two tools, or none without skills.
    pub fn defs(&self) -> Vec<ToolDef> {
        if self.skills.is_empty() {
            return Vec::new();
        }
        let names: Vec<&str> = self.skills.iter().map(|s| s.name.as_str()).collect();
        vec![
            ToolDef {
                name: LOAD_TOOL.into(),
                description: "Loads a skill's instructions, by a name from the skills listed in \
                              your instructions. Call it before doing what the skill covers, \
                              then follow what it returns. It also lists the skill's other \
                              files, which read_skill_file reads."
                    .into(),
                schema: json!({
                    "type": "object",
                    "properties": {
                        "name": {"type": "string", "enum": names}
                    },
                    "required": ["name"],
                    "additionalProperties": false
                }),
            },
            ToolDef {
                name: READ_TOOL.into(),
                description: format!(
                    "Reads one of a skill's files, when its instructions point to it. Text \
                     only, cut at {} KB.",
                    MAX_FILE / 1024
                ),
                schema: json!({
                    "type": "object",
                    "properties": {
                        "skill": {"type": "string", "enum": names},
                        "path": {"type": "string", "description": "The file's path inside the skill, as load_skill lists it"}
                    },
                    "required": ["skill", "path"],
                    "additionalProperties": false
                }),
            },
        ]
    }

    /// Runs load_skill or read_skill_file. `None` for any other tool. Files are small and
    /// local, so they are read on the caller's thread.
    pub fn call(&self, name: &str, input: Value) -> Option<Result<String, String>> {
        if self.skills.is_empty() || (name != LOAD_TOOL && name != READ_TOOL) {
            return None;
        }
        Some(if name == LOAD_TOOL {
            parse::<LoadInput>(input).and_then(|i| self.instructions(&i.name))
        } else {
            parse::<ReadInput>(input).and_then(|i| self.read(&i.skill, &i.path))
        })
    }

    fn find(&self, name: &str) -> Result<&Skill, String> {
        self.skills.iter().find(|s| s.name == name).ok_or_else(|| {
            let names: Vec<&str> = self.skills.iter().map(|s| s.name.as_str()).collect();
            format!(
                "no skill named {name:?}; the skills are: {}",
                names.join(", ")
            )
        })
    }

    /// SKILL.md's body, then the skill's other files.
    fn instructions(&self, name: &str) -> Result<String, String> {
        let skill = self.find(name)?;
        let (text, _) = read_capped(&skill.root.join(SKILL_FILE))?;
        let (_, body) = frontmatter(&text)?;
        let mut out = body.trim().to_string();
        let mut files = Vec::new();
        list_files(&skill.root, &skill.root, 0, &mut files);
        files.retain(|f| f != SKILL_FILE);
        files.sort();
        if !files.is_empty() {
            let more = files.len().saturating_sub(MAX_LISTED);
            files.truncate(MAX_LISTED);
            out.push_str(&format!(
                "\n\nFiles in this skill ({READ_TOOL}): {}",
                files.join(", ")
            ));
            if more > 0 {
                out.push_str(&format!(", and {more} more"));
            }
        }
        Ok(out)
    }

    /// A file inside the skill's folder, as text.
    fn read(&self, name: &str, path: &str) -> Result<String, String> {
        let skill = self.find(name)?;
        let file = inside(&skill.root, path)?;
        let (mut text, cut) = read_capped(&file)?;
        if cut {
            text.push_str(&format!("\n(cut at {} KB)", MAX_FILE / 1024));
        }
        Ok(text)
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct LoadInput {
    name: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ReadInput {
    skill: String,
    path: String,
}

fn parse<T: for<'de> Deserialize<'de>>(input: Value) -> Result<T, String> {
    serde_json::from_value(input).map_err(|e| format!("invalid input: {e}"))
}

/// Reads and checks one skill's folder.
fn load_skill(folder: &Path) -> Result<Skill, String> {
    let root = folder
        .canonicalize()
        .map_err(|e| format!("can't resolve the folder: {e}"))?;
    let (text, cut) = read_capped(&root.join(SKILL_FILE))?;
    if cut {
        return Err(format!("{SKILL_FILE} is over {} KB", MAX_FILE / 1024));
    }
    let (fields, body) = frontmatter(&text)?;
    let name = fields
        .get("name")
        .filter(|n| !n.is_empty())
        .ok_or("the frontmatter has no name")?;
    check_name(name)?;
    // The name as configured (a symlinked folder keeps its link's name); "." has none.
    let folder_name = folder
        .file_name()
        .or(root.file_name())
        .and_then(|n| n.to_str())
        .unwrap_or("");
    if name != folder_name {
        return Err(format!(
            "name {name:?} must match the folder's name {folder_name:?}"
        ));
    }
    let description = fields
        .get("description")
        .map(|d| d.split_whitespace().collect::<Vec<_>>().join(" "))
        .filter(|d| !d.is_empty())
        .ok_or("the frontmatter has no description")?;
    if body.trim().is_empty() {
        return Err("no instructions after the frontmatter".into());
    }
    let description = if description.chars().count() > MAX_DESCRIPTION {
        tracing::warn!(skill = %name, "description cut to {MAX_DESCRIPTION} characters");
        let mut cut: String = description.chars().take(MAX_DESCRIPTION - 1).collect();
        cut.push('…');
        cut
    } else {
        description
    };
    Ok(Skill {
        name: name.clone(),
        description,
        root,
    })
}

/// The spec's rule: 1 to 64 lowercase letters, digits and hyphens, with no hyphen at either
/// end or two in a row.
fn check_name(name: &str) -> Result<(), String> {
    let ok = !name.is_empty()
        && name.len() <= MAX_NAME
        && name
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')
        && !name.starts_with('-')
        && !name.ends_with('-')
        && !name.contains("--");
    if ok {
        Ok(())
    } else {
        Err(format!(
            "name {name:?} must be lowercase letters, digits and single hyphens, at most {MAX_NAME}"
        ))
    }
}

/// Splits SKILL.md into its frontmatter fields and its body.
///
/// The frontmatter is read as `key: value` lines, the value quoted (on one line) or plain (which
/// may go on over indented lines), plus YAML's `>` and `|` blocks. Indented lines under a key
/// with no value (a nested map such as `metadata:`) are skipped, as are comments; only
/// top-level keys are kept.
fn frontmatter(text: &str) -> Result<(HashMap<String, String>, &str), String> {
    let text = text.strip_prefix('\u{feff}').unwrap_or(text);
    let mut lines = text.split_inclusive('\n');
    if lines.next().map(str::trim_end) != Some("---") {
        return Err(format!("{SKILL_FILE} must start with a --- line"));
    }
    let mut offset = text.find('\n').map_or(text.len(), |i| i + 1);
    let mut block: Vec<&str> = Vec::new();
    let mut closed = false;
    for line in lines {
        offset += line.len();
        if line.trim_end() == "---" {
            closed = true;
            break;
        }
        block.push(line.trim_end_matches(['\n', '\r']));
    }
    if !closed {
        return Err("the frontmatter has no closing --- line".into());
    }
    let body = &text[offset..];

    let mut fields = HashMap::new();
    let mut i = 0;
    while i < block.len() {
        let line = block[i];
        i += 1;
        if line.trim().is_empty() || line.trim_start().starts_with('#') || indented(line) {
            continue;
        }
        let Some((key, value)) = line.split_once(':') else {
            return Err(format!("frontmatter line {line:?} is not key: value"));
        };
        let key = key.trim().to_string();
        let value = value.trim();
        let value = if let Some(style) = value.strip_prefix(['>', '|']) {
            if !matches!(style, "" | "-" | "+") {
                return Err(format!("{key}: unsupported block style {value:?}"));
            }
            let mut parts = Vec::new();
            while i < block.len() && (indented(block[i]) || block[i].trim().is_empty()) {
                parts.push(block[i].trim());
                i += 1;
            }
            let join = if value.starts_with('>') { " " } else { "\n" };
            parts.join(join).trim().to_string()
        } else if value.is_empty() || value.starts_with(['"', '\'']) {
            scalar(value).map_err(|e| format!("{key}: {e}"))?
        } else {
            // A plain value may go on over indented lines, folded into one.
            let mut plain = scalar(value)?;
            while i < block.len() && indented(block[i]) {
                plain.push(' ');
                plain.push_str(&scalar(block[i].trim())?);
                i += 1;
            }
            plain
        };
        fields.entry(key).or_insert(value);
    }
    Ok((fields, body))
}

fn indented(line: &str) -> bool {
    line.starts_with([' ', '\t'])
}

/// A one-line YAML value: "double quoted" (with \" \\ \n escapes), 'single quoted' ('' for a
/// quote), or plain, where " #" starts a comment.
fn scalar(value: &str) -> Result<String, String> {
    if let Some(rest) = value.strip_prefix('"') {
        let mut out = String::new();
        let mut chars = rest.chars();
        while let Some(c) = chars.next() {
            match c {
                '"' => {
                    return end_of_quoted(chars.as_str()).map(|()| out);
                }
                '\\' => match chars.next() {
                    Some('n') => out.push('\n'),
                    Some('t') => out.push('\t'),
                    Some(c @ ('"' | '\\' | '/')) => out.push(c),
                    other => return Err(format!("unsupported escape \\{}", other.unwrap_or(' '))),
                },
                c => out.push(c),
            }
        }
        Err("unclosed \"".into())
    } else if let Some(rest) = value.strip_prefix('\'') {
        let mut out = String::new();
        let mut chars = rest.chars().peekable();
        while let Some(c) = chars.next() {
            if c == '\'' {
                if chars.peek() == Some(&'\'') {
                    chars.next();
                    out.push('\'');
                } else {
                    return end_of_quoted(&chars.collect::<String>()).map(|()| out);
                }
            } else {
                out.push(c);
            }
        }
        Err("unclosed '".into())
    } else {
        let plain = value.find(" #").map_or(value, |i| &value[..i]);
        Ok(plain.trim().to_string())
    }
}

/// After a closing quote only a comment may follow.
fn end_of_quoted(rest: &str) -> Result<(), String> {
    let rest = rest.trim();
    if rest.is_empty() || rest.starts_with('#') {
        Ok(())
    } else {
        Err(format!("unexpected {rest:?} after the closing quote"))
    }
}

/// Up to MAX_FILE bytes of a text file, and whether it was longer.
fn read_capped(path: &Path) -> Result<(String, bool), String> {
    let shown = path.file_name().map_or_else(
        || path.display().to_string(),
        |n| n.to_string_lossy().into(),
    );
    let mut bytes = Vec::new();
    std::fs::File::open(path)
        .and_then(|f| f.take(MAX_FILE as u64 + 1).read_to_end(&mut bytes))
        .map_err(|e| format!("can't read {shown}: {e}"))?;
    let cut = bytes.len() > MAX_FILE;
    bytes.truncate(MAX_FILE);
    let text = match String::from_utf8(bytes) {
        Ok(text) => text,
        // The cut may have split a character: keep what comes before it.
        Err(e) if cut && e.utf8_error().error_len().is_none() => {
            let valid = e.utf8_error().valid_up_to();
            let mut bytes = e.into_bytes();
            bytes.truncate(valid);
            String::from_utf8(bytes).unwrap_or_default()
        }
        Err(_) => return Err(format!("{shown} is not a text file")),
    };
    Ok((text, cut))
}

/// `path` inside the skill's folder `root` (canonical), or why it can't be read. Absolute
/// paths, `..` and hidden files are refused before touching the disk; a symlink that leads out
/// of the folder is refused once resolved.
fn inside(root: &Path, path: &str) -> Result<PathBuf, String> {
    if path.trim().is_empty() {
        return Err("invalid input: path is empty".into());
    }
    for part in Path::new(path).components() {
        match part {
            Component::Normal(name) if name.to_string_lossy().starts_with('.') => {
                return Err(format!("{path:?}: hidden files can't be read"));
            }
            Component::Normal(_) | Component::CurDir => {}
            _ => {
                return Err(format!(
                    "{path:?}: give a path inside the skill's folder, without .. or a leading /"
                ));
            }
        }
    }
    let file = root
        .join(path)
        .canonicalize()
        .map_err(|_| format!("the skill has no file {path:?}"))?;
    if !file.starts_with(root) {
        return Err(format!("{path:?} leads out of the skill's folder"));
    }
    if !file.is_file() {
        return Err(format!("{path:?} is not a file"));
    }
    Ok(file)
}

/// The files under `dir` that read_skill_file would return, as paths relative to `root`.
/// Hidden entries are skipped, symlinked folders aren't followed, and a symlinked file is
/// listed only if it resolves inside the skill.
fn list_files(root: &Path, dir: &Path, depth: usize, out: &mut Vec<String>) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if out.len() >= MAX_WALKED {
            return;
        }
        if hidden(&path) {
            continue;
        }
        let Ok(kind) = entry.file_type() else {
            continue;
        };
        if kind.is_dir() {
            if depth + 1 < MAX_DEPTH {
                list_files(root, &path, depth + 1, out);
            }
            continue;
        }
        let resolved = path.canonicalize();
        let ok = resolved.is_ok_and(|p| p.starts_with(root) && p.is_file());
        if ok && let Ok(relative) = path.strip_prefix(root) {
            out.push(relative.to_string_lossy().replace('\\', "/"));
        }
    }
}

fn hidden(path: &Path) -> bool {
    path.file_name()
        .is_some_and(|n| n.to_string_lossy().starts_with('.'))
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// A fresh, empty folder under the system's temporary folder.
    pub fn scratch(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("voiced-skills-{}-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    pub fn write(path: &Path, text: &str) {
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, text).unwrap();
    }

    fn fields(text: &str) -> Result<(HashMap<String, String>, String), String> {
        frontmatter(text).map(|(f, b)| (f, b.to_string()))
    }

    #[test]
    fn frontmatter_fields_and_body() {
        let (f, body) = fields(
            "---\nname: goodnight\ndescription: Pauses the speakers: all of them. # note\n\
             license: MIT\nmetadata:\n  author: someone\n  name: nested\n---\n# Steps\nDo it.\n",
        )
        .unwrap();
        assert_eq!(f["name"], "goodnight");
        assert_eq!(f["description"], "Pauses the speakers: all of them.");
        assert_eq!(f["metadata"], "");
        assert_eq!(body, "# Steps\nDo it.\n");

        let (f, _) = fields(
            "\u{feff}---\r\nname: \"quoted\" # c\r\ndescription: 'it''s \"fine\"'\r\nx: \"a\\\"b\\\\\"\r\n---\r\nbody",
        )
        .unwrap();
        assert_eq!(f["name"], "quoted");
        assert_eq!(f["description"], "it's \"fine\"");
        assert_eq!(f["x"], "a\"b\\");

        let (f, _) =
            fields("---\nname: x\ndescription: >-\n  Folded over\n  two lines.\n---\nb").unwrap();
        assert_eq!(f["description"], "Folded over two lines.");
    }

    #[test]
    fn frontmatter_errors() {
        assert!(
            fields("# Just markdown\n")
                .unwrap_err()
                .contains("must start")
        );
        assert!(fields("---\nname: x\n").unwrap_err().contains("closing"));
        assert!(
            fields("---\nname x\n---\n")
                .unwrap_err()
                .contains("key: value")
        );
        assert!(
            fields("---\nname: \"open\n---\n")
                .unwrap_err()
                .contains("unclosed")
        );
        assert!(
            fields("---\nname: 'a' b\n---\n")
                .unwrap_err()
                .contains("after")
        );
    }

    #[test]
    fn skills_are_checked() {
        let dir = scratch("checked");
        let ok = "---\nname: good\ndescription: A  good\n  skill.\n---\nSteps.";
        write(&dir.join("good/SKILL.md"), ok);
        write(&dir.join("good/notes/more.md"), "More.");
        write(&dir.join("good/.secret"), "hidden");
        write(
            &dir.join("no-desc/SKILL.md"),
            "---\nname: no-desc\n---\nSteps.",
        );
        write(
            &dir.join("no-name/SKILL.md"),
            "---\ndescription: d\n---\nSteps.",
        );
        write(
            &dir.join("Bad_Name/SKILL.md"),
            "---\nname: Bad_Name\ndescription: d\n---\nx",
        );
        write(
            &dir.join("mismatch/SKILL.md"),
            "---\nname: other\ndescription: d\n---\nx",
        );
        write(
            &dir.join("empty/SKILL.md"),
            "---\nname: empty\ndescription: d\n---\n\n",
        );
        write(&dir.join("plain/SKILL.md"), "No frontmatter.");
        write(
            &dir.join("big/SKILL.md"),
            &format!(
                "---\nname: big\ndescription: d\n---\n{}",
                "x".repeat(MAX_FILE)
            ),
        );
        write(&dir.join("not-a-skill/README.md"), "x");
        // The same name again in a second folder: the first is kept.
        let other = scratch("checked-2");
        write(
            &other.join("good/SKILL.md"),
            "---\nname: good\ndescription: second\n---\nx",
        );

        let skills = Skills::load(&[dir.clone(), other, dir.join("missing")]);
        let names: Vec<&str> = skills.skills.iter().map(|s| s.name.as_str()).collect();
        assert_eq!(names, ["good"]);
        assert_eq!(skills.skills[0].description, "A good skill.");

        assert!(check_name("a-1").is_ok());
        for bad in ["-a", "a-", "a--b", "A", "a_b", ""] {
            assert!(check_name(bad).is_err(), "{bad}");
        }
        assert!(check_name(&"a".repeat(65)).is_err());
    }

    #[test]
    fn long_descriptions_are_cut() {
        let dir = scratch("long");
        let text = format!(
            "---\nname: long\ndescription: {}\n---\nx",
            "word ".repeat(100)
        );
        write(&dir.join("long/SKILL.md"), &text);
        let skills = Skills::load(&[dir]);
        let description = &skills.skills[0].description;
        assert_eq!(description.chars().count(), MAX_DESCRIPTION);
        assert!(description.ends_with('…'));
    }

    #[test]
    fn the_prompt_lists_each_skill() {
        assert_eq!(Skills::default().prompt(), "");
        assert!(Skills::default().defs().is_empty());
        let dir = scratch("prompt");
        write(
            &dir.join("b-skill/SKILL.md"),
            "---\nname: b-skill\ndescription: Does B.\n---\nx",
        );
        write(
            &dir.join("a-skill/SKILL.md"),
            "---\nname: a-skill\ndescription: Does A.\n---\nx",
        );
        let skills = Skills::load(&[dir]);
        assert_eq!(
            skills.prompt(),
            "\n\nSkills: when a request matches one, call load_skill with its name first and \
             follow what it says.\n- a-skill: Does A.\n- b-skill: Does B."
        );
        let defs = skills.defs();
        assert_eq!(defs[0].name, LOAD_TOOL);
        assert_eq!(
            defs[0].schema["properties"]["name"]["enum"],
            json!(["a-skill", "b-skill"])
        );
        assert_eq!(defs[1].name, READ_TOOL);
    }

    #[test]
    fn reads_stay_inside_the_skill() {
        let dir = scratch("escape");
        write(
            &dir.join("s/SKILL.md"),
            "---\nname: s\ndescription: d\n---\nx",
        );
        write(&dir.join("s/ok.js"), "1 + 1");
        write(&dir.join("s/.env"), "KEY=1");
        write(&dir.join("secret.txt"), "outside");
        let skills = Skills::load(std::slice::from_ref(&dir));
        let read = |path: &str| skills.read("s", path);

        assert_eq!(read("ok.js"), Ok("1 + 1".into()));
        assert_eq!(read("./ok.js"), Ok("1 + 1".into()));
        assert!(read("../secret.txt").unwrap_err().contains(".."));
        assert!(read("sub/../../secret.txt").unwrap_err().contains(".."));
        let absolute = dir.join("secret.txt").display().to_string();
        assert!(read(&absolute).unwrap_err().contains("leading /"));
        assert!(read(".env").unwrap_err().contains("hidden"));
        assert!(read("").unwrap_err().contains("empty"));
        assert!(read("missing.md").unwrap_err().contains("no file"));
        assert!(
            skills
                .read("t", "ok.js")
                .unwrap_err()
                .contains("no skill named")
        );
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(dir.join("secret.txt"), dir.join("s/link.txt")).unwrap();
            std::os::unix::fs::symlink(&dir, dir.join("s/up")).unwrap();
            std::os::unix::fs::symlink(dir.join("s/ok.js"), dir.join("s/alias.js")).unwrap();
            assert!(read("link.txt").unwrap_err().contains("leads out"));
            assert!(read("up/secret.txt").unwrap_err().contains("leads out"));
            assert_eq!(read("alias.js"), Ok("1 + 1".into()));
            // The listing shows only what can be read.
            let listed = skills.instructions("s").unwrap();
            assert!(
                listed.ends_with("(read_skill_file): alias.js, ok.js"),
                "{listed}"
            );
        }
    }

    #[test]
    fn files_are_capped() {
        let dir = scratch("cap");
        write(
            &dir.join("s/SKILL.md"),
            "---\nname: s\ndescription: d\n---\nx",
        );
        // A multi-byte character straddles the cut.
        write(
            &dir.join("s/big.md"),
            &format!("{}é{}", "a".repeat(MAX_FILE - 1), "b".repeat(100)),
        );
        std::fs::write(dir.join("s/blob.bin"), [0xff, 0xfe, 0x00]).unwrap();
        let skills = Skills::load(&[dir]);
        let big = skills.read("s", "big.md").unwrap();
        assert_eq!(big, format!("{}\n(cut at 16 KB)", "a".repeat(MAX_FILE - 1)));
        assert!(
            skills
                .read("s", "blob.bin")
                .unwrap_err()
                .contains("not a text file")
        );
    }
}
