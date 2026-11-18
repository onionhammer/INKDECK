//! What every front-end shares, built once from the place: one HTTP client, the tools (Home
//! Assistant, run_js, MCP servers, web search, skills) and what the system prompt adds to its
//! base.

use std::sync::Arc;
use std::time::Duration;

use anyhow::Result;

use crate::config::{self, Config};
use crate::mcp::Mcp;
use crate::place::Place;
use crate::search::Search;
use crate::skills::Skills;
use crate::tools::ToolSet;

pub struct Harness {
    /// One client for every stage: connections to each service stay open between turns, so a
    /// turn doesn't pay for a TLS handshake.
    pub http: reqwest::Client,
    pub tools: ToolSet,
    pub mcp: Arc<Mcp>,
    pub skills: Arc<Skills>,
    /// Follows the base prompt: a note when nothing in the home is connected, the
    /// instructions files, the skills.
    additions: String,
}

impl Harness {
    /// `server_logs`: what MCP servers started as commands write to stderr goes to ours.
    pub async fn build(place: &Place, server_logs: bool) -> Result<Harness> {
        let cfg = &place.config;
        let http = reqwest::Client::builder()
            .connect_timeout(Duration::from_secs(5))
            .read_timeout(Duration::from_secs(60))
            .pool_idle_timeout(Duration::from_secs(300))
            .tcp_nodelay(true)
            .build()?;
        let mcp = Mcp::connect(&cfg.mcp, server_logs).await;
        let skills = Arc::new(Skills::load(&place.skill_dirs));
        let mut tools = ToolSet::build(
            cfg.home_assistant.as_ref(),
            &cfg.js,
            mcp.clone(),
            skills.clone(),
            http.clone(),
        )?;
        if let Some(search) = Search::new(&cfg.search, http.clone())? {
            tools = tools.with_search(Arc::new(search));
        }

        let mut additions = String::new();
        if !tools.has_home() {
            // Some models claim to have done what they were asked even with no tool to do it.
            additions.push_str(
                " Nothing in the home (speakers, lights and the like) is connected right now, so \
                 you can't control any of it; if asked to, say that you can't. You can still \
                 answer questions.",
            );
        }
        for (file, text) in &place.instructions {
            let name = file.file_name().unwrap_or_default().to_string_lossy();
            additions.push_str(&format!("\n\nInstructions from {name}:\n\n{text}"));
        }
        additions.push_str(&skills.prompt());
        Ok(Harness {
            http,
            tools,
            mcp,
            skills,
            additions,
        })
    }

    /// The system prompt for replies that are `spoken` (a device) or read.
    pub fn prompt(&self, cfg: &Config, spoken: bool) -> String {
        let base = cfg.system_prompt.clone().unwrap_or_else(|| {
            if spoken {
                config::spoken_prompt()
            } else {
                config::written_prompt()
            }
        });
        base + &self.additions
    }
}
