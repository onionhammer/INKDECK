// A screen the board powers off behind (Sleep, a closed book, the standby clock, the Agent and
// News faces): drawn with a full refresh, then off at the second tick after that, so a key has a
// second to cancel. A key pops it: back to what was under it (or, live on USB, awake).
#pragma once
#include "ui/nav.h"

namespace apps {

class OffFaceScreen : public ui::Screen {
 public:
  bool chrome() const override { return false; }
  bool offFace() const override { return true; }
  bool onKey(ui::Key) override {
    ui::nav::pop();
    return true;
  }
  bool onTick() override { return offTick(); }

 protected:
  // From draw(): the face is on its way to the panel.
  void drawn() { drawn_ = true; }
  // From onTick(): at the second tick after drawn() (the first can come straight after the
  // refresh), powerOff(); if that returns, the power-off failed and the screen goes. True then.
  bool offTick() {
    if (!drawn_ || ++ticks_ < 2) return false;
    powerOff();
    ui::nav::pop();  // still here: the power-off failed
    return true;
  }
  // The count starts again (a face unplugged from USB takes the battery's path).
  void restartTicks() { ticks_ = 0; }
  // Saves what the next wake needs and calls power::off(); returns only if that failed.
  virtual void powerOff() = 0;

 private:
  bool drawn_ = false;
  int ticks_ = 0;  // since drawn
};

}  // namespace apps
