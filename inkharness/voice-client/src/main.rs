//! voice-client: plays the device's part against voiced, from a WAV file. Sends the
//! utterance, prints what comes back with timings, and saves the reply audio.

use std::path::PathBuf;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use clap::Parser;
use tokio::io::AsyncWriteExt;
use tokio::net::TcpStream;
use voice_proto::{
    ClientMsg, Frame, FrameDecoder, PROTO_VERSION, ServerMsg, read_frame, write_frame,
};

#[derive(Parser)]
#[command(about = "Send an utterance to voiced and save the spoken reply")]
struct Args {
    /// The daemon's address.
    #[arg(long, default_value_t = format!("127.0.0.1:{}", voice_proto::DEFAULT_PORT))]
    addr: String,
    /// The token voiced.toml asks for.
    #[arg(long, default_value = "")]
    token: String,
    /// The utterance: a 16-bit mono WAV (16 kHz is what the device sends).
    #[arg(long, conflicts_with = "silence")]
    wav: Option<PathBuf>,
    /// Send this many seconds of silence at 16 kHz instead of a file.
    #[arg(long)]
    silence: Option<f32>,
    /// Where to save the reply audio.
    #[arg(long, default_value = "reply.wav")]
    out: PathBuf,
    /// Send as fast as the socket takes it instead of at speaking speed.
    #[arg(long)]
    fast: bool,
    /// Send `abort` this many milliseconds after the utterance ends.
    #[arg(long)]
    abort_after_ms: Option<u64>,
}

/// What the device sends per frame: 512 samples, 32 ms at 16 kHz.
const FRAME_BYTES: usize = 1024;

#[tokio::main]
async fn main() -> Result<()> {
    let args = Args::parse();
    let (pcm, rate) = match (&args.wav, args.silence) {
        (Some(path), _) => read_wav(path)?,
        (None, Some(secs)) => (vec![0u8; (secs * 16000.0) as usize * 2], 16000),
        (None, None) => bail!("give --wav FILE or --silence SECONDS"),
    };

    let mut stream = TcpStream::connect(&args.addr)
        .await
        .with_context(|| format!("connecting to {}", args.addr))?;
    stream.set_nodelay(true)?;
    let mut decoder = FrameDecoder::new();

    let hello = ClientMsg::Hello {
        proto: PROTO_VERSION,
        device: "voice-client".into(),
        token: args.token.clone(),
        mic_rate: rate,
    };
    write_frame(&mut stream, &Frame::control(&hello)?).await?;
    match read_frame(&mut stream, &mut decoder).await? {
        Some(Frame::Control(json)) => match serde_json::from_str::<ServerMsg>(&json)? {
            ServerMsg::Hello { session, .. } => println!("connected, session {session}"),
            ServerMsg::Error { message } => bail!("the daemon refused: {message}"),
            other => bail!("expected hello, got {other:?}"),
        },
        _ => bail!("the daemon closed the connection"),
    }

    write_frame(&mut stream, &Frame::control(&ClientMsg::ListenStart)?).await?;
    let frame_time = Duration::from_secs_f64(FRAME_BYTES as f64 / (rate as f64 * 2.0));
    for chunk in pcm.chunks(FRAME_BYTES) {
        write_frame(&mut stream, &Frame::Audio(chunk.to_vec())).await?;
        if !args.fast {
            tokio::time::sleep(frame_time).await;
        }
    }
    write_frame(&mut stream, &Frame::control(&ClientMsg::ListenStop)?).await?;
    stream.flush().await?;
    let stopped = Instant::now();
    let ms = || stopped.elapsed().as_millis();
    println!(
        "sent {:.1} s of audio",
        pcm.len() as f32 / (rate as f32 * 2.0)
    );

    let mut abort_at = args
        .abort_after_ms
        .map(|ms| stopped + Duration::from_millis(ms));
    let mut reply = Vec::new();
    let mut reply_rate = 0u32;
    let mut first_audio = None;
    let mut text = String::new();
    loop {
        let frame = match abort_at {
            Some(at) => {
                tokio::select! {
                    frame = read_frame(&mut stream, &mut decoder) => frame?,
                    _ = tokio::time::sleep_until(at.into()) => {
                        abort_at = None;
                        println!("[{:>5} ms] sending abort", ms());
                        write_frame(&mut stream, &Frame::control(&ClientMsg::Abort)?).await?;
                        continue;
                    }
                }
            }
            None => read_frame(&mut stream, &mut decoder).await?,
        };
        let Some(frame) = frame else {
            bail!("the daemon closed the connection mid-turn")
        };
        match frame {
            Frame::Audio(pcm) => {
                if first_audio.is_none() {
                    first_audio = Some(ms());
                    println!("[{:>5} ms] first audio", ms());
                }
                reply.extend_from_slice(&pcm);
            }
            Frame::Control(json) => match serde_json::from_str::<ServerMsg>(&json) {
                Ok(ServerMsg::Stt { text }) => println!("[{:>5} ms] heard: {text:?}", ms()),
                Ok(ServerMsg::ReplyStart { rate }) => {
                    reply_rate = rate;
                    println!("[{:>5} ms] reply starts, {rate} Hz", ms());
                }
                Ok(ServerMsg::Text { delta }) => {
                    if text.is_empty() {
                        println!("[{:>5} ms] first text", ms());
                    }
                    text.push_str(&delta);
                }
                Ok(ServerMsg::ReplyEnd) => {
                    println!("[{:>5} ms] reply ends", ms());
                    break;
                }
                Ok(ServerMsg::Error { message }) => println!("[{:>5} ms] error: {message}", ms()),
                Ok(ServerMsg::Hello { .. }) => {}
                Err(e) => println!("unknown message {json}: {e}"),
            },
        }
    }

    println!("reply: {}", text.trim());
    if reply.is_empty() {
        println!("no reply audio");
        return Ok(());
    }
    if reply.len() % 2 != 0 {
        bail!("reply audio has an odd byte count ({})", reply.len());
    }
    let spec = hound::WavSpec {
        channels: 1,
        sample_rate: reply_rate,
        bits_per_sample: 16,
        sample_format: hound::SampleFormat::Int,
    };
    let mut writer = hound::WavWriter::create(&args.out, spec)?;
    for sample in reply.chunks_exact(2) {
        writer.write_sample(i16::from_le_bytes([sample[0], sample[1]]))?;
    }
    writer.finalize()?;
    println!(
        "saved {:.1} s of reply audio to {}",
        reply.len() as f32 / (reply_rate as f32 * 2.0),
        args.out.display()
    );
    Ok(())
}

fn read_wav(path: &PathBuf) -> Result<(Vec<u8>, u32)> {
    let mut reader =
        hound::WavReader::open(path).with_context(|| format!("opening {}", path.display()))?;
    let spec = reader.spec();
    if spec.channels != 1
        || spec.bits_per_sample != 16
        || spec.sample_format != hound::SampleFormat::Int
    {
        bail!(
            "{} is {} channel(s), {} bit; it must be 16-bit mono (try: ffmpeg -i in -ac 1 -ar 16000 -c:a pcm_s16le out.wav)",
            path.display(),
            spec.channels,
            spec.bits_per_sample
        );
    }
    let mut pcm = Vec::with_capacity(reader.len() as usize * 2);
    for sample in reader.samples::<i16>() {
        pcm.extend_from_slice(&sample?.to_le_bytes());
    }
    Ok((pcm, spec.sample_rate))
}
