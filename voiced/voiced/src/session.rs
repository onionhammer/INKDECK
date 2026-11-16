//! One device connection: collect an utterance, then run speech-to-text → model → speech,
//! with the stages overlapped so audio starts as soon as the first sentence exists.

use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Instant;

use anyhow::{Result, bail};
use bytes::Bytes;
use tokio::net::TcpStream;
use tokio::net::tcp::OwnedWriteHalf;
use tokio::sync::{Mutex, mpsc};
use tokio::task::JoinHandle;
use voice_proto::{
    ClientMsg, Frame, FrameDecoder, PROTO_VERSION, ServerMsg, read_frame, write_frame,
};

use crate::llm::{self, Llm, Outcome, Role, Turn};
use crate::sentence::Splitter;
use crate::stt::{Stt, SttSession};
use crate::tools::ToolSet;
use crate::tts::Tts;

pub struct App {
    pub token: String,
    pub system_prompt: String,
    pub stt: Box<dyn Stt>,
    pub llm: Box<dyn Llm>,
    pub tts: Box<dyn Tts>,
    /// The home's tools: what the thinking model gets.
    pub tools: ToolSet,
    /// What the first model gets: the home's tools, plus the think tool if there is a
    /// thinking model.
    pub fast_tools: ToolSet,
    pub think: Option<Thinker>,
}

/// The model a turn is handed to when the first one calls the think tool.
pub struct Thinker {
    pub llm: Box<dyn Llm>,
    pub phrases: Vec<String>,
    pub next: AtomicUsize,
}

impl Thinker {
    /// The next phrase to say while it thinks, in turn ("" if there are none).
    fn phrase(&self) -> &str {
        match self.phrases.len() {
            0 => "",
            n => &self.phrases[self.next.fetch_add(1, Ordering::Relaxed) % n],
        }
    }
}

/// Audio frame payload sent to the device: 85 ms at 24 kHz.
const AUDIO_CHUNK: usize = 4096;
/// Earlier turns kept as context (a user turn and its reply count as two).
const HISTORY_TURNS: usize = 12;
/// Longest utterance accepted, in seconds of audio.
const MAX_UTTERANCE_SECS: usize = 60;

type History = Arc<Mutex<Vec<Turn>>>;

pub async fn run(stream: TcpStream, app: Arc<App>, session: String) -> Result<()> {
    stream.set_nodelay(true)?;
    let (mut reader, writer) = stream.into_split();
    // Bounded, so a device that stops reading (its playback buffer is full) holds the
    // pipeline back instead of the daemon buffering the whole reply.
    let (tx, rx) = mpsc::channel::<Frame>(16);
    let writer = tokio::spawn(write_loop(writer, rx));

    let history: History = Arc::default();
    let mut decoder = FrameDecoder::new();
    let mut mic_rate = 0u32; // 0 until hello
    let mut utterance: Option<Box<dyn SttSession>> = None;
    let mut utterance_bytes = 0usize;
    let mut turn: Option<JoinHandle<()>> = None;

    let result: Result<()> = async {
        while let Some(frame) = read_frame(&mut reader, &mut decoder).await? {
            match frame {
                Frame::Audio(pcm) => {
                    let Some(stt) = utterance.as_mut() else {
                        continue;
                    };
                    utterance_bytes += pcm.len();
                    if utterance_bytes > MAX_UTTERANCE_SECS * mic_rate as usize * 2 {
                        utterance = None;
                        send(
                            &tx,
                            ServerMsg::Error {
                                message: "that was too long".into(),
                            },
                        )
                        .await;
                        send(&tx, ServerMsg::ReplyEnd).await;
                        continue;
                    }
                    stt.feed(&pcm).await;
                }
                Frame::Control(json) => {
                    let msg = match serde_json::from_str::<ClientMsg>(&json) {
                        Ok(msg) => msg,
                        Err(e) => {
                            tracing::debug!("ignoring control message {json}: {e}");
                            continue;
                        }
                    };
                    if mic_rate == 0 && !matches!(msg, ClientMsg::Hello { .. }) {
                        bail!("the device must say hello first");
                    }
                    match msg {
                        ClientMsg::Hello {
                            proto,
                            device,
                            token,
                            mic_rate: rate,
                        } => {
                            if proto != PROTO_VERSION {
                                send(
                                    &tx,
                                    ServerMsg::Error {
                                        message: format!("protocol {proto} is not supported"),
                                    },
                                )
                                .await;
                                bail!("device speaks protocol {proto}");
                            }
                            if !app.token.is_empty() && token != app.token {
                                send(
                                    &tx,
                                    ServerMsg::Error {
                                        message: "wrong token".into(),
                                    },
                                )
                                .await;
                                bail!("wrong token from {device}");
                            }
                            if !(8000..=48000).contains(&rate) {
                                bail!("mic rate {rate} is out of range");
                            }
                            mic_rate = rate;
                            tracing::info!(%device, mic_rate, "hello");
                            send(
                                &tx,
                                ServerMsg::Hello {
                                    proto: PROTO_VERSION,
                                    session: session.clone(),
                                },
                            )
                            .await;
                        }
                        ClientMsg::ListenStart => {
                            if cancel(&mut turn) {
                                send(&tx, ServerMsg::ReplyEnd).await;
                            }
                            utterance = Some(app.stt.start(mic_rate));
                            utterance_bytes = 0;
                        }
                        ClientMsg::ListenStop => {
                            let Some(stt) = utterance.take() else {
                                continue;
                            };
                            let secs = utterance_bytes as f32 / (mic_rate as f32 * 2.0);
                            turn = Some(tokio::spawn(run_turn(
                                app.clone(),
                                stt,
                                secs,
                                tx.clone(),
                                history.clone(),
                            )));
                        }
                        ClientMsg::Abort => {
                            utterance = None;
                            cancel(&mut turn);
                            send(&tx, ServerMsg::ReplyEnd).await;
                        }
                    }
                }
            }
        }
        Ok(())
    }
    .await;

    cancel(&mut turn);
    drop(tx);
    let _ = writer.await;
    result
}

/// Stops the turn in progress. True if there was one still running.
fn cancel(turn: &mut Option<JoinHandle<()>>) -> bool {
    match turn.take() {
        Some(handle) if !handle.is_finished() => {
            handle.abort();
            true
        }
        _ => false,
    }
}

async fn write_loop(mut writer: OwnedWriteHalf, mut rx: mpsc::Receiver<Frame>) {
    while let Some(frame) = rx.recv().await {
        if let Err(e) = write_frame(&mut writer, &frame).await {
            tracing::debug!("write failed: {e}");
            break;
        }
    }
}

async fn send(tx: &mpsc::Sender<Frame>, msg: ServerMsg) {
    if let Ok(frame) = Frame::control(&msg) {
        let _ = tx.send(frame).await;
    }
}

async fn run_turn(
    app: Arc<App>,
    stt: Box<dyn SttSession>,
    heard_secs: f32,
    tx: mpsc::Sender<Frame>,
    history: History,
) {
    if let Err(e) = turn_inner(&app, stt, heard_secs, &tx, &history).await {
        tracing::warn!("turn failed: {e:#}");
        send(
            &tx,
            ServerMsg::Error {
                message: format!("{e:#}"),
            },
        )
        .await;
    }
    send(&tx, ServerMsg::ReplyEnd).await;
}

async fn turn_inner(
    app: &App,
    stt: Box<dyn SttSession>,
    heard_secs: f32,
    tx: &mpsc::Sender<Frame>,
    history: &History,
) -> Result<()> {
    let started = Instant::now(); // the end of speech: every time below counts from here
    let ms = || started.elapsed().as_millis() as u64;

    let heard = stt.finish().await?;
    let stt_ms = ms();
    send(
        tx,
        ServerMsg::Stt {
            text: heard.clone(),
        },
    )
    .await;
    if heard.trim().is_empty() {
        tracing::info!(heard_secs, stt_ms, "heard nothing");
        return Ok(());
    }

    let past = history.lock().await.clone();
    let (delta_tx, mut delta_rx) = mpsc::channel::<String>(64);
    let (sentence_tx, mut sentence_rx) = mpsc::channel::<String>(16);

    // The first model answers, or hands the turn to the thinking model: then a phrase covers
    // the wait (unless the first model already said something) and the thinking model's
    // answer follows.
    let mut thought = false;
    let model = async {
        let request = |tools| llm::Request {
            system: &app.system_prompt,
            history: &past,
            user: &heard,
            tools,
        };
        let outcome = app
            .llm
            .respond(request(&app.fast_tools), delta_tx.clone())
            .await?;
        let (Outcome::Think { spoke }, Some(think)) = (outcome, &app.think) else {
            return anyhow::Ok(());
        };
        thought = true;
        tracing::info!(spoke, "handing the turn to the thinking model");
        // The newline ends the phrase as a sentence, so it is spoken at once.
        let phrase = if spoke { "" } else { think.phrase() };
        let _ = delta_tx.send(format!("{phrase}\n")).await;
        think.llm.respond(request(&app.tools), delta_tx).await?;
        Ok(())
    };

    // Text goes to the display as it comes, and to speech a sentence at a time.
    let mut first_token_ms = None;
    let split = async {
        let mut splitter = Splitter::new();
        let mut reply = String::new();
        while let Some(delta) = delta_rx.recv().await {
            first_token_ms.get_or_insert_with(ms);
            send(
                tx,
                ServerMsg::Text {
                    delta: delta.clone(),
                },
            )
            .await;
            reply.push_str(&delta);
            for sentence in splitter.push(&delta) {
                let _ = sentence_tx.send(sentence).await;
            }
        }
        if let Some(rest) = splitter.finish() {
            let _ = sentence_tx.send(rest).await;
        }
        drop(sentence_tx);
        reply
    };

    let mut first_audio_ms = None;
    let mut audio_bytes = 0usize;
    let speak = async {
        while let Some(sentence) = sentence_rx.recv().await {
            let (pcm_tx, mut pcm_rx) = mpsc::channel::<Bytes>(8);
            let synth = app.tts.synth(&sentence, pcm_tx);
            let forward = async {
                let mut pending: Vec<u8> = Vec::with_capacity(AUDIO_CHUNK * 2);
                while let Some(chunk) = pcm_rx.recv().await {
                    if first_audio_ms.is_none() {
                        first_audio_ms = Some(ms());
                        send(
                            tx,
                            ServerMsg::ReplyStart {
                                rate: app.tts.rate(),
                            },
                        )
                        .await;
                    }
                    pending.extend_from_slice(&chunk);
                    // Whole samples only; a split one waits for its other byte.
                    while pending.len() >= AUDIO_CHUNK {
                        let rest = pending.split_off(AUDIO_CHUNK);
                        audio_bytes += AUDIO_CHUNK;
                        let _ = tx
                            .send(Frame::Audio(std::mem::replace(&mut pending, rest)))
                            .await;
                    }
                }
                pending.truncate(pending.len() & !1);
                if !pending.is_empty() {
                    audio_bytes += pending.len();
                    let _ = tx.send(Frame::Audio(pending)).await;
                }
            };
            let (result, ()) = tokio::join!(synth, forward);
            result?;
        }
        anyhow::Ok(())
    };

    let (model_result, reply, speak_result) = tokio::join!(model, split, speak);
    let total_ms = ms();
    tracing::info!(
        heard_secs,
        stt_ms,
        first_token_ms = first_token_ms.unwrap_or(0),
        first_audio_ms = first_audio_ms.unwrap_or(0),
        total_ms,
        thought,
        reply_secs = audio_bytes as f32 / (app.tts.rate() as f32 * 2.0),
        "turn: {heard:?} -> {:?}",
        reply.trim()
    );
    model_result?;
    speak_result?;

    let mut history = history.lock().await;
    history.push(Turn {
        role: Role::User,
        text: heard,
    });
    if !reply.trim().is_empty() {
        history.push(Turn {
            role: Role::Assistant,
            text: reply.trim().to_string(),
        });
    } else {
        history.pop(); // a user turn with no reply would break the alternation
    }
    let extra = history.len().saturating_sub(HISTORY_TURNS);
    history.drain(..extra);
    Ok(())
}
