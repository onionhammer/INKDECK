// Voice assistant client (voice_client.h rather than voice.h, which apps/voice.h would shadow
// for the screens: AGENTS.md gotcha 8): holds a TCP session with voiced on the home network
// (docs/voice-protocol.md), sends what the mic hears while the talk key is down, and plays the
// spoken reply as it arrives. The daemon does the listening, thinking and speaking; this is the
// device's end of the wire.
//
// A session is open while the Voice screen is on top: open() joins Wi-Fi and connects in the
// background, close() ends it and lets the radio go. Its own task owns the socket and the
// audio streams (audio.h capture*/stream*), so the UI never waits on the network.
//
// Half-duplex: the mic is off while a reply plays. Pressing the talk key during a reply stops
// it and listens again.
#pragma once
#include <stdint.h>

#include <string>

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
  uint32_t generation = 0;  // changes with the state and the texts
};

bool configured();  // inkdeck.ini names a daemon and there is a Wi-Fi network

void open();
void close();

// The talk key. Safe from any task, the input timer's included: they only post an event.
void talkStart();
void talkStop();

Status status();
bool busy();  // connecting, listening, thinking or speaking: the idle power-off waits

}  // namespace voice
