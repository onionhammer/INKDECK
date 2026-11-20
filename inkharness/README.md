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
(`127.0.0.1:7392`; `""` turns it off). `--listen` and `--http` override them. Boards encrypt the
connection with the `token` (`voice_token` in `SD:/inkdeck.ini`, the same on both ends): each
side proves it has it without sending it, and every frame after the handshake is sealed
([docs/voice-protocol.md](../docs/voice-protocol.md#encryption)). Make it long and random
(`openssl rand -hex 24`). HTTP requests send it as `Authorization: Bearer <token>`. Boards on
firmware from before encryption speak protocol 1, which sends the token unencrypted: the daemon
refuses them unless `plaintext = true`. A config with no speech in or out for its model serves
HTTP only, with a warning.

On a machine with Tailscale, the default `listen` covers its tailnet address too, so a board with
a `[tailscale]` key in its `inkdeck.ini` reaches the daemon from any Wi-Fi with `voice_host` set
to the machine's name on the tailnet (`macbook`) or its 100.x address. The token still encrypts
the connection inside the tunnel. The tailnet's access rules must let the board reach port 7391.

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
| `/sessions` | The agent sessions, with an `[agent]` table ([Agent sessions](#agent-sessions)) |

HTTP turns get the same model, tools, skills and instructions as the chat.

## Agent sessions

With an `[agent]` table, the daemon keeps **sessions**: tasks that live on the daemon, are kept
on disk, and go on working with no board connected (issue #52). A board starts one by talking
on its Agent screen (an agent turn), follows the list and each session's plan and log, answers
the questions sessions ask, and talks into them; programs do the same over HTTP. Without the
table there are none, and boards talk as they always did.

```toml
[agent]
dir = "sessions"        # where sessions are kept; relative to the file this is in
place = "ONIONSERVE"    # the name boards show for where they run; default: the host's name, uppercased
pin_sha256 = "…"        # optional: printf %s 1234 | sha256sum. Boards unlock with the PIN, HTTP sends it
ticket_minutes = 60     # how long an unlock lets a board's new connections back in without the PIN
keep_days = 30          # finished sessions older than this are deleted
max_rounds = 40         # model rounds per run before a session fails with "too many steps"

[agent.llm]             # optional: the model sessions run on; unset: [chat.llm], else [llm]
backend = "openai_compat"
base_url = "https://openrouter.ai/api/v1"
model = "anthropic/claude-haiku-5.5"
api_key_env = "OPENROUTER_API_KEY"

[talk]                  # what HOLD BOOT outside the Agent may use; unset: every tool
tools = ["run_js", "web_search", "think_harder", "home_*"]   # names, or prefixes ending in *; the board's own always stay
```

- A session runs on `[agent.llm]` if there is one, else on `[chat.llm]`, else on `[llm]`: any
  backend `[llm]` takes, checked at startup like the others, and named on the log's
  `agent sessions` line. A text model suits it; one that speaks would pay for audio nobody
  hears. It gets the daemon's tools (home, MCP, web search, run_js, skills) and two of its own: `plan`, a checklist the board shows as a
  meter, and `ask_choice`, which puts a question with 2 to 6 options to the user and waits,
  minutes or days, for the answer. The board's tools stay out: it may be off. The prompt tells
  the model it works alone for someone who reads results on a small e-paper screen: plan first,
  keep the log terse, ask only when the choice is the user's, end with a short Markdown result,
  and open it with a heading when it is meant to be kept (the session is then a note).
- What it writes between tool calls and the calls themselves go to its log. Talking into a
  working session reaches the model at its next round; into an asking one, it answers in words;
  into a finished one, it starts again. Each run logs what it cost, as turns do.
- Sessions are JSON files in `dir`. After a restart, the working ones start again, told their
  plan and what they had done (the results of earlier tool calls are gone), and the asking ones
  wait on; an answer then starts them the same way.
- **Talk and agent stay apart.** A talk turn (`HOLD BOOT` outside the Agent, and every board from
  before sessions) keeps its own short conversation and the tools `[talk]` leaves it, and never
  sees a session. The token is on the board's SD card, so `pin_sha256` keeps a lost board or a
  copied card from reaching the agent: until a connection unlocks, it gets talk turns and the
  sessions' counts only. Wrong PINs back off after 3 (30 s, doubling to 30 min), and the count
  survives a restart.

The protocol is in [docs/voice-protocol.md](../docs/voice-protocol.md#agent-sessions). Over
HTTP, with the token, and with `X-Inkharness-Pin: <pin>` when a PIN is set (403 without it or
with a wrong one, 429 while wrong ones back off):

| Request | |
| --- | --- |
| `GET /sessions` | The list, as a board gets it unlocked |
| `GET /sessions/{id}` | One session in full |
| `POST /sessions` | `{"text"}` starts one: `{"session": "k7q2"}` |
| `POST /sessions/{id}/say` | `{"text"}` talks into it (answers its question in words) |
| `POST /sessions/{id}/answer` | `{"ask": "a1", "choice": 0}` |
| `POST /sessions/{id}/stop` | Stops a working or asking one |
| `DELETE /sessions/{id}` | Deletes it |

```sh
pin=(-H "Authorization: Bearer $TOKEN" -H "X-Inkharness-Pin: 1234" -H 'content-type: application/json')
curl -s "${pin[@]}" localhost:7392/sessions -d '{"text": "find a plumber for the kitchen leak this week"}'
curl -s "${pin[@]}" localhost:7392/sessions/k7q2/answer -d '{"ask": "a1", "choice": 0}'
```

With `[llm] backend = "mock"`, sessions play through a scripted run instead (`agent/mock.rs`): a
plan of three steps, a tool call and a result; "ask" in the task asks which of two plumbers to
book, "slow" takes 3 s a step, "note" makes it a note, "fail" fails it.

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
| Web search | `web_search` (`[search]`, `search.rs`): [SearXNG](https://docs.searxng.org) (your own instance, no key), [Brave](https://brave.com/search/api/) or [Tavily](https://tavily.com) (keys). Several are tried in order until one answers. The model gets each result's title, snippet, date and link, and Tavily's short answer; `recent` limits it to the last day, week, month or year |
| Device tools | What the board can do itself, announced in its hello: the INKDECK's are `find_books`, `open_book`, `weather` (its own forecast), `board_status`, `set_volume`, `set_alarm`, `show`, `show_note` (a long answer on the screen, kept as a note) and `read_note` (`src/apps/voice_tools.h`). A call goes down the board's connection and waits for its answer (up to 15 s). Each client brings its own; none are built in ([protocol](../docs/voice-protocol.md#device-tools), `device.rs`) |
| MCP | Any [MCP](https://modelcontextprotocol.io) server's tools (`[[mcp]]`, `.mcp.json`), over stdio or streamable HTTP, through the official Rust SDK (`rmcp`); named `<server>_<tool>`, optionally limited to an allowlist. `examples/mcp_demo.rs` is a small server to try it with |
| Scripts | `run_js`: JavaScript in a [QuickJS](https://github.com/quickjs-ng/quickjs) sandbox (`rquickjs`), for arithmetic and dates, or to chain the home and device tools, which it calls as `tools.list_speakers()`; no network, files or timers, with time, memory and tool-call limits (`[js]`) |
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
| `inkharness/` | The program: `main.rs` (the commands), `place.rs` (what it picks up from a folder), `harness.rs` (the tools and prompt both front-ends share), `chat.rs` (text turns and the terminal), `http.rs` (the HTTP API), `serve.rs` (the daemon), `session.rs` (a board's connection and its turns), `agent/` (agent sessions: `store.rs` on disk and as boards see them, `lock.rs` the PIN, `tools.rs` `ask_choice` and `plan`, `mock.rs` a scripted session model), `device.rs` (the board's own tools), `stt.rs`, `llm/` (`tool_loop.rs`, the turn every model backend shares, and one file per provider API: `anthropic.rs`, `openai.rs`), `tts.rs` (and its cache), `tools.rs`, `search.rs` (web search), `js.rs` (the JavaScript sandbox), `mcp.rs` (MCP servers as tools), `skills.rs` (Agent Skills), `meter.rs` (what a turn cost), `sentence.rs`, `local.rs` (sherpa-onnx) |
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

`voice-client --tools tools.json` plays a board with tools: an array of tools as the hello
announces them (`name`, `description`, `input_schema`), each with the `answer` to give when it's
called.

It plays a board's part in agent sessions too: `--unlock PIN` (or `--ticket T`), `--sync`,
`--open ID`, `--answer SESSION:ASK:N`, `--stop ID`, `--delete ID`, `--mode agent` (and
`--session ID`) for an agent turn, `--wait SECS` to go on printing, `--json` for the messages as
sent. A test daemon with sessions, every stage mocked and the PIN 1234, on ports of its own and
without the global config:

```sh
mkdir -p /tmp/ink-test && cat > /tmp/ink-test/inkharness.toml <<'TOML'
listen = "127.0.0.1:7491"
http = "127.0.0.1:7492"
token = "test"
[stt]
backend = "mock"
text = "find a plumber this week and ask me which"
[llm]
backend = "mock"
[tts]
backend = "mock"
[agent]
place = "TESTBOX"
pin_sha256 = "03ac674216f3e15c761ee1a5e255f067953623c8b388b4459e13f978d7c846f4"
TOML
INKHARNESS_HOME=/tmp/ink-test cargo run -p inkharness -- serve -C /tmp/ink-test
cargo run -p voice-client -- --addr 127.0.0.1:7491 --token test --unlock 1234 --sync --mode agent --silence 1 --wait 5
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
