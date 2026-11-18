# Voice protocol

How the device talks to the voice daemon (`voiced/`). The device sends what the mic hears; the
daemon turns it into text, asks a language model, and sends the spoken reply back. The Rust
types are in `voiced/voice-proto/src/lib.rs`; keep this file and that one in step.

## Transport

One TCP connection, opened by the device to the daemon (default port 7391). No TLS: it is meant
for the home network. Both sides should set `TCP_NODELAY`.

Every frame is:

| Bytes | Field | |
| --- | --- | --- |
| 1 | type | `1` control, `2` audio |
| 2 | length | payload bytes, little-endian, 0–65535 |
| length | payload | |

- **Control** payloads are one UTF-8 JSON object with a `type` field.
- **Audio** payloads are PCM: signed 16-bit little-endian, mono, whole samples.
- A frame of any other type is skipped. A control message with an unknown `type` is ignored.
  Both let later versions add things.

## Messages

Device → daemon:

| `type` | Fields | Meaning |
| --- | --- | --- |
| `hello` | `proto` (1), `device` (a name), `token`, `mic_rate` (Hz, 8000–48000) | First message. The daemon closes the connection on a wrong token or protocol |
| `listen_start` | | The audio frames that follow are one utterance. Cancels a reply still in progress |
| `listen_stop` | | The utterance is complete; the daemon answers |
| `abort` | | Drop the utterance or the reply in progress |

Daemon → device:

| `type` | Fields | Meaning |
| --- | --- | --- |
| `hello` | `proto`, `session` | Answer to the device's hello |
| `stt` | `text` | What the daemon heard (may be empty). When the model hears the recording itself, this comes alongside the reply, or not at all if the daemon has no speech-to-text |
| `text` | `delta` | The reply's text as it is generated, for the display. Can arrive before `reply_start` |
| `reply_start` | `rate` (Hz) | Audio frames follow at this sample rate |
| `reply_end` | | The turn is over |
| `error` | `message` | Something failed; a `reply_end` follows unless the connection closes |

## A turn

```
device                                   daemon
  hello                          →
                                 ←       hello
  listen_start                   →
  audio, audio, …                →       (32 ms frames, 1024 bytes at 16 kHz)
  listen_stop                    →
                                 ←       stt {text}
                                 ←       text {delta} …
                                 ←       reply_start {rate}
                                 ←       audio … (interleaved with more text)
                                 ←       reply_end
```

The connection stays open for further turns; the daemon keeps the conversation's last few turns
as context for as long as it does.

## Rules

- **Half-duplex.** The device doesn't send mic audio while it plays a reply.
- **Flow control is TCP's.** The daemon sends reply audio as fast as it is synthesised, which is
  faster than it plays. The device stops reading when its playback buffer is full; the daemon
  then waits. Nothing is dropped.
- **`reply_end` always closes a turn**, including one with no audio (nothing heard, an error) and
  one cut short by `abort` or a new `listen_start`. After a turn finishes on its own just as an
  `abort` arrives, a second `reply_end` can follow; ignore a `reply_end` with no turn open.
- **After `abort`**, audio already on its way still arrives. The device discards audio frames
  until the `reply_end`.
- **Limits.** An utterance over 60 s is refused with an `error`.
- The token crosses the network in plain text, like the audio.

## Testing without the board

`voiced/voice-client` plays the device's part from a WAV file:

```sh
cd voiced
cargo run -p voiced -- voiced.mock.toml          # every stage mocked, no keys needed
cargo run -p voice-client -- --silence 1.5       # or --wav command.wav
```

It prints each message with its time since `listen_stop` and saves the reply audio.
