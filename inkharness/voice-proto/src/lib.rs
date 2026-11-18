//! Wire protocol between the device and the voice daemon (docs/voice-protocol.md).
//!
//! One TCP connection. Every frame is `u8 type`, `u16 length` (little-endian), payload.
//! Type 1 is a UTF-8 JSON control message, type 2 is PCM audio (s16le, mono). Frames of any
//! other type are skipped, so later versions can add types.

use serde::{Deserialize, Serialize};

pub const PROTO_VERSION: u32 = 1;
pub const DEFAULT_PORT: u16 = 7391;
pub const MAX_PAYLOAD: usize = u16::MAX as usize;
pub const HEADER_LEN: usize = 3;

const TYPE_CONTROL: u8 = 1;
const TYPE_AUDIO: u8 = 2;

#[derive(Debug, Clone, PartialEq)]
pub enum Frame {
    Control(String),
    Audio(Vec<u8>),
}

#[derive(Debug, thiserror::Error)]
pub enum ProtoError {
    #[error("payload of {0} bytes is over the {MAX_PAYLOAD}-byte frame limit")]
    TooLong(usize),
    #[error("control frame is not UTF-8")]
    Utf8,
    #[error("control message: {0}")]
    Json(#[from] serde_json::Error),
}

impl Frame {
    pub fn control<T: Serialize>(msg: &T) -> Result<Frame, ProtoError> {
        Ok(Frame::Control(serde_json::to_string(msg)?))
    }

    pub fn encode(&self) -> Result<Vec<u8>, ProtoError> {
        let (kind, payload) = match self {
            Frame::Control(json) => (TYPE_CONTROL, json.as_bytes()),
            Frame::Audio(pcm) => (TYPE_AUDIO, pcm.as_slice()),
        };
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
            match kind {
                TYPE_CONTROL => {
                    return String::from_utf8(payload)
                        .map(|s| Some(Frame::Control(s)))
                        .map_err(|_| ProtoError::Utf8);
                }
                TYPE_AUDIO => return Ok(Some(Frame::Audio(payload))),
                _ => continue, // a type this version doesn't know
            }
        }
    }
}

/// Device → daemon.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ClientMsg {
    Hello {
        proto: u32,
        device: String,
        #[serde(default)]
        token: String,
        mic_rate: u32,
    },
    /// Audio frames that follow are one utterance.
    ListenStart,
    /// The utterance is complete; the daemon answers.
    ListenStop,
    /// Drop the utterance or the reply in progress. The daemon answers `reply_end`.
    Abort,
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
    Error {
        message: String,
    },
}

#[cfg(feature = "tokio")]
mod io {
    use super::{Frame, FrameDecoder};
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
}

#[cfg(feature = "tokio")]
pub use io::{read_frame, write_frame};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frames_survive_any_split() {
        let frames = vec![
            Frame::control(&ClientMsg::ListenStart).unwrap(),
            Frame::Audio(vec![1, 2, 3, 4]),
            Frame::Audio(vec![]),
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
            proto: 1,
            device: "inkdeck".into(),
            token: "t".into(),
            mic_rate: 16000,
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
}
