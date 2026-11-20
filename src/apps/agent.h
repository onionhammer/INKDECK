// Agent (issue #52): the sessions inkharness runs for you, jobs that keep working on the daemon
// while the board sleeps or is off, and the place to answer when one asks you to choose. Home →
// AGENT (03). Only in a build with INKDECK_VOICE (features.h). The wire: voice_client.h's agent
// half and docs/voice-protocol.md, "Agent sessions".
//
// The list: three counts (ASKS, inverted while any wait; RUNNING; PLANNED), then NEEDS YOU
// (inverted header), RUNNING and DONE, a two-line row per session: its title, then where it runs
// and what it's doing, and a tag (ASK; 4/7, 41%, RUN or READY; DONE, NOTE, FAIL or STOP). A
// finished one not opened yet says NEW. Pages flip whole. Agents in herdr terminals (Claude Code,
// Codex, anything else hc sees, on any machine) are sessions like the rest: the only sign is an
// outlined [hc] mark before the machine's name, and the program after it ("[hc] DESKTOP · CLAUDE
// CODE · APPROVE PLAN"). Their session screen shows the last lines of their terminal, and their
// questions are the prompts they're blocked on, each choice saying which keys it sends.
//   ▲▼ move   ● open   HOLD ● stop / delete   HOLD BOOT a new session   BOOT back
//
// A session: its title and state, then its question with the choices while it asks; else the
// plan as a checklist under a segment meter, its result once finished, and the tool calls newest
// first.
//   asking:  ▲▼ choice   ● send it   HOLD BOOT answer in words   BOOT back
//   else:    ▲▼ page   ● the result as a note (finished)   HOLD ● menu   HOLD BOOT talk into it
//            BOOT back
//
// HOLD BOOT on these screens goes to the agent: a new session from the list, which opens by
// itself once the voice sheet has gone, or into the session on its screen (answering its
// question, if it asks). Anywhere else it is a talk turn, the
// limited assistant that sees no sessions (issue #52, "Security: talk and agent").
//
// The PIN: with `agent` in INKDECK_PIN_ON, Home opens the Agent through the wheels
// (apps/lock.h), as System and Files, and every start or PWR asks again. The same digits unlock
// inkharness when it has its own PIN (pin_sha256). If inkharness still says locked (no PIN on the
// board, or the two differ), the list shows only the counts and ● asks for inkharness's PIN.
//
// The connection is held while any of these screens exists (voice::agentHold()), so the list
// stays live with a session or the voice sheet over it; it ends a few seconds after the last
// one goes.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

// Home opens it through openGuarded(Guarded::Agent, ...).
std::unique_ptr<ui::Screen> makeAgent();

// Once at start, after the voice client's: routes HOLD BOOT (voice::setTarget()) and passes a
// PIN entered at the lock or the Agent's wheels on to inkharness (voice::agentUnlock()).
void beginAgent();

}  // namespace apps
