// The agent client (agent_client.h rather than agent.h, which apps/agent.h would shadow for the
// screens: AGENTS.md gotcha 8): holds a TCP session with inkharness on the home network
// (docs/voice-protocol.md), sends what the mic hears while the talk key is down, and plays the
// spoken reply as it arrives. The daemon does the listening, thinking and speaking; this is the
// device's end of the wire. Only in a build with INKDECK_AGENT (features.h).
//
// A session is open while the agent sheet (HOLD BOOT, apps/agent_sheet.h) is on
// top: open() joins Wi-Fi and connects in the background, close() ends it and lets the radio go.
// talkStart() with no session open one too, the mic first, so what's said while Wi-Fi joins is
// kept (the capture ring holds 8 s) and sent once inkharness answers. Its own task owns the socket
// and the audio streams (audio.h capture*/stream*), so the UI never waits on the network.
//
// The connection is encrypted with a key made from agent_token (agent_noise.h): the token
// itself never crosses the network, and a daemon with another one is reported as such.
//
// Half-duplex: the mic is off while a reply plays. Pressing the talk key during a reply stops
// it and listens again.
//
// The board's own tools (apps/board_tools.h): the hello announces them, and the model's calls
// come down the connection (docs/voice-protocol.md, "Device tools"). The agent task hands each
// to the tool handler; toolResult() sends back what it gave, from any task.
//
// The agent (issue #52; docs/voice-protocol.md, "Agent sessions"): sessions that live on the
// daemon and keep working while the board is away. The Agent's screens (apps/agent.h) share
// this connection: while one holds it (agentHold()), it stays open, the list is synced and kept
// current, and the session one has open (agentOpen()) comes down whole whenever it changes.
// What a hold of the talk key says goes where setTarget()'s source says: a talk turn (the
// limited assistant, by default) or the agent.
//
// Check-ins (issue #52, "The board idles, sleeps and checks in"): with no Agent screen open the
// board doesn't hold Wi-Fi for the agent. The connection is live while an Agent screen is used
// (it lets go after two minutes without a key) or the board is on USB; otherwise a poll joins,
// syncs in one exchange and leaves again, every 5 minutes while something changes, backing off
// to 15 and 30 while nothing does, or sooner at the daemon's check_at. These background
// connects never touch what the agent sheet shows, and a talk press takes over from one. What
// the last list said is kept in NVS (Summary) for Home, the status bar, the chirps and the
// Agent face, which checks in from an RTC wake (checkInNow()).
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

struct cJSON;

namespace agent {

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
  std::string turnSession;  // with agentTurn: the session it was said into; empty: a new one
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
// `call` gets each tool_call in the agent task, with the turn it belongs to (keep it short:
// queue it). `input` is the call's input object, or null; the handler owns it (cJSON_Delete()).
// Before the first open().
using ToolCall = void (*)(uint32_t id, const std::string &name, cJSON *input, uint32_t turn);
void setTools(const char *definitions, ToolCall call);
// What a call gave, or why it failed (error). Dropped if the session has ended since.
void toolResult(uint32_t id, const std::string &text, bool error = false);

// --- The agent ---------------------------------------------------------------------------------

// Where an utterance goes: asked by the agent task as each one starts.
struct Target {
  bool agent = false;
  std::string session;  // with agent: the session to talk into; empty starts a new one
};
void setTarget(Target (*source)());

// One row of the list (the daemon's `sessions`). Numbers it didn't send are -1.
struct Row {
  std::string id;
  std::string title;
  std::string state;  // working, ask, idle (herdr: ready), done, note, fail, stopped
  std::string place;  // where it runs: the daemon's machine, or the herdr machine
  bool herdr = false;   // runs in a herdr terminal (`via`): the [hc] mark before the place
  std::string program;  // what runs it: "claude code", "codex"; empty for the daemon's own
  std::string sub;    // the question, the step or tool call under way, the result's first line
  int step = -1, steps = -1;  // the plan's progress: step `step` (from 1) of `steps`
  int progress = -1;          // 0-100
  uint32_t updated = 0;       // Unix seconds
  bool unread = false;        // finished or asking, and not opened since
  uint32_t expectAt = 0;      // a working session's expected end
  uint32_t nextAt = 0;        // a schedule's next run, a watch's next look
  std::vector<float> spark;   // a watch's readings, oldest first
};

struct AskOption {
  std::string title, right, detail;
};

struct Ask {
  std::string session, id, title, question, detail;
  std::vector<AskOption> options;
  int recommend = -1;
  uint32_t at = 0;
  uint32_t deadline = 0;  // when nobody answering does `then`: ask_again, recommend, give_up
  std::string then;
};

struct PlanStep {
  std::string text;
  char state = 't';  // 'd' done, 'n' now, 't' to come
};

struct LogLine {
  uint32_t at = 0;
  std::string text;
};

struct RunLine {
  uint32_t at = 0;
  std::string state;  // done, note, fail
  uint32_t secs = 0;
};

// A schedule (`schedule` in a session).
struct ScheduleView {
  bool present = false;
  std::string at;    // "07:00", the daemon's local time
  std::string days;  // "MTWTF.."
  std::string prompt;
  uint32_t next = 0;
  bool wake = false, note = false, speak = false;
  std::vector<RunLine> runs;  // newest first
};

// A watch (`watch` in a session).
struct WatchView {
  bool present = false;
  std::string condition;
  int every = 0;      // minutes
  uint32_t next = 0;  // 0 once met
  std::vector<float> values;
  bool met = false;
};

// One block of a panel the session drew (show_panel; docs/voice-protocol.md, "Panels").
struct PanelItem {
  std::string a, b, c, d;  // stat: label, value · kv: key, value · rows: title, sub, right, tag
};
struct PanelBlock {
  std::string kind;  // section, stat, kv, rows, meter, spark, text, choice; others are skipped
  std::string id, label, right, text, question;
  float value = 0, max = 0;
  std::vector<float> values;
  std::vector<PanelItem> items;  // stat, kv, rows; choice: options (title, right)
  int recommend = -1;
};
struct Panel {
  std::string title;
  std::vector<PanelBlock> blocks;
  uint32_t at = 0;  // a new one has a new time
};

// One session in full (the daemon's `session`).
struct Session {
  std::string id, title, state, place, prompt;
  bool herdr = false;
  std::string program;
  std::string backend;              // api, claude or herdr
  std::vector<std::string> screen;  // herdr: the last lines of its terminal
  std::string result;  // Markdown, once it has finished
  uint32_t started = 0, updated = 0;
  int step = -1, steps = -1;
  std::vector<PlanStep> plan;
  std::vector<LogLine> log;  // newest first
  bool asking = false;
  Ask ask;
  uint32_t expectAt = 0;
  float cost = -1;  // US dollars so far; -1: none yet
  bool costNotional = false;
  ScheduleView schedule;
  WatchView watch;
  bool hasPanel = false;
  Panel panel;
};

// What the daemon's hello says it can do (AgentHello::features).
enum Feature : uint32_t {
  kPause = 1 << 0,
  kRun = 1 << 1,
  kJobs = 1 << 2,
  kSchedule = 1 << 3,
  kWatch = 1 << 4,
  kDeadline = 1 << 5,
  kPanel = 1 << 6,
  kSpeak = 1 << 7,
};

// Words for a herdr agent's terminal, held by inkharness until the board says to type them
// (the daemon's `draft`): a mis-heard utterance must never reach a coding agent unseen.
struct Draft {
  std::string session, id, text;
  uint32_t turn = 0;  // the utterance it came from (Status::turn)
};

struct Sessions {
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
  int asks = 0, running = 0, idle = 0, planned = 0, unread = 0, older = 0, proposed = 0;
  uint32_t features = 0;   // Feature bits from the hello
  uint32_t checkAt = 0;    // the daemon's check_at (Unix seconds; 0: none)
  uint32_t nextAt = 0;     // the next scheduled run
  uint32_t latestAsk = 0, latestDone = 0;  // the list's latest: when to chirp
  std::string sayId, noteId;  // that newest result is a schedule's to say / show as a note
  bool hasAsk = false;     // the newest question waiting (unlocked): for the ask sheet
  Ask ask;
  std::vector<Row> rows;  // the last list (kept after the connection ends)
  bool hasOpen = false;        // `open` holds the session agentOpen() asked for
  Session open;
  std::string started;       // the session the last agent turn went to
  Draft draft;               // waiting to be typed (an empty id: none)
  uint32_t startedTurn = 0;  // that turn (Status::turn)
  std::string refusal;          // the daemon's last `refused` message
  std::string refusalSession;   // the session it named, if any
  std::string refusalRequest;   // what it refused: answer, send_draft, open...
  uint32_t refusals = 0;        // counts them, so a screen can tell a new one
  uint32_t generation = 0;   // changes with any of the above
};

// What Home, the status bar and the power code need, without copying the rows.
struct Glance {
  bool known = false;    // a list has been seen (now, or kept in NVS from before)
  bool locked = false;
  int asks = 0, running = 0, planned = 0, proposed = 0, unread = 0;
  uint32_t nextAt = 0, checkAt = 0;
  uint32_t latestAsk = 0, latestDone = 0;
  std::string sayId, noteId;      // what to do with the newest result (unlocked lists only)
  std::string askSession, askId;  // the newest question (unlocked lists only)
  uint32_t askAt = 0;
  uint32_t checkedAt = 0;  // when a list last came (Unix seconds)
  uint32_t nextCheck = 0;  // when the board looks again (Unix seconds; 0: when asked)
  bool live = false;       // the connection is up
  uint32_t generation = 0;
};
Glance glance();

// What the Agent face draws, kept in NVS between power-offs. Counts and times always; titles,
// the question and its choices only when content is allowed (setContentPolicy()): the flash
// can be read off a lost board.
struct SummaryRow {
  char title[40];
  char state[12];     // working, watch, done, note, fail, scheduled
  int8_t step, steps;
  uint32_t at;        // a working session's expected end, a watch's next look
  uint8_t sparkN;     // a watch's readings, scaled to 0-255 between sparkMin and sparkMax
  uint8_t spark[16];
  float sparkMin, sparkMax, sparkLast;
};
struct Summary {
  uint16_t version;
  uint8_t locked, content;
  uint16_t asks, running, planned, proposed, unread;
  uint32_t checkAt, nextAt, latestAsk, latestDone;
  char askTitle[40], askQuestion[120];
  uint8_t optionCount;
  int8_t recommend;
  char options[4][32];
  char nextTitle[40];  // the next schedule's title
  uint8_t rowCount;
  SummaryRow rows[5];
};
Summary summary();          // the last list's, or the one kept in NVS
uint32_t checkedAt();       // when that list came (Unix seconds; 0: never)
// Whether titles may be kept and shown off the Agent's screens (the PIN guard and
// agent_face_details decide: apps/agent.h).
void setContentPolicy(bool (*allowed)());

// The chirps' memory (apps::pollAgent()): the latest values already chirped for, in NVS so a
// check-in from power-off doesn't chirp again for what was heard before.
void seen(uint32_t &ask, uint32_t &done);
void markSeen(uint32_t ask, uint32_t done);

// Polls: look again soon (Home coming up, a key while ?n shows), at most once a minute.
void checkSoon();
// A poll is due or under way: the Reader shouldn't nap through it (apps/idle.cpp).
bool polling();
// From setup(), on an RTC wake (apps::agentWake()): joins, syncs, saves the summary and lets
// go, within deadlineMs; when the newest result is a schedule's to show as a note, it gets that
// session too (sessions().open). Starts the agent task if it isn't. abort, asked every 100 ms,
// stops the wait (the check-in finishes in the background). True if a list came.
bool checkInNow(uint32_t deadlineMs, bool (*abort)() = nullptr);

// An Agent screen exists (from its constructor to its destructor; they nest). While any does,
// the connection stays up, opening if need be, and the list is synced.
void hold();
void release();
// After a failed connection: try again (the Agent screen's RETRY).
void retry();
Sessions sessions();  // a copy

// The session to get whole and keep current (Agent::open); empty for none.
void follow(const std::string &session);
void answer(const std::string &session, const std::string &ask, int choice);
void stop(const std::string &session);
void remove(const std::string &session);
void pause(const std::string &session);
void resume(const std::string &session);
void runNow(const std::string &session);   // a schedule runs, a watch looks
void approve(const std::string &session);  // a job talk filed starts
void panelChoice(const std::string &session, const std::string &block, int choice);
// Says the session's last result as a reply (a schedule with speak, on USB): like a turn, the
// agent sheet shows and plays it. Needs the connection up (open() first).
void speak(const std::string &session);
// Types agent().draft into its herdr session, or drops it. Either way it's gone from agent().
void sendDraft();
void dropDraft();
// Unlocks the daemon with these digits, now or on the next connection. Kept in RAM only until
// the daemon answers; a right one leaves a ticket that lets later connections in without it,
// until the board powers off or the ticket runs out.
void unlock(const std::string &pin);

}  // namespace agent
