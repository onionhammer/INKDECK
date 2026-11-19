# Voice protocol

How the device talks to the assistant daemon (`inkharness serve`, in `inkharness/`). The device sends what the mic hears; the
daemon turns it into text, asks a language model, and sends the spoken reply back. The Rust
types are in `inkharness/voice-proto/src/lib.rs`; keep this file and that one in step.

## Transport

One TCP connection, opened by the device to the daemon (default port 7391), encrypted with the
token both ends hold ([Encryption](#encryption)). Both sides should set `TCP_NODELAY`.

Every frame is:

| Bytes | Field | |
| --- | --- | --- |
| 1 | type | see below |
| 2 | length | payload bytes, little-endian, 0–65535 |
| length | payload | |

| Type | Frame | Payload |
| --- | --- | --- |
| `1` | control | One UTF-8 JSON object with a `type` field |
| `2` | audio | PCM: signed 16-bit little-endian, mono, whole samples |
| `3` | handshake | A Noise handshake message: the first frame each way |
| `4` | sealed | A control or audio frame, encrypted: see [Encryption](#encryption) |

After the handshake every frame is sealed, both ways, and a frame that isn't (or doesn't open)
ends the connection. Inside a sealed frame, one of a type the receiver doesn't know is skipped,
and so is a control message with an unknown `type`: both let later versions add things.

## Encryption

The token (`token` in `inkharness.toml`, `voice_token` in `SD:/inkdeck.ini`) never crosses the
network. Both ends make a key from it, and the connection opens with a
[Noise](https://noiseprotocol.org/noise.html) handshake that only a side with that key can
complete:

- **Pattern:** `Noise_NNpsk0_25519_AESGCM_SHA256`, prologue `inkharness voice 2` (ASCII, no
  terminator). X25519 for the ephemeral keys, AES-256-GCM, SHA-256. AES-GCM because the board's
  mbedTLS has it in hardware; it carries ChaCha20 neither in hardware nor at all.
- **The key** (the `psk`): HKDF-SHA256 with the token's UTF-8 bytes as the input, the salt
  `inkharness voice psk` and no info, 32 bytes. An empty token makes a key too: the connection is
  still encrypted, but anyone can open one. Both ends need the same token, or both none.
- **Message 1** (device → daemon, a handshake frame): the device's ephemeral public key, then its
  `hello` sealed. **Message 2** (daemon → device): the daemon's ephemeral public key, then its
  answer to the hello sealed: its own `hello`, or an `error` saying why not (an unsupported
  protocol, a mic rate out of range) before it closes the connection. Carrying the hellos, the
  handshake adds no round trip.
- **A different token:** the daemon can't open message 1. It sends one unencrypted control frame,
  `{"type": "error", "message": "wrong token"}`, and closes the connection, so the device can say
  the tokens differ. Nothing else ever goes unencrypted.
- **After the handshake** each side has a key for what it sends (Noise's `Split()`: the first key
  is the device's, the second the daemon's). A sealed frame's payload is the AES-256-GCM
  encryption of the inner frame's type byte followed by its payload, then the 16-byte tag; the
  nonce is a counter per direction, from 0, as Noise numbers transport messages (4 zero bytes,
  then the counter big-endian). A sealed frame carries at most 65,518 bytes of payload.
- **What it gives.** No one on the network can read or change the token, the audio, the text or
  the tool calls. A frame changed, dropped, replayed or reordered on the way doesn't open, and
  ends the connection. Fresh ephemeral keys per connection give forward secrecy: a token stolen
  later doesn't open recordings of earlier sessions. The exception is message 1, sealed with the
  token's key alone, but it holds only the device's hello (its name, mic rate and tools).
- **What it doesn't.** Like any scheme built on a shared secret, a recorded handshake lets someone
  guess tokens offline. Use a long random one: `openssl rand -hex 24`.

`inkharness/voice-proto/src/crypto.rs` is the daemon's side (on the
[snow](https://crates.io/crates/snow) crate), `src/voice_noise.cpp` the board's (on mbedTLS).
crypto.rs's `reference` test runs the board's steps against snow.

## Messages

Device → daemon:

| `type` | Fields | Meaning |
| --- | --- | --- |
| `hello` | `proto` (2), `device` (a name), `mic_rate` (Hz, 8000–48000), `tools` (optional, see [Device tools](#device-tools)) | First message, in the handshake's message 1 |
| `listen_start` | | The audio frames that follow are one utterance. Cancels a reply still in progress |
| `listen_stop` | | The utterance is complete; the daemon answers |
| `abort` | | Drop the utterance or the reply in progress |
| `tool_result` | `id`, `text`, `error` (optional, default false) | The answer to a `tool_call` with that `id`: what the tool did or found, or with `error` why it couldn't |

Daemon → device:

| `type` | Fields | Meaning |
| --- | --- | --- |
| `hello` | `proto`, `session` | Answer to the device's hello, in the handshake's message 2 |
| `stt` | `text` | What the daemon heard (may be empty). When the model hears the recording itself, this comes alongside the reply, or not at all if the daemon has no speech-to-text |
| `text` | `delta` | The reply's text as it is generated, for the display. Can arrive before `reply_start` |
| `reply_start` | `rate` (Hz) | Audio frames follow at this sample rate |
| `reply_end` | | The turn is over |
| `error` | `message` | Something failed; a `reply_end` follows unless the connection closes |
| `tool_call` | `id` (a number), `name`, `input` (an object) | The model calls one of the device's tools; answer with `tool_result` |

## A turn

```
device                                   daemon
  handshake 1 {hello}            →
                                 ←       handshake 2 {hello}
  listen_start                   →       (every frame from here on sealed)
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

## Device tools

A client can do things itself: the INKDECK opens books, sets alarms and reports its battery. It
lists its tools in its `hello`, and the daemon adds them to that session's tools beside its own
(Home Assistant, MCP servers, `run_js`). The model calls them like any other tool, and scripts
in `run_js` can call them too. Nothing about a client's tools is built into the daemon, so each
kind of client brings its own.

```json
{"type": "hello", "proto": 2, "device": "inkdeck", "mic_rate": 16000,
 "tools": [{"name": "open_book",
            "description": "Opens a book on the board where it was left…",
            "input_schema": {"type": "object",
                             "properties": {"path": {"type": "string"}},
                             "required": ["path"]}}]}
```

- `name`: letters, digits, `_` and `-`, up to 64. `description` is for the model: say what the
  tool does and when to use it. `input_schema` is the JSON Schema of the input object (default:
  no input).
- The daemon leaves out tools with an invalid name, or one already in use (logging each), and
  takes at most 32.
- A daemon from before device tools ignores the `tools` field.

During a turn, between `listen_stop` and `reply_end`:

```
                                 ←       tool_call {id: 1, name: "open_book", input: {...}}
  tool_result {id: 1, text: "…"} →
                                 ←       text {delta} …
```

- **Answer every call, quickly.** The turn waits for the answer, for up to 15 s, after which
  the model hears that the device didn't answer. Calls in the same round can overlap; answer each
  by its `id`.
- **A turn cut short forgets its calls.** After `abort` or a new `listen_start`, the daemon drops
  the calls still waiting, and ignores an answer that comes for them. The device should not run a
  call from the old turn (one that arrives before that turn's `reply_end`).
- The answer is text for the model: short, plain and complete, since the model sees nothing else.

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

## Protocol 1

Before encryption the device sent its `hello` as the first control frame, with `"proto": 1` and
the `token` in it, the daemon answered with its own, and every frame went unencrypted. A daemon
takes it only with `plaintext = true` in `inkharness.toml`, for boards still on older firmware
(it logs a warning for each such connection); otherwise it answers with an `error` and closes. A
device on protocol 2 never falls back to it, so no one can talk it into sending the token.

## Testing without the board

`inkharness/voice-client` plays the device's part from a WAV file:

```sh
cd inkharness
cargo run -p inkharness -- serve --config inkharness.mock.toml   # every stage mocked, no keys needed
cargo run -p voice-client -- --silence 1.5                         # or --wav command.wav
```

It prints each message with its time since `listen_stop` and saves the reply audio. `--token`
gives the token (the mock config has none); `--plaintext` speaks protocol 1, as older boards do. With
`--tools tools.json` (an array of tools as in the hello, each with an `answer`), it announces
them and answers each call with its tool's `answer`.
