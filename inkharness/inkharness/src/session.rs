//! One device connection: collect an utterance, then run speech-to-text → model → speech,
//! with the stages overlapped so audio starts as soon as the first sentence exists.
//!
//! A model that hears gets the recording instead of a transcript, and speech-to-text, if there
//! is any, runs alongside it for the display and the history. A model that speaks sends its
//! own audio, which goes to the device in order with whatever text-to-speech says.
//!
//! The tools a device announces in its hello join the session's (device.rs): their calls go
//! down this connection, and their answers come back up it while the turn waits.
//!
//! The connection opens with protocol 2's handshake, which carries the hellos and seals every
//! frame after it (voice_proto's crypto.rs), or, where `plaintext` allows, protocol 1's
//! unencrypted hello.
//!
//! On a daemon with agent sessions (agent/), an encrypted connection can also follow them
//! (docs/voice-protocol.md, "Agent sessions"): unlock with the PIN if one is set, `sync` the
//! list, `open` one, answer, stop and delete them, and talk in agent turns, which start a
//! session or talk into one and get a short spoken reply while the session works on. Talk
//! turns (no mode, or "talk") are what they always were, and never reach the sessions.

use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, Instant};

use anyhow::{Result, anyhow, bail};
use bytes::Bytes;
use futures_util::FutureExt;
use tokio::net::TcpStream;
use tokio::net::tcp::{OwnedReadHalf, OwnedWriteHalf};
use tokio::sync::{Mutex, broadcast, mpsc};
use tokio::task::JoinHandle;
use voice_proto::{
    AgentHello, ClientMsg, DeviceTool, Frame, FrameReader, FrameWriter, Mode, PROTO_PLAINTEXT,
    PROTO_VERSION, Responder, ServerMsg, WRONG_TOKEN,
};

use crate::agent::{self, Agent, Unlock};
use crate::device::Device;
use crate::llm::{self, Llm, Outcome, Piece, Recording, Role, Turn};
use crate::meter;
use crate::sentence::Splitter;
use crate::stt::{Stt, SttSession};
use crate::tools::ToolSet;
use crate::tts::Tts;

pub struct App {
    /// For protocol 1, which sends it in the hello.
    pub token: String,
    /// The key protocol 2's handshake is made with: voice_proto::psk(token).
    pub psk: [u8; 32],
    /// Take devices on protocol 1 too.
    pub plaintext: bool,
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
    /// Agent sessions, when the daemon has an [agent] table.
    pub agent: Option<Arc<Agent>>,
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

/// What kind of turn an utterance makes.
#[derive(Clone)]
enum Kind {
    Talk,
    /// Starts a session, or talks into the one named.
    Agent(Option<String>),
    /// An agent turn that can't be: answered with this error when the utterance ends.
    Refused(String),
}

/// An utterance as it arrives: fed to speech-to-text, and kept for the models that hear.
struct Utterance {
    stt: Option<Box<dyn SttSession>>,
    pcm: Option<Vec<u8>>,
    bytes: usize,
    kind: Kind,
}

impl Utterance {
    fn start(app: &App, rate: u32, kind: Kind) -> Utterance {
        // A refused turn isn't listened to.
        let listen = !matches!(kind, Kind::Refused(_));
        Utterance {
            stt: app
                .stt
                .as_ref()
                .filter(|_| listen)
                .map(|stt| stt.start(rate)),
            pcm: (listen && app.records()).then(Vec::new),
            bytes: 0,
            kind,
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
/// What a request about sessions hears from a daemon without them.
const NO_SESSIONS: &str = "no sessions on this daemon";
/// The most often a followed list or session is sent again as it changes.
const PACE: Duration = Duration::from_secs(1);

type History = Arc<Mutex<Vec<Turn>>>;

/// The session's tools: the app's, with the device's if it has any.
struct Tools {
    /// For the thinking model.
    tools: ToolSet,
    /// For the first model.
    fast_tools: ToolSet,
}

pub async fn run(stream: TcpStream, app: Arc<App>, session: String) -> Result<()> {
    stream.set_nodelay(true)?;
    let (reader, writer) = stream.into_split();
    let mut reader = FrameReader::new(reader);
    let mut writer = FrameWriter::new(writer);
    let hello = open(&mut reader, &mut writer, &app, &session).await?;
    let mic_rate = hello.mic_rate;
    // Sessions only over an encrypted connection: protocol 1 would carry them in the open.
    let agent = app.agent.clone().filter(|_| hello.encrypted);
    let mut unlocked = agent.as_ref().is_some_and(|a| !a.has_pin());

    // Bounded, so a device that stops reading (its playback buffer is full) holds the
    // pipeline back instead of the daemon buffering the whole reply.
    let (tx, rx) = mpsc::channel::<Frame>(16);
    let writer = tokio::spawn(write_loop(writer, rx));
    let (follow_tx, follow_rx) = mpsc::channel::<Follow>(8);
    let follower = agent
        .clone()
        .map(|a| tokio::spawn(follow(a, tx.clone(), follow_rx, unlocked)));

    let history: History = Arc::default();
    let mut device: Option<Arc<Device>> = None;
    let mut tools = Arc::new(Tools {
        tools: app.tools.clone(),
        fast_tools: app.fast_tools.clone(),
    });
    if !hello.tools.is_empty() {
        let board = Arc::new(Device::new(
            hello.tools,
            &app.fast_tools.names(),
            tx.clone(),
        ));
        tools = Arc::new(Tools {
            tools: app.tools.with_device(board.clone()),
            fast_tools: app.fast_tools.with_device(board.clone()),
        });
        device = Some(board);
    }
    let names: Vec<&str> = device
        .iter()
        .flat_map(|d| d.defs())
        .map(|d| d.name.as_str())
        .collect();
    tracing::info!(device = %hello.device, mic_rate, encrypted = hello.encrypted, tools = ?names, "hello");
    if !hello.encrypted {
        tracing::warn!(device = %hello.device, "protocol 1: nothing on this connection is encrypted");
    }
    let mut utterance: Option<Utterance> = None;
    let mut turn: Option<JoinHandle<()>> = None;

    let result: Result<()> = async {
        while let Some(frame) = reader.read().await? {
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
                Frame::Handshake(_) | Frame::Sealed(_) => {
                    bail!("a handshake frame after the hello")
                }
                Frame::Control(json) => {
                    let msg = match serde_json::from_str::<ClientMsg>(&json) {
                        Ok(msg) => msg,
                        Err(e) => {
                            tracing::debug!("ignoring control message {json}: {e}");
                            continue;
                        }
                    };
                    match msg {
                        ClientMsg::Hello { .. } => tracing::debug!("ignoring a second hello"),
                        ClientMsg::ListenStart {
                            mode,
                            session: target,
                        } => {
                            if cancel(&mut turn) {
                                send(&tx, ServerMsg::ReplyEnd).await;
                            }
                            let kind = match (mode, &agent) {
                                (None | Some(Mode::Talk), _) => Kind::Talk,
                                (Some(Mode::Unknown), _) => Kind::Refused("unknown mode".into()),
                                (Some(Mode::Agent), None) => Kind::Refused(NO_SESSIONS.into()),
                                (Some(Mode::Agent), Some(_)) if !unlocked => {
                                    Kind::Refused("locked".into())
                                }
                                (Some(Mode::Agent), Some(a)) => match target {
                                    Some(id) if !a.exists(&id) => {
                                        Kind::Refused(agent::Refusal::NoSuchSession.to_string())
                                    }
                                    target => Kind::Agent(target),
                                },
                            };
                            utterance = Some(Utterance::start(&app, mic_rate, kind));
                        }
                        ClientMsg::ListenStop => {
                            let Some(heard) = utterance.take() else {
                                continue;
                            };
                            let tx = tx.clone();
                            turn = Some(match (heard.kind.clone(), &agent) {
                                (Kind::Agent(target), Some(a)) => tokio::spawn(run_agent_turn(
                                    app.clone(),
                                    a.clone(),
                                    heard,
                                    mic_rate,
                                    tx,
                                    target,
                                )),
                                (Kind::Talk, _) => tokio::spawn(run_turn(
                                    app.clone(),
                                    tools.clone(),
                                    heard,
                                    mic_rate,
                                    tx,
                                    history.clone(),
                                )),
                                (Kind::Refused(message), _) => tokio::spawn(async move {
                                    send(&tx, ServerMsg::Error { message }).await;
                                    send(&tx, ServerMsg::ReplyEnd).await;
                                }),
                                (Kind::Agent(_), None) => unreachable!("refused at listen_start"),
                            });
                        }
                        ClientMsg::Abort => {
                            utterance = None;
                            cancel(&mut turn);
                            send(&tx, ServerMsg::ReplyEnd).await;
                        }
                        ClientMsg::ToolResult { id, text, error } => match &device {
                            Some(device) => device.answer(id, text, error),
                            None => {
                                tracing::debug!(id, "a tool result from a device with no tools")
                            }
                        },
                        ClientMsg::Unlock { pin, ticket } => {
                            let Some(a) = &agent else {
                                refuse(&tx, "unlock", None, NO_SESSIONS).await;
                                continue;
                            };
                            let tried = match (pin, ticket) {
                                (Some(pin), _) => a.unlock_pin(&pin),
                                (None, Some(ticket)) => a.unlock_ticket(&ticket),
                                (None, None) => Unlock::Wrong { wait_s: None },
                            };
                            let ok = matches!(tried, Unlock::Ok { .. });
                            send(&tx, unlocked_msg(tried)).await;
                            if ok && !unlocked {
                                unlocked = true;
                                let _ = follow_tx.send(Follow::Unlocked).await;
                            }
                        }
                        ClientMsg::Sync {} => match &agent {
                            Some(_) => {
                                let _ = follow_tx.send(Follow::Sync).await;
                            }
                            None => refuse(&tx, "sync", None, NO_SESSIONS).await,
                        },
                        ClientMsg::Open { session: id } => match gate(&agent, unlocked) {
                            Ok(a) if a.seen(&id) => {
                                let _ = follow_tx.send(Follow::Open(id)).await;
                            }
                            Ok(_) => {
                                let message = agent::Refusal::NoSuchSession.to_string();
                                refuse(&tx, "open", Some(id), &message).await
                            }
                            Err(message) => refuse(&tx, "open", Some(id), &message).await,
                        },
                        ClientMsg::Close {} => {
                            let _ = follow_tx.send(Follow::Close).await;
                        }
                        ClientMsg::Answer {
                            session: id,
                            ask,
                            choice,
                        } => {
                            let done = gate(&agent, unlocked).and_then(|a| {
                                a.answer(&id, &ask, choice).map_err(|r| r.to_string())
                            });
                            if let Err(message) = done {
                                refuse(&tx, "answer", Some(id), &message).await;
                            }
                        }
                        ClientMsg::Stop { session: id } => {
                            let done = gate(&agent, unlocked)
                                .and_then(|a| a.stop(&id).map_err(|r| r.to_string()));
                            if let Err(message) = done {
                                refuse(&tx, "stop", Some(id), &message).await;
                            }
                        }
                        ClientMsg::Delete { session: id } => {
                            let done = gate(&agent, unlocked)
                                .and_then(|a| a.delete(&id).map_err(|r| r.to_string()));
                            if let Err(message) = done {
                                refuse(&tx, "delete", Some(id), &message).await;
                            }
                        }
                    }
                }
            }
        }
        Ok(())
    }
    .await;

    cancel(&mut turn);
    if let Some(follower) = follower {
        follower.abort();
    }
    // The writer ends once every way to it is gone: the device's tools hold one too.
    drop((tools, device, tx));
    let _ = writer.await;
    result
}

/// The sessions, if this connection may use them now; else why not.
fn gate(agent: &Option<Arc<Agent>>, unlocked: bool) -> Result<&Arc<Agent>, String> {
    match agent {
        None => Err(NO_SESSIONS.into()),
        Some(_) if !unlocked => Err("locked".into()),
        Some(agent) => Ok(agent),
    }
}

async fn refuse(tx: &mpsc::Sender<Frame>, request: &str, session: Option<String>, message: &str) {
    let msg = ServerMsg::Refused {
        request: request.into(),
        session,
        message: message.into(),
    };
    send(tx, msg).await;
}

fn unlocked_msg(tried: Unlock) -> ServerMsg {
    match tried {
        Unlock::Ok { ticket, ttl_s } => ServerMsg::Unlocked {
            ok: true,
            ticket: Some(ticket),
            ttl_s: Some(ttl_s),
            wait_s: None,
        },
        Unlock::Wrong { wait_s } => ServerMsg::Unlocked {
            ok: false,
            ticket: None,
            ttl_s: None,
            wait_s,
        },
        Unlock::Wait { wait_s } => ServerMsg::Unlocked {
            ok: false,
            ticket: None,
            ttl_s: None,
            wait_s: Some(wait_s),
        },
    }
}

/// What a connection asks its follower for.
enum Follow {
    /// Send the list now, and whenever it changes.
    Sync,
    /// Send this session now, and whenever it changes.
    Open(String),
    Close,
    /// The connection may see the sessions now: the list it follows is the full one.
    Unlocked,
}

/// A message sent again as what it shows changes: at most once a PACE, and not when it is the
/// same as the last one sent.
#[derive(Default)]
struct Paced {
    due: Option<tokio::time::Instant>,
    last: Option<tokio::time::Instant>,
    sent: String,
}

impl Paced {
    /// Something changed: send it now, or a PACE after the last one.
    fn soon(&mut self) {
        let now = tokio::time::Instant::now();
        if self.due.is_none() {
            self.due = Some(self.last.map_or(now, |last| (last + PACE).max(now)));
        }
    }

    fn is_due(&self, now: tokio::time::Instant) -> bool {
        self.due.is_some_and(|due| due <= now)
    }

    /// Sends `msg`, unless it is what went last and nobody asked for it (`asked`).
    async fn send(&mut self, tx: &mpsc::Sender<Frame>, msg: &ServerMsg, asked: bool) {
        self.due = None;
        let Ok(json) = serde_json::to_string(msg) else {
            return;
        };
        if !asked && json == self.sent {
            return;
        }
        self.last = Some(tokio::time::Instant::now());
        self.sent.clone_from(&json);
        let _ = tx.send(Frame::Control(json)).await;
    }
}

/// Sends a connection what it follows: after `sync`, the list whenever it changes and each new
/// question as it is asked (none while locked: the list's counts say one is waiting); after
/// `open`, that session whenever it changes, which keeps it seen.
async fn follow(
    agent: Arc<Agent>,
    tx: mpsc::Sender<Frame>,
    mut asks: mpsc::Receiver<Follow>,
    mut unlocked: bool,
) {
    let mut events = agent.subscribe();
    let mut sync = false;
    let mut open: Option<String> = None;
    let (mut list, mut one) = (Paced::default(), Paced::default());
    loop {
        let due = [list.due, one.due].into_iter().flatten().min();
        let wake = async {
            match due {
                Some(due) => tokio::time::sleep_until(due).await,
                None => std::future::pending().await,
            }
        };
        let (mut list_now, mut one_now) = (false, false);
        tokio::select! {
            ask = asks.recv() => match ask {
                None => return,
                Some(Follow::Sync) => {
                    sync = true;
                    list_now = true;
                }
                Some(Follow::Open(id)) => {
                    open = Some(id);
                    one_now = true;
                }
                Some(Follow::Close) => {
                    open = None;
                    one.due = None;
                }
                Some(Follow::Unlocked) => {
                    unlocked = true;
                    list_now = sync;
                }
            },
            event = events.recv() => match event {
                Ok(event) => {
                    if sync {
                        list.soon();
                    }
                    if open.as_deref() == Some(event.session.as_str()) {
                        one.soon();
                    }
                    if event.asked && sync && unlocked
                        && let Some(ask) = agent.question(&event.session)
                    {
                        send(&tx, ServerMsg::Ask(ask)).await;
                    }
                }
                Err(broadcast::error::RecvError::Lagged(_)) => {
                    if sync {
                        list.soon();
                    }
                    if open.is_some() {
                        one.soon();
                    }
                }
                Err(broadcast::error::RecvError::Closed) => return,
            },
            () = wake => {}
        }
        let now = tokio::time::Instant::now();
        if list_now || (sync && list.is_due(now)) {
            let msg = ServerMsg::Sessions(agent.list(!unlocked));
            list.send(&tx, &msg, list_now).await;
        }
        if let Some(id) = open.clone()
            && (one_now || one.is_due(now))
        {
            match agent.view(&id) {
                Some(view) => {
                    one.send(&tx, &ServerMsg::Session(Box::new(view)), one_now)
                        .await;
                    agent.seen(&id);
                }
                None => {
                    // Deleted: nothing more to send for it.
                    open = None;
                    one.due = None;
                }
            }
        }
    }
}

/// What a device said in its hello, once checked.
struct Hello {
    device: String,
    mic_rate: u32,
    tools: Vec<DeviceTool>,
    encrypted: bool,
}

/// Opens the connection and answers the device's hello: protocol 2's handshake, which seals
/// both halves, or protocol 1's unencrypted hello where `plaintext` allows it. A device that
/// can't be taken hears why (an `error`) and the connection ends.
async fn open(
    reader: &mut FrameReader<OwnedReadHalf>,
    writer: &mut FrameWriter<OwnedWriteHalf>,
    app: &App,
    session: &str,
) -> Result<Hello> {
    let refusal = |message: String| Frame::control(&ServerMsg::Error { message });
    match reader.read().await? {
        Some(Frame::Handshake(msg1)) => {
            let mut responder = Responder::new(&app.psk);
            let json = match responder.hello(&msg1) {
                Ok(json) => json,
                Err(e) => {
                    // Unencrypted, as there is no key to seal it with. It says only that the
                    // tokens differ, which the device could tell from the connection closing.
                    writer.write(&refusal(WRONG_TOKEN.into())?).await?;
                    bail!("{e}");
                }
            };
            // The answer rides in message 2, so a refusal is encrypted too.
            let hello = check_hello(&json, PROTO_VERSION);
            let agent: Option<AgentHello> = app
                .agent
                .as_ref()
                .map(|agent| agent.hello(!agent.has_pin()));
            let answer = match &hello {
                Ok(_) => ServerMsg::Hello {
                    proto: PROTO_VERSION,
                    session: session.into(),
                    agent,
                },
                Err(e) => ServerMsg::Error {
                    message: format!("{e:#}"),
                },
            };
            let (msg2, keys) = responder.answer(&answer)?;
            writer.write(&msg2).await?;
            let (hello, _) = hello?;
            let (sealer, opener) = keys.split();
            writer.seal(sealer);
            reader.seal(opener);
            Ok(hello)
        }
        Some(Frame::Control(json)) => {
            let hello = if app.plaintext {
                check_hello(&json, PROTO_PLAINTEXT).and_then(|(hello, token)| {
                    if !app.token.is_empty() && token != app.token {
                        bail!("{WRONG_TOKEN}");
                    }
                    Ok(Hello {
                        encrypted: false,
                        ..hello
                    })
                })
            } else {
                Err(anyhow!(
                    "this daemon takes encrypted connections only (protocol {PROTO_VERSION}): update the device"
                ))
            };
            match hello {
                Ok(hello) => {
                    let answer = ServerMsg::Hello {
                        proto: PROTO_PLAINTEXT,
                        session: session.into(),
                        agent: None,
                    };
                    writer.write(&Frame::control(&answer)?).await?;
                    Ok(hello)
                }
                Err(e) => {
                    writer.write(&refusal(format!("{e:#}"))?).await?;
                    Err(e)
                }
            }
        }
        Some(_) => bail!("the device must say hello first"),
        None => bail!("closed before saying hello"),
    }
}

/// The hello in `json`, if it is one in protocol `proto` with a mic rate that will do, and
/// the token it carries (protocol 1's).
fn check_hello(json: &str, proto: u32) -> Result<(Hello, String)> {
    let Ok(ClientMsg::Hello {
        proto: theirs,
        device,
        token,
        mic_rate,
        tools,
    }) = serde_json::from_str::<ClientMsg>(json)
    else {
        bail!("the device must say hello first");
    };
    if theirs != proto {
        bail!("protocol {theirs} is not supported");
    }
    if !(8000..=48000).contains(&mic_rate) {
        bail!("mic rate {mic_rate} is out of range");
    }
    let hello = Hello {
        device,
        mic_rate,
        tools,
        encrypted: true,
    };
    Ok((hello, token))
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

async fn write_loop(mut writer: FrameWriter<OwnedWriteHalf>, mut rx: mpsc::Receiver<Frame>) {
    while let Some(frame) = rx.recv().await {
        if let Err(e) = writer.write(&frame).await {
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
    tools: Arc<Tools>,
    heard: Utterance,
    rate: u32,
    tx: mpsc::Sender<Frame>,
    history: History,
) {
    let turn = turn_inner(&app, &tools, heard, rate, &tx, &history);
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
    pcm.as_chunks::<2>()
        .0
        .iter()
        .all(|s| (i16::from_le_bytes(*s) as i32).abs() < SILENCE_PEAK)
}

async fn turn_inner(
    app: &App,
    tools: &Tools,
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
    let (piece_tx, piece_rx) = mpsc::channel::<Piece>(64);

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
            rounds: llm::MAX_ROUNDS,
            steer: None,
        };
        let outcome = app
            .llm
            .respond(
                request(&user, recording.as_deref(), &tools.fast_tools),
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
            .respond(request(&text, recording.as_deref(), &tools.tools), piece_tx)
            .await?;
        Ok(())
    };

    let (model_result, spoken, ()) = tokio::join!(model, speak(app, tx, &ms, piece_rx), show);
    let total_ms = ms();
    let Spoken {
        reply,
        first_token_ms,
        first_audio_ms,
        audio_bytes,
        result: speak_result,
    } = spoken;

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

/// What came of playing a reply on the device.
struct Spoken {
    /// Its text.
    reply: String,
    /// When the first of it came, and when its first audio went (ms from the turn's clock).
    first_token_ms: Option<u64>,
    first_audio_ms: Option<u64>,
    audio_bytes: usize,
    /// Whether text-to-speech managed it.
    result: Result<()>,
}

/// Plays a model's reply on the device as it comes: its text goes to the display, and to
/// speech a sentence at a time; the model's own speech goes to the speaker after whatever text
/// came before it.
async fn speak(
    app: &App,
    tx: &mpsc::Sender<Frame>,
    ms: &(dyn Fn() -> u64 + Sync),
    mut piece_rx: mpsc::Receiver<Piece>,
) -> Spoken {
    let (say_tx, mut say_rx) = mpsc::channel::<Say>(16);
    let mut first_token_ms = None;
    let split = async {
        let mut splitter = Splitter::new();
        let mut reply = String::new();
        while let Some(piece) = piece_rx.recv().await {
            if let Piece::Tool(..) = piece {
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
                Piece::Tool(..) => {}
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

    let (reply, speak_result) = tokio::join!(split, speak);
    let (first_audio_ms, audio_bytes) = speak_result.as_ref().copied().unwrap_or((None, 0));
    Spoken {
        reply,
        first_token_ms,
        first_audio_ms,
        audio_bytes,
        result: speak_result.map(|_| ()),
    }
}

async fn run_agent_turn(
    app: Arc<App>,
    agent: Arc<Agent>,
    heard: Utterance,
    rate: u32,
    tx: mpsc::Sender<Frame>,
    target: Option<String>,
) {
    let turn = agent_turn(&app, &agent, heard, rate, &tx, target);
    if let (Err(e), _) = meter::measure(turn).await {
        tracing::warn!("agent turn failed: {e:#}");
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

/// An agent turn: what was said starts a session or goes into the one named, the device hears
/// which (`started`), and then a short spoken reply while the session works on.
async fn agent_turn(
    app: &App,
    agent: &Arc<Agent>,
    heard: Utterance,
    rate: u32,
    tx: &mpsc::Sender<Frame>,
    target: Option<String>,
) -> Result<()> {
    let started = Instant::now(); // the end of speech
    let ms = || started.elapsed().as_millis() as u64;
    let text = written_down(app, heard, rate).await?;
    let stt_ms = ms();
    send(tx, ServerMsg::Stt { text: text.clone() }).await;
    let text = text.trim();
    if text.is_empty() {
        tracing::info!(stt_ms, "agent turn: heard nothing");
        return Ok(());
    }
    let (id, said) = match target {
        None => (agent.start(text), None),
        Some(id) => {
            let said = agent.say(&id, text).map_err(|r| anyhow!("{r}"))?;
            (id, Some(said))
        }
    };
    send(
        tx,
        ServerMsg::Started {
            session: id.clone(),
        },
    )
    .await;

    let request = agent::voice_request(text, said.as_ref());
    let no_tools = app.tools.only(&[]);
    let (piece_tx, piece_rx) = mpsc::channel::<Piece>(64);
    let model = app.llm.respond(
        llm::Request {
            system: agent::VOICE_PROMPT,
            history: &[],
            user: &request,
            audio: None,
            tools: &no_tools,
            rounds: 1,
            steer: None,
        },
        piece_tx,
    );
    let (model_result, spoken) = tokio::join!(model, speak(app, tx, &ms, piece_rx));
    tracing::info!(
        session = %id,
        stt_ms,
        first_audio_ms = spoken.first_audio_ms.unwrap_or(0),
        total_ms = ms(),
        "agent turn: {text:?} -> {:?}",
        spoken.reply.trim()
    );
    model_result?;
    spoken.result
}

/// What was said, as text: from speech-to-text, or, with none, written down by the model that
/// heard it (a session needs the words).
async fn written_down(app: &App, heard: Utterance, rate: u32) -> Result<String> {
    if let Some(stt) = heard.stt {
        return stt.finish().await;
    }
    let Some(pcm) = heard.pcm.filter(|pcm| !silent(pcm)) else {
        return Ok(String::new());
    };
    let recording = Recording::new(&pcm, rate);
    let no_tools = app.tools.only(&[]);
    let (piece_tx, mut piece_rx) = mpsc::channel::<Piece>(64);
    let request = llm::Request {
        system: "Write down exactly what the speaker says, word for word, in the language they \
                 speak, and nothing else: no answer, no quotation marks, no notes.",
        history: &[],
        user: "",
        audio: Some(&recording),
        tools: &no_tools,
        rounds: 1,
        steer: None,
    };
    let collect = async {
        let mut text = String::new();
        while let Some(piece) = piece_rx.recv().await {
            if let Piece::Text(t) | Piece::Transcript(t) = piece {
                text.push_str(&t);
            }
        }
        text
    };
    let (result, text) = tokio::join!(app.llm.respond(request, piece_tx), collect);
    result?;
    Ok(text.trim().to_string())
}

#[cfg(test)]
mod tests {
    use async_trait::async_trait;
    use serde_json::json;
    use tokio::net::tcp::{OwnedReadHalf, OwnedWriteHalf};
    use voice_proto::{DeviceTool, Initiator, psk};

    use voice_proto::State;

    use super::*;
    use crate::config;

    #[test]
    fn silence_is_near_zero() {
        assert!(super::silent(&[0, 0, 10, 0, 0xf6, 0xff])); // 0, 10, -10
        assert!(!super::silent(&[0, 0, 0x00, 0x01])); // 256
        assert!(!super::silent(&[0x00, 0xff])); // -256
    }

    /// Hears the recording, opens a book on the board in a script, and says what that gave.
    struct OpensABook;

    #[async_trait]
    impl Llm for OpensABook {
        fn hears(&self) -> bool {
            true
        }

        async fn respond(
            &self,
            request: llm::Request<'_>,
            out: mpsc::Sender<Piece>,
        ) -> Result<Outcome> {
            let names: Vec<&str> = request
                .tools
                .defs()
                .iter()
                .map(|d| d.name.as_str())
                .collect();
            assert!(names.contains(&"open_book"), "{names:?}");
            let code = r#"tools.open_book({path: "/sdcard/Books/Dune.epub"})"#;
            let result = request.tools.call("run_js", json!({"code": code})).await;
            out.send(Piece::Text(format!("{result:?}"))).await?;
            Ok(Outcome::Done)
        }
    }

    /// A daemon with this token, serving one connection; its address.
    async fn daemon(token: &str, plaintext: bool) -> std::net::SocketAddr {
        let tools = ToolSet::build(
            None,
            &config::Js::default(),
            Default::default(),
            Default::default(),
            reqwest::Client::new(),
        )
        .unwrap();
        let app = Arc::new(App {
            token: token.into(),
            psk: psk(token),
            plaintext,
            system_prompt: String::new(),
            stt: None,
            llm: Box::new(OpensABook),
            tts: None,
            fast_tools: tools.clone(),
            tools,
            think: None,
            agent: None,
        });
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        tokio::spawn(async move {
            let (stream, _) = listener.accept().await.unwrap();
            run(stream, app, "s1".into()).await
        });
        addr
    }

    fn hello(proto: u32, token: &str, tools: Vec<DeviceTool>) -> ClientMsg {
        ClientMsg::Hello {
            proto,
            device: "test".into(),
            token: token.into(),
            mic_rate: 16000,
            tools,
        }
    }

    type Board = (FrameReader<OwnedReadHalf>, FrameWriter<OwnedWriteHalf>);

    fn halves(stream: TcpStream) -> Board {
        let (reader, writer) = stream.into_split();
        (FrameReader::new(reader), FrameWriter::new(writer))
    }

    /// The daemon's answer to a board's handshake made with `token`: its hello or error, and
    /// the sealed connection if the handshake went through.
    async fn handshake(
        addr: std::net::SocketAddr,
        token: &str,
        msg: ClientMsg,
    ) -> (ServerMsg, Option<Board>) {
        let (mut reader, mut writer) = halves(TcpStream::connect(addr).await.unwrap());
        let mut initiator = Initiator::new(&psk(token));
        writer.write(&initiator.hello(&msg).unwrap()).await.unwrap();
        match reader.read().await.unwrap() {
            Some(Frame::Handshake(msg2)) => {
                let (json, keys) = initiator.finish(&msg2).unwrap();
                let (sealer, opener) = keys.split();
                reader.seal(opener);
                writer.seal(sealer);
                (serde_json::from_str(&json).unwrap(), Some((reader, writer)))
            }
            Some(Frame::Control(json)) => {
                assert!(
                    reader.read().await.unwrap().is_none(),
                    "the daemon hangs up"
                );
                (serde_json::from_str(&json).unwrap(), None)
            }
            other => panic!("expected a handshake, got {other:?}"),
        }
    }

    #[tokio::test]
    async fn the_model_calls_the_boards_tools() {
        let addr = daemon("t", false).await;
        let tools = vec![DeviceTool {
            name: "open_book".into(),
            description: "Opens a book on the board.".into(),
            input_schema: json!({"type": "object", "properties": {"path": {"type": "string"}}}),
        }];
        let (answer, board) = handshake(addr, "t", hello(PROTO_VERSION, "", tools)).await;
        assert_eq!(
            answer,
            ServerMsg::Hello {
                proto: PROTO_VERSION,
                session: "s1".into(),
                agent: None,
            }
        );
        let (mut reader, mut writer) = board.unwrap();
        writer
            .write(
                &Frame::control(&ClientMsg::ListenStart {
                    mode: None,
                    session: None,
                })
                .unwrap(),
            )
            .await
            .unwrap();
        let speech: Vec<u8> = (0..1600)
            .flat_map(|i| ((i % 64) as i16 * 100).to_le_bytes())
            .collect();
        writer.write(&Frame::Audio(speech)).await.unwrap();
        writer
            .write(&Frame::control(&ClientMsg::ListenStop).unwrap())
            .await
            .unwrap();

        let mut said = String::new();
        loop {
            let Some(Frame::Control(json)) = reader.read().await.unwrap() else {
                continue;
            };
            match serde_json::from_str::<ServerMsg>(&json).unwrap() {
                ServerMsg::ToolCall { id, name, input } => {
                    assert_eq!(name, "open_book");
                    assert_eq!(input, json!({"path": "/sdcard/Books/Dune.epub"}));
                    let answer = ClientMsg::ToolResult {
                        id,
                        text: "Opening Dune once the reply has played.".into(),
                        error: false,
                    };
                    writer
                        .write(&Frame::control(&answer).unwrap())
                        .await
                        .unwrap();
                }
                ServerMsg::Text { delta } => said.push_str(&delta),
                ServerMsg::ReplyEnd => break,
                ServerMsg::Error { message } => panic!("{message}"),
                _ => {}
            }
        }
        assert_eq!(said, r#"Ok("=> Opening Dune once the reply has played.")"#);
    }

    #[tokio::test]
    async fn a_board_with_tools_that_leaves_ends_its_connection() {
        let tools = ToolSet::build(
            None,
            &config::Js::default(),
            Default::default(),
            Default::default(),
            reqwest::Client::new(),
        )
        .unwrap();
        let app = Arc::new(App {
            token: "t".into(),
            psk: psk("t"),
            plaintext: false,
            system_prompt: String::new(),
            stt: None,
            llm: Box::new(OpensABook),
            tts: None,
            fast_tools: tools.clone(),
            tools,
            think: None,
            agent: None,
        });
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        let connection = tokio::spawn(async move {
            let (stream, _) = listener.accept().await.unwrap();
            run(stream, app, "s1".into()).await
        });
        let tools = vec![DeviceTool {
            name: "open_book".into(),
            description: "Opens a book on the board.".into(),
            input_schema: json!({"type": "object"}),
        }];
        let (_, board) = handshake(addr, "t", hello(PROTO_VERSION, "", tools)).await;
        drop(board);
        let ended = tokio::time::timeout(Duration::from_secs(2), connection).await;
        assert!(ended.is_ok(), "the connection's task outlived the board");
    }

    #[tokio::test]
    async fn a_board_with_another_token_hears_so() {
        let addr = daemon("t", false).await;
        let (answer, board) = handshake(addr, "not t", hello(PROTO_VERSION, "", vec![])).await;
        assert_eq!(
            answer,
            ServerMsg::Error {
                message: WRONG_TOKEN.into()
            }
        );
        assert!(board.is_none());
    }

    #[tokio::test]
    async fn a_hello_it_cant_take_is_refused_in_message_2() {
        let addr = daemon("t", false).await;
        let (answer, _) = handshake(addr, "t", hello(3, "", vec![])).await;
        assert_eq!(
            answer,
            ServerMsg::Error {
                message: "protocol 3 is not supported".into()
            }
        );
    }

    /// A daemon with agent sessions behind the PIN 1234, whose speech-to-text always hears
    /// `heard`; its address and the agent.
    async fn agent_daemon(name: &str, heard: &str) -> (std::net::SocketAddr, Arc<Agent>) {
        let dir = crate::skills::tests::scratch(name);
        let tools = ToolSet::build(
            None,
            &config::Js::default(),
            Default::default(),
            Default::default(),
            reqwest::Client::new(),
        )
        .unwrap();
        let mut cfg = crate::agent::tests::cfg(&dir);
        cfg.pin_sha256 = Some(agent::sha256_hex("1234"));
        let http = reqwest::Client::new();
        let agent = Agent::open(
            &cfg,
            agent::model(&config::Llm::Mock, http.clone()).unwrap(),
            tools.clone(),
            agent::prompt(),
        )
        .unwrap();
        let app = Arc::new(App {
            token: "t".into(),
            psk: psk("t"),
            plaintext: false,
            system_prompt: String::new(),
            stt: Some(
                crate::stt::build(&config::Stt::Mock { text: heard.into() }, http.clone()).unwrap(),
            ),
            llm: llm::build(&config::Llm::Mock, http.clone()).unwrap(),
            tts: Some(crate::tts::build(&config::Tts::Mock, http).unwrap()),
            fast_tools: tools.clone(),
            tools,
            think: None,
            agent: Some(agent.clone()),
        });
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        tokio::spawn(async move {
            let mut n = 0;
            loop {
                let (stream, _) = listener.accept().await.unwrap();
                n += 1;
                tokio::spawn(run(stream, app.clone(), format!("s{n}")));
            }
        });
        (addr, agent)
    }

    async fn put(writer: &mut FrameWriter<OwnedWriteHalf>, msg: ClientMsg) {
        writer.write(&Frame::control(&msg).unwrap()).await.unwrap();
    }

    /// The next control message, skipping audio.
    async fn next(reader: &mut FrameReader<OwnedReadHalf>) -> ServerMsg {
        loop {
            let frame = tokio::time::timeout(Duration::from_secs(10), reader.read())
                .await
                .expect("the daemon went quiet")
                .unwrap()
                .expect("the daemon hung up");
            if let Frame::Control(json) = frame {
                return serde_json::from_str(&json).unwrap();
            }
        }
    }

    /// Messages until one `until` takes, which it returns; the ones before it too.
    async fn until(
        reader: &mut FrameReader<OwnedReadHalf>,
        mut until: impl FnMut(&ServerMsg) -> bool,
    ) -> (ServerMsg, Vec<ServerMsg>) {
        let mut before = Vec::new();
        loop {
            let msg = next(reader).await;
            if until(&msg) {
                return (msg, before);
            }
            before.push(msg);
        }
    }

    /// Says something: an utterance of a little noise in the given mode.
    async fn utter(
        writer: &mut FrameWriter<OwnedWriteHalf>,
        mode: Option<Mode>,
        session: Option<&str>,
    ) {
        let start = ClientMsg::ListenStart {
            mode,
            session: session.map(str::to_string),
        };
        put(writer, start).await;
        let speech: Vec<u8> = (0..1600)
            .flat_map(|i| ((i % 64) as i16 * 100).to_le_bytes())
            .collect();
        writer.write(&Frame::Audio(speech)).await.unwrap();
        put(writer, ClientMsg::ListenStop).await;
    }

    #[tokio::test]
    async fn a_board_unlocks_follows_and_answers_its_sessions() {
        let (addr, agent) = agent_daemon("conn-agent", "find a plumber and ask me which").await;
        let (answer, board) = handshake(addr, "t", hello(PROTO_VERSION, "", vec![])).await;
        assert_eq!(
            answer,
            ServerMsg::Hello {
                proto: PROTO_VERSION,
                session: "s1".into(),
                agent: Some(AgentHello {
                    place: "HOME".into(),
                    pin: true,
                    unlocked: false
                }),
            }
        );
        let (mut reader, mut writer) = board.unwrap();

        // Locked: the counts only, and nothing else about the sessions.
        put(&mut writer, ClientMsg::Sync {}).await;
        let ServerMsg::Sessions(list) = next(&mut reader).await else {
            panic!("no list")
        };
        assert!(list.locked && list.sessions.is_empty());
        put(
            &mut writer,
            ClientMsg::Open {
                session: "k7q2".into(),
            },
        )
        .await;
        assert_eq!(
            next(&mut reader).await,
            ServerMsg::Refused {
                request: "open".into(),
                session: Some("k7q2".into()),
                message: "locked".into()
            }
        );
        utter(&mut writer, Some(Mode::Agent), None).await;
        assert_eq!(
            next(&mut reader).await,
            ServerMsg::Error {
                message: "locked".into()
            }
        );
        assert_eq!(next(&mut reader).await, ServerMsg::ReplyEnd);
        // A talk turn works as ever.
        utter(&mut writer, None, None).await;
        let (_, before) = until(&mut reader, |m| *m == ServerMsg::ReplyEnd).await;
        assert!(before.contains(&ServerMsg::Stt {
            text: "find a plumber and ask me which".into()
        }));
        assert!(
            !before
                .iter()
                .any(|m| matches!(m, ServerMsg::Started { .. }))
        );

        put(
            &mut writer,
            ClientMsg::Unlock {
                pin: Some("0000".into()),
                ticket: None,
            },
        )
        .await;
        assert_eq!(
            next(&mut reader).await,
            ServerMsg::Unlocked {
                ok: false,
                ticket: None,
                ttl_s: None,
                wait_s: None
            }
        );
        put(
            &mut writer,
            ClientMsg::Unlock {
                pin: Some("1234".into()),
                ticket: None,
            },
        )
        .await;
        let ServerMsg::Unlocked {
            ok: true,
            ticket: Some(ticket),
            ttl_s: Some(3600),
            wait_s: None,
        } = next(&mut reader).await
        else {
            panic!("not unlocked")
        };
        // Following the list, it gets the full one now.
        let ServerMsg::Sessions(list) = next(&mut reader).await else {
            panic!("no list")
        };
        assert!(!list.locked);

        // An agent turn: heard, started, a short spoken reply; the session works on.
        utter(&mut writer, Some(Mode::Agent), None).await;
        let (_, turn) = until(&mut reader, |m| *m == ServerMsg::ReplyEnd).await;
        let id = turn
            .iter()
            .find_map(|m| match m {
                ServerMsg::Started { session } => Some(session.clone()),
                _ => None,
            })
            .expect("started");
        let at = |want: fn(&ServerMsg) -> bool| turn.iter().position(want).unwrap();
        assert!(
            at(|m| matches!(m, ServerMsg::Stt { .. }))
                < at(|m| matches!(m, ServerMsg::Started { .. }))
        );
        assert!(
            at(|m| matches!(m, ServerMsg::Started { .. }))
                < at(|m| matches!(m, ServerMsg::ReplyStart { .. }))
        );
        // Its question comes as it is asked (or came during the turn).
        let ask = match turn.iter().find(|m| matches!(m, ServerMsg::Ask(_))) {
            Some(ServerMsg::Ask(ask)) => ask.clone(),
            _ => match until(&mut reader, |m| matches!(m, ServerMsg::Ask(_)))
                .await
                .0
            {
                ServerMsg::Ask(ask) => ask,
                _ => unreachable!(),
            },
        };
        assert_eq!((ask.session.as_str(), ask.id.as_str()), (id.as_str(), "a1"));
        assert_eq!(ask.question, "Which plumber should I book?");

        put(
            &mut writer,
            ClientMsg::Answer {
                session: id.clone(),
                ask: "a1".into(),
                choice: 0,
            },
        )
        .await;
        let done = |m: &ServerMsg| {
            matches!(m, ServerMsg::Sessions(list)
            if list.sessions.first().is_some_and(|s| s.state == State::Done))
        };
        until(&mut reader, done).await;
        put(
            &mut writer,
            ClientMsg::Answer {
                session: id.clone(),
                ask: "a1".into(),
                choice: 0,
            },
        )
        .await;
        let (refused, _) = until(&mut reader, |m| matches!(m, ServerMsg::Refused { .. })).await;
        assert_eq!(
            refused,
            ServerMsg::Refused {
                request: "answer".into(),
                session: Some(id.clone()),
                message: "already answered".into()
            }
        );
        put(
            &mut writer,
            ClientMsg::Open {
                session: id.clone(),
            },
        )
        .await;
        let (session, _) = until(&mut reader, |m| matches!(m, ServerMsg::Session(_))).await;
        let ServerMsg::Session(view) = session else {
            unreachable!()
        };
        assert_eq!(view.state, State::Done);
        assert!(view.result.unwrap().contains("Riverside"));
        assert!(!agent.list(false).sessions[0].unread, "opened is seen");

        // Talking into it from its screen: the session hears it, and says so.
        utter(&mut writer, Some(Mode::Agent), Some(&id)).await;
        let (_, turn) = until(&mut reader, |m| *m == ServerMsg::ReplyEnd).await;
        assert!(turn.contains(&ServerMsg::Started {
            session: id.clone()
        }));
        utter(&mut writer, Some(Mode::Agent), Some("nope")).await;
        let (_, turn) = until(&mut reader, |m| *m == ServerMsg::ReplyEnd).await;
        assert!(turn.contains(&ServerMsg::Error {
            message: "no such session".into()
        }));

        // A second connection comes back in with the ticket.
        let (_, board) = handshake(addr, "t", hello(PROTO_VERSION, "", vec![])).await;
        let (mut reader2, mut writer2) = board.unwrap();
        put(
            &mut writer2,
            ClientMsg::Unlock {
                pin: None,
                ticket: Some(ticket.clone()),
            },
        )
        .await;
        let ServerMsg::Unlocked {
            ok: true,
            ticket: Some(again),
            ..
        } = next(&mut reader2).await
        else {
            panic!("the ticket didn't unlock")
        };
        assert_eq!(again, ticket);
        put(
            &mut writer2,
            ClientMsg::Delete {
                session: id.clone(),
            },
        )
        .await;
        put(
            &mut writer2,
            ClientMsg::Stop {
                session: id.clone(),
            },
        )
        .await;
        assert_eq!(
            next(&mut reader2).await,
            ServerMsg::Refused {
                request: "stop".into(),
                session: Some(id.clone()),
                message: "no such session".into()
            }
        );
    }

    /// Hears the recording: writes it down when asked to, and otherwise says it will do it.
    struct Hears;

    #[async_trait]
    impl Llm for Hears {
        fn hears(&self) -> bool {
            true
        }

        async fn respond(
            &self,
            request: llm::Request<'_>,
            out: mpsc::Sender<Piece>,
        ) -> Result<Outcome> {
            assert!(request.tools.defs().is_empty(), "no tools for these");
            let said = if request.system.starts_with("Write down") {
                assert!(request.audio.is_some());
                "Find a plumber for Saturday."
            } else {
                assert!(request.user.contains("Find a plumber for Saturday."));
                "I'll find one."
            };
            out.send(Piece::Transcript(said.into())).await?;
            Ok(Outcome::Done)
        }
    }

    #[tokio::test]
    async fn with_no_speech_to_text_the_model_that_hears_writes_it_down() {
        let dir = crate::skills::tests::scratch("conn-hears");
        let tools = ToolSet::build(
            None,
            &config::Js::default(),
            Default::default(),
            Default::default(),
            reqwest::Client::new(),
        )
        .unwrap();
        let agent = Agent::open(
            &crate::agent::tests::cfg(&dir),
            agent::model(&config::Llm::Mock, reqwest::Client::new()).unwrap(),
            tools.clone(),
            agent::prompt(),
        )
        .unwrap();
        let app = Arc::new(App {
            token: "t".into(),
            psk: psk("t"),
            plaintext: false,
            system_prompt: String::new(),
            stt: None,
            llm: Box::new(Hears),
            tts: None,
            fast_tools: tools.clone(),
            tools,
            think: None,
            agent: Some(agent.clone()),
        });
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        tokio::spawn(async move {
            let (stream, _) = listener.accept().await.unwrap();
            run(stream, app, "s1".into()).await
        });
        let (answer, board) = handshake(addr, "t", hello(PROTO_VERSION, "", vec![])).await;
        assert!(matches!(
            answer,
            ServerMsg::Hello {
                agent: Some(AgentHello { unlocked: true, .. }),
                ..
            }
        ));
        let (mut reader, mut writer) = board.unwrap();
        utter(&mut writer, Some(Mode::Agent), None).await;
        let (_, turn) = until(&mut reader, |m| *m == ServerMsg::ReplyEnd).await;
        assert_eq!(
            turn[0],
            ServerMsg::Stt {
                text: "Find a plumber for Saturday.".into()
            }
        );
        let ServerMsg::Started { session } = &turn[1] else {
            panic!("{turn:?}")
        };
        assert!(turn.contains(&ServerMsg::Text {
            delta: "I'll find one.".into()
        }));
        assert_eq!(
            agent.view(session).unwrap().prompt,
            "Find a plumber for Saturday."
        );
    }

    #[tokio::test]
    async fn without_agent_sessions_the_requests_are_refused() {
        let addr = daemon("t", false).await;
        let (answer, board) = handshake(addr, "t", hello(PROTO_VERSION, "", vec![])).await;
        assert!(matches!(answer, ServerMsg::Hello { agent: None, .. }));
        let (mut reader, mut writer) = board.unwrap();
        put(&mut writer, ClientMsg::Sync {}).await;
        assert_eq!(
            next(&mut reader).await,
            ServerMsg::Refused {
                request: "sync".into(),
                session: None,
                message: NO_SESSIONS.into()
            }
        );
        utter(&mut writer, Some(Mode::Agent), None).await;
        assert_eq!(
            next(&mut reader).await,
            ServerMsg::Error {
                message: NO_SESSIONS.into()
            }
        );
        assert_eq!(next(&mut reader).await, ServerMsg::ReplyEnd);
    }

    /// The daemon's answer to a protocol 1 hello.
    async fn plain_hello(addr: std::net::SocketAddr, token: &str) -> ServerMsg {
        let (mut reader, mut writer) = halves(TcpStream::connect(addr).await.unwrap());
        let msg = Frame::control(&hello(PROTO_PLAINTEXT, token, vec![])).unwrap();
        writer.write(&msg).await.unwrap();
        let Some(Frame::Control(json)) = reader.read().await.unwrap() else {
            panic!("no answer")
        };
        serde_json::from_str(&json).unwrap()
    }

    #[tokio::test]
    async fn protocol_1_only_with_plaintext() {
        let refused = plain_hello(daemon("t", false).await, "t").await;
        assert!(
            matches!(&refused, ServerMsg::Error { message } if message.contains("encrypted")),
            "{refused:?}"
        );
        assert_eq!(
            plain_hello(daemon("t", true).await, "t").await,
            ServerMsg::Hello {
                proto: PROTO_PLAINTEXT,
                session: "s1".into(),
                agent: None,
            }
        );
        assert_eq!(
            plain_hello(daemon("t", true).await, "x").await,
            ServerMsg::Error {
                message: WRONG_TOKEN.into()
            }
        );
    }
}
