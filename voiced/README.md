# voiced

The voice assistant daemon. The device streams what its mic hears over TCP
([protocol](../docs/voice-protocol.md)); voiced turns it into text, asks a language model that
can use tools (speakers through Home Assistant), and streams the spoken reply back.

```
mic → TCP → speech-to-text → model + tools → text-to-speech → TCP → speaker
```

Each stage is a trait with swappable backends, chosen in `voiced.toml`:

| Stage | Backends |
| --- | --- |
| Speech to text | `openai_compat` (any `/audio/transcriptions` server, hosted or local), `sherpa_streaming` (in-process), `mock` |
| Model | `anthropic` (Claude), `openai_compat` (any `/chat/completions` server: Ollama, llama.cpp, vLLM), `mock` |
| Text to speech | `openai_compat` (any `/audio/speech` server that returns PCM), `sherpa_pocket` (in-process), `mock` |
| Tools | Home Assistant's REST API: `list_speakers`, `play_music`, `media_control` (media players only) |
| MCP | Any [MCP](https://modelcontextprotocol.io) server's tools (`[[mcp]]`), over stdio or streamable HTTP, through the official Rust SDK (`rmcp`); named `<server>_<tool>`, optionally limited to an allowlist. `examples/mcp_demo.rs` is a small server to try it with |
| Scripts | `run_js`: JavaScript in a [QuickJS](https://github.com/quickjs-ng/quickjs) sandbox (`rquickjs`), for arithmetic and dates, or to chain the home tools, which it calls as `tools.list_speakers()`; no network, files or timers, with time, memory and tool-call limits (`[js]`) |
| Skills | [Agent Skills](https://agentskills.io) (`[skills]`): a folder per skill with a `SKILL.md`. The system prompt lists each one's name and description; `load_skill` returns the instructions and `read_skill_file` the skill's other files, read from inside its folder only (16 KB each). Up to 32 skills |

The stages overlap: the reply's first sentence is synthesised and sent while the model is still
writing the rest. Each turn logs its timings from the end of speech (`stt_ms`,
`first_token_ms`, `first_audio_ms`, `total_ms`).

With a `[think]` section the model gets a `think_harder` tool for questions that need more
than a quick answer. Calling it hands the turn to a second model (one that reasons, or a
larger one): a short phrase is said at once while it works, then its answer. Short sentences
are cached as audio (`[cache]`), and the phrases are synthesised at startup, so they play
without waiting for a request. The model's replies aren't cached: a cached "Playing jazz in
the kitchen" would skip the tool call that plays it.

A skill costs the model a round to load, and another for each file it reads, so what a task
always needs belongs in `SKILL.md` itself: the goodnight skill carries its script. `run_js`
scripts can't call the skill tools.

## Layout

| Path | |
| --- | --- |
| `voice-proto/` | Framing and messages, shared by the daemon and the test client |
| `voiced/` | The daemon: `session.rs` (a connection and its turns), `stt.rs`, `llm/` (`tool_loop.rs`, the turn every model backend shares, and one file per provider API: `anthropic.rs`, `openai.rs`), `tts.rs` (and its cache), `tools.rs`, `js.rs` (the JavaScript sandbox), `mcp.rs` (MCP servers as tools), `skills.rs` (Agent Skills), `sentence.rs`, `local.rs` (sherpa-onnx) |
| `voice-client/` | Plays the device's part from a WAV file |
| `skills/` | Agent Skills; `goodnight` pauses every speaker that's playing in one `run_js` script |
| `voiced.toml.example` | Hosted services; copy to `voiced.toml` (gitignored) |
| `voiced.openrouter.toml.example` | The same through OpenRouter, one key for all three stages |
| `setup_openrouter.py` | Asks for that key and the models, tries them, writes `voiced.toml` |
| `voiced.mock.toml` | Every stage mocked: no keys, no models |
| `voiced.local.toml` | In-process speech in and out, mock model |

## Run

```sh
cargo test --workspace
cargo run --release -p voiced -- voiced.mock.toml
say -o command.wav --data-format=LEI16@16000 "What speakers are there?"   # macOS; any 16-bit mono WAV
cargo run --release -p voice-client -- --wav command.wav
```

With hosted services: copy `voiced.toml.example`, fill in the model names, export the keys it
names (`ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, `HOME_ASSISTANT_TOKEN`), and run with that file.

Through OpenRouter, `uv run setup_openrouter.py` does that by asking: it takes the key (kept in
`.env`, gitignored), lists the models and voices to pick from, tries each stage with a short
request, plays the speech model's PCM to find its sample rate, and writes `voiced.toml`.
For a model that thinks before it answers, `reasoning = "off"` under `[llm]` (or a level:
`minimal` to `xhigh`) is sent as OpenRouter's `reasoning` parameter; the script offers it.

Keys can also sit in a `.env` file (`NAME=value` lines) next to the config or in the working
directory; the environment wins over it.

## In-process models

`--features local` links [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) (Apache-2.0; it
downloads prebuilt libraries at build time). Models go in `models/` (gitignored), from the
project's releases:

```sh
mkdir -p models && cd models
base=https://github.com/k2-fsa/sherpa-onnx/releases/download
curl -L $base/asr-models/sherpa-onnx-streaming-zipformer-en-kroko-2025-08-06.tar.bz2 | tar xj
curl -L $base/tts-models/sherpa-onnx-pocket-tts-int8-2026-01-26.tar.bz2 | tar xj
cd .. && cargo run --release -p voiced --features local -- voiced.local.toml
```

- The speech-to-text model decodes while the utterance arrives, so the transcript is ready
  tens of milliseconds after the end of speech. The 57 MB model above is the small one; the
  Nemotron streaming models in the same release are larger and more accurate, with different
  file names (`encoder`, `decoder`, `joiner` in the config).
- This Pocket TTS export is licensed for non-commercial use (see the LICENSE in its folder).

Measured on an Apple Silicon Mac, 2 threads, CPU, 2026-11-15, with the mock model: transcript
62 ms after the end of speech; first audio 650–740 ms (about 570 ms of that is Pocket TTS
producing its first chunk); a 5 s reply fully synthesised in 1.1–1.3 s.
