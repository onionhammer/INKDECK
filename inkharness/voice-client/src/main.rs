//! voice-client: plays the device's part against inkharness, from a WAV file. Sends the
//! utterance, prints what comes back with timings, and saves the reply audio. It connects as
//! a board does: encrypted with the token (protocol 2), or with `--plaintext` as older boards
//! did (protocol 1).
//!
//! It can also drive agent sessions as a board does (docs/voice-protocol.md, "Agent sessions"):
//! unlock with the PIN or a ticket, follow the list or one session, answer, stop and delete,
//! and talk in agent turns.

use std::path::PathBuf;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use clap::Parser;
use tokio::net::TcpStream;
use tokio::net::tcp::{OwnedReadHalf, OwnedWriteHalf};
use voice_proto::{
    ClientMsg, DeviceTool, Frame, FrameReader, FrameWriter, Initiator, Mode, PROTO_PLAINTEXT,
    PROTO_VERSION, ServerMsg, StepState, Via, psk,
};

#[derive(Parser)]
#[command(
    about = "Send an utterance to inkharness and save the spoken reply, or drive its agent sessions"
)]
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

    /// The utterance's turn: "talk" (sent as such) or "agent". Unset, no mode is sent, as from
    /// boards before agent sessions (a talk turn).
    #[arg(long, value_parser = ["talk", "agent"])]
    mode: Option<String>,
    /// An agent turn talks into this session (answering its question, if it has one) instead
    /// of starting one.
    #[arg(long, value_name = "ID", requires = "mode")]
    session: Option<String>,
    /// Unlock the sessions with this PIN first.
    #[arg(long, value_name = "PIN")]
    unlock: Option<String>,
    /// Unlock with a ticket from an earlier unlock (tried before --unlock).
    #[arg(long, value_name = "TICKET")]
    ticket: Option<String>,
    /// Follow the list: print `sessions` now and as it changes, and each new `ask`.
    #[arg(long)]
    sync: bool,
    /// Follow one session: print `session` now and as it changes.
    #[arg(long, value_name = "ID")]
    open: Option<String>,
    /// Answer a question: the session's id, the question's id and the option's index.
    #[arg(long, value_name = "SESSION:ASK:N")]
    answer: Option<String>,
    /// Stop a working or asking session.
    #[arg(long, value_name = "ID")]
    stop: Option<String>,
    /// Delete a session.
    #[arg(long, value_name = "ID")]
    delete: Option<String>,
    /// Type a herdr session's draft (from a `draft` message) into it.
    #[arg(long, value_name = "SESSION:DRAFT")]
    send_draft: Option<String>,
    /// Throw a draft away.
    #[arg(long, value_name = "SESSION:DRAFT")]
    drop_draft: Option<String>,
    /// Send each draft as it comes (as a board would once you confirm it).
    #[arg(long)]
    send_drafts: bool,
    /// After everything else, go on printing what comes for this many seconds (with --sync or
    /// --open, 5 unless given).
    #[arg(long, value_name = "SECS")]
    wait: Option<f32>,
    /// Print each control message as the JSON it came as.
    #[arg(long)]
    json: bool,
}

/// What the device sends per frame: 512 samples, 32 ms at 16 kHz.
const FRAME_BYTES: usize = 1024;

type Reader = FrameReader<OwnedReadHalf>;
type Writer = FrameWriter<OwnedWriteHalf>;

#[tokio::main]
async fn main() -> Result<()> {
    let args = Args::parse();
    let utterance = match (&args.wav, args.silence) {
        (Some(path), _) => Some(read_wav(path)?),
        (None, Some(secs)) => Some((vec![0u8; (secs * 16000.0) as usize * 2], 16000)),
        (None, None) => None,
    };
    let requests = args.unlock.is_some()
        || args.ticket.is_some()
        || args.sync
        || args.open.is_some()
        || args.answer.is_some()
        || args.stop.is_some()
        || args.delete.is_some()
        || args.send_draft.is_some()
        || args.drop_draft.is_some();
    if utterance.is_none() && !requests {
        bail!(
            "give --wav FILE or --silence SECONDS, or a session request (--sync, --open, \
             --answer, --stop, --delete, --send-draft, --drop-draft, --unlock, --ticket)"
        );
    }
    let send_draft = args.send_draft.as_deref().map(parse_draft).transpose()?;
    let drop_draft = args.drop_draft.as_deref().map(parse_draft).transpose()?;
    let answer = args.answer.as_deref().map(parse_answer).transpose()?;
    let rate = utterance.as_ref().map_or(16000, |(_, rate)| *rate);

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
    let answer_json = if args.plaintext {
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
    if args.json {
        println!("{answer_json}");
    }
    match serde_json::from_str::<ServerMsg>(&answer_json)? {
        ServerMsg::Hello {
            session,
            proto,
            agent,
        } => {
            println!(
                "connected in {} ms, session {session}, protocol {proto}{}",
                opened.elapsed().as_millis(),
                if proto == PROTO_PLAINTEXT {
                    " (unencrypted)"
                } else {
                    ""
                }
            );
            match agent {
                Some(agent) => println!(
                    "agent sessions at {}: {}",
                    agent.place,
                    match (agent.pin, agent.unlocked) {
                        (_, true) => "unlocked",
                        (true, false) => "locked, a PIN unlocks them",
                        (false, false) => "locked",
                    }
                ),
                None => println!("no agent sessions on this daemon"),
            }
        }
        ServerMsg::Error { message } => bail!("the daemon refused: {message}"),
        other => bail!("expected hello, got {other:?}"),
    }

    let started = Instant::now();
    let mut out = Printer {
        json: args.json,
        started,
    };

    // Unlock first, waiting for each answer.
    for (pin, ticket) in [(None, args.ticket.clone()), (args.unlock.clone(), None)] {
        if pin.is_none() && ticket.is_none() {
            continue;
        }
        put(&mut writer, &ClientMsg::Unlock { pin, ticket }).await?;
        loop {
            let msg = next(&mut reader, &mut out).await?;
            if matches!(msg, ServerMsg::Unlocked { .. } | ServerMsg::Refused { .. }) {
                break;
            }
        }
    }
    if args.sync {
        put(&mut writer, &ClientMsg::Sync {}).await?;
    }
    if let Some(session) = &args.open {
        put(
            &mut writer,
            &ClientMsg::Open {
                session: session.clone(),
            },
        )
        .await?;
    }
    if let Some((session, ask, choice)) = answer {
        put(
            &mut writer,
            &ClientMsg::Answer {
                session,
                ask,
                choice,
            },
        )
        .await?;
    }
    if let Some(session) = &args.stop {
        put(
            &mut writer,
            &ClientMsg::Stop {
                session: session.clone(),
            },
        )
        .await?;
    }
    if let Some(session) = &args.delete {
        put(
            &mut writer,
            &ClientMsg::Delete {
                session: session.clone(),
            },
        )
        .await?;
    }
    if let Some((session, draft)) = send_draft {
        put(&mut writer, &ClientMsg::SendDraft { session, draft }).await?;
    }
    if let Some((session, draft)) = drop_draft {
        put(&mut writer, &ClientMsg::DropDraft { session, draft }).await?;
    }

    if let Some((pcm, rate)) = utterance {
        let mode = match args.mode.as_deref() {
            Some("agent") => Some(Mode::Agent),
            Some(_) => Some(Mode::Talk),
            None => None,
        };
        let start = ClientMsg::ListenStart {
            mode,
            session: args.session.clone(),
        };
        turn(
            &args,
            &mut reader,
            &mut writer,
            &mut out,
            start,
            &pcm,
            rate,
            &answers,
        )
        .await?;
    }

    let wait = args.wait.unwrap_or(if args.sync || args.open.is_some() {
        5.0
    } else {
        0.0
    });
    let until = tokio::time::Instant::now() + Duration::from_secs_f32(wait.max(0.0));
    loop {
        tokio::select! {
            msg = next(&mut reader, &mut out) => { msg?; }
            _ = tokio::time::sleep_until(until) => break,
        }
    }
    Ok(())
}

/// One utterance and its reply, which is saved.
#[allow(clippy::too_many_arguments)]
async fn turn(
    args: &Args,
    reader: &mut Reader,
    writer: &mut Writer,
    out: &mut Printer,
    start: ClientMsg,
    pcm: &[u8],
    rate: u32,
    answers: &std::collections::HashMap<String, String>,
) -> Result<()> {
    put(writer, &start).await?;
    let frame_time = Duration::from_secs_f64(FRAME_BYTES as f64 / (rate as f64 * 2.0));
    for chunk in pcm.chunks(FRAME_BYTES) {
        writer.write(&Frame::Audio(chunk.to_vec())).await?;
        if !args.fast {
            tokio::time::sleep(frame_time).await;
        }
    }
    put(writer, &ClientMsg::ListenStop).await?;
    writer.flush().await?;
    let stopped = Instant::now();
    out.started = stopped;
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
                        put(writer, &ClientMsg::Abort).await?;
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
            Frame::Control(json) => {
                if args.json {
                    println!("[{:>5} ms] {json}", ms());
                }
                let msg = match serde_json::from_str::<ServerMsg>(&json) {
                    Ok(msg) => msg,
                    Err(e) => {
                        println!("unknown message {json}: {e}");
                        continue;
                    }
                };
                match msg {
                    ServerMsg::Stt { text } => println!("[{:>5} ms] heard: {text:?}", ms()),
                    ServerMsg::ReplyStart { rate } => {
                        reply_rate = rate;
                        println!("[{:>5} ms] reply starts, {rate} Hz", ms());
                    }
                    ServerMsg::Text { delta } => {
                        if text.is_empty() {
                            println!("[{:>5} ms] first text", ms());
                        }
                        text.push_str(&delta);
                    }
                    ServerMsg::ReplyEnd => {
                        println!("[{:>5} ms] reply ends", ms());
                        break;
                    }
                    ServerMsg::Error { message } => {
                        println!("[{:>5} ms] error: {message}", ms())
                    }
                    ServerMsg::ToolCall { id, name, input } => {
                        println!("[{:>5} ms] tool call: {name} {input}", ms());
                        let (text, error) = match answers.get(&name) {
                            Some(answer) => (answer.clone(), false),
                            None => (format!("no tool named {name}"), true),
                        };
                        let result = ClientMsg::ToolResult { id, text, error };
                        put(writer, &result).await?;
                    }
                    ServerMsg::Hello { .. } => {}
                    ServerMsg::Draft { session, id, text } if args.send_drafts => {
                        println!(
                            "[{:>5} ms] draft {id} for {session}: {text:?}; sending it",
                            ms()
                        );
                        put(writer, &ClientMsg::SendDraft { session, draft: id }).await?;
                    }
                    other if !args.json => out.show(&other),
                    _ => {}
                }
            }
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
    let mut wav = hound::WavWriter::create(&args.out, spec)?;
    for sample in reply.as_chunks::<2>().0 {
        wav.write_sample(i16::from_le_bytes(*sample))?;
    }
    wav.finalize()?;
    println!(
        "saved {:.1} s of reply audio to {}",
        reply.len() as f32 / (reply_rate as f32 * 2.0),
        args.out.display()
    );
    Ok(())
}

async fn put(writer: &mut Writer, msg: &ClientMsg) -> Result<()> {
    writer.write(&Frame::control(msg)?).await?;
    Ok(())
}

/// The next control message, printed. Audio outside a turn is skipped.
async fn next(reader: &mut Reader, out: &mut Printer) -> Result<ServerMsg> {
    loop {
        let Some(frame) = reader.read().await? else {
            bail!("the daemon closed the connection")
        };
        let Frame::Control(json) = frame else {
            continue;
        };
        let msg = match serde_json::from_str::<ServerMsg>(&json) {
            Ok(msg) => msg,
            Err(e) => {
                println!("unknown message {json}: {e}");
                continue;
            }
        };
        if out.json {
            println!("[{:>5} ms] {json}", out.started.elapsed().as_millis());
        } else {
            out.show(&msg);
        }
        return Ok(msg);
    }
}

/// `SESSION:ASK:N`.
/// `SESSION:DRAFT`.
fn parse_draft(text: &str) -> Result<(String, String)> {
    match text.split_once(':') {
        Some((session, draft)) if !session.is_empty() && !draft.is_empty() => {
            Ok((session.to_string(), draft.to_string()))
        }
        _ => bail!("give SESSION:DRAFT, such as hb2c75bf:d1"),
    }
}

fn parse_answer(text: &str) -> Result<(String, String, usize)> {
    let parts: Vec<&str> = text.split(':').collect();
    let [session, ask, choice] = parts[..] else {
        bail!("--answer takes SESSION:ASK:N, such as k7q2:a1:0");
    };
    let choice = choice
        .parse()
        .with_context(|| format!("--answer: {choice:?} isn't an option's index"))?;
    Ok((session.to_string(), ask.to_string(), choice))
}

/// Prints the agent messages briefly, with the time since `started`.
struct Printer {
    json: bool,
    started: Instant,
}

impl Printer {
    fn show(&self, msg: &ServerMsg) {
        let at = format!("[{:>5} ms]", self.started.elapsed().as_millis());
        match msg {
            ServerMsg::Sessions(list) => {
                let c = &list.counts;
                println!(
                    "{at} sessions{}: {} ask, {} running, {} idle, {} planned, {} unread{}",
                    if list.locked { " (locked)" } else { "" },
                    c.ask,
                    c.running,
                    c.idle,
                    c.planned,
                    c.unread,
                    if list.older > 0 {
                        format!(", {} older", list.older)
                    } else {
                        String::new()
                    }
                );
                for s in &list.sessions {
                    let step = match (s.step, s.steps) {
                        (Some(step), Some(steps)) => format!(" {step}/{steps}"),
                        _ => String::new(),
                    };
                    println!(
                        "    {} {:?}{step} {}{} · {} · {}",
                        s.id,
                        s.state,
                        s.title,
                        if s.unread { " (unread)" } else { "" },
                        where_(s.via, &s.place, s.program.as_deref(), s.progress),
                        s.sub
                    );
                }
            }
            ServerMsg::Session(s) => {
                let step = match (s.step, s.steps) {
                    (Some(step), Some(steps)) => format!(", step {step} of {steps}"),
                    _ => String::new(),
                };
                println!("{at} session {} {:?}{step}: {}", s.id, s.state, s.title);
                println!(
                    "    {} · backend {:?}",
                    where_(s.via, &s.place, s.program.as_deref(), None),
                    s.backend
                );
                println!("    prompt: {}", s.prompt);
                for item in s.plan.iter().flatten() {
                    let mark = match item.state {
                        StepState::Done => "■",
                        StepState::Now => "▶",
                        StepState::Todo => "□",
                    };
                    println!("    {mark} {}", item.text);
                }
                for line in s.log.iter().take(8) {
                    println!("    log {}: {}", line.at, line.text);
                }
                if let Some(result) = &s.result {
                    for line in result.lines() {
                        println!("    | {line}");
                    }
                }
                if let Some(ask) = &s.ask {
                    println!("    asking {}: {}", ask.id, ask.question);
                }
                for line in s.screen.iter().flatten() {
                    println!("    screen │ {line}");
                }
            }
            ServerMsg::Ask(ask) => {
                println!(
                    "{at} ask {} in {} ({}): {}",
                    ask.id, ask.session, ask.title, ask.question
                );
                if let Some(detail) = &ask.detail {
                    println!("    {detail}");
                }
                for (i, option) in ask.options.iter().enumerate() {
                    println!(
                        "    {i}. {}{}{}",
                        option.title,
                        option
                            .right
                            .as_deref()
                            .map(|r| format!("  {r}"))
                            .unwrap_or_default(),
                        if ask.recommend == Some(i) {
                            "  PICK"
                        } else {
                            ""
                        }
                    );
                }
                println!("    answer: --answer {}:{}:N", ask.session, ask.id);
            }
            ServerMsg::Started { session } => println!("{at} started: session {session}"),
            ServerMsg::Draft { session, id, text } => {
                println!("{at} draft {id} for {session}: {text:?}");
                println!(
                    "    send it: --send-draft {session}:{id} (or drop it: --drop-draft {session}:{id})"
                );
            }
            ServerMsg::Unlocked {
                ok,
                ticket,
                ttl_s,
                wait_s,
            } => match (ok, ticket, wait_s) {
                (true, Some(ticket), _) => println!(
                    "{at} unlocked: ticket {ticket} for {} s",
                    ttl_s.unwrap_or(0)
                ),
                (true, None, _) => println!("{at} unlocked"),
                (false, _, Some(wait)) => println!("{at} not unlocked: wait {wait} s"),
                (false, _, None) => println!("{at} not unlocked: wrong PIN or ticket"),
            },
            ServerMsg::Refused {
                request,
                session,
                message,
            } => println!(
                "{at} refused {request}{}: {message}",
                session
                    .as_deref()
                    .map(|s| format!(" {s}"))
                    .unwrap_or_default()
            ),
            ServerMsg::Error { message } => println!("{at} error: {message}"),
            other => println!("{at} {other:?}"),
        }
    }
}

/// Where a session runs, as the board shows it: `[hc] DESKTOP · CODEX · 40%`.
fn where_(via: Via, place: &str, program: Option<&str>, progress: Option<u8>) -> String {
    let mut out = String::new();
    if via == Via::Herdr {
        out.push_str("[hc] ");
    }
    out.push_str(place);
    if let Some(program) = program {
        out.push_str(&format!(" · {}", program.to_uppercase()));
    }
    if let Some(progress) = progress {
        out.push_str(&format!(" · {progress}%"));
    }
    out
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

#[cfg(test)]
mod tests {
    #[test]
    fn answers_parse() {
        assert_eq!(
            super::parse_answer("k7q2:a1:2").unwrap(),
            ("k7q2".into(), "a1".into(), 2)
        );
        assert!(super::parse_answer("k7q2:a1").is_err());
        assert_eq!(
            super::parse_draft("hb2c75bf:d1").unwrap(),
            ("hb2c75bf".into(), "d1".into())
        );
        assert!(super::parse_draft("hb2c75bf").is_err());
        assert!(super::parse_answer("k7q2:a1:x").is_err());
    }
}
