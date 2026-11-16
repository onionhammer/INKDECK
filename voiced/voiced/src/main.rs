//! voiced: the voice assistant daemon. Devices connect over TCP (docs/voice-protocol.md),
//! send an utterance, and get the spoken reply back.

mod config;
mod js;
mod llm;
#[cfg(feature = "local")]
mod local;
mod mcp;
mod sentence;
mod session;
mod skills;
mod stt;
mod tools;
mod tts;

use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, Result};
use tokio::net::TcpListener;

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "info".into()),
        )
        .init();

    let path = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "voiced.toml".into());
    let cfg = config::load(&path)?;
    config::load_dotenv(&path);
    // One client for every stage: connections to each service stay open between turns, so a
    // turn doesn't pay for a TLS handshake.
    let http = reqwest::Client::builder()
        .connect_timeout(Duration::from_secs(5))
        .read_timeout(Duration::from_secs(60))
        .pool_idle_timeout(Duration::from_secs(300))
        .tcp_nodelay(true)
        .build()?;
    let mcp = mcp::Mcp::connect(&cfg.mcp).await;
    let base = std::path::Path::new(&path)
        .parent()
        .unwrap_or(std::path::Path::new(""));
    let dirs: Vec<_> = cfg.skills.dirs.iter().map(|d| base.join(d)).collect();
    let skills = Arc::new(skills::Skills::load(&dirs));
    let tools = tools::ToolSet::build(
        cfg.home_assistant.as_ref(),
        &cfg.js,
        mcp,
        skills.clone(),
        http.clone(),
    )?;
    let mut system_prompt = cfg.system_prompt.clone();
    if !tools.has_home() {
        // Some models claim to have done what they were asked even with no tool to do it.
        system_prompt.push_str(
            " Nothing in the home is connected right now, so you can't control any devices; if \
             asked to, say that you can't. You can still answer questions.",
        );
    }
    system_prompt.push_str(&skills.prompt());
    let think = match &cfg.think {
        Some(think) => Some(session::Thinker {
            llm: llm::build(&think.llm, http.clone())?,
            phrases: think.phrases.clone(),
            next: Default::default(),
        }),
        None => None,
    };
    let fast_tools = if think.is_some() {
        tools.with_think()
    } else {
        tools.clone()
    };
    let mut tts = tts::build(&cfg.tts, http.clone())?;
    if cfg.cache.tts_entries > 0 {
        tts = Box::new(tts::Cached::new(tts, cfg.cache.tts_entries));
    }
    let app = Arc::new(session::App {
        token: cfg.token.clone(),
        system_prompt,
        stt: stt::build(&cfg.stt, http.clone())?,
        llm: llm::build(&cfg.llm, http.clone())?,
        tts,
        tools,
        fast_tools,
        think,
    });

    // Synthesise the phrases now, so the first time one is said it comes from the cache.
    if cfg.cache.tts_entries > 0 {
        let mut phrases = cfg.cache.phrases.clone();
        if let Some(think) = &cfg.think {
            phrases.extend(think.phrases.iter().cloned());
        }
        let app = app.clone();
        tokio::spawn(async move {
            for phrase in phrases {
                let (tx, mut rx) = tokio::sync::mpsc::channel(8);
                let drain = async { while rx.recv().await.is_some() {} };
                let (result, ()) = tokio::join!(app.tts.synth(&phrase, tx), drain);
                if let Err(e) = result {
                    tracing::warn!(phrase, "couldn't synthesise a cached phrase: {e:#}");
                }
            }
        });
    }

    let listener = TcpListener::bind(&cfg.listen)
        .await
        .with_context(|| format!("listening on {}", cfg.listen))?;
    tracing::info!(
        listen = %cfg.listen,
        tools = app.tools.defs().len(),
        think = app.think.is_some(),
        "voiced is up"
    );
    if cfg.token.is_empty() {
        tracing::warn!("no token set: any device on the network can connect");
    }

    let mut next_session = 0u64;
    loop {
        let (stream, peer) = tokio::select! {
            accepted = listener.accept() => accepted?,
            _ = tokio::signal::ctrl_c() => return Ok(()),
        };
        next_session += 1;
        let session = format!("s{next_session}");
        let app = app.clone();
        tokio::spawn(async move {
            tracing::info!(%peer, %session, "connected");
            match session::run(stream, app, session.clone()).await {
                Ok(()) => tracing::info!(%session, "disconnected"),
                Err(e) => tracing::warn!(%session, "closed: {e:#}"),
            }
        });
    }
}
