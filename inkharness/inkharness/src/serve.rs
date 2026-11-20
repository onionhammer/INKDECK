//! `inkharness serve`: the daemon. Devices connect over TCP (docs/voice-protocol.md), encrypted
//! with the token, send an utterance and get the spoken reply back; programs send text over HTTP (http.rs). A config
//! that can't serve devices (no speech in or out for its model) still serves HTTP. With an
//! [agent] table, both reach the agent sessions too (agent/).

use std::sync::Arc;

use anyhow::{Context, Result};
use tokio::net::TcpListener;

use crate::agent::{self, Agent};
use crate::chat::Chat;
use crate::harness::Harness;
use crate::http;
use crate::place::Place;
use crate::{llm, session, stt, tts};

pub async fn run(place: Place, harness: Harness) -> Result<()> {
    let cfg = &place.config;
    let agent = agent::start(cfg, &harness).await?;
    let devices = match cfg.check() {
        Ok(()) => Some(device_app(&place, &harness, agent.clone())?),
        Err(e) if !cfg.http.is_empty() => {
            tracing::warn!("no device port: {e:#}");
            None
        }
        Err(e) => return Err(e),
    };
    if cfg.token.is_empty() {
        tracing::warn!("no token set: anyone who can reach the ports can use them");
    }
    if cfg.plaintext && devices.is_some() {
        tracing::warn!(
            "plaintext = true: devices on protocol 1 send the token and the audio unencrypted"
        );
    }

    let api = if cfg.http.is_empty() {
        None
    } else {
        let listener = TcpListener::bind(&cfg.http)
            .await
            .with_context(|| format!("HTTP on {}", cfg.http))?;
        let chat = Chat::build(cfg, &harness)?;
        let skills = harness
            .skills
            .names()
            .iter()
            .map(|s| s.to_string())
            .collect();
        let api = Arc::new(http::Api::new(chat, cfg.token.clone(), skills).with_agent(agent));
        Some(tokio::spawn(http::serve(listener, api)))
    };
    let listener = match &devices {
        Some(_) => Some(
            TcpListener::bind(&cfg.listen)
                .await
                .with_context(|| format!("listening on {}", cfg.listen))?,
        ),
        None => None,
    };
    tracing::info!(
        devices = %listener.as_ref().map_or("off", |_| cfg.listen.as_str()),
        http = %if api.is_some() { cfg.http.as_str() } else { "off" },
        model = %cfg.llm.name(),
        tools = harness.tools.defs().len(),
        skills = harness.skills.names().len(),
        agent = cfg.agent.is_some(),
        instructions = place.instructions.len(),
        "inkharness is up"
    );

    let (Some(listener), Some(app)) = (listener, devices) else {
        tokio::signal::ctrl_c().await?;
        return Ok(());
    };
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

/// What a device session needs: speech in and out, the models as configured, and the agent
/// sessions if there are any. Talk turns get the tools `[talk]` leaves them.
fn device_app(
    place: &Place,
    harness: &Harness,
    agent: Option<Arc<Agent>>,
) -> Result<Arc<session::App>> {
    let cfg = &place.config;
    let http = &harness.http;
    let tools = match &cfg.talk.tools {
        Some(patterns) => {
            let tools = harness.tools.only(patterns);
            let names: Vec<&str> = tools.defs().iter().map(|d| d.name.as_str()).collect();
            tracing::info!(?names, "talk tools");
            tools
        }
        None => harness.tools.clone(),
    };
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
    let tts = match &cfg.tts {
        Some(tts) => {
            let mut tts = tts::build(tts, http.clone())?;
            if cfg.cache.tts_entries > 0 {
                tts = Box::new(tts::Cached::new(tts, cfg.cache.tts_entries));
            }
            Some(tts)
        }
        None => None,
    };
    let llm = llm::build(&cfg.llm, http.clone())?;
    // One reply_start per turn: every voice in a reply has to play at the same rate.
    let mut rates: Vec<(&str, u32)> = Vec::new();
    rates.extend(llm.speaks().map(|r| ("[llm] audio_rate", r)));
    if let Some(think) = &think {
        rates.extend(think.llm.speaks().map(|r| ("[think.llm] audio_rate", r)));
    }
    if let (Some(tts), false) = (&tts, rates.is_empty()) {
        rates.push(("[tts]", tts.rate()));
    }
    if let Some((name, rate)) = rates.iter().find(|(_, r)| *r != rates[0].1) {
        anyhow::bail!(
            "{name} is {rate} Hz but {} is {} Hz: a reply plays at one rate",
            rates[0].0,
            rates[0].1
        );
    }
    let app = Arc::new(session::App {
        token: cfg.token.clone(),
        psk: voice_proto::psk(&cfg.token),
        plaintext: cfg.plaintext,
        system_prompt: harness.prompt(cfg, true),
        stt: cfg
            .stt
            .as_ref()
            .map(|stt| stt::build(stt, http.clone()))
            .transpose()?,
        llm,
        tts,
        tools,
        fast_tools,
        think,
        agent,
    });
    if app.think.is_some() && app.tts.is_none() {
        tracing::warn!("no [tts]: the think phrases go unsaid");
    }

    // Synthesise the phrases now, so the first time one is said it comes from the cache.
    if cfg.cache.tts_entries > 0 && app.tts.is_some() {
        let mut phrases = cfg.cache.phrases.clone();
        if let Some(think) = &cfg.think {
            phrases.extend(think.phrases.iter().cloned());
        }
        let app = app.clone();
        tokio::spawn(async move {
            let Some(tts) = &app.tts else { return };
            for phrase in phrases {
                let (tx, mut rx) = tokio::sync::mpsc::channel(8);
                let drain = async { while rx.recv().await.is_some() {} };
                let (result, ()) = tokio::join!(tts.synth(&phrase, tx), drain);
                if let Err(e) = result {
                    tracing::warn!(phrase, "couldn't synthesise a cached phrase: {e:#}");
                }
            }
        });
    }
    Ok(app)
}
