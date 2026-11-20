#include "ui/nav.h"

#include <Arduino.h>
#include <time.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "audio.h"
#include "pmic.h"
#include "rtc.h"
#include "screen.h"
#include "wlan.h"

namespace ui::nav {
namespace {

constexpr uint32_t kTickMs = 1000;

std::vector<std::unique_ptr<Screen>> stack;
std::vector<std::unique_ptr<Screen>> retired;  // popped during dispatch; freed after it
Refresh pending = Refresh::None;
uint32_t lastTick = 0;
std::string drawnStatus;  // clock + battery as last drawn, to spot when they change
void (*powerKeyHandler)() = nullptr;
void (*talkKeyHandler)(bool) = nullptr;
bool dialSwapped = false;  // the swap orientDial() chose at the dial's press, kept for its repeats
std::atomic<bool> talkOk{false};  // talkAllowed(), worked out when the stack changes
std::mutex talkToLock;            // guards talkTo
TalkTo talkTo;                    // talkTarget(), worked out with talkOk

void request(Refresh r) {
  if (r > pending) pending = r;
}

void retireTop() {
  retired.push_back(std::move(stack.back()));
  stack.pop_back();
}

// After every change to the stack, for talkAllowed() in other tasks.
void stackChanged() {
  bool ok = !stack.empty() && !locked();
  if (ok) {
    const Screen &t = *stack.back();
    const Screen *under = stack.size() > 1 ? stack[stack.size() - 2].get() : nullptr;
    ok = !t.offFace() && t.allowsTalk() && (!under || under->allowsTalk());
  }
  talkOk = ok;
  TalkTo to;
  for (size_t i = stack.size(); i-- > 0;) {
    if (!stack[i]->overlay()) {
      to = stack[i]->talkTo();
      break;
    }
  }
  std::lock_guard<std::mutex> l(talkToLock);
  talkTo = std::move(to);
}

StatusInfo statusFor(const Screen &s) {
  StatusInfo info;
  info.tag = s.tag();
  info.context = s.context();
  info.contextIsPath = s.contextIsPath();
  info.temp = s.statusTemp();
  const pmic::Status p = pmic::status();
  info.battery = p.percent;
  info.usb = p.vbus;
  info.wifi = wlan::radioOn();
  switch (audio::status().state) {
    case audio::State::Playing: info.playback = Playback::Playing; break;
    case audio::State::Paused: info.playback = Playback::Paused; break;
    case audio::State::Stopped: break;
  }
  if (!s.statusClock()) {
    info.clock.clear();
  } else if (rtc::timeValid()) {
    const time_t now = time(nullptr);
    tm t;
    localtime_r(&now, &t);
    char buf[8];
    snprintf(buf, sizeof buf, "%02d:%02d", t.tm_hour, t.tm_min);
    info.clock = buf;
  } else {
    info.clock = "--:--";
  }
  return info;
}

std::string signature(const StatusInfo &info) {
  return info.clock + "|" + std::to_string(info.battery) + "|" + (info.usb ? "u" : "") +
         (info.wifi ? "w" : "") + std::to_string(static_cast<int>(info.playback)) + "|" +
         info.temp;
}

// How many screens home() keeps: the root, or everything up to the topmost lock screen.
size_t homeDepth() {
  for (size_t i = stack.size(); i-- > 0;) {
    if (stack[i]->locks()) return i + 1;
  }
  return 1;
}

// The screen that owns the chrome and body: the top one, or the one under an overlay.
Screen &baseScreen() {
  for (size_t i = stack.size(); i-- > 0;) {
    if (!stack[i]->overlay()) return *stack[i];
  }
  return *stack.front();
}

void render(Refresh refresh) {
  Screen &s = *stack.back();
  Screen &base = baseScreen();
  Canvas &c = screen::canvas();
  const uint32_t t0 = millis();

  screen::lock();
  if (base.landscape() != screen::landscape()) {
    screen::setLandscape(base.landscape());
    refresh = Refresh::Full;
  }
  screen::clear();
  Rect body{0, 0, c.width(), c.height()};
  if (base.chrome()) {
    const StatusInfo info = statusFor(base);
    drawStatusBar(c, body.takeTop(kStatusBarH), info);
    Hint hints[kMaxHints];
    const int n = s.hints(hints);
    drawHintBar(c, body.takeBottom(kHintBarH), hints, n);
    drawnStatus = signature(info);
  }
  base.draw(c, body);
  if (&s != &base) s.draw(c, body);
  const uint32_t t1 = millis();
  screen::present(refresh == Refresh::Full);
  screen::unlock();
  log_i("render %s%s: draw %lu ms, panel %lu ms", base.tag().c_str(),
        &s != &base ? " + overlay" : "", (unsigned long)(t1 - t0),
        (unsigned long)(millis() - t1));
}

// Landscape turns the board so its wheel is on the top edge, where ▲ (rolled towards the
// portrait top) now rolls to the right. Swapping ▲ and ▼ there keeps left as ▲ and right as ▼,
// as they were when landscape had the wheel underneath. Not for a screen that turns with the
// board (Screen::swapsDial(), the picture viewer). Decided when the dial is pressed, not per
// repeat, so a held dial can't turn back on itself if the screen turns under it.
Key orientDial(Key key) {
  if (key != Key::Up && key != Key::Down) return key;
  if (!input::lastKeyRepeat()) {
    const Screen &base = baseScreen();
    dialSwapped = base.landscape() && base.swapsDial();
  }
  if (!dialSwapped) return key;
  return key == Key::Up ? Key::Down : Key::Up;
}

void dispatch(Key key) {
  key = orientDial(key);
  log_d("key %s", input::name(key));
  if (key == Key::BackLong || key == Key::BackLongUp) {
    if (talkKeyHandler) talkKeyHandler(key == Key::BackLong);
    request(Refresh::Partial);
    return;
  }
  if (key == Key::Power) {
    if (powerKeyHandler) powerKeyHandler();
    return;
  }
  if (stack.back()->onKey(key)) {
    request(Refresh::Partial);
  } else if (key == Key::Back) {
    if (stack.size() > 1) {
      pop();
    } else {
      request(Refresh::Full);  // nothing to go back to: clean the screen instead
    }
  }
}

void tick() {
  Screen &s = *stack.back();
  if (s.onTick()) request(Refresh::Partial);
  Screen &base = baseScreen();
  // The screen under a menu or sheet keeps its live values (a forecast that arrives, the time).
  if (&base != &s && base.onTick()) request(Refresh::Partial);
  if (base.chrome() && signature(statusFor(base)) != drawnStatus) request(Refresh::Partial);
}

}  // namespace

void begin(std::unique_ptr<Screen> root) {
  stack.clear();
  stack.push_back(std::move(root));
  stack.back()->onEnter();
  stackChanged();
  lastTick = millis();
  request(Refresh::Full);
}

void flush() {
  if (pending == Refresh::None) return;
  const Refresh r = pending;
  pending = Refresh::None;
  render(r);
}

void loop() {
  const uint32_t sinceTick = millis() - lastTick;
  input::Key key;
  if (input::wait(key, sinceTick < kTickMs ? kTickMs - sinceTick : 0)) {
    // Handle every queued key before drawing, so fast dialling doesn't queue up refreshes.
    do {
      dispatch(key);
    } while (input::wait(key, 0));
  }
  if (millis() - lastTick >= kTickMs) {
    lastTick = millis();
    tick();
  }
  retired.clear();
  flush();
}

void push(std::unique_ptr<Screen> screen) {
  stack.back()->onLeave();
  stack.push_back(std::move(screen));
  stack.back()->onEnter();
  stackChanged();
  request(Refresh::Partial);
}

void pop() {
  if (stack.size() <= 1) return;
  stack.back()->onLeave();
  retireTop();
  stack.back()->onEnter();
  stackChanged();
  request(Refresh::Partial);
}

void home() {
  const size_t depth = homeDepth();
  if (stack.size() > depth) {
    stack.back()->onLeave();
    while (stack.size() > depth) retireTop();
    stack.back()->onEnter();
    stackChanged();
  }
  request(Refresh::Full);
}

void replace(std::unique_ptr<Screen> screen) {
  stack.back()->onLeave();
  retireTop();
  stack.push_back(std::move(screen));
  stack.back()->onEnter();
  stackChanged();
  request(Refresh::Partial);
}

void invalidate(Refresh refresh) { request(refresh); }

Screen *top() { return stack.empty() ? nullptr : stack.back().get(); }

Screen *base() { return stack.empty() ? nullptr : &baseScreen(); }

bool locked() {
  for (const auto &s : stack) {
    if (s->locks()) return true;
  }
  return false;
}

bool talkAllowed() { return talkOk.load(); }

TalkTo talkTarget() {
  std::lock_guard<std::mutex> l(talkToLock);
  return talkTo;
}

void onPowerKey(void (*handler)()) { powerKeyHandler = handler; }

void onTalkKey(void (*handler)(bool held)) { talkKeyHandler = handler; }

}  // namespace ui::nav
