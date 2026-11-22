//! Reader view: the article in a web page, as Markdown a board's News lays out (its NoteDoc:
//! paragraphs, headings, lists, quotes, `[text](url)` links), with its pictures. A board asks
//! with `read` and gets `page_image`s, then `page` (docs/voice-protocol.md, "Reading a page");
//! `GET /read?url=` and `inkharness read` answer the same.
//!
//! The article is found by dom_smoothie, a port of Mozilla's Readability (Firefox's Reader View),
//! in its own candidate mode, which keeps text that sits in sibling blocks (Ars Technica's
//! first half, which Readability's single best container misses). What it keeps is then tidied
//! for a 1-bit screen: players, forms and the like go, then blocks of page furniture
//! ("Advertisement", "Listen to this article"), a block that repeats the one before, and headings
//! left with nothing under them.
//!
//! Pictures, when the board asks for them (`Fit`): each figure, or picture on its own, becomes
//! `![](inkimg:N)` in the text with its caption after it in italics, and the first
//! MAX_PICTURES are fetched (the srcset's smallest that's still sharp), scaled to the board's
//! column, their levels stretched and dithered to 1 bit (Atkinson's, which keeps light areas
//! clean on e-paper). One that fails, or turns out to be an icon, goes with its caption. A
//! page whose text shows no picture near the top gets its lead picture (og:image) there: BBC and
//! Ars keep theirs outside the text Readability finds. A picture on its own near the end is a
//! credit's logo (The Conversation's), and goes.

use std::collections::HashSet;
use std::sync::OnceLock;
use std::time::Duration;

use anyhow::{Context, Result, anyhow, bail};
use dom_query::{Document, Selection};
use dom_smoothie::{CandidateSelectMode, Config, Readability, TextMode};
use futures_util::StreamExt;
use image::GrayImage;
use image::imageops::FilterType;

/// Of a page downloaded.
pub const MAX_PAGE: usize = 5 * 1024 * 1024;
/// Of the text sent: a `page` message must fit one frame (64 KB, JSON-escaped).
pub const MAX_TEXT: usize = 40 * 1024;
/// Pictures fetched for a page.
pub const MAX_PICTURES: usize = 12;
/// A picture's bits (with base64 a third more) must fit one frame too.
const MAX_PICTURE_BYTES: usize = 40 * 1024;
/// Of a picture downloaded.
const MAX_IMAGE: usize = 8 * 1024 * 1024;
/// A figure's picture narrower or shorter than this (as served) is an icon or a tracker.
const MIN_SIDE: u32 = 96;
/// The same for a picture on its own, or the page's lead picture: more often a logo.
const MIN_SIDE_LOOSE: u32 = 200;
/// Words in a loose picture's address, class or alt text that say it isn't a picture.
const NOT_PICTURES: &[&str] = &[
    "logo",
    "icon",
    "avatar",
    "badge",
    "sprite",
    "placeholder",
    "spacer",
    "pixel",
    "author",
];
/// Less than this isn't an article.
const MIN_TEXT: usize = 200;
const TIMEOUT: Duration = Duration::from_secs(20);
/// A browser's: some sites serve a stub, or nothing, to clients they don't know.
const USER_AGENT: &str = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 \
                          (KHTML, like Gecko) Chrome/130.0 Safari/537.36";
const CUT_LINE: &str = "*[Cut off here: too long for the board.]*";
/// In the text until the pictures are in: a picture, a picture on its own (not in a figure),
/// the page's lead picture, and a caption.
const IMG_MARK: &str = "INKIMG:";
const LOOSE_MARK: &str = "INKLOOSE:";
const LEAD_MARK: &str = "INKLEAD:";
const CAP_MARK: &str = "INKCAP:";

/// Inside the article, what never shows on the board (pictures, when wanted, are taken out
/// first).
const DROP: &str = "figure, figcaption, picture, img, svg, video, audio, iframe, object, embed, \
                    canvas, button, form, input, select, nav, aside, noscript, template, \
                    [aria-hidden=true], [hidden]";

/// How big the board shows a picture at most: its text column's width and a page's height.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Fit {
    pub width: u32,
    pub height: u32,
}

/// A picture, dithered: rows of (width + 7) / 8 bytes, the leftmost pixel in the top bit, 1 for
/// ink.
#[derive(Debug, Clone, PartialEq)]
pub struct Picture {
    /// `inkimg:N` in the text.
    pub index: u32,
    pub width: u32,
    pub height: u32,
    pub bits: Vec<u8>,
}

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
    pub pictures: Vec<Picture>,
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

/// Up to `max` bytes of a response's body.
async fn body(resp: reqwest::Response, max: usize) -> Result<Vec<u8>> {
    let mut body = Vec::new();
    let mut stream = resp.bytes_stream();
    while let Some(chunk) = stream.next().await {
        let chunk = chunk.map_err(|e| anyhow!("{}", short_error(&e)))?;
        body.extend_from_slice(&chunk);
        if body.len() >= max {
            body.truncate(max);
            break;
        }
    }
    Ok(body)
}

/// Fetches the page at `url` and reads its article; with `fit`, its pictures too.
pub async fn read(url: &str, fit: Option<Fit>) -> Result<Page> {
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
    let html = String::from_utf8_lossy(&body(resp, MAX_PAGE).await?).into_owned();
    let (mut page, sources) =
        tokio::task::spawn_blocking(move || extract_marked(&html, &final_url, fit))
            .await
            .context("reading the page")??;
    tracing::debug!(?sources, "pictures");
    if let Some(fit) = fit {
        let fetches = sources
            .iter()
            .enumerate()
            .map(|(i, (src, min_side))| picture(i as u32, src, *min_side, fit));
        page.pictures = futures_util::future::join_all(fetches)
            .await
            .into_iter()
            .flatten()
            .collect();
    }
    let shown: HashSet<u32> = page.pictures.iter().map(|p| p.index).collect();
    page.markdown = place_pictures(&page.markdown, &shown);
    // Only the pictures the text shows.
    let used: HashSet<u32> = page
        .markdown
        .match_indices("](inkimg:")
        .filter_map(|(at, m)| {
            let rest = &page.markdown[at + m.len()..];
            rest[..rest.find(')')?].parse().ok()
        })
        .collect();
    page.pictures.retain(|p| used.contains(&p.index));
    Ok(page)
}

/// The article in `html`, which came from `url` (for its links and the site's name), without
/// pictures.
pub fn extract(html: &str, url: &str) -> Result<Page> {
    let (mut page, _) = extract_marked(html, url, None)?;
    page.markdown = place_pictures(&page.markdown, &HashSet::new());
    Ok(page)
}

/// A picture to fetch: its address, and how small it may be and still count.
type Source = (String, u32);

/// The article, with each picture's place marked in the text (`fit`: pictures wanted), and the
/// pictures to fetch, by index.
fn extract_marked(html: &str, url: &str, fit: Option<Fit>) -> Result<(Page, Vec<Source>)> {
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
    let base = reqwest::Url::parse(url).ok();
    let mut sources = match (fit, &base) {
        (Some(fit), Some(base)) => mark_pictures(&doc, base, fit),
        _ => Vec::new(),
    };
    doc.select(DROP).remove();
    let mut markdown = tidy(&doc.md(None), MAX_TEXT);
    if markdown.len() < MIN_TEXT {
        bail!("no article on the page");
    }
    // The lead picture, first: shown if no picture the text has near the top comes.
    if fit.is_some()
        && sources.len() < MAX_PICTURES
        && let (Some(lead), Some(base)) = (article.image.as_deref(), &base)
        && let Ok(lead) = base.join(lead.trim())
        && matches!(lead.scheme(), "http" | "https")
        && !loose_but_not_a_picture(lead.as_str())
    {
        let n = sources.len();
        sources.push((lead.to_string(), MIN_SIDE_LOOSE));
        markdown = format!("{LEAD_MARK}{n}\n\n{markdown}");
    }
    let host = base
        .as_ref()
        .and_then(|u| {
            u.host_str()
                .map(|h| h.trim_start_matches("www.").to_string())
        })
        .unwrap_or_default();
    let page = Page {
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
        pictures: Vec::new(),
    };
    Ok((page, sources))
}

// --- Pictures ------------------------------------------------------------------------------------

fn escape_html(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}

/// The best address for `img` (in `holder`, its figure or itself): from a srcset, the smallest
/// candidate at least twice the column (sharp once scaled down), else the largest; else src.
fn source(img: &Selection, holder: &Selection, base: &reqwest::Url, fit: Fit) -> Option<String> {
    let mut sets: Vec<String> = Vec::new();
    for s in holder.select("source").iter() {
        let kind = s.attr("type").map(|t| t.to_string()).unwrap_or_default();
        if kind.contains("avif") || kind.contains("svg") {
            continue; // the board's decoder can't
        }
        if let Some(v) = s.attr("srcset").or_else(|| s.attr("data-srcset")) {
            sets.push(v.to_string());
        }
    }
    for name in ["srcset", "data-srcset"] {
        if let Some(v) = img.attr(name) {
            sets.push(v.to_string());
        }
    }
    let want = fit.width * 2;
    let mut best: Option<(u32, String)> = None;
    for set in &sets {
        // Candidates are comma-separated, but a URL may hold commas: split at ", ".
        for candidate in set.split(", ").flat_map(|c| c.split(",\n")) {
            let mut parts = candidate.split_whitespace();
            let Some(u) = parts.next() else { continue };
            let w = parts
                .next()
                .and_then(|d| d.strip_suffix('w'))
                .and_then(|n| n.parse::<u32>().ok())
                .unwrap_or(0);
            let better = match &best {
                None => true,
                Some((bw, _)) => (w >= want && (*bw < want || w < *bw)) || (*bw < want && w > *bw),
            };
            if better {
                best = Some((w, u.to_string()));
            }
        }
    }
    let raw = best.map(|(_, u)| u).or_else(|| {
        ["src", "data-src", "data-lazy-src", "data-original"]
            .iter()
            .find_map(|n| img.attr(n).map(|v| v.to_string()))
    })?;
    let url = base.join(raw.trim()).ok()?;
    let path = url.path().to_ascii_lowercase();
    if !matches!(url.scheme(), "http" | "https") || path.ends_with(".svg") {
        return None;
    }
    Some(url.to_string())
}

fn loose_but_not_a_picture(words: &str) -> bool {
    let words = words.to_ascii_lowercase();
    NOT_PICTURES.iter().any(|w| words.contains(w))
}

/// Each figure, then each picture outside one, becomes a marker paragraph (and its caption
/// one); the first MAX_PICTURES get one. What to fetch, by index.
fn mark_pictures(doc: &Document, base: &reqwest::Url, fit: Fit) -> Vec<Source> {
    let mut sources = Vec::new();
    for fig in doc.select("figure").iter() {
        if sources.len() >= MAX_PICTURES {
            break;
        }
        let img = fig.select("img").first();
        if !img.exists() {
            continue;
        }
        let Some(src) = source(&img, &fig, base, fit) else {
            continue;
        };
        let n = sources.len();
        sources.push((src, MIN_SIDE));
        let caption = one_line(&fig.select("figcaption").text());
        let caption = if caption.is_empty() {
            String::new()
        } else {
            format!("<p>{CAP_MARK}{n} {}</p>", escape_html(&caption))
        };
        fig.replace_with_html(format!("<p>{IMG_MARK}{n}</p>{caption}"));
    }
    for img in doc.select("img").iter() {
        if sources.len() >= MAX_PICTURES {
            break;
        }
        let holder = img.parent();
        let holder = if holder.is("picture") {
            holder
        } else {
            img.clone()
        };
        let Some(src) = source(&img, &holder, base, fit) else {
            continue;
        };
        let words = format!(
            "{src} {} {} {}",
            img.attr("class").unwrap_or_default(),
            img.attr("alt").unwrap_or_default(),
            img.attr("id").unwrap_or_default()
        );
        if loose_but_not_a_picture(&words) {
            continue;
        }
        let n = sources.len();
        sources.push((src, MIN_SIDE_LOOSE));
        holder.replace_with_html(format!("<p>{LOOSE_MARK}{n}</p>"));
    }
    sources
}

/// The markers in the text, made Markdown: `![](inkimg:N)` for a picture that came, its caption
/// in italics; one that didn't goes, caption and all. The lead picture shows only if no other
/// comes in the first blocks, and a picture on its own in the last quarter (or three) goes.
fn place_pictures(md: &str, shown: &HashSet<u32>) -> String {
    let index = |block: &str, mark: &str| -> Option<(u32, usize)> {
        let at = block.find(mark)? + mark.len();
        let digits: String = block[at..]
            .chars()
            .take_while(|c| c.is_ascii_digit())
            .collect();
        digits.parse().ok().map(|n| (n, at + digits.len()))
    };
    let blocks: Vec<&str> = md.split("\n\n").collect();
    let picture = |block: &str| -> Option<u32> {
        index(block, IMG_MARK)
            .or_else(|| index(block, LOOSE_MARK))
            .map(|(n, _)| n)
            .filter(|n| shown.contains(n))
    };
    let early = blocks.iter().take(6).any(|b| picture(b).is_some());
    let mut out: Vec<String> = Vec::new();
    for (i, block) in blocks.iter().enumerate() {
        if let Some((n, _)) = index(block, LEAD_MARK) {
            if shown.contains(&n) && !early {
                out.push(format!("![](inkimg:{n})"));
            }
        } else if let Some((n, _)) = index(block, LOOSE_MARK) {
            let late = i + 3 >= blocks.len() || i * 4 >= blocks.len() * 3;
            if shown.contains(&n) && !late {
                out.push(format!("![](inkimg:{n})"));
            }
        } else if let Some((n, _)) = index(block, IMG_MARK) {
            if shown.contains(&n) {
                out.push(format!("![](inkimg:{n})"));
            }
        } else if let Some((n, end)) = index(block, CAP_MARK) {
            let text = block[end..].trim().trim_matches('*').trim();
            if shown.contains(&n) && !text.is_empty() {
                out.push(format!("*{text}*"));
            }
        } else {
            out.push(block.to_string());
        }
    }
    out.join("\n\n")
}

/// The picture at `src`, dithered to fit; None if it can't be had or is too small to be one.
async fn picture(index: u32, src: &str, min_side: u32, fit: Fit) -> Option<Picture> {
    let resp = client()
        .get(src)
        .header(
            "Accept",
            "image/webp,image/png,image/jpeg,image/gif;q=0.8,*/*;q=0.1",
        )
        .send()
        .await
        .ok()?;
    if !resp.status().is_success() {
        tracing::debug!(src, status = resp.status().as_u16(), "picture refused");
        return None;
    }
    let bytes = body(resp, MAX_IMAGE).await.ok()?;
    let src = src.to_string();
    tokio::task::spawn_blocking(move || match image::load_from_memory(&bytes) {
        Ok(img) => dither_to_fit(index, &img.to_luma8(), min_side, fit),
        Err(e) => {
            tracing::debug!(src, "picture unreadable: {e}");
            None
        }
    })
    .await
    .ok()
    .flatten()
}

/// Scaled into `fit` (never up), levels stretched, dithered; None if either side is under
/// `min_side` as served.
pub fn dither_to_fit(index: u32, gray: &GrayImage, min_side: u32, fit: Fit) -> Option<Picture> {
    let (w, h) = gray.dimensions();
    if w < min_side || h < min_side {
        return None;
    }
    let scale = (fit.width as f32 / w as f32)
        .min(fit.height as f32 / h as f32)
        .min(1.0);
    let (tw, th) = (
        ((w as f32 * scale).round() as u32).max(1),
        ((h as f32 * scale).round() as u32).max(1),
    );
    let small = image::imageops::resize(gray, tw, th, FilterType::Triangle);
    let bits = atkinson(&small);
    if bits.len() > MAX_PICTURE_BYTES {
        return None;
    }
    Some(Picture {
        index,
        width: tw,
        height: th,
        bits,
    })
}

/// Atkinson's dither, after stretching the levels between the 1st and 99th percentiles.
fn atkinson(img: &GrayImage) -> Vec<u8> {
    let (w, h) = (img.width() as usize, img.height() as usize);
    let mut hist = [0usize; 256];
    for p in img.pixels() {
        hist[p.0[0] as usize] += 1;
    }
    let total = w * h;
    let level = |share: f32| {
        let want = (total as f32 * share) as usize;
        let mut seen = 0;
        for (v, n) in hist.iter().enumerate() {
            seen += n;
            if seen > want {
                return v as f32;
            }
        }
        255.0
    };
    let (lo, hi) = (level(0.01), level(0.99));
    let span = (hi - lo).max(32.0);
    let mut buf: Vec<f32> = img
        .pixels()
        .map(|p| ((p.0[0] as f32 - lo) / span).clamp(0.0, 1.0))
        .collect();
    let row = w.div_ceil(8);
    let mut bits = vec![0u8; row * h];
    for y in 0..h {
        for x in 0..w {
            let old = buf[y * w + x];
            let ink = old < 0.5;
            if ink {
                bits[y * row + x / 8] |= 0x80 >> (x % 8);
            }
            let err = (old - if ink { 0.0 } else { 1.0 }) / 8.0;
            for (dx, dy) in [(1i32, 0i32), (2, 0), (-1, 1), (0, 1), (1, 1), (0, 2)] {
                let (nx, ny) = (x as i32 + dx, y as i32 + dy);
                if nx >= 0 && (nx as usize) < w && (ny as usize) < h {
                    buf[ny as usize * w + nx as usize] += err;
                }
            }
        }
    }
    bits
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
    // A block that is one link: its text ("[ 26 Comments ](…#comments)").
    let block = match (
        block.strip_prefix('['),
        block.find("]("),
        block.ends_with(')'),
    ) {
        (Some(_), Some(end), true) if !block[1..end].contains('[') => &block[1..end],
        _ => block,
    };
    if block.len() > 300 {
        return false;
    }
    let lower = block.trim().to_lowercase();
    if lower.ends_with("comments")
        && lower
            .trim_end_matches("comments")
            .trim()
            .chars()
            .all(|c| c.is_ascii_digit() || c == ',')
    {
        return true;
    }
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
