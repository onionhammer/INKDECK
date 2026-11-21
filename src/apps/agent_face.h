// The Agent face (issue #52, "The Agent face"), and the check-ins the board makes from
// power-off.
//
// PWR on an Agent screen, or anywhere while a session asks or works (but over an open book,
// which keeps its cover), shows an overview of the agent the way PWR on a book shows its cover,
// then powers off. No chrome: the question waiting and its choices, what runs (a plan's meter, a
// watch's sparkline, when it expects to be done), what finished since, and the next schedule;
// with nothing going on, NOTHING RUNNING over the next schedule. While the PIN guards the
// Agent (unless agent_face_details = on) or inkharness is locked: counts and times only. A
// 47 px band at the foot: HOLD [PWR] TO RESUME, then CHECKED 14:30 · NEXT 14:35 (CHECK-INS
// PAUSED · BATTERY LOW under 15 %).
//
// Off, it checks in: the RTC boots the board every 5 minutes while something changes, backing
// off to 15 and 30, or sooner at the daemon's check_at (never sooner than 5 minutes). A check-in
// joins, syncs, chirps for a new question (rising) or result (falling), redraws only what
// changed (a partial refresh, a full one every half hour) and powers off again. HOLD PWR boots
// the board into the Agent, behind the PIN wheels when they guard it. On USB the face stays on
// and live instead of the desk face; a key wakes it.
//
// Sleep, Standby and a closed book check in only when the daemon asks (check_at): for a
// schedule that wakes the board. A run to show as a note goes on the panel then, and the board
// powers off in Sleep under it; nothing else changes on screen, and nothing chirps.
#pragma once
#include <time.h>

namespace apps {

// PWR's choice (apps::powerKey(), pollPower()): an Agent screen is on the stack, or a session
// asks or works. False in a build without the agent.
bool agentFaceWanted();
// Shows the face: on battery it powers off behind it, on USB it stays live. devWake: the dev
// tool's, on battery's path wherever it is, with the RTC a minute later.
void showAgentFace(bool usb, bool devWake = false);
// PWR while the face is up (before its power-off, or on USB): back to what was under it.
bool wakeAgentFace();

// For Sleep, Standby and the closed book on their way off: when to wake for the agent (the
// daemon's check_at, 5 minutes from now at the soonest; 0 for none), kept for the wake to find.
time_t agentCheckWake();
// The earlier of two wakes, 0 meaning none.
time_t earliestWake(time_t a, time_t b);

// From setup(), right after standbyWake(): an RTC wake from the Agent face, Sleep, a closed book
// or the edition with a check-in due. Checks in and powers off again; false means boot normally.
bool agentWake();
// From standbyWake(), before it draws: a check-in riding the minute wake when one is due. True
// if it showed a note (the board then powers off in Sleep).
bool agentStandbyCheck();

// Dev tools (any task): the face's battery path now, with a wake a minute later.
void requestAgentFace();
void pollAgentFace();  // from pollPower(): acts on requestAgentFace()

}  // namespace apps
