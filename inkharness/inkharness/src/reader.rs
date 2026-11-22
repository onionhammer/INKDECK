//! Reader view: the article in a web page, as Markdown a board's News lays out (its NoteDoc:
//! paragraphs, headings, lists, quotes, `[text](url)` links). A board asks with `read` and gets
//! `page` (docs/voice-protocol.md, "Reading a page"); `GET /read?url=` and `inkharness read`
//! answer the same.
//!
//! The article is found by dom_smoothie, a port of Mozilla's Readability (Firefox's Reader View),
//! in its own candidate mode, which keeps text that sits in sibling blocks (Ars Technica's
//! first half, which Readability's single best container misses). What it keeps is then tidied
//! for a 1-bit screen with no pictures: figures, images, captions, players and forms go, then
//! blocks of page furniture ("Advertisement", "Listen to this article"), a block that repeats
//! the one before, and headings left with nothing under them.

use std::sync::OnceLock;
use std::time::Duration;

use anyhow::{Context, Result, anyhow, bail};
use dom_query::Document;
use dom_smoothie::{CandidateSelectMode, Config, Readability, TextMode};
use futures_util::StreamExt;

/// Of a page downloaded.
pub const MAX_PAGE: usize = 5 * 1024 * 1024;
/// Of the text sent: a `page` message must fit one frame (64 KB, JSON-escaped).
pub const MAX_TEXT: usize = 40 * 1024;
/// Less than this isn't an article.
const MIN_TEXT: usize = 200;
const TIMEOUT: Duration = Duration::from_secs(20);
/// A browser's: some sites serve a stub, or nothing, to clients they don't know.
const USER_AGENT: &str = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 \
                          (KHTML, like Gecko) Chrome/130.0 Safari/537.36";
const CUT_LINE: &str = "*[Cut off here: too long for the board.]*";

/// Inside the article, what never shows on the board.
const DROP: &str = "figure, figcaption, picture, img, svg, video, audio, iframe, object, embed, \
                    canvas, button, form, input, select, nav, aside, noscript, template, \
                    [aria-hidden=true], [hidden]";

#[derive(Debug, Clone, Default, PartialEq)]
pub struct Page {
    /// Where the page ended up, past redirects.
    pub url: String,
    pub title: String,
    pub byline: String,
    /// The site's name ("BBC News"), else its host.
    pub site: String,
    /// Unix seconds; 0 if the page doesn't say.
    pub published: i64,
    pub markdown: String,
}

fn client() -> &'static reqwest::Client {
    static CLIENT: OnceLock<reqwest::Client> = OnceLock::new();
    CLIENT.get_or_init(|| {
        reqwest::Client::builder()
            .user_agent(USER_AGENT)
            .timeout(TIMEOUT)
            .build()
            .expect("an HTTP client")
    })
}

/// Fetches the page at `url` and reads its article.
pub async fn read(url: &str) -> Result<Page> {
    let parsed = reqwest::Url::parse(url).map_err(|_| anyhow!("not a web address"))?;
    if !matches!(parsed.scheme(), "http" | "https") {
        bail!("not a web address");
    }
    let resp = client()
        .get(parsed)
        .header("Accept", "text/html,application/xhtml+xml;q=0.9,*/*;q=0.5")
        .send()
        .await
        .map_err(|e| anyhow!("{}", short_error(&e)))?;
    let status = resp.status();
    if !status.is_success() {
        bail!("HTTP {}", status.as_u16());
    }
    if let Some(kind) = resp.headers().get(reqwest::header::CONTENT_TYPE)
        && let Ok(kind) = kind.to_str()
        && !kind.contains("html")
        && !kind.contains("xml")
    {
        bail!(
            "not a web page ({})",
            kind.split(';').next().unwrap_or(kind)
        );
    }
    let final_url = resp.url().to_string();
    let mut body = Vec::new();
    let mut stream = resp.bytes_stream();
    while let Some(chunk) = stream.next().await {
        let chunk = chunk.map_err(|e| anyhow!("{}", short_error(&e)))?;
        body.extend_from_slice(&chunk);
        if body.len() >= MAX_PAGE {
            body.truncate(MAX_PAGE);
            break;
        }
    }
    let html = String::from_utf8_lossy(&body).into_owned();
    tokio::task::spawn_blocking(move || extract(&html, &final_url))
        .await
        .context("reading the page")?
}

/// The article in `html`, which came from `url` (for its links and the site's name).
pub fn extract(html: &str, url: &str) -> Result<Page> {
    let cfg = Config {
        candidate_select_mode: CandidateSelectMode::DomSmoothie,
        text_mode: TextMode::Raw,
        ..Default::default()
    };
    let mut reader =
        Readability::new(html, Some(url), Some(cfg)).map_err(|e| anyhow!("unreadable: {e}"))?;
    let article = reader
        .parse()
        .map_err(|_| anyhow!("no article on the page"))?;
    let doc = Document::from(article.content.as_ref());
    doc.select(DROP).remove();
    let markdown = tidy(&doc.md(None), MAX_TEXT);
    if markdown.len() < MIN_TEXT {
        bail!("no article on the page");
    }
    let host = reqwest::Url::parse(url)
        .ok()
        .and_then(|u| {
            u.host_str()
                .map(|h| h.trim_start_matches("www.").to_string())
        })
        .unwrap_or_default();
    Ok(Page {
        url: url.to_string(),
        title: one_line(&article.title),
        byline: article.byline.as_deref().map(one_line).unwrap_or_default(),
        site: article
            .site_name
            .as_deref()
            .map(one_line)
            .filter(|s| !s.is_empty())
            .unwrap_or(host),
        published: article
            .published_time
            .as_deref()
            .and_then(|t| chrono::DateTime::parse_from_rfc3339(t.trim()).ok())
            .map(|t| t.timestamp())
            .unwrap_or(0),
        markdown,
    })
}

fn one_line(s: &str) -> String {
    s.split_whitespace().collect::<Vec<_>>().join(" ")
}

/// A network error in a few words, for the board's screen.
fn short_error(e: &reqwest::Error) -> &'static str {
    if e.is_timeout() {
        "the site took too long"
    } else if e.is_connect() {
        "couldn't reach the site"
    } else if e.is_redirect() {
        "too many redirects"
    } else {
        "the download failed"
    }
}

/// Page furniture that lands inside the text as a block of its own.
fn boilerplate(block: &str) -> bool {
    if block.len() > 60 {
        return false;
    }
    let b = block
        .trim_matches(|c: char| c == '*' || c == '_' || c == ':' || c.is_whitespace())
        .to_lowercase();
    const WORDS: &[&str] = &[
        "advertisement",
        "advert",
        "skip advertisement",
        "listen to this article",
        "by",
        "posted",
        "updated",
        "share",
        "share this article",
        "watch next",
        "read more",
        "continue reading",
        "story continues below advertisement",
        "related",
        "most read",
        "in brief",
    ];
    WORDS.contains(&b.as_str())
        || b.starts_with("published on ")
        || b.starts_with("estimated ") && b.ends_with(" minutes")
}

/// `![alt](url)` out of a block (dom_query's Markdown keeps images that sit in links).
fn without_images(block: &str) -> String {
    let mut out = String::with_capacity(block.len());
    let mut rest = block;
    while let Some(at) = rest.find("![") {
        out.push_str(&rest[..at]);
        let after = &rest[at..];
        match after
            .find("](")
            .and_then(|m| after[m..].find(')').map(|c| m + c + 1))
        {
            Some(end) => rest = &after[end..],
            None => {
                out.push_str(after);
                rest = "";
            }
        }
    }
    out.push_str(rest);
    // A link left with no text: "[](url)".
    while let Some(at) = out.find("[](") {
        match out[at..].find(')') {
            Some(end) => out.replace_range(at..at + end + 1, ""),
            None => break,
        }
    }
    out.trim().to_string()
}

/// Markdown's backslash escapes out: the board's NoteDoc would print them.
fn unescape(block: &str) -> String {
    let mut out = String::with_capacity(block.len());
    let mut chars = block.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\\' && chars.peek().is_some_and(|n| n.is_ascii_punctuation()) {
            continue;
        }
        out.push(c);
    }
    out
}

fn is_heading(block: &str) -> bool {
    block.starts_with('#')
}

/// The Markdown, tidied and cut at a block past `max` bytes.
pub fn tidy(md: &str, max: usize) -> String {
    let mut kept: Vec<String> = Vec::new();
    for block in md.split("\n\n") {
        let block = unescape(&without_images(block));
        if block.is_empty() || boilerplate(&block) || kept.last() == Some(&block) {
            continue;
        }
        kept.push(block);
    }
    // Headings with nothing under them, and rules at the end.
    let mut out: Vec<String> = Vec::new();
    for (i, block) in kept.iter().enumerate() {
        let next_is_heading = kept.get(i + 1).is_none_or(|b| is_heading(b));
        if is_heading(block) && next_is_heading {
            continue;
        }
        out.push(block.clone());
    }
    while out
        .last()
        .is_some_and(|b| b == "---" || b == "***" || is_heading(b))
    {
        out.pop();
    }
    let mut text = String::new();
    for block in out {
        if text.len() >= max {
            text.push_str("\n\n");
            text.push_str(CUT_LINE);
            break;
        }
        if !text.is_empty() {
            text.push_str("\n\n");
        }
        text.push_str(&block);
    }
    if text.len() > max + 4096 {
        // One block that never ends: cut it at a space.
        let mut at = max;
        while !text.is_char_boundary(at) {
            at -= 1;
        }
        let at = text[..at].rfind(' ').unwrap_or(at);
        text.truncate(at);
        text.push_str("\n\n");
        text.push_str(CUT_LINE);
    }
    text
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tidy_drops_pictures_furniture_repeats_and_bare_headings() {
        let md = "## Title\n\nAdvertisement\n\nThe dek.\n\nThe dek.\n\nA \\| B\n\n\
                  ![a ship](https://x/y.jpg)\n\nText with [a link](https://a.b/c).\n\n\
                  [![](https://x/i.png)](https://x/big)\n\n## More on this story\n\n---";
        assert_eq!(
            tidy(md, MAX_TEXT),
            "## Title\n\nThe dek.\n\nA | B\n\nText with [a link](https://a.b/c)."
        );
    }

    #[test]
    fn tidy_cuts_at_a_block() {
        let md = format!(
            "{}\n\n{}\n\n{}",
            "a ".repeat(100),
            "b ".repeat(100),
            "c ".repeat(100)
        );
        let out = tidy(&md, 250);
        assert!(out.ends_with(CUT_LINE));
        assert!(!out.contains('c'));
    }

    #[test]
    fn extract_reads_an_article_and_its_links() {
        let para = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod \
                    tempor incididunt ut labore et dolore magna aliqua. ";
        let html = format!(
            "<html><head><title>A story - The Site</title>\
             <meta property=\"og:site_name\" content=\"The Site\">\
             <meta property=\"article:published_time\" content=\"2026-11-22T06:01:00Z\"></head>\
             <body><nav><a href=\"/\">Home</a> <a href=\"/news\">News</a></nav>\
             <article><h1>A story</h1><p>{para}{para}<a href=\"/more\">more</a></p>\
             <figure><img src=\"a.jpg\"><figcaption>A caption</figcaption></figure>\
             <p>{para}{para}</p><p>{para}</p></article>\
             <footer>Copyright</footer></body></html>"
        );
        let page = extract(&html, "https://example.com/news/a-story").unwrap();
        assert_eq!(page.site, "The Site");
        assert_eq!(page.published, 1795327260);
        assert!(page.markdown.contains("[more](https://example.com/more)"));
        assert!(!page.markdown.contains("caption"));
        assert!(!page.markdown.contains("Copyright"));
    }
}
