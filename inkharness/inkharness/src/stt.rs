//! Speech to text. A session takes one utterance as it arrives and returns its transcript.

use anyhow::{Context, Result, bail};
use async_trait::async_trait;

use crate::config;
use crate::meter;

pub trait Stt: Send + Sync {
    /// Starts an utterance of s16le mono PCM at `rate`.
    fn start(&self, rate: u32) -> Box<dyn SttSession>;
}

#[async_trait]
pub trait SttSession: Send {
    async fn feed(&mut self, pcm: &[u8]);
    async fn finish(self: Box<Self>) -> Result<String>;
}

pub fn build(cfg: &config::Stt, http: reqwest::Client) -> Result<Box<dyn Stt>> {
    Ok(match cfg {
        config::Stt::OpenaiCompat {
            base_url,
            model,
            api_key_env,
            language,
        } => Box::new(OpenAiCompat {
            http,
            url: format!("{}/audio/transcriptions", base_url.trim_end_matches('/')),
            model: model.clone(),
            key: config::env_key(api_key_env),
            language: language.clone(),
        }),
        #[cfg(feature = "local")]
        config::Stt::SherpaStreaming {
            dir,
            encoder,
            decoder,
            joiner,
            threads,
            provider,
        } => Box::new(crate::local::StreamingStt::load(
            dir, encoder, decoder, joiner, *threads, provider,
        )?),
        #[cfg(not(feature = "local"))]
        config::Stt::SherpaStreaming { .. } => {
            bail!("the sherpa_streaming backend needs a build with --features local")
        }
        config::Stt::Mock { text } => Box::new(Mock { text: text.clone() }),
    })
}

/// Collects the utterance and posts it as a WAV when it ends. A streaming backend would send
/// in `feed` instead; the trait is the same.
#[derive(Clone)]
struct OpenAiCompat {
    http: reqwest::Client,
    url: String,
    model: String,
    key: Option<String>,
    language: Option<String>,
}

impl Stt for OpenAiCompat {
    fn start(&self, rate: u32) -> Box<dyn SttSession> {
        Box::new(OpenAiSession {
            api: self.clone(),
            rate,
            pcm: Vec::new(),
        })
    }
}

struct OpenAiSession {
    api: OpenAiCompat,
    rate: u32,
    pcm: Vec<u8>,
}

#[async_trait]
impl SttSession for OpenAiSession {
    async fn feed(&mut self, pcm: &[u8]) {
        self.pcm.extend_from_slice(pcm);
    }

    async fn finish(self: Box<Self>) -> Result<String> {
        if self.pcm.len() < 2 {
            return Ok(String::new());
        }
        let file = reqwest::multipart::Part::bytes(wav(&self.pcm, self.rate))
            .file_name("utterance.wav")
            .mime_str("audio/wav")?;
        let mut form = reqwest::multipart::Form::new()
            .text("model", self.api.model.clone())
            .text("response_format", "json")
            .part("file", file);
        if let Some(language) = &self.api.language {
            form = form.text("language", language.clone());
        }
        let mut request = self.api.http.post(&self.api.url).multipart(form);
        if let Some(key) = &self.api.key {
            request = request.bearer_auth(key);
        }
        let response = request.send().await.context("speech-to-text request")?;
        let status = response.status();
        let id = generation_id(&response);
        let body = response.text().await?;
        if !status.is_success() {
            bail!("speech-to-text returned {status}: {body}");
        }
        let json: serde_json::Value =
            serde_json::from_str(&body).context("speech-to-text response")?;
        meter::record("stt", id.as_deref(), json["usage"]["cost"].as_f64());
        Ok(json["text"].as_str().unwrap_or_default().trim().to_string())
    }
}

/// OpenRouter's id for a request (its /api/v1/generation endpoint prices it).
pub fn generation_id(response: &reqwest::Response) -> Option<String> {
    let id = response.headers().get("x-generation-id")?.to_str().ok()?;
    Some(id.to_string())
}

/// A 16-bit mono WAV file around `pcm`.
pub fn wav(pcm: &[u8], rate: u32) -> Vec<u8> {
    let data_len = (pcm.len() & !1) as u32;
    let mut out = Vec::with_capacity(44 + data_len as usize);
    out.extend_from_slice(b"RIFF");
    out.extend_from_slice(&(36 + data_len).to_le_bytes());
    out.extend_from_slice(b"WAVEfmt ");
    out.extend_from_slice(&16u32.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes()); // PCM
    out.extend_from_slice(&1u16.to_le_bytes()); // mono
    out.extend_from_slice(&rate.to_le_bytes());
    out.extend_from_slice(&(rate * 2).to_le_bytes()); // bytes per second
    out.extend_from_slice(&2u16.to_le_bytes()); // block align
    out.extend_from_slice(&16u16.to_le_bytes()); // bits per sample
    out.extend_from_slice(b"data");
    out.extend_from_slice(&data_len.to_le_bytes());
    out.extend_from_slice(&pcm[..data_len as usize]);
    out
}

struct Mock {
    text: String,
}

impl Stt for Mock {
    fn start(&self, _rate: u32) -> Box<dyn SttSession> {
        Box::new(MockSession {
            text: self.text.clone(),
            bytes: 0,
        })
    }
}

struct MockSession {
    text: String,
    bytes: usize,
}

#[async_trait]
impl SttSession for MockSession {
    async fn feed(&mut self, pcm: &[u8]) {
        self.bytes += pcm.len();
    }

    async fn finish(self: Box<Self>) -> Result<String> {
        Ok(if self.bytes == 0 {
            String::new()
        } else {
            self.text
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wav_header_sizes() {
        let file = wav(&[1, 0, 2, 0, 3], 16000); // the odd last byte is dropped
        assert_eq!(file.len(), 44 + 4);
        assert_eq!(&file[0..4], b"RIFF");
        assert_eq!(u32::from_le_bytes(file[4..8].try_into().unwrap()), 36 + 4);
        assert_eq!(u32::from_le_bytes(file[24..28].try_into().unwrap()), 16000);
        assert_eq!(u32::from_le_bytes(file[40..44].try_into().unwrap()), 4);
    }
}
