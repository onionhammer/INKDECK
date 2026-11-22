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
| `listen_start` | `mode` (optional: `"talk"`, the default, or `"agent"`), `session` (optional, agent only) | The audio frames that follow are one utterance. Cancels a reply still in progress. An agent turn starts a session or talks into one ([Agent sessions](#agent-sessions)) |
| `listen_stop` | | The utterance is complete; the daemon answers |
| `abort` | | Drop the utterance or the reply in progress |
| `tool_result` | `id`, `text`, `error` (optional, default false) | The answer to a `tool_call` with that `id`: what the tool did or found, or with `error` why it couldn't |
| `read` | `id` (a number), `url`, `image_width` and `image_height` (optional: the largest picture to show, in pixels) | Fetch a web page and read its article ([Reading a page](#reading-a-page)). Only to a daemon whose hello has `reader` |

Daemon → device:

| `type` | Fields | Meaning |
| --- | --- | --- |
| `hello` | `proto`, `session` (this connection's name), `agent` (when the daemon has [agent sessions](#agent-sessions)), `reader` (true when it answers `read`) | Answer to the device's hello, in the handshake's message 2 |
| `stt` | `text` | What the daemon heard (may be empty). When the model hears the recording itself, this comes alongside the reply, or not at all if the daemon has no speech-to-text |
| `text` | `delta` | The reply's text as it is generated, for the display. Can arrive before `reply_start` |
| `reply_start` | `rate` (Hz) | Audio frames follow at this sample rate |
| `reply_end` | | The turn is over |
| `error` | `message` | Something failed; a `reply_end` follows unless the connection closes |
| `tool_call` | `id` (a number), `name`, `input` (an object) | The model calls one of the device's tools; answer with `tool_result` |
| `page_image` | `id`, `index`, `width`, `height`, `bits` (base64) | One of the article's pictures, before its `page` |
| `page` | `id`, then `title`, `byline`, `site`, `published` (Unix seconds), `markdown`, or `error` | The answer to `read` with that `id` |

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

## Reading a page

News on the board asks the daemon for a web page's article when a feed gives only a summary and
a link (BBC News), and for the pages a story links to. The daemon fetches the page (20 s, 5 MB at
most, as a browser would ask) and finds the article with dom_smoothie, a port of Mozilla's
Readability (Firefox's Reader View; `inkharness/src/reader.rs`). It sends the article as
Markdown for the board's NoteDoc: paragraphs, headings, lists and quotes, links as
`[text](url)` with absolute addresses, and no players or page furniture, cut at a paragraph
past 40 KB so the message fits one frame.

With `image_width` and `image_height`, the article's pictures come too: each figure, or picture
on its own, stands in the text as `![](inkimg:N)` on a line of its own, its caption after it in
italics, and the daemon sends picture N first as a `page_image`. Up to 12, each scaled to fit
the size asked (never up), its levels stretched and dithered to 1 bit (Atkinson's), in rows of
(width + 7) / 8 bytes, the leftmost pixel in the top bit, 1 for ink. One that doesn't come, or
is too small to be a picture (an icon, a logo), goes from the text with its caption. A page
whose text shows no picture near the top gets its lead picture (`og:image`) there; a picture on
its own near the end (a credit's logo) goes. Without them, the text has no pictures or captions.

```
  read {id: 7, url: "https://www.bbc.co.uk/news/articles/cj752ddve27ko",
        image_width: 420, image_height: 643}  →
                                 ←       page_image {id: 7, index: 0, width: 420, height: 236,
                                                     bits: "…"}
                                 ←       page {id: 7, title: "Russia-bound ship still on fire…",
                                               byline: "Hafsa Khalil", site: "BBC News",
                                               published: 1795285329, markdown: "…"}
  read {id: 8, url: "https://example.com/members-only"}  →
                                 ←       page {id: 8, error: "HTTP 403"}
```

- It isn't a turn: it can come at any time, and pages and turns don't wait for each other.
  Answers can come out of order; match them by `id`.
- `error` is for the screen, in a few words ("HTTP 403", "the site took too long", "no article
  on the page"). The board doesn't fetch a page itself after the daemon reports an error, since
  the site would say the same; it does when the daemon isn't there to ask, or its hello has no
  `reader`.
- It doesn't need the agent's PIN: the page is on the open web, and the board already proved
  the token.
- `GET /read?url=` on the HTTP API answers the same (`&images=420x600` for the pictures, in
  `pictures`), and `inkharness read <url>` prints it (`--pictures DIR` saves them as PNGs).

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

## Agent sessions

A daemon with an `[agent]` table in `inkharness.toml` keeps **sessions**: agent runs that live on
the daemon, are kept on disk, and go on working with no board connected. Each has a title, a
state, an optional plan, a log of what it did, a result, and at most one question waiting for an
answer. The board follows them on the Agent screen, answers their questions and talks into them.
Everything here is additive: `proto` stays 2, and a board or daemon from before sessions skips
the messages it doesn't know.

There are two kinds of turn, and the daemon keeps them apart, whatever else the board asks for:

- **Talk** (`listen_start` with no `mode`, as every board before sessions sends it, or
  `"mode": "talk"`): today's voice, with its own short conversation per connection and the tools
  `[talk] tools` leaves it (the device's always stay). It never sees, starts or answers a session.
  Its one way into the Agent is `propose_job`: it files a job, a `proposed` session that waits on
  the list until someone behind the PIN starts it (`approve`).
- **Agent** (`"mode": "agent"`): starts a session, or talks into the one in `session`.

Sessions are offered only on encrypted connections (protocol 2). On a protocol 1 connection the
daemon's hello has no `agent`, and the session requests are refused.

### The hello

The daemon's `hello` gains `agent` when it has sessions:

```json
{"type": "hello", "proto": 2, "session": "s1",
 "agent": {"place": "ONIONSERVE", "pin": true, "unlocked": false,
           "features": ["pause", "run", "jobs", "schedule", "watch", "deadline"]}}
```

- `place`: the name the board shows for where sessions run (`[agent] place`, or the host's
  name, uppercased).
- `pin`: whether a PIN guards the sessions (`[agent] pin_sha256`).
- `unlocked`: whether this connection may use them now: true from the start when no PIN is set.
- `features`: what this daemon can do beyond the first version of sessions: `pause` (`pause`,
  `resume`), `run` (`run`), `jobs` (`approve`, the `proposed` state), `schedule`, `watch`,
  `deadline`, `panel` (`panel_choice`), `speak` (`speak`). A daemon from before them sends no
  `features`, and drops a request it doesn't know with no answer, so the board offers only
  what is listed.
- No `agent`: no sessions on this daemon.

### Device → daemon

| `type` | Fields | Meaning |
| --- | --- | --- |
| `listen_start` | `mode` (optional: `"talk"` default, `"agent"`), `session` (optional, agent only) | As above. Agent without `session`: the utterance starts a new session. With `session`: it talks into that one; if the session has a question waiting, the utterance's text is the answer |
| `unlock` | `pin` (digits, as a string) **or** `ticket` | Unlocks this connection. Answered with `unlocked` |
| `sync` | | Send `sessions` now, again whenever the list changes (at most once a second), and `ask` whenever a new question appears, for as long as the connection lasts |
| `open` | `session` | Send `session` for it now and whenever it changes (at most once a second), until another `open` or a `close`. Marks it seen (clears `unread`), and keeps it seen while open |
| `close` | | Stop sending `session` updates |
| `answer` | `session`, `ask` (the question's id), `choice` (an index into its `options`) | Answers a question. The session carries on |
| `stop` | `session` | Stops a working or asking session, a schedule or a watch (state `stopped`) |
| `pause` | `session` | Pauses a working or asking session, a schedule or a watch (state `paused`): its run stops, and it keeps its plan, what it was told and its question. A deadline doesn't run out while paused. Not for herdr sessions |
| `resume` | `session` | Picks a paused session up where it was: an asking one asks again, a working one starts a new run that is told it was paused, a schedule or a watch waits for its next time |
| `run` | `session` | A schedule: runs now. A watch: looks now |
| `panel_choice` | `session`, `block` (a `choice` block's `id`), `choice` (an index into its `options`) | Answers a choice on the session's panel ([Panels](#panels)): the session hears it as the user's words |
| `speak` | `session` | Says the session's last result (a schedule with `speak`, on a board on USB): answered like a turn's reply, `text`, `reply_start`, audio and `reply_end`, with no `stt`; `refused` (`nothing to say`) when it has no result |
| `approve` | `session` | Starts a job talk filed (a `proposed` session). With `[agent] backend = "herdr"`, it starts as a program of its own, under a new id, and the job goes |
| `delete` | `session` | Deletes a session for good |
| `send_draft` | `session`, `draft` (the `draft` message's `id`) | Types a herdr session's draft into its program ([Drafts](#drafts)) |
| `drop_draft` | `session`, `draft` | Throws a draft away |

### Daemon → device

| `type` | Fields | Meaning |
| --- | --- | --- |
| `sessions` | `locked`, `counts`, `check_at`, `next_at`, `latest`, `ask`, `sessions`, `older` | The list ([below](#sessions)) |
| `session` | one session in full | [Below](#session) |
| `ask` | a question | A new question appeared, for the board's ask sheet ([below](#ask)). Also inside `session` |
| `started` | `session` | During an agent turn: the session the utterance went to (new or existing), once the daemon knows, before `reply_start` |
| `draft` | `session`, `id`, `text` | During an agent turn into a herdr session, in place of `started` and the reply: the words, waiting for the board to send them ([Drafts](#drafts)) |
| `unlocked` | `ok`, `ticket` (when ok), `ttl_s` (when ok), `wait_s` (when further tries are refused for a while) | The answer to `unlock` ([Unlock](#unlock)) |
| `refused` | `request` (the refused message's `type`), `session` (if it named one), `message` | A request that isn't a turn and can't be done. `message` is one of `locked`, `no such session`, `already answered`, `no such choice`, `not running`, `not paused`, `no sessions on this daemon`, and for herdr sessions `not waiting any more`, `it's busy`, `not allowed by [herdr]`, `herdr is not reachable`, `no such draft`, `the draft has expired`, or a sentence |

`sessions`, `session` and `ask` come whenever something changes, so they can arrive at any
time: between turns, and in the middle of one (between a reply's `text` and its audio frames).
The board should take them wherever they come.

An agent `listen_start` that can't be done (the connection is locked, the daemon has no
sessions, the session doesn't exist, a `mode` this daemon doesn't know) is still a turn: the
daemon takes the audio, and after `listen_stop` answers with `error {message}` (`locked`,
`no sessions on this daemon`, `no such session`, `unknown mode`; into a herdr session also
`it's busy` and the other refusals above) and `reply_end`.

### `sessions`

```json
{"type": "sessions", "locked": false,
 "counts": {"ask": 2, "running": 4, "planned": 1, "unread": 1, "idle": 1, "proposed": 0},
 "check_at": 1795190300, "next_at": 1795237200,
 "latest": {"ask": 1795190000, "done": 1795189500},
 "ask": {"session": "k7q2", "id": "a1", "…": "the ask message without its type"},
 "older": 0,
 "sessions": [
   {"id": "k7q2", "title": "Plumber · kitchen leak", "state": "ask",
    "place": "ONIONSERVE", "via": "inkharness", "program": null,
    "sub": "Which plumber should I book?",
    "step": null, "steps": null, "progress": null,
    "updated": 1795190000, "unread": true},
   {"id": "h3kq9mtc", "title": "Fix the build", "state": "working",
    "place": "DESKTOP", "via": "herdr", "program": "codex",
    "sub": "Running cargo test", "step": null, "steps": null, "progress": 40,
    "updated": 1795190100, "unread": false},
   {"id": "w2fd", "title": "Lisbon fare", "state": "watch",
    "place": "ONIONSERVE", "via": "inkharness", "program": null,
    "sub": "the fare to Lisbon under $700", "step": null, "steps": null, "progress": null,
    "updated": 1795190000, "unread": false,
    "next_at": 1795190300, "spark": [760, 741, 712]}
 ]}
```

- `state`: `working`, `ask` (waiting on an answer), `idle` (ready for more, nothing to do: a
  herdr session), `done` (finished with a result), `note` (finished; the result is meant to be
  read as a note), `fail` (gave up or failed; the result says why), `stopped`, `scheduled` (a
  schedule waiting for its next run), `watch` (a watch waiting for its next look), `paused`,
  `proposed` (a job talk filed, waiting for someone to start it). Treat a state you don't know
  as finished; a board from before the last four does, and shows the word as it is.
- `via`: who runs it, `"inkharness"` (the api and claude backends) or `"herdr"` (a program in a
  herdr terminal; the board draws an outlined `[hc]` mark before `place`). Absent means
  `"inkharness"`. `program`: what runs it, lowercase (`"claude code"`, `"codex"`, `"bash"`), or
  null for the api backend; the board shows it after the place (`DESKTOP · CLAUDE CODE · …`).
- `place`: where it runs: the daemon's place, or a herdr session's machine (uppercased).
- `counts`: over all sessions, not only those listed. `ask`: asking; `running`: working, of
  every kind; `idle`: ready for more; `planned`: schedules and watches; `proposed`: jobs
  waiting to be started; `unread`: finished or asking, and not opened since.
- `check_at`: when the board should check in next (Unix seconds), or null: the earliest of a
  working session's `expect_at` (still in the future), a watch's next look, and a schedule that
  wakes the board (its next run, plus as long as its last run took, at least a minute). Always
  in the future when sent; the board keeps its own floor (it doesn't wake more often than every
  5 minutes when off).
- `next_at`: the next scheduled run (Unix seconds), or absent: Home's NEXT.
- `latest`: `ask`, when the newest waiting question was asked, and `done`, when the newest
  unread result came (a session that finished, or a schedule's run; herdr sessions, which
  finish after every task, don't count), both 0 for none. The board keeps the last values it
  saw, and chirps when one grows (rising: a question, falling: a result). Unlocked, `say` and
  `note` name the session when that newest result is a schedule's run with `speak` (a board on
  USB says it: `speak`) or `show_note` (a board that woke for it shows it as a note).
- `ask`: the newest question waiting, in full (the `ask` message without its `type`), or
  absent: for the board's ask sheet, which may not have seen the `ask` message (it was off).
- A row's `expect_at` (a working session's expected end), `next_at` (a schedule's next run or
  a watch's next look) and `spark` (a watch's last readings, oldest first, at most 24) are
  there only when they apply.
- `step`, `steps`: the plan's progress, when the session has a plan: the current step (from 1)
  and the number of steps. `progress`: 0–100 when known (a herdr program that reports it),
  else null.
- `sub`: one line, at most 80 characters, plain text: the question while asking; the plan's
  current step, or the last log line, while working; the result's first line when finished.
- `title`: at most 40 characters, from the model (with its first plan) or, until then, from the
  prompt. The board uppercases it.
- `updated`: when the session last changed. All times are Unix seconds (UTC).
- Order: asking and proposed first (the oldest question first), then working (the most
  recently active first), then idle and paused, then schedules and watches, then the rest,
  newest first. At most 40; `older` counts the rest.
- **Locked** (a PIN is set and this connection hasn't unlocked): `"locked": true` with the
  counts, `check_at`, `next_at` and `latest`, no `ask`, `"sessions": []` and `"older": 0`. That
  is enough for Home's row, the status bar's `?2`, the Agent face and the chirps, and says
  nothing of what the sessions are.

### `session`

```json
{"type": "session", "id": "k7q2", "title": "Plumber · kitchen leak", "state": "working",
 "place": "ONIONSERVE", "via": "inkharness", "program": null, "backend": "api",
 "prompt": "Find a plumber for the kitchen leak this week",
 "started": 1795189000, "updated": 1795190000,
 "step": 2, "steps": 3,
 "plan": [{"text": "Find plumbers near home", "state": "done"},
          {"text": "Check reviews and prices", "state": "now"},
          {"text": "Ask which to book", "state": "todo"}],
 "log": [{"at": 1795190000, "text": "web_search plumbers open Saturday"}],
 "result": null,
 "ask": null,
 "screen": null,
 "expect_at": 1795191800,
 "cost": {"usd": 0.0412, "notional": false}}
```

- `via`, `program`, `place`: as in the list.
- `backend`: `"api"`, `"claude"` or `"herdr"` ([Backends](#backends)), for the board's header.
- `prompt`: what started it, in the user's words (cut to 2000 characters); a herdr session it
  didn't start: the command it runs.
- `plan`: the checklist, or null without one; each step `done`, `now` or `todo` (all `done`
  once the session is done or a note).
- `log`: newest first, at most 20 lines of at most 120 characters: a tool call (its name and a
  short form of its input), a short line from the model, what the user said into it
  (`you: …`), a question asked and its answer (`asked: …`, `answered: …`), `stopped`.
- `result`: Markdown, at most 8 KB, once the session has finished; null while it works or asks,
  including when it works again after being talked into (it then writes a new one).
- `ask`: the question waiting (the `ask` message without its `type`), or null.
- `screen`: a herdr session's last lines of screen (at most 12, plain text, each at most 80
  characters), or null. It is read while a board has the session open (and while HTTP looks),
  at most every 2 s, and a new `session` follows when it changes.
- `expect_at`: when a working session expects to be done (the model's `plan` with
  `expect_minutes`), absent otherwise.
- `cost`: what its model requests have cost so far, in US dollars (`notional`: on a
  subscription, what it would have cost), absent before the first.
- `schedule` (a schedule): `at` (local time on the daemon, `"07:00"`), `days` (Monday first, a
  letter for each day it runs: `"MTWTF.."`), `next` (the next run), `prompt` (what each run
  does), `wake_board`, `show_note` and `speak` (what the board does with a result), and `runs`
  (the last 10, newest first: `at`, `state` `done`, `note` or `fail`, `secs`). The session's
  `result` is the last run's.
- `watch` (a watch): `condition`, `every_minutes`, `next` (the next look; null once met),
  `values` (the last readings, oldest first) and `met`.
- `panel`: the screen the session drew last ([Panels](#panels)), absent before it draws one.

### Panels

A session draws a screen on the board with `show_panel {title, blocks}`: it comes as the
session's `panel`, `{"title", "blocks", "at"}`, and the board lays the blocks out top to bottom
in its own type and 1-bit rules, under its own status bar and hints. The model never sends
pixels, fonts or HTML. The same title again replaces the screen in place. At most 40 blocks and
8 KB. Each block is an object with a `kind`:

| `kind` | Fields |
| --- | --- |
| `section` | `label`, `right` (optional): a header line |
| `stat` | `items`: up to 4 `{label, value}`, big figures in a row |
| `kv` | `items`: `{key, value}` lines |
| `rows` | `items`: `{title, sub, right, tag}` (all but `title` optional) |
| `meter` | `label`, `value`, `max`, `right` (optional): a bar |
| `spark` | `label`, `values` (numbers), `right` (optional): a small chart |
| `text` | `markdown`: the Note's subset |
| `choice` | `id`, `question`, `options` (2 to 6 `{title, right}`), `recommend` (optional): pinned at the foot; the board sends `panel_choice` |

A board skips a kind it doesn't know. Too tall for the screen, it scrolls, with a `choice` kept
at the foot.

### Schedules and watches

A session becomes a **schedule** when its model calls `schedule {at, days, prompt, wake_board,
show_note, speak}` ("every weekday at seven, brief me"). It then waits (`scheduled`) for its
next run, at `at` local time on the daemon on one of `days`. A run is a fresh run of `prompt`:
no conversation, plan or Claude Code session from the one before. It ends back in `scheduled`,
with its result as the session's `result`, `unread`, and recorded in `runs`; a failed run is
recorded too, and the schedule stays. Words said into a schedule (an agent turn into it) can
change it: the model calls `schedule` again, or with `cancel`. That isn't a run, and it leaves
the last result. A run missed by more than an hour (the daemon was down) is skipped and
logged. `stop` ends a schedule; `pause` suspends it.

A session becomes a **watch** when its model calls `watch {every_minutes, condition}`: every
so often (at least 5 minutes; 30 on the claude backend) a fresh run looks, records a reading
(`watch {value}`) and waits again (`watch`), until a look finds the condition holds (`watch
{met: true}`). Then it finishes (`done` or `note`) with what it found, and the board is told.

### `ask`

```json
{"type": "ask", "session": "k7q2", "id": "a1", "title": "Plumber · kitchen leak",
 "question": "Which plumber should I book?", "detail": "Three can come before the weekend.",
 "options": [{"title": "Riverside · Thu 08:00", "right": "$145", "detail": "4.8 ★ (212)…"},
             {"title": "Northgate · Wed 13:00", "right": "$120", "detail": null}],
 "recommend": 0, "at": 1795190000,
 "deadline": {"at": 1795197200, "then": "recommend"}}
```

- `deadline` (when the model gave one): `at` (Unix seconds) and `then`, what happens if nobody
  answers by then: `ask_again` (the same question, with a new `at`, and `ask` again),
  `recommend` (the recommended option is taken) or `give_up` (the session is told nobody
  answered, and finishes with what it has). Absent: it waits as long as it takes.
- The model asks with its `ask_choice` tool, which only sessions have. 2 to 6 options; an
  option's `title` is at most 32 characters, `right` (a price, a time) at most 12, `detail` at
  most 400; the question at most 200 and its `detail` at most 400. Absent fields are null.
  `recommend`: the option the session recommends (the board's `PICK`), or null.
- `id` numbers a session's questions (`a1`, `a2`, …). `at`: when it was asked.
- The board adds "something else" itself: an answer in words, by voice (`listen_start` with
  `mode: "agent"` and the `session`).
- The model gets `{"choice": n, "title": "…"}` back for an answer by key, or `{"text": "…"}` for
  one in words, and carries on. A session asks one question at a time, and its question waits
  as long as it takes, a daemon restart included.
- `ask` goes to connections that sent `sync` and are unlocked. A locked connection gets no
  `ask`: the `ask` count in `sessions` says one is waiting.

### Unlock

- Without `pin_sha256` every connection is unlocked from the start, and `unlock` (with a PIN
  or a ticket) answers ok.
- `unlock {pin}`: the daemon compares the PIN's SHA-256 (lowercase hex) with `pin_sha256`, in
  constant time. Right: `unlocked {ok: true, ticket, ttl_s}`; the connection is unlocked and
  the count of wrong PINs resets. Wrong: `unlocked {ok: false}`.
- After 3 wrong PINs in a row (across connections, and HTTP's too), further tries are refused
  for 30 s, then twice as long after each further wrong one, up to 30 min, like the board's own
  PIN. A refused try isn't checked, the right PIN included: `unlocked {ok: false, wait_s}`, with
  the seconds left. The wrong PIN that starts a wait also carries `wait_s`. The daemon keeps the
  count on disk, so a restart doesn't clear it.
- `unlock {ticket}`: a ticket from an earlier unlock unlocks a new connection without the PIN,
  for `ticket_minutes` (60 by default) from when it was issued. The answer gives the same
  ticket and the seconds it has left. A wrong or expired ticket gets `unlocked {ok: false}`
  (and doesn't count as a wrong PIN): ask for the PIN. Tickets are random (128 bits) and live
  in the daemon's memory, so a daemon restart ends them; the board keeps its own in RAM.
- While locked: `sessions` is the locked form; `open`, `answer`, `stop` and `delete` get
  `refused {message: "locked"}`; an agent turn gets `error {message: "locked"}` and
  `reply_end`; no `ask` is sent. Talk turns work as before.

### An agent turn

```
device                                   daemon
  listen_start {mode: "agent"}   →
  audio …, listen_stop           →
                                 ←       stt {text}
                                 ←       started {session: "k7q2"}
                                 ←       text {delta} …, reply_start, audio …   (a short reply)
                                 ←       reply_end
                                 ←       sessions …, session … (if open)  (the session works on)
```

- The session's run goes on in the background after `reply_end`: the session hears the words
  at once and works with the daemon's tools, not the board's.
- The spoken reply is short. For a new session, what it is going to do ("I'll compare plumbers
  who can come this week and ask you to pick"). Talking into a working session adds the words
  to its conversation (the model reads them at its next round), and the reply acknowledges
  them. Talking into an asking session answers its question with the words. Talking into a
  finished session starts it again with them, and the reply answers from its result if it can.
- Without speech-to-text on the daemon, the model that hears the recording writes the words
  down first: a session needs them as text. It is told to transcribe only, in its system prompt
  and beside the recording. A transcript with more words than the recording could hold (over
  4.5 a second, plus 3) is an answer, not a transcript: the turn fails with `error {message:
  "couldn't make out what was said"}` and `reply_end`, and the words reach no session.
- Into a herdr session, the turn ends with a `draft` instead ([Drafts](#drafts)).
- **No reply**: with `[agent] reply = "none"` in the daemon's config, an agent turn has no
  reply at all. The words are written down (speech-to-text, or that one request to the model
  that hears) and handed to the session as above, and the turn is `stt {text}`, `started
  {session}`, `reply_end`: no `text`, no `reply_start`, no audio. Refusals are as above
  (`error {message}`, then `reply_end`). Talk turns still answer aloud.

  ```
  listen_start {mode: "agent"}, audio …, listen_stop   →
                                 ←       stt {text}
                                 ←       started {session: "k7q2"}
                                 ←       reply_end
  ```

### Backends

A session runs one of three ways, its `backend`. New sessions (from the board or HTTP) run on
`[agent] backend` (`"api"` unless set); herdr sessions also turn up on their own.

| `backend` | Runs on | Pays with |
| --- | --- | --- |
| `api` | `[agent.llm]`, else `[chat.llm]`, else `[llm]`, with inkharness's tools | the provider's API |
| `claude` | a headless Claude Code run per session (`claude -p`, `[agent.claude]`) | `ANTHROPIC_API_KEY` if set, else the subscription Claude Code is logged in with |
| `herdr` | any program in a herdr terminal (Claude Code, Codex, a shell…), here or on another herdr machine (`[herdr]`) | whatever that program uses |

- **claude**: the session's prompt and later words go to Claude Code as messages while it
  runs. Its tool calls are the log, its todo list (TodoWrite) the plan, its final answer the
  result, and its `ask_choice` and `plan` are inkharness's own, so its questions reach the board
  as above. It works in a folder of its own; stopped, its process ends. `program` is
  `"claude code"`.
- Messages, states and answers are the same for every backend; herdr sessions differ in what
  is described below.

### herdr

herdr keeps programs alive in terminals of their own, here and on other machines (its command
is `hc`); inkharness follows them through its API (`hc connect`).

- **Which terminals are sessions**: every one whose program reports its status to herdr (or
  is named in `[herdr] programs`), on the machines `[herdr] machines` names (all, unless set),
  without anyone starting it from the board. Its id stays the same while the terminal lives
  (`h` and seven letters and digits), `via` is `"herdr"`, `program` its program, `place` its
  machine, `title` the terminal's name or title (else its program and folder).
- **States** come from the program's own status: working → `working` (with `progress` when it
  says), blocked → `ask`, idle → `idle`, done → `done`, error → `fail`, the program or its
  terminal gone → `done` (`fail` with a non-zero exit code). A program that reports nothing is
  `working` while something runs in its shell, else `idle`. Finished herdr sessions stay
  listed; `delete` only takes one off the list (its program goes on), until it does something
  new.
- **Questions**: when the program blocks, the daemon reads its screen and puts the question to
  the board: `detail` is the program's own message, and the options are the choices on its
  screen, each with `right` saying what it sends (`"KEY 1"`, `"ENTER"`, `"Y"`). `answer`
  presses those keys, if the terminal still waits on that same question; else `refused
  {message: "not waiting any more"}`. A question that isn't for permission can be answered in
  words (typed in). A question with no choices on screen has none (the board's "something
  else" still works).
- **Talking into it**: words are never typed straight away. An agent turn with its `session`
  ends with a `draft` ([Drafts](#drafts)); `send_draft` types it into the program when it is
  `idle`, `done`, or asking a question (not one for permission), and while it works it is
  refused, `it's busy`, and kept to send again.
- **Stop** presses its stop key (when `[herdr] interrupt` allows; refused otherwise, `not
  allowed by [herdr]`). Each action checks its `[herdr]` policy: answering, typing, stopping.
- **Starting one**: api and claude sessions get a `herdr_run` tool (when `[herdr] run`
  allows): "start Codex on the desktop to fix the build" is a session that starts it and
  finishes; the new terminal is a session of its own. With `[agent] backend = "herdr"`, a new
  session from the board starts `[herdr] default_program` on `default_machine` with the words
  as its task, and `started` names the new terminal's session.
- If herdr can't be reached, its sessions keep their last state, and actions on them get
  `herdr is not reachable` until it is back.

### Drafts

A program in a terminal does what it is typed, so words said into a herdr session wait for
the board to confirm them. (Words for inkharness's own sessions, api and claude, still go to
them at once.)

```
device                                   daemon
  listen_start {mode: "agent", session: "hb2c75bf"}, audio …, listen_stop   →
                                 ←       stt {text}
                                 ←       draft {session: "hb2c75bf", id: "d7", text: "run the tests again"}
                                 ←       reply_end
  send_draft {session: "hb2c75bf", draft: "d7"}   →   (typed into its program)
     or drop_draft {session: "hb2c75bf", draft: "d7"}
```

- The turn into a herdr session is `stt {text}`, `draft {session, id, text}`, `reply_end`: no
  `started`, no spoken reply, whatever `[agent] reply` says. Nothing is typed.
- The daemon keeps a draft for 2 minutes. `send_draft` types it, as the session's state and
  `[herdr] send` allow. Refused (`it's busy`, `not waiting any more`, `not allowed by [herdr]`,
  `herdr is not reachable`), it gets `refused {request: "send_draft", session, message}` and the
  draft is kept, to send again while it lasts; sent, it is gone, and the session's `log` shows
  the words (`you: …` or `answered: …`). A draft that was sent, dropped or never made gets
  `no such draft`; one older than 2 minutes, `the draft has expired`.
- `drop_draft` throws it away and gets no answer.
- The turn itself is refused as before (`locked`, `no such session`, …), and at once with `not
  allowed by [herdr]` when `[herdr] send` is `deny`: no draft is made that couldn't be sent.
- While locked, `send_draft` and `drop_draft` get `refused {message: "locked"}`.
- Over HTTP, `POST /sessions/{id}/say` on a herdr session answers `{"draft": {session, id,
  text}}` the same way, unless it says `"confirm": true`; `POST /sessions/{id}/send_draft` and
  `/drop_draft` take `{"draft"}`.

### Over HTTP

The same sessions, with the same JSON, are at `/sessions` on the daemon's HTTP port, for scripts
and for testing without a board; with a PIN set, each request sends it in `X-Inkharness-Pin`
(inkharness/README.md, "Agent sessions"). `pause`, `resume`, `run` and `approve` are
`POST /sessions/{id}/pause` and so on.

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

It drives agent sessions too: `--unlock PIN` or `--ticket T` first, `--sync` and `--open ID` to
follow, `--answer SESSION:ASK:N`, `--stop ID`, `--delete ID`, `--send-draft SESSION:DRAFT` and
`--drop-draft SESSION:DRAFT` (or `--send-drafts`, which sends each draft as it comes), and
`--mode agent` (with `--session ID` to talk into one) for an agent turn; `--wait SECS` goes on printing what comes,
and `--json` prints each message as it was sent. With `[llm] backend = "mock"`, sessions play
through a scripted run instead of a model: a plan of three steps, a tool call, then a result;
"ask" in the task makes it ask which of two plumbers to book, "slow" takes 3 s a step, "note"
makes the result a note and "fail" fails it (inkharness/src/agent/mock.rs).

```sh
cargo run -p voice-client -- --token T --unlock 1234 --sync --mode agent --silence 1 --wait 5
cargo run -p voice-client -- --token T --unlock 1234 --answer k7q2:a1:0 --open k7q2
```
