//! Text to speech. One call per sentence; PCM (s16le mono) is sent on as it arrives.

use anyhow::{Context, Result, bail};
use async_trait::async_trait;
use bytes::Bytes;
use futures_util::StreamExt;
use tokio::sync::mpsc;

use crate::config;

#[async_trait]
pub trait Tts: Send + Sync {
    /// Sample rate of the PCM `synth` produces.
    fn rate(&self) -> u32;
    /// Speaks `text` into `out`. Chunks may split a sample; the caller rejoins them. Stops
    /// early without error if the receiver is gone.
    async fn synth(&self, text: &str, out: mpsc::Sender<Bytes>) -> Result<()>;
}

pub fn build(cfg: &config::Tts, http: reqwest::Client) -> Result<Box<dyn Tts>> {
    Ok(match cfg {
        config::Tts::OpenaiCompat {
            base_url,
            model,
            voice,
            api_key_env,
            rate,
        } => Box::new(OpenAiCompat {
            http,
            url: format!("{}/audio/speech", base_url.trim_end_matches('/')),
            model: model.clone(),
            voice: voice.clone(),
            key: config::env_key(api_key_env),
            rate: *rate,
        }),
        #[cfg(feature = "local")]
        config::Tts::SherpaPocket {
            dir,
            voice,
            threads,
            provider,
            steps,
        } => Box::new(crate::local::PocketTts::load(
            dir, voice, *threads, provider, *steps,
        )?),
        #[cfg(not(feature = "local"))]
        config::Tts::SherpaPocket { .. } => {
            bail!("the sherpa_pocket backend needs a build with --features local")
        }
        config::Tts::Mock => Box::new(Mock),
    })
}

struct OpenAiCompat {
    http: reqwest::Client,
    url: String,
    model: String,
    voice: String,
    key: Option<String>,
    rate: u32,
}

#[async_trait]
impl Tts for OpenAiCompat {
    fn rate(&self) -> u32 {
        self.rate
    }

    async fn synth(&self, text: &str, out: mpsc::Sender<Bytes>) -> Result<()> {
        let mut request = self.http.post(&self.url).json(&serde_json::json!({
            "model": self.model,
            "voice": self.voice,
            "input": text,
            "response_format": "pcm",
        }));
        if let Some(key) = &self.key {
            request = request.bearer_auth(key);
        }
        let response = request.send().await.context("text-to-speech request")?;
        let status = response.status();
        // The price isn't in the response; OpenRouter's generation endpoint has it.
        crate::meter::record("tts", crate::stt::generation_id(&response).as_deref(), None);
        if !status.is_success() {
            bail!(
                "text-to-speech returned {status}: {}",
                response.text().await?
            );
        }
        let mut body = response.bytes_stream();
        while let Some(chunk) = body.next().await {
            let chunk = chunk.context("text-to-speech stream")?;
            if out.send(chunk).await.is_err() {
                break;
            }
        }
        Ok(())
    }
}

/// Sentences longer than this aren't cached: long ones rarely come round again.
pub const CACHED_CHARS: usize = 80;

/// Keeps the PCM of short sentences in memory, so one said again (a phrase played while the
/// model thinks, "Done.", an error) plays without a request.
pub struct Cached {
    inner: Box<dyn Tts>,
    entries: usize,
    store: std::sync::Mutex<Store>,
}

#[derive(Default)]
struct Store {
    pcm: std::collections::HashMap<String, Bytes>,
    /// Keys, oldest first; the oldest goes when the cache is full.
    order: std::collections::VecDeque<String>,
}

/// Bytes per chunk when a cached sentence is played back.
const REPLAY_CHUNK: usize = 8192;

impl Cached {
    /// `entries`: how many sentences to keep (at least 1).
    pub fn new(inner: Box<dyn Tts>, entries: usize) -> Self {
        Cached {
            inner,
            entries: entries.max(1),
            store: Default::default(),
        }
    }

    fn get(&self, key: &str) -> Option<Bytes> {
        self.store.lock().unwrap().pcm.get(key).cloned()
    }

    fn put(&self, key: String, pcm: Bytes) {
        let mut store = self.store.lock().unwrap();
        if store.pcm.insert(key.clone(), pcm).is_none() {
            store.order.push_back(key);
        }
        while store.order.len() > self.entries {
            if let Some(old) = store.order.pop_front() {
                store.pcm.remove(&old);
            }
        }
    }

    #[cfg(test)]
    fn len(&self) -> usize {
        self.store.lock().unwrap().pcm.len()
    }
}

#[async_trait]
impl Tts for Cached {
    fn rate(&self) -> u32 {
        self.inner.rate()
    }

    async fn synth(&self, text: &str, out: mpsc::Sender<Bytes>) -> Result<()> {
        let key = text.trim();
        if key.chars().count() > CACHED_CHARS {
            return self.inner.synth(text, out).await;
        }
        if let Some(pcm) = self.get(key) {
            tracing::debug!(text = key, "speech from the cache");
            for start in (0..pcm.len()).step_by(REPLAY_CHUNK) {
                let end = (start + REPLAY_CHUNK).min(pcm.len());
                if out.send(pcm.slice(start..end)).await.is_err() {
                    break;
                }
            }
            return Ok(());
        }
        // Pass the audio on as it comes, and keep it if the whole sentence arrived.
        let (tx, mut rx) = mpsc::channel::<Bytes>(8);
        let synth = self.inner.synth(text, tx);
        let forward = async {
            let mut all = Vec::new();
            let mut complete = true;
            while let Some(chunk) = rx.recv().await {
                all.extend_from_slice(&chunk);
                if complete && out.send(chunk).await.is_err() {
                    complete = false; // the listener left; finish the synthesis for the cache
                }
            }
            all
        };
        let (result, all) = tokio::join!(synth, forward);
        result?;
        if !all.is_empty() {
            self.put(key.to_string(), Bytes::from(all));
        }
        Ok(())
    }
}

/// A short tone per sentence, its length following the text's, so the audio path can be
/// tested without a service.
struct Mock;

const MOCK_RATE: u32 = 24000;

#[async_trait]
impl Tts for Mock {
    fn rate(&self) -> u32 {
        MOCK_RATE
    }

    async fn synth(&self, text: &str, out: mpsc::Sender<Bytes>) -> Result<()> {
        let ms = (text.chars().count() as u32 * 50).clamp(200, 3000);
        let samples = MOCK_RATE * ms / 1000;
        let mut pcm = Vec::with_capacity(samples as usize * 2);
        for n in 0..samples {
            let t = n as f32 / MOCK_RATE as f32;
            let fade = (n.min(samples - n) as f32 / 480.0).min(1.0); // 20 ms in and out
            let v = (t * 440.0 * std::f32::consts::TAU).sin() * 8000.0 * fade;
            pcm.extend_from_slice(&(v as i16).to_le_bytes());
        }
        for chunk in pcm.chunks(4801) {
            // an odd size on purpose: callers must cope with split samples
            if out.send(Bytes::copy_from_slice(chunk)).await.is_err() {
                break;
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;
    use std::sync::atomic::{AtomicUsize, Ordering};

    use super::*;

    /// Counts its requests; the PCM is the text's bytes.
    struct Counting(Arc<AtomicUsize>);

    #[async_trait]
    impl Tts for Counting {
        fn rate(&self) -> u32 {
            16000
        }
        async fn synth(&self, text: &str, out: mpsc::Sender<Bytes>) -> Result<()> {
            self.0.fetch_add(1, Ordering::SeqCst);
            for piece in text.as_bytes().chunks(3) {
                let _ = out.send(Bytes::copy_from_slice(piece)).await;
            }
            Ok(())
        }
    }

    async fn say(tts: &Cached, text: &str) -> Vec<u8> {
        let (tx, mut rx) = mpsc::channel(4);
        let (result, pcm) = tokio::join!(tts.synth(text, tx), async {
            let mut pcm = Vec::new();
            while let Some(chunk) = rx.recv().await {
                pcm.extend_from_slice(&chunk);
            }
            pcm
        });
        result.unwrap();
        pcm
    }

    #[tokio::test]
    async fn repeats_come_from_the_cache() {
        let requests = Arc::new(AtomicUsize::new(0));
        let tts = Cached::new(Box::new(Counting(requests.clone())), 2);
        let count = || requests.load(Ordering::SeqCst);

        assert_eq!(say(&tts, "Let me think.").await, b"Let me think.");
        assert_eq!(count(), 1);
        // The same sentence, spaced differently: from the cache, chunked, no request.
        assert_eq!(say(&tts, " Let me think. ").await, b"Let me think.");
        assert_eq!(count(), 1);

        say(&tts, "Done.").await;
        say(&tts, "Okay.").await; // the cache holds 2: the oldest goes
        assert_eq!(tts.len(), 2);
        say(&tts, "Let me think.").await;
        assert_eq!(count(), 4);

        let long = "x".repeat(CACHED_CHARS + 1);
        say(&tts, &long).await;
        say(&tts, &long).await;
        assert_eq!(count(), 6, "long sentences aren't kept");
    }
}
