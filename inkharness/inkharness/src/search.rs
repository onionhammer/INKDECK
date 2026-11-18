//! The web_search tool: a query to the backends in `[search]` (SearXNG, Brave, Tavily), in
//! order until one answers, and the top results as text for the model: an answer if the
//! backend wrote one, then each result's title, snippet, date and link.

use std::time::Duration;

use anyhow::{Context, Result, bail};
use serde_json::{Value, json};

use crate::config;
use crate::tools::ToolDef;

pub const TOOL: &str = "web_search";

/// A backend gets this long to answer before the next is tried.
const TIMEOUT: Duration = Duration::from_secs(10);
/// Of each snippet, in characters.
const SNIPPET_CHARS: usize = 400;
/// Of an answer or infobox.
const ANSWER_CHARS: usize = 800;

pub struct Search {
    backends: Vec<Backend>,
    http: reqwest::Client,
}

struct Backend {
    cfg: config::Search,
    /// Brave's and Tavily's.
    key: Option<String>,
}

/// How recent the results must be.
#[derive(Clone, Copy, Debug, PartialEq)]
enum Recent {
    Day,
    Week,
    Month,
    Year,
}

#[derive(Debug, Default, PartialEq)]
struct Found {
    answer: Option<String>,
    hits: Vec<Hit>,
}

#[derive(Debug, PartialEq)]
struct Hit {
    title: String,
    url: String,
    snippet: String,
    date: Option<String>,
}

impl Search {
    /// None if `[search]` names no backend. A hosted backend needs its key.
    pub fn new(cfgs: &[config::Search], http: reqwest::Client) -> Result<Option<Search>> {
        let mut backends = Vec::new();
        for cfg in cfgs {
            let key = match cfg {
                config::Search::Searxng { .. } => None,
                config::Search::Brave { api_key_env, .. }
                | config::Search::Tavily { api_key_env, .. } => {
                    Some(config::lookup(api_key_env).with_context(|| {
                        format!("[search] {} needs a key in ${api_key_env}", cfg.name())
                    })?)
                }
            };
            backends.push(Backend {
                cfg: cfg.clone(),
                key,
            });
        }
        if backends.is_empty() {
            return Ok(None);
        }
        let names: Vec<&str> = backends.iter().map(|b| b.cfg.name()).collect();
        tracing::info!(backends = ?names, "web search");
        Ok(Some(Search { backends, http }))
    }

    pub fn def(&self) -> ToolDef {
        ToolDef {
            name: TOOL.into(),
            description: "Searches the web and returns the top results: each one's title, a \
                          snippet, its date and its link, and sometimes a short answer. Use it \
                          for anything current or that you aren't sure of: news, results and \
                          scores, opening hours, prices, events, people, places. Answer from the \
                          results briefly, in your own words, without reading out links; if they \
                          don't settle it, say what they do say."
                .into(),
            schema: json!({
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "What to search for, as you would type it into a search engine"},
                    "recent": {"type": "string", "enum": ["day", "week", "month", "year"], "description": "Only results from the last day, week, month or year (for news)"}
                },
                "required": ["query"],
                "additionalProperties": false
            }),
        }
    }

    /// The tool. `Err` is a message for the model.
    pub async fn call(&self, input: Value) -> Result<String, String> {
        let Some(query) = input["query"]
            .as_str()
            .map(str::trim)
            .filter(|q| !q.is_empty())
        else {
            return Err("invalid input: query is required".into());
        };
        let recent = match input["recent"].as_str() {
            None => None,
            Some("day") => Some(Recent::Day),
            Some("week") => Some(Recent::Week),
            Some("month") => Some(Recent::Month),
            Some("year") => Some(Recent::Year),
            Some(other) => return Err(format!("invalid input: recent can't be {other:?}")),
        };
        let mut failures = Vec::new();
        for backend in &self.backends {
            let name = backend.cfg.name();
            match tokio::time::timeout(TIMEOUT, self.ask(backend, query, recent)).await {
                Ok(Ok(found)) if !found.hits.is_empty() || found.answer.is_some() => {
                    return Ok(format(query, &found));
                }
                Ok(Ok(_)) => failures.push(format!("{name}: no results")),
                Ok(Err(e)) => {
                    tracing::warn!(backend = name, "search failed: {e:#}");
                    failures.push(format!("{name}: {e:#}"));
                }
                Err(_) => {
                    tracing::warn!(backend = name, "search took over {} s", TIMEOUT.as_secs());
                    failures.push(format!("{name}: no answer in {} s", TIMEOUT.as_secs()));
                }
            }
        }
        if failures.iter().all(|f| f.ends_with("no results")) {
            return Ok(format!("No results for {query:?}."));
        }
        Err(format!("the search failed ({})", failures.join("; ")))
    }

    async fn ask(&self, backend: &Backend, query: &str, recent: Option<Recent>) -> Result<Found> {
        match &backend.cfg {
            config::Search::Searxng {
                url,
                engines,
                language,
                results,
            } => {
                let mut params = vec![("q", query.to_string()), ("format", "json".into())];
                if let Some(engines) = engines {
                    params.push(("engines", engines.clone()));
                }
                if let Some(language) = language {
                    params.push(("language", language.clone()));
                }
                if let Some(recent) = recent {
                    params.push(("time_range", recent.word().into()));
                }
                let url = with_params(&format!("{}/search", url.trim_end_matches('/')), &params)?;
                let request = self.http.get(url);
                let body = send(request, "SearXNG").await.map_err(|e| {
                    if e.to_string().contains("403") {
                        e.context("is json in search.formats in its settings.yml?")
                    } else {
                        e
                    }
                })?;
                Ok(searxng(&body, *results))
            }
            config::Search::Brave {
                country,
                base_url,
                results,
                ..
            } => {
                let mut params = vec![
                    ("q", query.to_string()),
                    ("count", results.min(&20).to_string()),
                ];
                if let Some(country) = country {
                    params.push(("country", country.clone()));
                }
                if let Some(recent) = recent {
                    let freshness = match recent {
                        Recent::Day => "pd",
                        Recent::Week => "pw",
                        Recent::Month => "pm",
                        Recent::Year => "py",
                    };
                    params.push(("freshness", freshness.into()));
                }
                let url = with_params(
                    &format!("{}/web/search", base_url.trim_end_matches('/')),
                    &params,
                )?;
                let request = self
                    .http
                    .get(url)
                    .header("Accept", "application/json")
                    .header(
                        "X-Subscription-Token",
                        backend.key.as_deref().unwrap_or_default(),
                    );
                Ok(brave(&send(request, "Brave").await?, *results))
            }
            config::Search::Tavily {
                depth,
                base_url,
                results,
                ..
            } => {
                let mut body = json!({
                    "query": query,
                    "max_results": results,
                    "search_depth": depth,
                    "include_answer": true,
                });
                if let Some(recent) = recent {
                    body["time_range"] = json!(recent.word());
                }
                let request = self
                    .http
                    .post(format!("{}/search", base_url.trim_end_matches('/')))
                    .bearer_auth(backend.key.as_deref().unwrap_or_default())
                    .json(&body);
                Ok(tavily(&send(request, "Tavily").await?, *results))
            }
        }
    }
}

impl Recent {
    fn word(self) -> &'static str {
        match self {
            Recent::Day => "day",
            Recent::Week => "week",
            Recent::Month => "month",
            Recent::Year => "year",
        }
    }
}

fn with_params(url: &str, params: &[(&str, String)]) -> Result<reqwest::Url> {
    reqwest::Url::parse_with_params(url, params).with_context(|| format!("bad url {url:?}"))
}

async fn send(request: reqwest::RequestBuilder, who: &str) -> Result<Value> {
    let response = request
        .send()
        .await
        .with_context(|| format!("{who} request"))?;
    let status = response.status();
    if !status.is_success() {
        let text = response.text().await.unwrap_or_default();
        bail!("{who} returned {status}: {}", cut(text.trim(), 200));
    }
    response
        .json()
        .await
        .with_context(|| format!("{who} response"))
}

fn searxng(body: &Value, max: usize) -> Found {
    // Answers are strings in older versions, objects with an `answer` in newer ones.
    let answer = body["answers"]
        .as_array()
        .and_then(|a| a.first())
        .and_then(|a| a.as_str().or_else(|| a["answer"].as_str()))
        .map(str::to_string)
        .or_else(|| {
            let infobox = body["infoboxes"].as_array()?.first()?;
            let content = infobox["content"]
                .as_str()
                .filter(|c| !c.trim().is_empty())?;
            Some(match infobox["infobox"].as_str() {
                Some(title) => format!("{title}: {content}"),
                None => content.to_string(),
            })
        });
    let hits = body["results"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|r| {
            Some(Hit {
                title: r["title"].as_str()?.to_string(),
                url: r["url"].as_str()?.to_string(),
                snippet: r["content"].as_str().unwrap_or_default().to_string(),
                date: r["publishedDate"]
                    .as_str()
                    .map(|d| d.chars().take(10).collect()),
            })
        })
        .take(max)
        .collect();
    Found { answer, hits }
}

fn brave(body: &Value, max: usize) -> Found {
    let hits = body["web"]["results"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|r| {
            let mut snippet = r["description"].as_str().unwrap_or_default().to_string();
            for extra in r["extra_snippets"].as_array().into_iter().flatten().take(2) {
                if let Some(extra) = extra.as_str() {
                    snippet.push(' ');
                    snippet.push_str(extra);
                }
            }
            Some(Hit {
                title: r["title"].as_str()?.to_string(),
                url: r["url"].as_str()?.to_string(),
                snippet,
                date: r["page_age"]
                    .as_str()
                    .map(|d| d.chars().take(10).collect())
                    .or_else(|| r["age"].as_str().map(str::to_string)),
            })
        })
        .take(max)
        .collect();
    Found { answer: None, hits }
}

fn tavily(body: &Value, max: usize) -> Found {
    let hits = body["results"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|r| {
            Some(Hit {
                title: r["title"].as_str()?.to_string(),
                url: r["url"].as_str()?.to_string(),
                snippet: r["content"].as_str().unwrap_or_default().to_string(),
                date: r["published_date"].as_str().map(str::to_string),
            })
        })
        .take(max)
        .collect();
    Found {
        answer: body["answer"]
            .as_str()
            .filter(|a| !a.trim().is_empty())
            .map(str::to_string),
        hits,
    }
}

/// The results as the model reads them.
fn format(query: &str, found: &Found) -> String {
    let mut out = format!("Results for {query:?}:\n");
    if let Some(answer) = &found.answer {
        out.push_str(&format!("Answer: {}\n", cut(&plain(answer), ANSWER_CHARS)));
    }
    for (i, hit) in found.hits.iter().enumerate() {
        out.push_str(&format!("{}. {}", i + 1, plain(&hit.title)));
        if let Some(date) = &hit.date {
            out.push_str(&format!(" ({date})"));
        }
        let snippet = plain(&hit.snippet);
        if !snippet.is_empty() {
            out.push_str(&format!("\n   {}", cut(&snippet, SNIPPET_CHARS)));
        }
        out.push_str(&format!("\n   {}\n", hit.url));
    }
    out.trim_end().to_string()
}

/// Text without HTML tags or the common entities, on one line.
fn plain(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut in_tag = false;
    for c in text.chars() {
        match c {
            '<' => in_tag = true,
            '>' if in_tag => in_tag = false,
            _ if in_tag => {}
            _ => out.push(c),
        }
    }
    let out = out
        .replace("&amp;", "&")
        .replace("&quot;", "\"")
        .replace("&#39;", "'")
        .replace("&#x27;", "'")
        .replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&nbsp;", " ");
    out.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn cut(text: &str, max: usize) -> String {
    match text.char_indices().nth(max) {
        Some((i, _)) => format!("{}…", &text[..i]),
        None => text.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};

    use axum::extract::{RawQuery, State};
    use axum::http::{HeaderMap, StatusCode};
    use axum::routing::{get, post};

    use super::*;

    #[test]
    fn each_backends_results() {
        let found = searxng(
            &json!({"query": "q", "answers": [{"answer": "330 m"}], "results": [
                {"title": "Tower <b>facts</b>", "url": "https://a", "content": "It is &quot;tall&quot;.", "publishedDate": "2026-02-13T00:00:37"},
                {"title": "No url"},
                {"title": "Second", "url": "https://b", "content": "", "publishedDate": null}
            ]}),
            5,
        );
        assert_eq!(found.answer.as_deref(), Some("330 m"));
        assert_eq!(found.hits.len(), 2);
        assert_eq!(
            format("eiffel", &found),
            "Results for \"eiffel\":\nAnswer: 330 m\n1. Tower facts (2026-02-13)\n   It is \"tall\".\n   https://a\n2. Second\n   https://b"
        );
        let infobox = searxng(&json!({"answers": ["old style"], "results": []}), 5);
        assert_eq!(infobox.answer.as_deref(), Some("old style"));
        let infobox = searxng(
            &json!({"infoboxes": [{"infobox": "Eiffel Tower", "content": "A tower in Paris."}]}),
            5,
        );
        assert_eq!(
            infobox.answer.as_deref(),
            Some("Eiffel Tower: A tower in Paris.")
        );

        let found = brave(
            &json!({"web": {"results": [{"title": "T", "url": "https://c", "description": "One <strong>two</strong>.",
                     "extra_snippets": ["Three."], "page_age": "2026-11-01T10:00:00"}]}}),
            5,
        );
        assert_eq!(
            format("q", &found),
            "Results for \"q\":\n1. T (2026-11-01)\n   One two. Three.\n   https://c"
        );

        let found = tavily(
            &json!({"answer": "Yes.", "results": [{"title": "T", "url": "https://d", "content": "C"}]}),
            5,
        );
        assert_eq!(
            format("q", &found),
            "Results for \"q\":\nAnswer: Yes.\n1. T\n   C\n   https://d"
        );
        assert_eq!(cut("abcdef", 3), "abc…");
    }

    fn pairs(query: Option<String>) -> Vec<(String, String)> {
        let url = reqwest::Url::parse(&format!("http://x/?{}", query.unwrap_or_default())).unwrap();
        url.query_pairs()
            .map(|(k, v)| (k.into_owned(), v.into_owned()))
            .collect()
    }

    /// A SearXNG that fails, then a Brave and a Tavily that answer; records what each was sent.
    async fn fake(searxng_status: StatusCode) -> (String, Arc<Mutex<Vec<String>>>) {
        type Seen = Arc<Mutex<Vec<String>>>;
        let seen: Seen = Arc::default();
        let app = axum::Router::new()
            .route(
                "/sx/search",
                get(move |State(seen): State<Seen>, RawQuery(q): RawQuery| async move {
                    seen.lock().unwrap().push(format!("searxng {:?}", pairs(q)));
                    (searxng_status, axum::Json(json!({"results": []})))
                }),
            )
            .route(
                "/brave/web/search",
                get(|State(seen): State<Seen>, headers: HeaderMap, RawQuery(q): RawQuery| async move {
                    let key = headers["x-subscription-token"].to_str().unwrap().to_string();
                    seen.lock().unwrap().push(format!("brave {key} {:?}", pairs(q)));
                    axum::Json(json!({"web": {"results": [{"title": "B", "url": "https://b", "description": "From Brave."}]}}))
                }),
            )
            .route(
                "/tavily/search",
                post(|State(seen): State<Seen>, headers: HeaderMap, axum::Json(body): axum::Json<Value>| async move {
                    let auth = headers["authorization"].to_str().unwrap().to_string();
                    seen.lock().unwrap().push(format!("tavily {auth} {body}"));
                    axum::Json(json!({"answer": "From Tavily.", "results": []}))
                }),
            )
            .with_state(seen.clone());
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = format!("http://{}", listener.local_addr().unwrap());
        tokio::spawn(async move { axum::serve(listener, app).await });
        (url, seen)
    }

    fn backends(url: &str) -> Vec<config::Search> {
        toml::from_str::<config::Config>(&format!(
            r#"
            [llm]
            backend = "mock"
            [[search]]
            backend = "searxng"
            url = "{url}/sx/"
            language = "en"
            [[search]]
            backend = "brave"
            api_key_env = "INKHARNESS_TEST_SEARCH_KEY"
            base_url = "{url}/brave"
            results = 3
            [[search]]
            backend = "tavily"
            api_key_env = "INKHARNESS_TEST_SEARCH_KEY"
            base_url = "{url}/tavily"
            "#
        ))
        .unwrap()
        .search
    }

    #[tokio::test]
    async fn the_next_backend_answers_when_one_fails() {
        // SAFETY: tests that read this variable don't run alongside others that change it.
        unsafe { std::env::set_var("INKHARNESS_TEST_SEARCH_KEY", "k") };
        let (url, seen) = fake(StatusCode::FORBIDDEN).await;
        let search = Search::new(&backends(&url), reqwest::Client::new())
            .unwrap()
            .unwrap();
        let out = search
            .call(json!({"query": "news", "recent": "week"}))
            .await;
        assert_eq!(
            out,
            Ok("Results for \"news\":\n1. B\n   From Brave.\n   https://b".into())
        );
        let seen = seen.lock().unwrap().clone();
        assert_eq!(seen.len(), 2, "{seen:?}");
        assert!(
            seen[0].contains(r#"("time_range", "week")"#)
                && seen[0].contains(r#"("language", "en")"#),
            "{seen:?}"
        );
        assert!(
            seen[1].starts_with("brave k ")
                && seen[1].contains(r#"("freshness", "pw")"#)
                && seen[1].contains(r#"("count", "3")"#),
            "{seen:?}"
        );

        assert!(
            search
                .call(json!({}))
                .await
                .unwrap_err()
                .contains("query is required")
        );
        assert!(
            search
                .call(json!({"query": "x", "recent": "hour"}))
                .await
                .is_err()
        );
    }

    #[tokio::test]
    async fn tavily_and_the_answer() {
        // SAFETY: as above.
        unsafe { std::env::set_var("INKHARNESS_TEST_SEARCH_KEY", "k") };
        let (url, seen) = fake(StatusCode::OK).await;
        let only_tavily: Vec<_> = backends(&url)
            .into_iter()
            .filter(|b| b.name() == "tavily")
            .collect();
        let search = Search::new(&only_tavily, reqwest::Client::new())
            .unwrap()
            .unwrap();
        let out = search
            .call(json!({"query": "is it raining in Paris"}))
            .await;
        assert_eq!(
            out,
            Ok("Results for \"is it raining in Paris\":\nAnswer: From Tavily.".into())
        );
        let seen = seen.lock().unwrap().clone();
        assert!(seen[0].starts_with("tavily Bearer k {"), "{seen:?}");
        assert!(seen[0].contains(r#""include_answer":true"#), "{seen:?}");

        // SearXNG answering with nothing, and no key for Brave.
        let sx: Vec<_> = backends(&url).into_iter().take(1).collect();
        let search = Search::new(&sx, reqwest::Client::new()).unwrap().unwrap();
        assert_eq!(
            search.call(json!({"query": "zzz"})).await,
            Ok("No results for \"zzz\".".into())
        );
        let brave = vec![config::Search::Brave {
            api_key_env: "INKHARNESS_TEST_NO_SUCH_KEY".into(),
            country: None,
            base_url: url,
            results: 5,
        }];
        let err = Search::new(&brave, reqwest::Client::new()).err().unwrap();
        assert!(err.to_string().contains("INKHARNESS_TEST_NO_SUCH_KEY"));
    }
}
