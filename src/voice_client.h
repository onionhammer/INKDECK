// Voice assistant client (voice_client.h rather than voice.h, which apps/voice.h would shadow
// for the screens: AGENTS.md gotcha 8): holds a TCP session with inkharness on the home network
// (docs/voice-protocol.md), sends what the mic hears while the talk key is down, and plays the
// spoken reply as it arrives. The daemon does the listening, thinking and speaking; this is the
// device's end of the wire. Only in a build with INKDECK_VOICE (features.h).
//
// A session is open while the Voice screen or the voice sheet (HOLD BOOT, apps/voice.h) is on
// top: open() joins Wi-Fi and connects in the background, close() ends it and lets the radio go.
// talkStart() with no session open one too, the mic first, so what's said while Wi-Fi joins is
// kept (the capture ring holds 8 s) and sent once inkharness answers. Its own task owns the socket
// and the audio streams (audio.h capture*/stream*), so the UI never waits on the network.
//
// The connection is encrypted with a key made from voice_token (voice_noise.h): the token
// itself never crosses the network, and a daemon with another one is reported as such.
//
// Half-duplex: the mic is off while a reply plays. Pressing the talk key during a reply stops
// it and listens again.
//
// The board's own tools (apps/voice_tools.h): the hello announces them, and the model's calls
// come down the connection (docs/voice-protocol.md, "Device tools"). The voice task hands each
// to the tool handler; answer() sends back what it gave, from any task.
//
// The agent (issue #52; docs/voice-protocol.md, "Agent sessions"): sessions that live on the
// daemon and keep working while the board is away. The Agent's screens (apps/agent.h) share
// this connection: while one holds it (agentHold()), it stays open, the list is synced and kept
// current, and the session one has open (agentOpen()) comes down whole whenever it changes.
// What a hold of the talk key says goes where setTarget()'s source says: a talk turn (the
// limited assistant, by default) or the agent.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

struct cJSON;

namespace voice {

enum class State : uint8_t {
  Off,         // no session
  Connecting,  // joining Wi-Fi, finding the daemon
  Ready,       // connected, waiting for the talk key
  Listening,   // the key is down: mic to the daemon
  Thinking,    // utterance sent, waiting for the reply
  Speaking,    // reply audio playing
  Error,       // no session; `error` says why. The talk key tries again
};

struct Status {
  State state = State::Off;
  std::string heard;  // what the daemon made of the last utterance
  std::string reply;  // the reply's text so far
  std::string error;  // why the session failed, or what the daemon reported for this turn
  float levelDb = -96;  // the mic's level while listening
  bool mic = false;     // the mic is on: listening, or a talk that waits for the session
  uint32_t turn = 0;    // counts utterances: a tool call belongs to the turn it came in
  bool agentTurn = false;   // the last utterance went to the agent, not a talk turn
  uint32_t generation = 0;  // changes with the state and the texts
};

bool configured();  // inkdeck.ini names a daemon and there is a Wi-Fi network

void begin();  // starts the task; open() and talkStart() work only after it
void open();
void close();

// The talk key. Safe from any task, the input timer's included: they only post an event.
// With no session (or a failed one), talkStart() opens one and the press is heard.
void talkStart();
void talkStop();

Status status();
bool busy();  // connecting, listening, thinking or speaking: the idle power-off waits

// The tools: `definitions` is the JSON array the hello announces (static: it is kept), and
// `call` gets each tool_call in the voice task, with the turn it belongs to (keep it short:
// queue it). `input` is the call's input object, or null; the handler owns it (cJSON_Delete()).
// Before the first open().
using ToolCall = void (*)(uint32_t id, const std::string &name, cJSON *input, uint32_t turn);
void setTools(const char *definitions, ToolCall call);
// What a call gave, or why it failed (error). Dropped if the session has ended since.
void answer(uint32_t id, const std::string &text, bool error = false);

// --- The agent ---------------------------------------------------------------------------------

// Where an utterance goes: asked by the voice task as each one starts.
struct Target {
  bool agent = false;
  std::string session;  // with agent: the session to talk into; empty starts a new one
};
void setTarget(Target (*source)());

// One row of the list (the daemon's `sessions`). Numbers it didn't send are -1.
struct AgentRow {
  std::string id;
  std::string title;
  std::string state;  // working, ask, done, note, fail, stopped (later: watch, scheduled)
  std::string place;  // where it runs: the daemon's machine
  std::string sub;    // the question, the step or tool call under way, the result's first line
  int step = -1, steps = -1;  // the plan's progress: step `step` (from 1) of `steps`
  int progress = -1;          // 0-100
  uint32_t updated = 0;       // Unix seconds
  bool unread = false;        // finished or asking, and not opened since
};

struct AskOption {
  std::string title, right, detail;
};

struct Ask {
  std::string session, id, title, question, detail;
  std::vector<AskOption> options;
  int recommend = -1;
  uint32_t at = 0;
};

struct PlanStep {
  std::string text;
  char state = 't';  // 'd' done, 'n' now, 't' to come
};

struct LogLine {
  uint32_t at = 0;
  std::string text;
};

// One session in full (the daemon's `session`).
struct AgentSession {
  std::string id, title, state, place, prompt;
  std::string result;  // Markdown, once it has finished
  uint32_t started = 0, updated = 0;
  int step = -1, steps = -1;
  std::vector<PlanStep> plan;
  std::vector<LogLine> log;  // newest first
  bool asking = false;
  Ask ask;
};

struct Agent {
  bool known = false;      // a daemon has said hello since the board started
  bool supported = false;  // and it has sessions (its hello had `agent`)
  bool pin = false;        // it wants its PIN before it shows them
  bool unlocked = false;   // this connection may use them
  bool unlocking = false;  // an unlock is on its way
  std::string unlockNote;  // after a refused unlock: WRONG PIN, WAIT 30 S
  std::string place;       // the daemon's machine
  bool connected = false;  // the connection is up now
  bool synced = false;     // a list has arrived on it
  bool locked = false;     // that list was the locked form: counts only
  int asks = 0, running = 0, planned = 0, unread = 0, older = 0;
  std::vector<AgentRow> rows;  // the last list (kept after the connection ends)
  bool hasOpen = false;        // `open` holds the session agentOpen() asked for
  AgentSession open;
  std::string started;       // the session the last agent turn went to
  uint32_t startedTurn = 0;  // that turn (Status::turn)
  std::string refusal;       // the daemon's last `refused` message
  uint32_t generation = 0;   // changes with any of the above
};

// An Agent screen exists (from its constructor to its destructor; they nest). While any does,
// the connection stays up, opening if need be, and the list is synced.
void agentHold();
void agentRelease();
// After a failed connection: try again (the Agent screen's RETRY).
void agentRetry();
Agent agent();  // a copy

// The session to get whole and keep current (Agent::open); empty for none.
void agentOpen(const std::string &session);
void agentAnswer(const std::string &session, const std::string &ask, int choice);
void agentStop(const std::string &session);
void agentDelete(const std::string &session);
// Unlocks the daemon with these digits, now or on the next connection. Kept in RAM only until
// the daemon answers; a right one leaves a ticket that lets later connections in without it,
// until the board powers off or the ticket runs out.
void agentUnlock(const std::string &pin);

}  // namespace voice
