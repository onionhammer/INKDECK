//! voice-client: plays the device's part against inkharness, from a WAV file. Sends the
//! utterance, prints what comes back with timings, and saves the reply audio. It connects as
//! a board does: encrypted with the token (protocol 2), or with `--plaintext` as older boards
//! did (protocol 1).

use std::path::PathBuf;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use clap::Parser;
use tokio::net::TcpStream;
use voice_proto::{
    ClientMsg, DeviceTool, Frame, FrameReader, FrameWriter, Initiator, PROTO_PLAINTEXT,
    PROTO_VERSION, ServerMsg, psk,
};

#[derive(Parser)]
#[command(about = "Send an utterance to inkharness and save the spoken reply")]
struct Args {
    /// The daemon's address.
    #[arg(long, default_value_t = format!("127.0.0.1:{}", voice_proto::DEFAULT_PORT))]
    addr: String,
    /// The token in inkharness.toml: the key the connection is encrypted with.
    #[arg(long, default_value = "")]
    token: String,
    /// Speak protocol 1, as boards did before encryption: the token and everything else
    /// unencrypted. The daemon takes it only with `plaintext = true`.
    #[arg(long)]
    plaintext: bool,
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
    /// Play a board with tools: a JSON file holding an array of
    /// `{"name", "description", "input_schema", "answer"}`. Each call is printed and answered
    /// with its tool's `answer`.
    #[arg(long, value_name = "FILE")]
    tools: Option<PathBuf>,
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

    // The tools to announce, and what each answers.
    let mut tools: Vec<DeviceTool> = Vec::new();
    let mut answers = std::collections::HashMap::new();
    if let Some(path) = &args.tools {
        let text =
            std::fs::read_to_string(path).with_context(|| format!("reading {}", path.display()))?;
        let list: Vec<serde_json::Value> = serde_json::from_str(&text).context("the tools file")?;
        for mut item in list {
            let answer = item["answer"].as_str().unwrap_or("Done.").to_string();
            item.as_object_mut().map(|o| o.remove("answer"));
            let tool: DeviceTool =
                serde_json::from_value(item).context("a tool in the tools file")?;
            answers.insert(tool.name.clone(), answer);
            tools.push(tool);
        }
    }

    let stream = TcpStream::connect(&args.addr)
        .await
        .with_context(|| format!("connecting to {}", args.addr))?;
    stream.set_nodelay(true)?;
    let (reader, writer) = stream.into_split();
    let mut reader = FrameReader::new(reader);
    let mut writer = FrameWriter::new(writer);

    let opened = Instant::now();
    let answer = if args.plaintext {
        let hello = ClientMsg::Hello {
            proto: PROTO_PLAINTEXT,
            device: "voice-client".into(),
            token: args.token.clone(),
            mic_rate: rate,
            tools,
        };
        writer.write(&Frame::control(&hello)?).await?;
        match reader.read().await? {
            Some(Frame::Control(json)) => json,
            _ => bail!("the daemon closed the connection"),
        }
    } else {
        // The hello rides in the handshake; the token only keys it.
        let hello = ClientMsg::Hello {
            proto: PROTO_VERSION,
            device: "voice-client".into(),
            token: String::new(),
            mic_rate: rate,
            tools,
        };
        let mut initiator = Initiator::new(&psk(&args.token));
        writer.write(&initiator.hello(&hello)?).await?;
        match reader.read().await? {
            Some(Frame::Handshake(msg)) => {
                let (json, keys) = initiator
                    .finish(&msg)
                    .context("the daemon's handshake didn't open")?;
                let (sealer, opener) = keys.split();
                writer.seal(sealer);
                reader.seal(opener);
                json
            }
            Some(Frame::Control(json)) => json, // a refusal, unencrypted
            _ => bail!("the daemon closed the connection"),
        }
    };
    match serde_json::from_str::<ServerMsg>(&answer)? {
        ServerMsg::Hello { session, proto } => println!(
            "connected in {} ms, session {session}, protocol {proto}{}",
            opened.elapsed().as_millis(),
            if proto == PROTO_PLAINTEXT {
                " (unencrypted)"
            } else {
                ""
            }
        ),
        ServerMsg::Error { message } => bail!("the daemon refused: {message}"),
        other => bail!("expected hello, got {other:?}"),
    }

    writer
        .write(&Frame::control(&ClientMsg::ListenStart)?)
        .await?;
    let frame_time = Duration::from_secs_f64(FRAME_BYTES as f64 / (rate as f64 * 2.0));
    for chunk in pcm.chunks(FRAME_BYTES) {
        writer.write(&Frame::Audio(chunk.to_vec())).await?;
        if !args.fast {
            tokio::time::sleep(frame_time).await;
        }
    }
    writer
        .write(&Frame::control(&ClientMsg::ListenStop)?)
        .await?;
    writer.flush().await?;
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
                    frame = reader.read() => frame?,
                    _ = tokio::time::sleep_until(at.into()) => {
                        abort_at = None;
                        println!("[{:>5} ms] sending abort", ms());
                        writer.write(&Frame::control(&ClientMsg::Abort)?).await?;
                        continue;
                    }
                }
            }
            None => reader.read().await?,
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
            Frame::Handshake(_) | Frame::Sealed(_) => bail!("a handshake frame mid-turn"),
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
                Ok(ServerMsg::ToolCall { id, name, input }) => {
                    println!("[{:>5} ms] tool call: {name} {input}", ms());
                    let (text, error) = match answers.get(&name) {
                        Some(answer) => (answer.clone(), false),
                        None => (format!("no tool named {name}"), true),
                    };
                    let result = ClientMsg::ToolResult { id, text, error };
                    writer.write(&Frame::control(&result)?).await?;
                }
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
