// Navigation: a stack of screens, one key language for all of them, and the render loop.
//
// Key language (docs/ui-design.md "Input mapping"); every screen follows it:
//   ▲ / ▼    the dial: move the cursor, turn the page, change the value on screen. On a
//            landscape screen the wheel is on top and the navigator swaps the two, so rolling
//            left is ▲ and right is ▼ there. Not in the picture viewer, which turns between
//            portrait and landscape with the board: there a roll does the same either way
//            (swapsDial())
//   ●        primary action on what's selected (open, play, pause, run)
//   HOLD ●   secondary action, shown in the hint bar when a screen has one (skip, edit, sync)
//   BOOT     back: the screen may use it first (parent folder), otherwise it pops the screen.
//            On the root, with nothing to pop, it redraws with a full refresh (cleans the
//            screen)
//   HOLD BOOT talk to the assistant, from any screen (onTalkKey(), apps/hold_boot.h; only in a
//            build with the agent, INKDECK_AGENT in features.h): held about
//            half a second, the agent sheet rises over the lower half; let go to send. Not while
//            the lock is on the stack, under an off face, or over a screen that has the mic or
//            the speaker (allowsTalk()). While music or a podcast plays, it raises the player
//            sheet instead (apps/music.h). Screens never see it.
//
// Screens draw only their body. For portrait screens with chrome() the navigator draws the
// status bar (tag(), context(), battery, clock) and the key-hint bar (hints()) around it.
// Redraws happen after keys are handled: a consumed key redraws with a partial refresh; a
// screen can ask for more with nav::invalidate(Refresh::Full). The status-bar clock and
// battery redraw by themselves when they change.
#pragma once
#include <memory>
#include <string>

#include "input.h"
#include "ui/components.h"

namespace ui {

using input::Key;

enum class Refresh : uint8_t { None, Partial, Full };

// Where HOLD BOOT's words go (issue #52, "Security: talk and agent"). Talk is the default: the
// limited assistant, which sees no sessions. The agent is only reached from the Agent's screens.
struct TalkTo {
  bool agent = false;
  std::string session;  // with agent: the session to talk into; empty starts a new one
};

class Screen {
 public:
  virtual ~Screen() = default;

  // Status bar: inverted module tag ("01 READER") and context text (path, title).
  virtual std::string tag() const = 0;
  virtual std::string context() const { return {}; }
  virtual bool contextIsPath() const { return false; }  // ellipsize it from the front
  // Optional reading before the battery ("21.4°C"). Home shows the room temperature.
  virtual std::string statusTemp() const { return {}; }
  // false: no clock in the status bar (the Clock is one).
  virtual bool statusClock() const { return true; }
  // Up to kMaxHints key hints for the bottom bar, left to right. Returns the count.
  virtual int hints(Hint *out) const { return 0; }
  // false: draw() gets the whole screen and the navigator draws no chrome.
  virtual bool chrome() const { return true; }
  virtual bool landscape() const { return false; }
  // false: the navigator doesn't swap ▲ and ▼ while this screen is landscape. For a screen that
  // turns with the board (the picture viewer), so the same roll of the wheel does the same thing
  // in either orientation.
  virtual bool swapsDial() const { return true; }
  // true: drawn on top of the screen below it (menus, sheets). The status bar and body come
  // from the screen below; the hints come from this one.
  virtual bool overlay() const { return false; }
  // true for a screen that is read for long stretches without keys (the Reader): the idle
  // power-off waits for a low battery, and the board light-sleeps between keys (apps/idle.h).
  // Such a screen must not need onTick() while idle; its status-bar clock then updates with
  // the next key.
  virtual bool reading() const { return false; }
  // true for a reading() screen that turns with the board (the picture viewer): its naps keep
  // the IMU's wake on motion armed, and a movement keeps the board awake a few seconds, long
  // enough for onTick() to see how it's held now.
  virtual bool wakeOnMotion() const { return false; }
  // Called when the board starts light-sleeping under this screen, and before it powers off
  // behind a closed book: save anything that a power loss shouldn't take (the Reader saves its
  // place).
  virtual void onNap() {}
  // true for a reading() screen with a book open (the Reader): on battery, 15 minutes without a
  // key close the book. The panel shows its cover, the board powers off, and the next start
  // opens the book again (apps/closed_book.h).
  virtual bool closesWhenIdle() const { return false; }
  // true for the last frame before a power-off (Sleep, Standby, a closed book): nothing else
  // starts a power-off or a sync while it's on top.
  virtual bool offFace() const { return false; }
  // An off face that still chirps (the Agent face: a new question or result heard at a
  // check-in, apps/agent_face.h). Others are powered off, and their news waits on Home.
  virtual bool chirps() const { return false; }
  // false: HOLD BOOT doesn't talk over this screen (the Recorder, which has the mic; an alarm
  // ringing). Asked of the screen on top and the one under it when the stack changes.
  virtual bool allowsTalk() const { return true; }
  // true for the full player (Music): HOLD BOOT on it while something plays raises no player
  // sheet, which would only repeat it (apps::openPlayer()).
  virtual bool isPlayer() const { return false; }
  // true for the lock screen (apps/lock.h): nothing under it can be reached until it pops
  // itself, and HOLD BOOT doesn't talk while it's on the stack.
  virtual bool locks() const { return false; }
  // Where HOLD BOOT's words go while this is the top screen that isn't a sheet (nav::base()):
  // a talk turn, by default, or the agent (the Agent's screens, apps/agent.h). A sheet over it
  // that says the agent (the ask sheet: words answer its question) speaks for both.
  virtual TalkTo talkTo() const { return {}; }

  // Called when the screen becomes the top of the stack (pushed, or uncovered by a pop) and
  // when it stops being the top (popped, or covered by a push).
  virtual void onEnter() {}
  virtual void onLeave() {}

  // Returns true if the key was used (the screen then redraws). An unused Back pops.
  virtual bool onKey(Key key) = 0;

  // Called once a second while on top, or under an overlay. Return true to redraw (live values,
  // playback).
  virtual bool onTick() { return false; }

  // Draws the body: the area between the chrome bands, or the whole screen without chrome.
  virtual void draw(Canvas &c, const Rect &body) = 0;
};

namespace nav {

// Installs the root screen, to be drawn with a full refresh by the next flush() or loop(), so
// screens pushed straight after it (the Clock on an alarm wake, the lock) come up in the same
// refresh.
void begin(std::unique_ptr<Screen> root);
// Draws now if a redraw is pending.
void flush();
// One pass of the UI loop: waits for keys or the next tick, dispatches, redraws. Call from
// Arduino's loop().
void loop();

// Stack operations. Safe to call from inside a screen's onKey (a popped screen is destroyed
// after its handler returns).
void push(std::unique_ptr<Screen> screen);
void pop();
void home();  // back to the root (or to the lock screen if one is on the stack), full refresh
void replace(std::unique_ptr<Screen> screen);

void invalidate(Refresh refresh = Refresh::Partial);
Screen *top();
Screen *base();  // the top screen that isn't an overlay (the one under a menu)
bool locked();   // a screen that locks() is on the stack

// True if HOLD BOOT may talk over the screens on the stack now: no lock on it, no off face on
// top, and the top screen and the one under it allowsTalk(). Any task may ask (input::onBackHold()
// handlers do, in the timer task); it changes only with the stack.
bool talkAllowed();
// What nav::base() says HOLD BOOT talks to (Screen::talkTo()). Any task may ask; it changes
// only with the stack.
TalkTo talkTarget();

// What a PWR press does (input::Key::Power; screens never see it).
void onPowerKey(void (*handler)());
// What HOLD BOOT does (Key::BackLong, then Key::BackLongUp at its release: held true, then
// false). Called in the UI task whether or not talkAllowed(); screens never see these keys. A
// redraw follows.
void onTalkKey(void (*handler)(bool held));
// Questions the agent has waiting, for the status bar's ?n (apps/agent.h); none without one.
void onAsks(int (*count)());

}  // namespace nav
}  // namespace ui
