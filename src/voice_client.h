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
// Half-duplex: the mic is off while a reply plays. Pressing the talk key during a reply stops
// it and listens again.
//
// The board's own tools (apps/voice_tools.h): the hello announces them, and the model's calls
// come down the connection (docs/voice-protocol.md, "Device tools"). The voice task hands each
// to the tool handler; answer() sends back what it gave, from any task.
#pragma once
#include <stdint.h>

#include <string>

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

}  // namespace voice
