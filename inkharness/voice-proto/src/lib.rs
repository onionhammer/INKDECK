//! Wire protocol between the device and the voice daemon (docs/voice-protocol.md).
//!
//! One TCP connection. Every frame is `u8 type`, `u16 length` (little-endian), payload.
//! Type 1 is a UTF-8 JSON control message, type 2 is PCM audio (s16le, mono). Protocol 2
//! opens with a Noise handshake (type 3, crypto.rs) keyed from the shared token, and every
//! frame after it is sealed (type 4): a control or audio frame encrypted and authenticated
//! whole. Protocol 1 sends control and audio frames as they are. Frames of any other type are
//! skipped, so later versions can add types.

use serde::{Deserialize, Serialize};

mod crypto;
pub use crypto::{Initiator, Keys, NOISE, Opener, PROLOGUE, Responder, Sealer, psk};

pub const PROTO_VERSION: u32 = 2;
/// The unencrypted protocol: a daemon takes it only when told to (`plaintext = true`).
pub const PROTO_PLAINTEXT: u32 = 1;
pub const DEFAULT_PORT: u16 = 7391;
pub const MAX_PAYLOAD: usize = u16::MAX as usize;
pub const HEADER_LEN: usize = 3;
/// What sealing adds to a frame's payload: its type byte and the 16-byte tag.
pub const SEAL_OVERHEAD: usize = 1 + 16;
/// The largest payload a sealed frame can carry.
pub const MAX_SEALED_PAYLOAD: usize = MAX_PAYLOAD - SEAL_OVERHEAD;

const TYPE_CONTROL: u8 = 1;
const TYPE_AUDIO: u8 = 2;
const TYPE_HANDSHAKE: u8 = 3;
const TYPE_SEALED: u8 = 4;

#[derive(Debug, Clone, PartialEq)]
pub enum Frame {
    Control(String),
    Audio(Vec<u8>),
    /// A Noise handshake message: protocol 2's first frame each way.
    Handshake(Vec<u8>),
    /// A control or audio frame, encrypted. An `Opener` opens it.
    Sealed(Vec<u8>),
}

#[derive(Debug, thiserror::Error)]
pub enum ProtoError {
    #[error("payload of {0} bytes is over the frame limit")]
    TooLong(usize),
    #[error("control frame is not UTF-8")]
    Utf8,
    #[error("control message: {0}")]
    Json(#[from] serde_json::Error),
    /// The handshake message didn't open: the other side's token is different.
    #[error("the handshake failed: the tokens don't match")]
    Handshake,
    #[error("a sealed frame didn't open: it was changed, replayed or reordered")]
    Open,
    #[error("an unencrypted frame after the handshake")]
    NotSealed,
    #[error("only control and audio frames can be sealed")]
    NotSealable,
    #[error("noise: {0}")]
    Noise(#[from] snow::Error),
}

impl Frame {
    pub fn control<T: Serialize>(msg: &T) -> Result<Frame, ProtoError> {
        Ok(Frame::Control(serde_json::to_string(msg)?))
    }

    fn parts(&self) -> (u8, &[u8]) {
        match self {
            Frame::Control(json) => (TYPE_CONTROL, json.as_bytes()),
            Frame::Audio(pcm) => (TYPE_AUDIO, pcm),
            Frame::Handshake(msg) => (TYPE_HANDSHAKE, msg),
            Frame::Sealed(sealed) => (TYPE_SEALED, sealed),
        }
    }

    /// None for a type this version doesn't know.
    fn from_parts(kind: u8, payload: Vec<u8>) -> Result<Option<Frame>, ProtoError> {
        Ok(Some(match kind {
            TYPE_CONTROL => {
                Frame::Control(String::from_utf8(payload).map_err(|_| ProtoError::Utf8)?)
            }
            TYPE_AUDIO => Frame::Audio(payload),
            TYPE_HANDSHAKE => Frame::Handshake(payload),
            TYPE_SEALED => Frame::Sealed(payload),
            _ => return Ok(None),
        }))
    }

    pub fn encode(&self) -> Result<Vec<u8>, ProtoError> {
        let (kind, payload) = self.parts();
        if payload.len() > MAX_PAYLOAD {
            return Err(ProtoError::TooLong(payload.len()));
        }
        let mut out = Vec::with_capacity(HEADER_LEN + payload.len());
        out.push(kind);
        out.extend_from_slice(&(payload.len() as u16).to_le_bytes());
        out.extend_from_slice(payload);
        Ok(out)
    }
}

/// Splits a byte stream back into frames. Feed it whatever the socket returns.
#[derive(Default)]
pub struct FrameDecoder {
    buf: Vec<u8>,
}

impl FrameDecoder {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn push(&mut self, bytes: &[u8]) {
        self.buf.extend_from_slice(bytes);
    }

    /// The next complete frame, or `None` if more bytes are needed.
    pub fn next_frame(&mut self) -> Result<Option<Frame>, ProtoError> {
        loop {
            if self.buf.len() < HEADER_LEN {
                return Ok(None);
            }
            let len = u16::from_le_bytes([self.buf[1], self.buf[2]]) as usize;
            if self.buf.len() < HEADER_LEN + len {
                return Ok(None);
            }
            let kind = self.buf[0];
            let payload: Vec<u8> = self.buf[HEADER_LEN..HEADER_LEN + len].to_vec();
            self.buf.drain(..HEADER_LEN + len);
            if let Some(frame) = Frame::from_parts(kind, payload)? {
                return Ok(Some(frame));
            } // else a type this version doesn't know
        }
    }
}

/// A tool the device runs itself, announced in its hello: the model can call it like any
/// other, and the daemon passes the call on (`tool_call`, `tool_result`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DeviceTool {
    /// Letters, digits, `_` and `-`, up to 64.
    pub name: String,
    pub description: String,
    /// JSON Schema of the input object.
    #[serde(default = "no_input")]
    pub input_schema: serde_json::Value,
}

fn no_input() -> serde_json::Value {
    serde_json::json!({"type": "object", "properties": {}})
}

/// Device → daemon.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ClientMsg {
    Hello {
        proto: u32,
        device: String,
        /// Protocol 1 only. Protocol 2 proves the token in the handshake and leaves this out.
        #[serde(default, skip_serializing_if = "String::is_empty")]
        token: String,
        mic_rate: u32,
        /// What the device can do itself. A daemon from before tools ignores it.
        #[serde(default, skip_serializing_if = "Vec::is_empty")]
        tools: Vec<DeviceTool>,
    },
    /// Audio frames that follow are one utterance. `mode` says what kind of turn it is (talk
    /// when absent, as on boards from before agent sessions); an agent turn may name the
    /// `session` it talks into.
    ListenStart {
        #[serde(default, skip_serializing_if = "Option::is_none")]
        mode: Option<Mode>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        session: Option<String>,
    },
    /// The utterance is complete; the daemon answers.
    ListenStop,
    /// Drop the utterance or the reply in progress. The daemon answers `reply_end`.
    Abort,
    /// The answer to a `tool_call`: what the tool did or found, or why it couldn't.
    ToolResult {
        id: u32,
        text: String,
        #[serde(default)]
        error: bool,
    },
    /// Unlocks the connection's agent sessions with the PIN (digits) or a ticket from an earlier
    /// unlock. Answered with `unlocked`.
    Unlock {
        #[serde(default, skip_serializing_if = "Option::is_none")]
        pin: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        ticket: Option<String>,
    },
    /// Send `sessions` now and whenever the list changes, and `ask` for each new question, for
    /// as long as the connection lasts.
    Sync {},
    /// Send `session` for this one now and whenever it changes, until another `open` or a
    /// `close`. Marks it seen.
    Open { session: String },
    /// Stop sending `session` updates.
    Close {},
    /// Answers a session's pending question with one of its options.
    Answer {
        session: String,
        /// The question's id.
        ask: String,
        /// An index into its options.
        choice: usize,
    },
    /// Stops a working or asking session.
    Stop { session: String },
    /// Deletes a session for good.
    Delete { session: String },
}

/// What kind of turn an utterance is.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Mode {
    /// Today's voice: its own short conversation and the talk tools, never the sessions.
    Talk,
    /// Starts a session, or talks into one.
    Agent,
    /// A mode this version doesn't know.
    #[serde(other)]
    Unknown,
}

/// Daemon → device.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ServerMsg {
    Hello {
        proto: u32,
        session: String,
        /// Present when the daemon has agent sessions.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        agent: Option<AgentHello>,
    },
    /// What the daemon heard.
    Stt {
        text: String,
    },
    /// Audio frames follow at this sample rate.
    ReplyStart {
        rate: u32,
    },
    /// The reply's text as it is generated (for the display); may precede `reply_start`.
    Text {
        delta: String,
    },
    /// The turn is over. May arrive without a `reply_start` (nothing to say, or an abort).
    ReplyEnd,
    /// The model calls one of the device's tools; the device answers `tool_result` with the
    /// same id.
    ToolCall {
        id: u32,
        name: String,
        input: serde_json::Value,
    },
    Error {
        message: String,
    },
    /// The list of agent sessions (docs/voice-protocol.md, "Agent sessions").
    Sessions(SessionList),
    /// One session in full.
    Session(SessionView),
    /// A new question from a session.
    Ask(AskView),
    /// During an agent turn: the session the utterance went to, before `reply_start`.
    Started {
        session: String,
    },
    /// The answer to `unlock`.
    Unlocked {
        ok: bool,
        /// When ok: lets a later connection unlock without the PIN.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        ticket: Option<String>,
        /// When ok: how long the ticket lasts, in seconds.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        ttl_s: Option<u64>,
        /// When further tries are refused for this many seconds (too many wrong PINs).
        #[serde(default, skip_serializing_if = "Option::is_none")]
        wait_s: Option<u64>,
    },
    /// A request that isn't a turn and can't be done.
    Refused {
        /// The refused message's `type`.
        request: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        session: Option<String>,
        message: String,
    },
}

/// The `agent` field of the daemon's hello.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AgentHello {
    /// Where the sessions run, as the board shows it.
    pub place: String,
    /// Whether a PIN guards the sessions.
    pub pin: bool,
    /// Whether this connection may use them now.
    pub unlocked: bool,
}

/// What a session is doing.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum State {
    Working,
    /// Waiting on an answer to its question.
    Ask,
    /// Finished with a result.
    Done,
    /// Finished; the result is meant to be read as a note.
    Note,
    /// Gave up or failed; the result says why.
    Fail,
    /// Stopped by the user.
    Stopped,
    /// A state this version doesn't know (later ones add `watch`, `scheduled`).
    #[serde(other)]
    Unknown,
}

impl State {
    /// Done, note, fail or stopped.
    pub fn finished(self) -> bool {
        matches!(
            self,
            State::Done | State::Note | State::Fail | State::Stopped
        )
    }
}

/// The `sessions` message.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SessionList {
    /// Locked: only the counts and `check_at`, no sessions.
    pub locked: bool,
    pub counts: Counts,
    /// When the board should check in next (Unix seconds), if the daemon wants it to.
    pub check_at: Option<u64>,
    pub sessions: Vec<SessionRow>,
    /// Sessions past the ones listed.
    pub older: usize,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Counts {
    pub ask: usize,
    pub running: usize,
    pub planned: usize,
    pub unread: usize,
}

/// A session in the list.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SessionRow {
    pub id: String,
    pub title: String,
    pub state: State,
    pub place: String,
    /// One line: the question, the step or last tool call, or the result's first line.
    pub sub: String,
    /// The plan's current step, from 1.
    pub step: Option<usize>,
    pub steps: Option<usize>,
    /// 0 to 100, when known.
    pub progress: Option<u8>,
    pub updated: u64,
    pub unread: bool,
}

/// The `session` message: one session in full.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SessionView {
    pub id: String,
    pub title: String,
    pub state: State,
    pub place: String,
    pub prompt: String,
    pub started: u64,
    pub updated: u64,
    pub step: Option<usize>,
    pub steps: Option<usize>,
    pub plan: Option<Vec<PlanItem>>,
    /// Newest first.
    pub log: Vec<LogLine>,
    /// Markdown, for a finished session.
    pub result: Option<String>,
    pub ask: Option<AskView>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PlanItem {
    pub text: String,
    pub state: StepState,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum StepState {
    Done,
    Now,
    Todo,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LogLine {
    pub at: u64,
    pub text: String,
}

/// A session's question: the `ask` message, and a `session`'s `ask` field.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AskView {
    pub session: String,
    pub id: String,
    /// The session's title.
    pub title: String,
    pub question: String,
    pub detail: Option<String>,
    pub options: Vec<AskOption>,
    /// The option the session recommends.
    pub recommend: Option<usize>,
    pub at: u64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AskOption {
    pub title: String,
    /// A short value shown at the right: a price, a time.
    #[serde(default)]
    pub right: Option<String>,
    #[serde(default)]
    pub detail: Option<String>,
}

/// What the daemon answers, unencrypted, to a handshake made with another token.
pub const WRONG_TOKEN: &str = "wrong token";

#[cfg(feature = "tokio")]
mod io {
    use super::{Frame, FrameDecoder, Opener, ProtoError, Sealer};
    use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

    /// Reads until a whole frame is in. `None` when the peer closed the connection. Safe to
    /// drop mid-read (in a `select!`): partial data stays in the decoder.
    pub async fn read_frame<R: AsyncRead + Unpin>(
        reader: &mut R,
        decoder: &mut FrameDecoder,
    ) -> std::io::Result<Option<Frame>> {
        let mut chunk = [0u8; 4096];
        loop {
            if let Some(frame) = decoder.next_frame().map_err(std::io::Error::other)? {
                return Ok(Some(frame));
            }
            let n = reader.read(&mut chunk).await?;
            if n == 0 {
                return Ok(None);
            }
            decoder.push(&chunk[..n]);
        }
    }

    pub async fn write_frame<W: AsyncWrite + Unpin>(
        writer: &mut W,
        frame: &Frame,
    ) -> std::io::Result<()> {
        let bytes = frame.encode().map_err(std::io::Error::other)?;
        writer.write_all(&bytes).await
    }

    /// The receiving half of a connection. Once sealed (after the handshake) it opens every
    /// frame and refuses one that isn't sealed.
    pub struct FrameReader<R> {
        inner: R,
        decoder: FrameDecoder,
        opener: Option<Opener>,
    }

    impl<R: AsyncRead + Unpin> FrameReader<R> {
        pub fn new(inner: R) -> Self {
            FrameReader {
                inner,
                decoder: FrameDecoder::new(),
                opener: None,
            }
        }

        pub fn seal(&mut self, opener: Opener) {
            self.opener = Some(opener);
        }

        /// The next frame, opened if the connection is sealed. `None` when the peer closed
        /// the connection. Safe to drop mid-read, like `read_frame`.
        pub async fn read(&mut self) -> std::io::Result<Option<Frame>> {
            loop {
                let Some(frame) = read_frame(&mut self.inner, &mut self.decoder).await? else {
                    return Ok(None);
                };
                let Some(opener) = self.opener.as_mut() else {
                    return Ok(Some(frame));
                };
                let Frame::Sealed(sealed) = frame else {
                    return Err(std::io::Error::other(ProtoError::NotSealed));
                };
                if let Some(frame) = opener.open(&sealed).map_err(std::io::Error::other)? {
                    return Ok(Some(frame));
                } // else a type this version doesn't know
            }
        }
    }

    /// The sending half of a connection. Once sealed it seals every frame.
    pub struct FrameWriter<W> {
        inner: W,
        sealer: Option<Sealer>,
    }

    impl<W: AsyncWrite + Unpin> FrameWriter<W> {
        pub fn new(inner: W) -> Self {
            FrameWriter {
                inner,
                sealer: None,
            }
        }

        pub fn seal(&mut self, sealer: Sealer) {
            self.sealer = Some(sealer);
        }

        pub async fn write(&mut self, frame: &Frame) -> std::io::Result<()> {
            match self.sealer.as_mut() {
                Some(sealer) => {
                    let sealed = sealer.seal(frame).map_err(std::io::Error::other)?;
                    write_frame(&mut self.inner, &sealed).await
                }
                None => write_frame(&mut self.inner, frame).await,
            }
        }

        pub async fn flush(&mut self) -> std::io::Result<()> {
            self.inner.flush().await
        }
    }
}

#[cfg(feature = "tokio")]
pub use io::{FrameReader, FrameWriter, read_frame, write_frame};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frames_survive_any_split() {
        let frames = vec![
            Frame::control(&ClientMsg::ListenStart {
                mode: None,
                session: None,
            })
            .unwrap(),
            Frame::Audio(vec![1, 2, 3, 4]),
            Frame::Audio(vec![]),
            Frame::Handshake(vec![5; 48]),
            Frame::Sealed(vec![6; 17]),
            Frame::control(&ServerMsg::Text {
                delta: "héllo".into(),
            })
            .unwrap(),
        ];
        let bytes: Vec<u8> = frames.iter().flat_map(|f| f.encode().unwrap()).collect();
        for step in 1..bytes.len() {
            let mut decoder = FrameDecoder::new();
            let mut got = Vec::new();
            for piece in bytes.chunks(step) {
                decoder.push(piece);
                while let Some(frame) = decoder.next_frame().unwrap() {
                    got.push(frame);
                }
            }
            assert_eq!(got, frames, "split every {step} bytes");
        }
    }

    #[test]
    fn unknown_frame_types_are_skipped() {
        let mut bytes = vec![9, 2, 0, 0xAA, 0xBB];
        bytes.extend(Frame::Audio(vec![7, 7]).encode().unwrap());
        let mut decoder = FrameDecoder::new();
        decoder.push(&bytes);
        assert_eq!(
            decoder.next_frame().unwrap(),
            Some(Frame::Audio(vec![7, 7]))
        );
        assert_eq!(decoder.next_frame().unwrap(), None);
    }

    #[test]
    fn oversized_payload_is_refused() {
        assert!(Frame::Audio(vec![0; MAX_PAYLOAD + 1]).encode().is_err());
    }

    #[test]
    fn message_json_shape() {
        let hello = ClientMsg::Hello {
            proto: 2,
            device: "inkdeck".into(),
            token: String::new(),
            mic_rate: 16000,
            tools: vec![],
        };
        assert_eq!(
            serde_json::to_string(&hello).unwrap(),
            r#"{"type":"hello","proto":2,"device":"inkdeck","mic_rate":16000}"#
        );
        let hello = ClientMsg::Hello {
            proto: 1,
            device: "inkdeck".into(),
            token: "t".into(),
            mic_rate: 16000,
            tools: vec![],
        };
        assert_eq!(
            serde_json::to_string(&hello).unwrap(),
            r#"{"type":"hello","proto":1,"device":"inkdeck","token":"t","mic_rate":16000}"#
        );
        assert_eq!(
            serde_json::from_str::<ClientMsg>(r#"{"type":"listen_stop"}"#).unwrap(),
            ClientMsg::ListenStop
        );
        assert_eq!(
            serde_json::to_string(&ServerMsg::ReplyStart { rate: 24000 }).unwrap(),
            r#"{"type":"reply_start","rate":24000}"#
        );
    }

    #[test]
    fn device_tools() {
        let hello: ClientMsg = serde_json::from_str(
            r#"{"type":"hello","proto":2,"device":"inkdeck","mic_rate":16000,
                "tools":[{"name":"open_book","description":"Opens a book.",
                          "input_schema":{"type":"object","properties":{"path":{"type":"string"}}}},
                         {"name":"board_status","description":"How the board is."}]}"#,
        )
        .unwrap();
        let ClientMsg::Hello { tools, token, .. } = hello else {
            panic!("not a hello")
        };
        assert_eq!(token, "");
        assert_eq!(
            tools[0].input_schema["properties"]["path"]["type"],
            "string"
        );
        assert_eq!(tools[1].input_schema["type"], "object");
        assert_eq!(
            serde_json::to_string(&ServerMsg::ToolCall {
                id: 3,
                name: "open_book".into(),
                input: serde_json::json!({"path": "x"}),
            })
            .unwrap(),
            r#"{"type":"tool_call","id":3,"name":"open_book","input":{"path":"x"}}"#
        );
        assert_eq!(
            serde_json::from_str::<ClientMsg>(r#"{"type":"tool_result","id":3,"text":"ok"}"#)
                .unwrap(),
            ClientMsg::ToolResult {
                id: 3,
                text: "ok".into(),
                error: false
            }
        );
    }

    /// Parses `json` as `T`, checks it against `expect`, and that it writes back as `json`.
    fn round_trip<T>(json: &str, expect: T)
    where
        T: Serialize + for<'de> Deserialize<'de> + PartialEq + std::fmt::Debug,
    {
        let parsed: T = serde_json::from_str(json).unwrap();
        assert_eq!(parsed, expect);
        let written: serde_json::Value = serde_json::to_value(&parsed).unwrap();
        let given: serde_json::Value = serde_json::from_str(json).unwrap();
        assert_eq!(written, given, "{json}");
    }

    #[test]
    fn listen_start_modes() {
        // A board from before agent sessions: a talk turn.
        round_trip(
            r#"{"type":"listen_start"}"#,
            ClientMsg::ListenStart {
                mode: None,
                session: None,
            },
        );
        round_trip(
            r#"{"type":"listen_start","mode":"agent","session":"k7q2"}"#,
            ClientMsg::ListenStart {
                mode: Some(Mode::Agent),
                session: Some("k7q2".into()),
            },
        );
        round_trip(
            r#"{"type":"listen_start","mode":"talk"}"#,
            ClientMsg::ListenStart {
                mode: Some(Mode::Talk),
                session: None,
            },
        );
        // A mode from a later version still parses, so the turn can be answered.
        assert_eq!(
            serde_json::from_str::<ClientMsg>(r#"{"type":"listen_start","mode":"later"}"#).unwrap(),
            ClientMsg::ListenStart {
                mode: Some(Mode::Unknown),
                session: None,
            }
        );
    }

    #[test]
    fn session_requests() {
        round_trip(
            r#"{"type":"unlock","pin":"1234"}"#,
            ClientMsg::Unlock {
                pin: Some("1234".into()),
                ticket: None,
            },
        );
        round_trip(
            r#"{"type":"unlock","ticket":"abcd"}"#,
            ClientMsg::Unlock {
                pin: None,
                ticket: Some("abcd".into()),
            },
        );
        round_trip(r#"{"type":"sync"}"#, ClientMsg::Sync {});
        // Fields this version doesn't know are ignored.
        assert_eq!(
            serde_json::from_str::<ClientMsg>(r#"{"type":"sync","since":1795190000}"#).unwrap(),
            ClientMsg::Sync {}
        );
        round_trip(
            r#"{"type":"open","session":"k7q2"}"#,
            ClientMsg::Open {
                session: "k7q2".into(),
            },
        );
        round_trip(r#"{"type":"close"}"#, ClientMsg::Close {});
        round_trip(
            r#"{"type":"answer","session":"k7q2","ask":"a1","choice":1}"#,
            ClientMsg::Answer {
                session: "k7q2".into(),
                ask: "a1".into(),
                choice: 1,
            },
        );
        round_trip(
            r#"{"type":"stop","session":"k7q2"}"#,
            ClientMsg::Stop {
                session: "k7q2".into(),
            },
        );
        round_trip(
            r#"{"type":"delete","session":"k7q2"}"#,
            ClientMsg::Delete {
                session: "k7q2".into(),
            },
        );
    }

    fn plumber_ask() -> AskView {
        AskView {
            session: "k7q2".into(),
            id: "a1".into(),
            title: "Plumber · kitchen leak".into(),
            question: "Which plumber should I book?".into(),
            detail: Some("Three can come before the weekend.".into()),
            options: vec![
                AskOption {
                    title: "Riverside · Thu 08:00".into(),
                    right: Some("$145".into()),
                    detail: Some("4.8 ★ (212)…".into()),
                },
                AskOption {
                    title: "Northgate · Wed 13:00".into(),
                    right: Some("$120".into()),
                    detail: None,
                },
            ],
            recommend: Some(0),
            at: 1795190000,
        }
    }

    #[test]
    fn session_messages() {
        round_trip(
            r#"{"type":"hello","proto":2,"session":"s1",
                "agent":{"place":"ONIONSERVE","pin":true,"unlocked":false}}"#,
            ServerMsg::Hello {
                proto: 2,
                session: "s1".into(),
                agent: Some(AgentHello {
                    place: "ONIONSERVE".into(),
                    pin: true,
                    unlocked: false,
                }),
            },
        );
        // No agent field: no sessions on this daemon.
        round_trip(
            r#"{"type":"hello","proto":2,"session":"s1"}"#,
            ServerMsg::Hello {
                proto: 2,
                session: "s1".into(),
                agent: None,
            },
        );
        round_trip(
            r#"{"type":"sessions","locked":false,
                "counts":{"ask":2,"running":4,"planned":0,"unread":1},
                "check_at":null,"older":0,
                "sessions":[{"id":"k7q2","title":"Plumber · kitchen leak","state":"ask",
                             "place":"ONIONSERVE","sub":"Which plumber should I book?",
                             "step":null,"steps":null,"progress":null,
                             "updated":1795190000,"unread":true}]}"#,
            ServerMsg::Sessions(SessionList {
                locked: false,
                counts: Counts {
                    ask: 2,
                    running: 4,
                    planned: 0,
                    unread: 1,
                },
                check_at: None,
                older: 0,
                sessions: vec![SessionRow {
                    id: "k7q2".into(),
                    title: "Plumber · kitchen leak".into(),
                    state: State::Ask,
                    place: "ONIONSERVE".into(),
                    sub: "Which plumber should I book?".into(),
                    step: None,
                    steps: None,
                    progress: None,
                    updated: 1795190000,
                    unread: true,
                }],
            }),
        );
        round_trip(
            r#"{"type":"session","id":"k7q2","title":"Plumber · kitchen leak","state":"working",
                "place":"ONIONSERVE","prompt":"Find a plumber for the kitchen leak this week",
                "started":1795189000,"updated":1795190000,"step":2,"steps":3,
                "plan":[{"text":"Find plumbers near home","state":"done"},
                        {"text":"Check reviews and prices","state":"now"},
                        {"text":"Ask which to book","state":"todo"}],
                "log":[{"at":1795190000,"text":"web_search plumbers open Saturday"}],
                "result":null,"ask":null}"#,
            ServerMsg::Session(SessionView {
                id: "k7q2".into(),
                title: "Plumber · kitchen leak".into(),
                state: State::Working,
                place: "ONIONSERVE".into(),
                prompt: "Find a plumber for the kitchen leak this week".into(),
                started: 1795189000,
                updated: 1795190000,
                step: Some(2),
                steps: Some(3),
                plan: Some(vec![
                    PlanItem {
                        text: "Find plumbers near home".into(),
                        state: StepState::Done,
                    },
                    PlanItem {
                        text: "Check reviews and prices".into(),
                        state: StepState::Now,
                    },
                    PlanItem {
                        text: "Ask which to book".into(),
                        state: StepState::Todo,
                    },
                ]),
                log: vec![LogLine {
                    at: 1795190000,
                    text: "web_search plumbers open Saturday".into(),
                }],
                result: None,
                ask: None,
            }),
        );
        round_trip(
            r#"{"type":"ask","session":"k7q2","id":"a1","title":"Plumber · kitchen leak",
                "question":"Which plumber should I book?",
                "detail":"Three can come before the weekend.",
                "options":[{"title":"Riverside · Thu 08:00","right":"$145","detail":"4.8 ★ (212)…"},
                           {"title":"Northgate · Wed 13:00","right":"$120","detail":null}],
                "recommend":0,"at":1795190000}"#,
            ServerMsg::Ask(plumber_ask()),
        );
        round_trip(
            r#"{"type":"started","session":"k7q2"}"#,
            ServerMsg::Started {
                session: "k7q2".into(),
            },
        );
        round_trip(
            r#"{"type":"unlocked","ok":true,"ticket":"t","ttl_s":3600}"#,
            ServerMsg::Unlocked {
                ok: true,
                ticket: Some("t".into()),
                ttl_s: Some(3600),
                wait_s: None,
            },
        );
        round_trip(
            r#"{"type":"unlocked","ok":false,"wait_s":30}"#,
            ServerMsg::Unlocked {
                ok: false,
                ticket: None,
                ttl_s: None,
                wait_s: Some(30),
            },
        );
        round_trip(
            r#"{"type":"refused","request":"answer","session":"k7q2","message":"locked"}"#,
            ServerMsg::Refused {
                request: "answer".into(),
                session: Some("k7q2".into()),
                message: "locked".into(),
            },
        );
        round_trip(
            r#"{"type":"refused","request":"sync","message":"no sessions on this daemon"}"#,
            ServerMsg::Refused {
                request: "sync".into(),
                session: None,
                message: "no sessions on this daemon".into(),
            },
        );
        // A session's question is the ask message without its type; a missing option field
        // reads as null, and a state from a later version still parses.
        let ask: AskView = serde_json::from_str(
            r#"{"session":"k7q2","id":"a1","title":"t","question":"q","detail":null,
                "options":[{"title":"A"},{"title":"B","right":"$1"}],"recommend":null,"at":1}"#,
        )
        .unwrap();
        assert_eq!(ask.options[0].right, None);
        assert_eq!(
            serde_json::from_str::<State>(r#""watch""#).unwrap(),
            State::Unknown
        );
        assert!(State::Stopped.finished() && !State::Ask.finished());
    }
}
