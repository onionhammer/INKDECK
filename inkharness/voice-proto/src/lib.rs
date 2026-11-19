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
    /// Audio frames that follow are one utterance.
    ListenStart,
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
}

/// Daemon → device.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ServerMsg {
    Hello {
        proto: u32,
        session: String,
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
            Frame::control(&ClientMsg::ListenStart).unwrap(),
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
}
