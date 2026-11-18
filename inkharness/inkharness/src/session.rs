//! One device connection: collect an utterance, then run speech-to-text → model → speech,
//! with the stages overlapped so audio starts as soon as the first sentence exists.
//!
//! A model that hears gets the recording instead of a transcript, and speech-to-text, if there
//! is any, runs alongside it for the display and the history. A model that speaks sends its
//! own audio, which goes to the device in order with whatever text-to-speech says.

use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Instant;

use anyhow::{Result, bail};
use bytes::Bytes;
use futures_util::FutureExt;
use tokio::net::TcpStream;
use tokio::net::tcp::OwnedWriteHalf;
use tokio::sync::{Mutex, mpsc};
use tokio::task::JoinHandle;
use voice_proto::{
    ClientMsg, Frame, FrameDecoder, PROTO_VERSION, ServerMsg, read_frame, write_frame,
};

use crate::llm::{self, Llm, Outcome, Piece, Recording, Role, Turn};
use crate::meter;
use crate::sentence::Splitter;
use crate::stt::{Stt, SttSession};
use crate::tools::ToolSet;
use crate::tts::Tts;

pub struct App {
    pub token: String,
    pub system_prompt: String,
    /// None when every model hears the recording itself.
    pub stt: Option<Box<dyn Stt>>,
    pub llm: Box<dyn Llm>,
    /// None when every model speaks its reply itself.
    pub tts: Option<Box<dyn Tts>>,
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

impl App {
    /// The sample rate of the reply's audio: the speaking model's or text-to-speech's (main
    /// checks that they agree).
    pub fn reply_rate(&self) -> u32 {
        self.llm
            .speaks()
            .or_else(|| self.tts.as_ref().map(|t| t.rate()))
            .or_else(|| self.think.as_ref().and_then(|t| t.llm.speaks()))
            .unwrap_or(24000)
    }

    /// True if a model hears the recording, so the session keeps it.
    fn records(&self) -> bool {
        self.llm.hears() || self.think.as_ref().is_some_and(|t| t.llm.hears())
    }
}

/// An utterance as it arrives: fed to speech-to-text, and kept for the models that hear.
struct Utterance {
    stt: Option<Box<dyn SttSession>>,
    pcm: Option<Vec<u8>>,
    bytes: usize,
}

impl Utterance {
    fn start(app: &App, rate: u32) -> Utterance {
        Utterance {
            stt: app.stt.as_ref().map(|stt| stt.start(rate)),
            pcm: app.records().then(Vec::new),
            bytes: 0,
        }
    }

    async fn feed(&mut self, pcm: &[u8]) {
        if let Some(stt) = self.stt.as_mut() {
            stt.feed(pcm).await;
        }
        if let Some(all) = self.pcm.as_mut() {
            all.extend_from_slice(pcm);
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
    let mut utterance: Option<Utterance> = None;
    let mut turn: Option<JoinHandle<()>> = None;

    let result: Result<()> = async {
        while let Some(frame) = read_frame(&mut reader, &mut decoder).await? {
            match frame {
                Frame::Audio(pcm) => {
                    let Some(heard) = utterance.as_mut() else {
                        continue;
                    };
                    heard.bytes += pcm.len();
                    if heard.bytes > MAX_UTTERANCE_SECS * mic_rate as usize * 2 {
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
                    heard.feed(&pcm).await;
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
                            utterance = Some(Utterance::start(&app, mic_rate));
                        }
                        ClientMsg::ListenStop => {
                            let Some(heard) = utterance.take() else {
                                continue;
                            };
                            turn = Some(tokio::spawn(run_turn(
                                app.clone(),
                                heard,
                                mic_rate,
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
    heard: Utterance,
    rate: u32,
    tx: mpsc::Sender<Frame>,
    history: History,
) {
    let turn = turn_inner(&app, heard, rate, &tx, &history);
    if let (Err(e), _) = meter::measure(turn).await {
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

/// What goes to the device's speaker, in order.
enum Say {
    /// For text-to-speech.
    Sentence(String),
    /// The model's own speech.
    Pcm(Bytes),
}

/// Reply audio on its way to the device in frames of AUDIO_CHUNK bytes; the first sends
/// `reply_start`.
struct AudioOut<'a> {
    tx: &'a mpsc::Sender<Frame>,
    rate: u32,
    pending: Vec<u8>,
    /// When the first audio went, in ms from the end of speech.
    first_ms: Option<u64>,
    bytes: usize,
}

impl<'a> AudioOut<'a> {
    fn new(tx: &'a mpsc::Sender<Frame>, rate: u32) -> Self {
        AudioOut {
            tx,
            rate,
            pending: Vec::with_capacity(AUDIO_CHUNK * 2),
            first_ms: None,
            bytes: 0,
        }
    }

    async fn push(&mut self, chunk: &[u8], now_ms: u64) {
        if self.first_ms.is_none() {
            self.first_ms = Some(now_ms);
            send(self.tx, ServerMsg::ReplyStart { rate: self.rate }).await;
        }
        self.pending.extend_from_slice(chunk);
        // Whole samples only; a split one waits for its other byte.
        while self.pending.len() >= AUDIO_CHUNK {
            let rest = self.pending.split_off(AUDIO_CHUNK);
            self.bytes += AUDIO_CHUNK;
            let frame = std::mem::replace(&mut self.pending, rest);
            let _ = self.tx.send(Frame::Audio(frame)).await;
        }
    }

    /// Sends the rest, at the end of a stretch of speech (an odd last byte is dropped).
    async fn flush(&mut self) {
        self.pending.truncate(self.pending.len() & !1);
        if !self.pending.is_empty() {
            self.bytes += self.pending.len();
            let frame = std::mem::take(&mut self.pending);
            let _ = self.tx.send(Frame::Audio(frame)).await;
        }
    }
}

/// Below this peak level (about -54 dBFS) an utterance is taken as silence: a model that
/// hears isn't asked about it.
const SILENCE_PEAK: i32 = 64;

fn silent(pcm: &[u8]) -> bool {
    pcm.chunks_exact(2)
        .all(|s| (i16::from_le_bytes([s[0], s[1]]) as i32).abs() < SILENCE_PEAK)
}

async fn turn_inner(
    app: &App,
    heard: Utterance,
    rate: u32,
    tx: &mpsc::Sender<Frame>,
    history: &History,
) -> Result<()> {
    let started = Instant::now(); // the end of speech: every time below counts from here
    let ms = || started.elapsed().as_millis() as u64;
    let heard_secs = heard.bytes as f32 / (rate as f32 * 2.0);

    // The recording, for the models that hear; none if it is silence.
    let recording = heard
        .pcm
        .as_deref()
        .filter(|pcm| !silent(pcm))
        .map(|pcm| Arc::new(Recording::new(pcm, rate)));
    // The transcript, if there is speech-to-text: the display, a model that reads and the
    // history can all wait for it. Polled in this task, so its request is metered.
    let stt = heard.stt;
    let transcript = async move {
        match stt {
            Some(stt) => stt.finish().await.map_err(|e| format!("{e:#}")),
            None => Ok(String::new()),
        }
    }
    .boxed()
    .shared();

    // A model that reads waits for the transcript; one that hears starts at once.
    let mut stt_ms = None;
    let user = if app.llm.hears() {
        if recording.is_none() {
            tracing::info!(heard_secs, "heard nothing (silence)");
            return Ok(());
        }
        String::new()
    } else {
        let text = transcript.clone().await.map_err(anyhow::Error::msg)?;
        stt_ms = Some(ms());
        send(tx, ServerMsg::Stt { text: text.clone() }).await;
        if text.trim().is_empty() {
            tracing::info!(heard_secs, stt_ms, "heard nothing");
            return Ok(());
        }
        text
    };

    let past = history.lock().await.clone();
    let (piece_tx, mut piece_rx) = mpsc::channel::<Piece>(64);
    let (say_tx, mut say_rx) = mpsc::channel::<Say>(16);

    // For a model that heard the recording: the transcript goes to the display when it comes.
    let show = async {
        if !app.llm.hears() || app.stt.is_none() {
            return;
        }
        match transcript.clone().await {
            Ok(text) => {
                stt_ms = Some(ms());
                send(tx, ServerMsg::Stt { text }).await;
            }
            Err(e) => tracing::warn!("speech-to-text failed: {e}"),
        }
    };

    // The first model answers, or hands the turn to the thinking model: then a phrase covers
    // the wait (unless the first model already said something) and the thinking model's
    // answer follows.
    let mut thought = false;
    let model = async {
        let request = |user, audio, tools| llm::Request {
            system: &app.system_prompt,
            history: &past,
            user,
            audio,
            tools,
        };
        let outcome = app
            .llm
            .respond(
                request(&user, recording.as_deref(), &app.fast_tools),
                piece_tx.clone(),
            )
            .await?;
        let (Outcome::Think { spoke }, Some(think)) = (outcome, &app.think) else {
            return anyhow::Ok(());
        };
        thought = true;
        tracing::info!(spoke, "handing the turn to the thinking model");
        // It hears the recording, or reads what was said (waiting for the transcript if the
        // first model heard it instead).
        let text = if think.llm.hears() || !app.llm.hears() {
            user.clone()
        } else {
            transcript.clone().await.map_err(anyhow::Error::msg)?
        };
        if app.tts.is_some() {
            // The newline ends the phrase as a sentence, so it is spoken at once.
            let phrase = if spoke { "" } else { think.phrase() };
            let _ = piece_tx.send(Piece::Text(format!("{phrase}\n"))).await;
        }
        think
            .llm
            .respond(request(&text, recording.as_deref(), &app.tools), piece_tx)
            .await?;
        Ok(())
    };

    // Text goes to the display as it comes, and to speech a sentence at a time; the model's
    // own speech goes to the speaker after whatever text came before it.
    let mut first_token_ms = None;
    let split = async {
        let mut splitter = Splitter::new();
        let mut reply = String::new();
        while let Some(piece) = piece_rx.recv().await {
            if let Piece::Tool(_) = piece {
                continue; // a device shows only what is said
            }
            first_token_ms.get_or_insert_with(ms);
            match piece {
                Piece::Text(delta) => {
                    send(
                        tx,
                        ServerMsg::Text {
                            delta: delta.clone(),
                        },
                    )
                    .await;
                    reply.push_str(&delta);
                    for sentence in splitter.push(&delta) {
                        let _ = say_tx.send(Say::Sentence(sentence)).await;
                    }
                }
                Piece::Transcript(delta) => {
                    send(
                        tx,
                        ServerMsg::Text {
                            delta: delta.clone(),
                        },
                    )
                    .await;
                    reply.push_str(&delta);
                }
                Piece::Audio(pcm) => {
                    if let Some(rest) = splitter.finish() {
                        let _ = say_tx.send(Say::Sentence(rest)).await;
                    }
                    let _ = say_tx.send(Say::Pcm(pcm)).await;
                }
                Piece::Tool(_) => {}
            }
        }
        if let Some(rest) = splitter.finish() {
            let _ = say_tx.send(Say::Sentence(rest)).await;
        }
        drop(say_tx);
        reply
    };

    let speak = async {
        let mut out = AudioOut::new(tx, app.reply_rate());
        while let Some(say) = say_rx.recv().await {
            match say {
                Say::Pcm(pcm) => out.push(&pcm, ms()).await,
                Say::Sentence(sentence) => {
                    out.flush().await;
                    let Some(tts) = &app.tts else {
                        tracing::warn!(sentence, "no [tts] to say this with");
                        continue;
                    };
                    let (pcm_tx, mut pcm_rx) = mpsc::channel::<Bytes>(8);
                    let synth = tts.synth(&sentence, pcm_tx);
                    let forward = async {
                        while let Some(chunk) = pcm_rx.recv().await {
                            out.push(&chunk, ms()).await;
                        }
                        out.flush().await;
                    };
                    let (result, ()) = tokio::join!(synth, forward);
                    result?;
                }
            }
        }
        out.flush().await;
        anyhow::Ok((out.first_ms, out.bytes))
    };

    let (model_result, reply, speak_result, ()) = tokio::join!(model, split, speak, show);
    let total_ms = ms();
    let (first_audio_ms, audio_bytes) = speak_result.as_ref().copied().unwrap_or((None, 0));

    // The history keeps the transcript, or the recording when there is none.
    let said = if app.llm.hears() {
        transcript.clone().await.unwrap_or_default()
    } else {
        user
    };
    let charges = meter::so_far();
    let (cost, unpriced) = meter::total(&charges);
    tracing::info!(
        heard_secs,
        stt_ms = stt_ms.unwrap_or(0),
        first_token_ms = first_token_ms.unwrap_or(0),
        first_audio_ms = first_audio_ms.unwrap_or(0),
        total_ms,
        thought,
        reply_secs = audio_bytes as f32 / (app.reply_rate() as f32 * 2.0),
        cost,
        unpriced,
        requests = %meter::ids(&charges),
        "turn: {:?} -> {:?}",
        if said.is_empty() { "(heard by the model)" } else { said.as_str() },
        reply.trim()
    );
    model_result?;
    speak_result?;

    let mut history = history.lock().await;
    history.push(Turn {
        role: Role::User,
        audio: if said.is_empty() { recording } else { None },
        text: said,
    });
    if !reply.trim().is_empty() {
        history.push(Turn {
            role: Role::Assistant,
            text: reply.trim().to_string(),
            audio: None,
        });
    } else {
        history.pop(); // a user turn with no reply would break the alternation
    }
    let extra = history.len().saturating_sub(HISTORY_TURNS);
    history.drain(..extra);
    Ok(())
}

#[cfg(test)]
mod tests {
    #[test]
    fn silence_is_near_zero() {
        assert!(super::silent(&[0, 0, 10, 0, 0xf6, 0xff])); // 0, 10, -10
        assert!(!super::silent(&[0, 0, 0x00, 0x01])); // 256
        assert!(!super::silent(&[0x00, 0xff])); // -256
    }
}
