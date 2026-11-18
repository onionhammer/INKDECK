# inkharness

An assistant with tools, skills and MCP servers that you start in any folder, and the daemon
INKDECK boards talk to.

- `inkharness`: a conversation in the terminal. `inkharness what time is it in Tokyo` answers once.
- `inkharness serve`: the daemon. Boards stream what their mic hears over TCP
  ([protocol](../docs/voice-protocol.md)) and get the spoken reply back; programs send text
  over HTTP.

Both read the same things from the folder they start in: `inkharness.toml`, `AGENTS.md`,
skills and `.mcp.json` ([What it picks up](#what-it-picks-up)).

## Install

```sh
CARGO_NET_GIT_FETCH_WITH_CLI=true cargo install --locked --git https://github.com/onionhammer/INKDECK inkharness
```

That builds it and puts `inkharness` in `~/.cargo/bin`. It needs Rust ([rustup.rs](https://rustup.rs)).
The repository is private, so the variable has cargo fetch through `git`, which has your
GitHub credentials (`gh auth setup-git` sets them up). Add `--features local` for the
in-process speech models ([In-process models](#in-process-models)), and run the line again to
update.

From a checkout, `inkharness/install.sh` does the same from the files you have. Then:

```sh
inkharness init     # a starter ~/.config/inkharness/inkharness.toml: say which model, where the key is
inkharness          # in any folder
```

`cargo uninstall inkharness` removes it.

## What it picks up

From the folder it starts in, and from the global config folder (`~/.config/inkharness`, or
`$INKHARNESS_HOME`):

| File | |
| --- | --- |
| `inkharness.toml` | The settings ([example](inkharness.toml.example)). The folder's file is layered over the global one: tables merge key by key, other values (arrays too) replace, and a stage that names another `backend` replaces the global one whole. Paths in a file are relative to its folder. `--config FILE` takes the folder file's place |
| `.env` | Keys (`NAME=value` lines), the folder's over the global one's. The environment wins over both |
| `AGENTS.md` | Instructions for the model, added to the system prompt (the global file first). `AGENT.md` or `CLAUDE.md` if there's no `AGENTS.md`. Cut at 64 KB |
| `skills/` | [Agent Skills](https://agentskills.io), as are `.agents/skills/` and `.claude/skills/`, the global `skills/`, and `[skills] dirs`. Of two skills with one name, the folder's is kept |
| `.mcp.json` | MCP servers, written as Claude Code, Cursor and others write them. They join `[[mcp]]`; of two servers with one name, the folder's wins |

```json
{
  "mcpServers": {
    "files": { "command": "npx", "args": ["-y", "@modelcontextprotocol/server-filesystem", "."] },
    "home": {
      "type": "http",
      "url": "http://homeassistant.local:8123/api/mcp",
      "headers": { "Authorization": "Bearer ${HOME_ASSISTANT_TOKEN}" }
    }
  }
}
```

`${VAR}` and `${VAR:-default}` come from the environment and `.env`; a server whose variable isn't
set is left out. `type` is `stdio` (the default with a `command`) or `http`; `sse` is tried as
streamable HTTP. `"disabled": true` skips a server, and inkharness also reads `tools` (an
allowlist) and `timeout_ms`. A folder's commands run when inkharness starts there, so read a
`.mcp.json` you didn't write first.

The conversation's first lines say what it read. `-v` logs each step, `-C DIR` starts as if in
`DIR`.

## Chat

```text
$ inkharness
inkharness 0.1.0 · openai/gpt-audio-mini
  read    ~/.config/inkharness/.env, ~/.config/inkharness/inkharness.toml, AGENTS.md
  skills  goodnight
  tools   7
› what's 17 times 23?
· run_js
17 times 23 is 391.
```

The reply prints as it's written, and each tool call is shown on a dim line (on stderr). `/reset`
starts over, `/tools` lists the tools, `/quit` or Ctrl-D leaves, and Ctrl-C stops a reply. From a
pipe, each line is a turn. The chat uses `[llm]`, or `[chat.llm]` if there is one. A model that
speaks (`output = "audio"`) still speaks there: OpenAI's audio models answer text only with
audio, so the transcript is shown and the audio goes nowhere. A text model in `[chat.llm]` is
cheaper and quicker.

## Serve

`inkharness serve` listens for boards on `listen` (`0.0.0.0:7391`) and answers HTTP on `http`
(`127.0.0.1:7392`; `""` turns it off). `--listen` and `--http` override them. With a `token`, boards
send it in `hello` (`voice_token` in `SD:/inkdeck.ini`), and HTTP requests as
`Authorization: Bearer <token>`. A config with no speech in or out for its model serves HTTP
only, with a warning.

```sh
curl -s localhost:7392/chat -d '{"text": "what speakers are there?"}' -H 'content-type: application/json'
# {"conversation":"c1","reply":"The kitchen and the office.","tools":["list_speakers"]}
curl -sN localhost:7392/chat -H 'content-type: application/json' \
  -d '{"text": "and which is playing?", "conversation": "c1", "stream": true}'
# data: {"tool":"list_speakers"}   data: {"text":"The kitchen"} ...   data: {"done":true,...}
```

| Request | |
| --- | --- |
| `GET /health` | `{"ok": true, "model": …}`, and with the token (or none set) the tools and skills |
| `POST /chat` | `{"text", "conversation"?, "stream"?}`. Without `conversation`, a new one starts and its id comes back; send it again to go on (the last 100 are kept, in memory). The answer is `{"reply", "conversation", "tools"}`; with `"stream": true`, server-sent events: `{"text"}` as it's written, `{"tool"}` as a call starts, then `{"done": true, "reply", "conversation"}` or `{"error"}` |

HTTP turns get the same model, tools, skills and instructions as the chat.

## A spoken turn

A board streams what its mic hears; inkharness turns it into text, asks a model that can use
tools (speakers through Home Assistant), and streams the spoken reply back:

```
mic → TCP → speech-to-text → model + tools → text-to-speech → TCP → speaker
```

A model that hears can take the recording itself, and one that speaks can say its own reply,
which leaves out one stage or both ([Models that hear or speak](#models-that-hear-or-speak)).

Each stage is a trait with swappable backends, chosen in `inkharness.toml`:

| Stage | Backends |
| --- | --- |
| Speech to text | `openai_compat` (any `/audio/transcriptions` server, hosted or local), `sherpa_streaming` (in-process), `mock` |
| Model | `anthropic` (Claude), `openai_compat` (any `/chat/completions` server: Ollama, llama.cpp, vLLM; with `input = "audio"` and `output = "audio"` for models that hear and speak), `mock` |
| Text to speech | `openai_compat` (any `/audio/speech` server that returns PCM), `sherpa_pocket` (in-process), `mock` |
| Tools | Home Assistant's REST API: `list_speakers`, `play_music`, `media_control` (media players only) |
| MCP | Any [MCP](https://modelcontextprotocol.io) server's tools (`[[mcp]]`, `.mcp.json`), over stdio or streamable HTTP, through the official Rust SDK (`rmcp`); named `<server>_<tool>`, optionally limited to an allowlist. `examples/mcp_demo.rs` is a small server to try it with |
| Scripts | `run_js`: JavaScript in a [QuickJS](https://github.com/quickjs-ng/quickjs) sandbox (`rquickjs`), for arithmetic and dates, or to chain the home tools, which it calls as `tools.list_speakers()`; no network, files or timers, with time, memory and tool-call limits (`[js]`) |
| Skills | [Agent Skills](https://agentskills.io) (`skills/`, `[skills]`): a folder per skill with a `SKILL.md`. The system prompt lists each one's name and description; `load_skill` returns the instructions and `read_skill_file` the skill's other files, read from inside its folder only (16 KB each). Up to 32 skills |

The stages overlap: the reply's first sentence is synthesised and sent while the model is still
writing the rest. Each turn logs its timings from the end of speech (`stt_ms`,
`first_token_ms`, `first_audio_ms`, `total_ms`), and what it cost: `cost` adds up the prices
the responses stated (OpenRouter states them for speech-to-text and the model, not for
text-to-speech; `unpriced` counts those), and `requests` lists each request's stage and
OpenRouter generation id, which `GET /api/v1/generation?id=` prices.

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
| `inkharness/` | The program: `main.rs` (the commands), `place.rs` (what it picks up from a folder), `harness.rs` (the tools and prompt both front-ends share), `chat.rs` (text turns and the terminal), `http.rs` (the HTTP API), `serve.rs` (the daemon), `session.rs` (a board's connection and its turns), `stt.rs`, `llm/` (`tool_loop.rs`, the turn every model backend shares, and one file per provider API: `anthropic.rs`, `openai.rs`), `tts.rs` (and its cache), `tools.rs`, `js.rs` (the JavaScript sandbox), `mcp.rs` (MCP servers as tools), `skills.rs` (Agent Skills), `meter.rs` (what a turn cost), `sentence.rs`, `local.rs` (sherpa-onnx) |
| `voice-client/` | Plays a board's part from a WAV file |
| `skills/` | Agent Skills, found when inkharness starts in this folder; `goodnight` pauses every speaker that's playing in one `run_js` script |
| `install.sh` | Installs inkharness from this checkout (`cargo install --path`) |
| `inkharness.toml.example` | Hosted services; copy to `inkharness.toml` (gitignored here) or to `~/.config/inkharness/` |
| `inkharness.openrouter.toml.example` | The same through OpenRouter, one key for all three stages |
| `setup_openrouter.py` | Asks for that key and the models, tries them, writes `inkharness.toml` |
| `compare_pipelines.py` | Runs the same spoken commands through each pipeline shape and compares cost, tokens, latency and what happened in the home |
| `inkharness.mock.toml` | Every stage mocked: no keys, no models |
| `inkharness.local.toml` | In-process speech in and out, mock model |
| `start.sh` | Runs the daemon from this folder, built from the checkout |

## Develop

```sh
cargo test --workspace
cargo run --release -p inkharness -- serve --config inkharness.mock.toml
say -o command.wav --data-format=LEI16@16000 "What speakers are there?"   # macOS; any 16-bit mono WAV
cargo run --release -p voice-client -- --wav command.wav
cargo clean
```

With hosted services: copy `inkharness.toml.example`, fill in the model names, and put the keys it
names (`ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, `HOME_ASSISTANT_TOKEN`) in the environment or `.env`.

Through OpenRouter, `uv run setup_openrouter.py` does that by asking: it takes the key (kept in
`.env`, gitignored), lists the models and voices to pick from, tries each stage with a short
request, plays the speech model's PCM to find its sample rate, and writes `inkharness.toml`.
For a model that thinks before it answers, `reasoning = "off"` under `[llm]` (or a level:
`minimal` to `xhigh`) is sent as OpenRouter's `reasoning` parameter; the script offers it.

## Models that hear or speak

Under `[llm]` (`openai_compat`), two settings drop stages:

| Setting | The model gets / gives | Then |
| --- | --- | --- |
| `input = "audio"` | The recording, as an `input_audio` WAV part | `[stt]` is optional. If it is there, it runs alongside the model, only for the device's display and the history (which otherwise keeps the recording) |
| `output = "audio"`, `voice` | Its own speech: PCM16 at `audio_rate` (24000 for OpenAI), with a transcript for the display | `[tts]` is optional. It is still needed for a `[think]` model that writes, and for the think phrases |

```toml
[llm]
backend = "openai_compat"
base_url = "https://openrouter.ai/api/v1"
model = "openai/gpt-audio-mini"
api_key_env = "OPENROUTER_API_KEY"
input = "audio"
output = "audio"
voice = "alloy"
```

Models on OpenRouter that hear and use tools:
`curl "https://openrouter.ai/api/v1/models?input_modalities=audio&supported_parameters=tools"`; ones that
also speak: `...&output_modalities=audio`. inkharness checks at startup that every model has the stages
it needs, and that a speaking model and `[tts]` play at the same rate (a reply has one
`reply_start`). An utterance that is silence (peak under about -54 dBFS) isn't sent to a model
that hears. Decision models (OpenRouter's `/api/alpha/decisions`, such as Cloudflare's Clef Omni)
hear audio too, but only answer typed questions (yes or no, a choice, a score), never text, so
they can't take the model's place.

`compare_pipelines.py` measures the three shapes against each other. It runs each as a real
inkharness with a fake Home Assistant that records the actions it's asked for, sends the same
commands made with `say`, and prices every request through OpenRouter's generation endpoint:

```sh
cargo build -p inkharness -p voice-client
uv run compare_pipelines.py --repeat 2 --out results.json
cargo clean
```

Measured 2026-11-17 through OpenRouter, 8 commands sent twice each (6 speaker actions, "what
speakers do we have?", a sum), on fresh connections, with the speech cache off. Latency is from
the end of speech; cost is what OpenRouter charged per turn.

| Pipeline | Stages | Correct | First audio | Turn | $/turn | Of which STT / model / TTS |
| --- | --- | --- | --- | --- | --- | --- |
| whisper-large-v3-turbo → qwen3.6-35b-a3b:nitro → simba-3.5 (today's) | 3 | 16/16 | 2.65 s | 2.89 s | $0.00070 | 2% / 6% / 93% |
| gpt-audio-mini hears → simba-3.5 | 2 | 16/16 | 2.63 s | 2.72 s | $0.00179 | 0 / 60% / 40% |
| qwen3.8-omni-flash hears → simba-3.5 | 2 | 16/16 | 3.42 s | 5.43 s | $0.00151 | 0 / 28% / 72% |
| gemini-3.5-flash-lite hears (reasoning minimal) → simba-3.5 | 2 | 14/16 | 5.79 s | 5.80 s | $0.00207 | 0 / 76% / 24% |
| gpt-audio-mini hears and speaks | 1 | 16/16 | 2.42 s | 2.77 s | $0.00122 | 0 / 100% / 0 |
| gpt-audio hears and speaks | 1 | 16/16 | 2.40 s | 2.92 s | $0.00965 | 0 / 100% / 0 |

- Text-to-speech is most of today's cost (simba-3.5 is $15 per million characters). Speech-to-text
  costs almost nothing ($0.000015 a turn) and takes about 0.67 s.
- Skipping it saves less time than that. A command takes 2.6 to 2.8 model requests
  (`list_speakers`, the action, the answer), and the hearing models are slower per request than
  qwen3.6:nitro. They are also dearer: their text tokens cost more, and the recording goes again
  with every request of the turn.
- gpt-audio-mini hearing and speaking is the best of the alternatives: as accurate, about 0.2 s
  sooner to the first audio, at 1.7 times the cost. gpt-audio is no faster at 14 times the cost.
- qwen3.8-omni-flash says what it is about to do before each tool call ("I'll check the speakers
  first"), so it starts speaking sooner but finishes later, and pays to have that spoken.
- Gemini made 4 to 5 requests a turn and sometimes ended with nothing to say. Its reasoning
  can't be turned off, and inkharness doesn't yet send a model's `reasoning_details` back between
  tool rounds, which OpenRouter says reasoning models need for tool calling. Treat its row as
  unfair until it does.

## In-process models

`--features local` links [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) (Apache-2.0; it
downloads prebuilt libraries at build time). Models go in `models/` (gitignored), from the
project's releases:

```sh
mkdir -p models && cd models
base=https://github.com/k2-fsa/sherpa-onnx/releases/download
curl -L $base/asr-models/sherpa-onnx-streaming-zipformer-en-kroko-2025-08-06.tar.bz2 | tar xj
curl -L $base/tts-models/sherpa-onnx-pocket-tts-int8-2026-01-26.tar.bz2 | tar xj
cd .. && cargo run --release -p inkharness --features local -- serve --config inkharness.local.toml
```

- The speech-to-text model decodes while the utterance arrives, so the transcript is ready
  tens of milliseconds after the end of speech. The 57 MB model above is the small one; the
  Nemotron streaming models in the same release are larger and more accurate, with different
  file names (`encoder`, `decoder`, `joiner` in the config).
- This Pocket TTS export is licensed for non-commercial use (see the LICENSE in its folder).

Measured on an Apple Silicon Mac, 2 threads, CPU, 2026-11-15, with the mock model: transcript
62 ms after the end of speech; first audio 650–740 ms (about 570 ms of that is Pocket TTS
producing its first chunk); a 5 s reply fully synthesised in 1.1–1.3 s.
