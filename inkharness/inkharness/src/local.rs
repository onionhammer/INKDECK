//! Models running inside the daemon through sherpa-onnx (feature `local`): no HTTP hop and no
//! second process. Speech-to-text decodes while the utterance is still arriving.

use std::sync::Arc;

use anyhow::{Context, Result, bail};
use async_trait::async_trait;
use bytes::Bytes;
use sherpa_onnx::{
    GenerationConfig, OfflineTts, OfflineTtsConfig, OfflineTtsModelConfig,
    OfflineTtsPocketModelConfig, OnlineModelConfig, OnlineRecognizer, OnlineRecognizerConfig,
    OnlineStream, OnlineTransducerModelConfig, Wave,
};
use tokio::sync::mpsc;

use crate::stt::{Stt, SttSession};
use crate::tts::Tts;

fn file(dir: &str, name: &str) -> Result<Option<String>> {
    let path = std::path::Path::new(dir).join(name);
    if !path.is_file() {
        bail!("{} is missing", path.display());
    }
    Ok(Some(path.to_string_lossy().into_owned()))
}

/// A streaming transducer (zipformer, Nemotron): encoder, decoder, joiner and tokens in `dir`.
pub struct StreamingStt {
    recognizer: Arc<OnlineRecognizer>,
}

impl StreamingStt {
    pub fn load(
        dir: &str,
        encoder: &str,
        decoder: &str,
        joiner: &str,
        threads: i32,
        provider: &str,
    ) -> Result<Self> {
        let config = OnlineRecognizerConfig {
            model_config: OnlineModelConfig {
                transducer: OnlineTransducerModelConfig {
                    encoder: file(dir, encoder)?,
                    decoder: file(dir, decoder)?,
                    joiner: file(dir, joiner)?,
                },
                tokens: file(dir, "tokens.txt")?,
                num_threads: threads,
                provider: Some(provider.to_string()),
                ..Default::default()
            },
            decoding_method: Some("greedy_search".into()),
            ..Default::default()
        };
        let recognizer = OnlineRecognizer::create(&config)
            .with_context(|| format!("loading the speech-to-text model in {dir}"))?;
        Ok(Self {
            recognizer: Arc::new(recognizer),
        })
    }
}

impl Stt for StreamingStt {
    fn start(&self, rate: u32) -> Box<dyn SttSession> {
        Box::new(StreamingSession {
            stream: self.recognizer.create_stream(),
            recognizer: self.recognizer.clone(),
            rate: rate as i32,
            odd: None,
        })
    }
}

struct StreamingSession {
    recognizer: Arc<OnlineRecognizer>,
    stream: OnlineStream,
    rate: i32,
    /// The first byte of a sample split across two frames.
    odd: Option<u8>,
}

impl StreamingSession {
    fn decode_ready(&self) {
        while self.recognizer.is_ready(&self.stream) {
            self.recognizer.decode(&self.stream);
        }
    }
}

#[async_trait]
impl SttSession for StreamingSession {
    async fn feed(&mut self, pcm: &[u8]) {
        let mut bytes = Vec::with_capacity(pcm.len() + 1);
        bytes.extend(self.odd.take());
        bytes.extend_from_slice(pcm);
        if bytes.len() % 2 == 1 {
            self.odd = bytes.pop();
        }
        let samples: Vec<f32> = bytes
            .chunks_exact(2)
            .map(|s| i16::from_le_bytes([s[0], s[1]]) as f32 / 32768.0)
            .collect();
        // Inference on this worker thread; the runtime moves other tasks off it meanwhile.
        tokio::task::block_in_place(|| {
            self.stream.accept_waveform(self.rate, &samples);
            self.decode_ready();
        });
    }

    async fn finish(self: Box<Self>) -> Result<String> {
        tokio::task::block_in_place(|| {
            // Trailing silence lets the model emit the last word.
            let tail = vec![0.0f32; self.rate as usize / 2];
            self.stream.accept_waveform(self.rate, &tail);
            self.stream.input_finished();
            self.decode_ready();
            let text = self
                .recognizer
                .get_result(&self.stream)
                .map(|r| r.text)
                .unwrap_or_default();
            Ok(text.trim().to_string())
        })
    }
}

/// Pocket TTS: a small CPU model that speaks in the voice of a reference recording.
pub struct PocketTts {
    tts: Arc<OfflineTts>,
    rate: u32,
    reference: Arc<(Vec<f32>, i32)>,
    steps: i32,
}

impl PocketTts {
    pub fn load(
        dir: &str,
        voice_wav: &str,
        threads: i32,
        provider: &str,
        steps: i32,
    ) -> Result<Self> {
        let config = OfflineTtsConfig {
            model: OfflineTtsModelConfig {
                pocket: OfflineTtsPocketModelConfig {
                    lm_flow: file(dir, "lm_flow.int8.onnx")?,
                    lm_main: file(dir, "lm_main.int8.onnx")?,
                    encoder: file(dir, "encoder.onnx")?,
                    decoder: file(dir, "decoder.int8.onnx")?,
                    text_conditioner: file(dir, "text_conditioner.onnx")?,
                    vocab_json: file(dir, "vocab.json")?,
                    token_scores_json: file(dir, "token_scores.json")?,
                    ..Default::default()
                },
                num_threads: threads,
                provider: Some(provider.to_string()),
                ..Default::default()
            },
            ..Default::default()
        };
        let tts = OfflineTts::create(&config)
            .with_context(|| format!("loading the text-to-speech model in {dir}"))?;
        let voice = Wave::read(voice_wav)
            .with_context(|| format!("reading the voice sample {voice_wav}"))?;
        let rate = tts.sample_rate() as u32;
        Ok(Self {
            tts: Arc::new(tts),
            rate,
            reference: Arc::new((voice.samples().to_vec(), voice.sample_rate())),
            steps,
        })
    }
}

#[async_trait]
impl Tts for PocketTts {
    fn rate(&self) -> u32 {
        self.rate
    }

    async fn synth(&self, text: &str, out: mpsc::Sender<Bytes>) -> Result<()> {
        let tts = self.tts.clone();
        let reference = self.reference.clone();
        let steps = self.steps;
        let text = text.to_string();
        let done = tokio::task::spawn_blocking(move || {
            let config = GenerationConfig {
                reference_audio: Some(reference.0.clone()),
                reference_sample_rate: reference.1,
                num_steps: steps,
                ..Default::default()
            };
            // The callback gets each newly generated piece; returning false stops the model.
            let callback = move |samples: &[f32], _progress: f32| {
                let mut pcm = Vec::with_capacity(samples.len() * 2);
                for s in samples {
                    pcm.extend_from_slice(&((s.clamp(-1.0, 1.0) * 32767.0) as i16).to_le_bytes());
                }
                out.blocking_send(Bytes::from(pcm)).is_ok()
            };
            tts.generate_with_config(&text, &config, Some(callback))
                .is_some()
        })
        .await?;
        if !done {
            bail!("text-to-speech produced nothing");
        }
        Ok(())
    }
}
