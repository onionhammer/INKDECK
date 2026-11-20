#include "apps/lock.h"

#include <Arduino.h>

#include <algorithm>
#include <utility>

#include "apps/home.h"
#include "audio.h"
#include "input.h"
#include "pin_places.h"
#include "settings.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;
using ui::TextStyle;

constexpr int kMinDigits = 4;  // settings: a PIN is 4 to 8 digits
constexpr int kMaxDigits = 8;

constexpr TextStyle kDigit{&ui::fonts::kMono64ExtraBold, -3};
constexpr TextStyle kNeighbour{&ui::fonts::kMono28ExtraBold, 0};
constexpr TextStyle kArrow{&ui::fonts::kMono18Bold, 0};

// The padlock, in the Sleep frog's sprite language: '#' ink, '+' 50 % dither, 'o' paper inside
// the sprite, ' ' outside it (a 25 % dither shadow falls one cell down and right).
const char *const kPadlock[] = {
    "    ########    ",  //
    "   #++++++++#   ",  //
    "   #+######+#   ",  //
    "   #+#    #+#   ",  //
    "   #+#    #+#   ",  //
    "   #+#    #+#   ",  //
    " ############## ",  //
    " #++++++++++++# ",  //
    " #++++++++++++# ",  //
    " #+++++oo+++++# ",  //
    " #++++oooo++++# ",  //
    " #++++oooo++++# ",  //
    " #+++++oo+++++# ",  //
    " #+++++oo+++++# ",  //
    " #++++++++++++# ",  //
    " ############## ",  //
};
constexpr int kPadlockRows = sizeof kPadlock / sizeof *kPadlock;
constexpr int kPadlockCols = 16;
constexpr int kCell = 12;

// Body layout, top to bottom.
constexpr int kTopH = 290;  // the padlock's band
constexpr int kWheelGap = 12;
constexpr int kWheelMaxW = 84;
constexpr int kArrowH = 24;
constexpr int kNeighbourH = 40;
constexpr int kWindowH = 104;
constexpr int kColumnH = 2 * kArrowH + 2 * 6 + 2 * kNeighbourH + 2 * 8 + kWindowH;

char padlockAt(int x, int y) {
  if (x < 0 || y < 0 || x >= kPadlockCols || y >= kPadlockRows) return ' ';
  return kPadlock[y][x];
}

void drawPadlock(ui::Canvas &c, int x0, int y0) {
  using namespace ui;
  // One cell past the sprite on the right and below, for the shadow.
  for (int y = 0; y <= kPadlockRows; ++y) {
    for (int x = 0; x <= kPadlockCols; ++x) {
      const Rect cell{x0 + x * kCell + 1, y0 + y * kCell + 1, kCell - 2, kCell - 2};
      const char ch = padlockAt(x, y);
      if (ch == '#') {
        fill(c, cell, kInk);
      } else if (ch == '+') {
        dither50(c, cell);
      } else if (ch == ' ' && padlockAt(x - 1, y - 1) != ' ') {
        dither25(c, cell);
      }
    }
  }
}

// 1 px dashed outline inside r: a wheel a new PIN may leave out.
void dashedFrame(ui::Canvas &c, const Rect &r) {
  ui::dashedH(c, r.x, r.y, r.w);
  ui::dashedH(c, r.x, r.bottom() - 1, r.w);
  ui::dashedV(c, r.x, r.y, r.h);
  ui::dashedV(c, r.right() - 1, r.y, r.h);
}

std::string digitText(int d) { return std::string(1, static_cast<char>('0' + (d + 10) % 10)); }

// What the wheels are for: opening the lock, opening a module (openGuarded()), one step of a
// PinTask, or digits checked elsewhere (makePinEntry()).
enum class Step : uint8_t { Unlock, Gate, Current, New, Confirm, Entry };

void (*pinListener)(const std::string &) = nullptr;  // onPinEntered()

void pinEntered(const std::string &pin) {
  if (pinListener) pinListener(pin);
}

// --- Wrong PINs: a growing wait after a few in a row --------------------------------------

constexpr int kFreeMisses = 3;                     // tries before the first wait
constexpr uint32_t kFirstWaitMs = 30 * 1000;       // then doubled with each miss
constexpr uint32_t kMaxWaitMs = 30 * 60 * 1000;
int misses = -1;          // in a row, settings::pinMisses(); -1 until read from NVS
uint32_t lastMissMs = 0;  // since boot, so a restart starts the wait again

int missCount() {
  if (misses < 0) misses = std::max(0, settings::pinMisses());
  return misses;
}

// How long the next try still has to wait, in ms (0: now).
uint32_t waitLeftMs() {
  const int n = missCount();
  if (n < kFreeMisses) return 0;
  const int doublings = n - kFreeMisses;
  const uint32_t wait = doublings >= 6 ? kMaxWaitMs : std::min(kMaxWaitMs, kFirstWaitMs << doublings);
  const uint32_t since = millis() - lastMissMs;
  return since >= wait ? 0 : wait - since;
}

// "WAIT 25 S", "WAIT 4 MIN", or "" when the next try can go. Rounded up to 5 s, then to whole
// minutes, so the countdown redraws every 5 s at most.
std::string waitText() {
  const uint32_t ms = waitLeftMs();
  if (ms == 0) return {};
  const uint32_t s = (ms + 999) / 1000;
  if (s <= 60) return "WAIT " + std::to_string((s + 4) / 5 * 5) + " S";
  return "WAIT " + std::to_string((s + 59) / 60) + " MIN";
}

void missed() {
  misses = missCount() + 1;
  lastMissMs = millis();
  settings::setPinMisses(misses);
  log_w("lock: wrong PIN (%d in a row)", misses);
}

void matched() {
  if (missCount() == 0) return;
  misses = 0;
  settings::setPinMisses(0);
}

// --- Guarded modules: when each was last in use ---------------------------------------------

constexpr uint32_t kGuardGraceMs = 60 * 1000;  // no PIN again within a minute of use

// open: its PIN passed (or none was needed) since it was last closed; ms: last in use.
struct Guard {
  bool open = false;
  uint32_t ms = 0;
};
Guard guards[3];  // by Guarded
int onTop = -1;   // the Guarded module whose screen is on top, or -1

Guard &guardOf(Guarded m) { return guards[static_cast<int>(m)]; }
const char *labelOf(Guarded m) {
  switch (m) {
    case Guarded::Settings: return "SYSTEM";
    case Guarded::Files: return "FILES";
    case Guarded::Agent: return "AGENT";
  }
  return "";
}

// The build asks for the PIN at this module, and one is set.
bool applies(Guarded m) {
  pinplaces::Place place = pinplaces::kFiles;
  switch (m) {
    case Guarded::Settings: place = pinplaces::kSettings; break;
    case Guarded::Files: place = pinplaces::kFiles; break;
    case Guarded::Agent: place = pinplaces::kAgent; break;
  }
  return settings::hasPin() && pinplaces::asks(place);
}
bool fresh(Guarded m) {
  const Guard &g = guardOf(m);
  return g.open && millis() - g.ms < kGuardGraceMs;
}
void openUp(Guarded m) {
  guardOf(m).open = true;
  guardOf(m).ms = millis();
}

class PinScreen : public ui::Screen {
 public:
  PinScreen() = default;  // the lock
  PinScreen(PinTask task, std::function<void(const std::string &)> done)
      : step_(settings::hasPin() ? Step::Current : Step::New), task_(task), done_(std::move(done)) {}
  // A module's wheels: open() makes its screen; without it they're in front of one already
  // (guardedOnTop()), and the right PIN goes back to it.
  PinScreen(Guarded module, std::function<std::unique_ptr<ui::Screen>()> open)
      : step_(Step::Gate), module_(module), open_(std::move(open)) {}
  // Digits for someone else to check (makePinEntry()).
  PinScreen(std::string tag, std::function<void(const std::string &)> entered)
      : step_(Step::Entry), done_(std::move(entered)), tag_(std::move(tag)) {}

  std::string tag() const override {
    switch (step_) {
      case Step::Unlock: return "LOCKED";
      case Step::Gate: return moduleTag(labelOf(module_));
      case Step::Entry: return tag_;
      default: return moduleTag("SYSTEM");
    }
  }
  std::string context() const override {
    // Short: the status bar has room for about 23 characters here.
    switch (step_) {
      case Step::Unlock:
      case Step::Gate: break;
      case Step::Current: return "CURRENT PIN";
      case Step::New: return "NEW PIN";
      case Step::Confirm: return "NEW PIN AGAIN";
      case Step::Entry: return "INKHARNESS PIN";
    }
    switch (settings::pendingPin()) {
      case settings::PinChange::New: return "NEW PIN ON UNLOCK";
      case settings::PinChange::Off: return "PIN OFF ON UNLOCK";
      case settings::PinChange::None: break;
    }
    return "ENTER PIN";
  }
  bool locks() const override { return step_ == Step::Unlock; }
  int hints(ui::Hint *out) const override {
    int n = 0;
    if (checksPin() && waitLeftMs() > 0) {  // only BOOT until the wait is over
      if (step_ != Step::Unlock) out[n++] = {"BOOT", cancelVerb()};
      return n;
    }
    out[n++] = {ui::glyph::kDial, "DIGIT"};
    if (step_ == Step::New || step_ == Step::Entry) {
      const bool lastWheel = cursor_ == kMaxDigits - 1;
      out[n++] = {ui::glyph::kPress, lastWheel ? "DONE" : "NEXT"};
      if (cursor_ >= kMinDigits - 1 && !lastWheel) out[n++] = {ui::glyph::kHoldPress, "DONE"};
    } else {
      out[n++] = {ui::glyph::kPress, last() ? lastVerb() : "NEXT"};
    }
    if (cursor_ > 0) {
      out[n++] = {"BOOT", "BACK"};
    } else if (step_ != Step::Unlock) {
      out[n++] = {"BOOT", cancelVerb()};
    }
    return n;
  }

  // A ring sheet or an off screen over the wheels starts the PIN again.
  void onEnter() override { reset(); }

  bool onKey(Key key) override {
    if (checksPin() && waitLeftMs() > 0 && key != Key::Back) return false;  // after misses
    if (key == Key::SelectLong) {  // a new PIN of fewer than eight digits
      if ((step_ != Step::New && step_ != Step::Entry) || cursor_ < kMinDigits - 1) return false;
      notice_.clear();
      chooseNew(cursor_ + 1);
      return true;
    }
    notice_.clear();
    switch (key) {
      // ▲ turns the wheel to the digit drawn above the window (one less), ▼ to the one below.
      case Key::Up: digits_[cursor_] = (digits_[cursor_] + 9) % 10; return true;
      case Key::Down: digits_[cursor_] = (digits_[cursor_] + 1) % 10; return true;
      case Key::Select:
        if (step_ == Step::New || step_ == Step::Entry) {
          if (cursor_ == kMaxDigits - 1) {
            chooseNew(kMaxDigits);
          } else {
            ++cursor_;
          }
        } else if (last()) {
          check();
        } else {
          ++cursor_;
        }
        return true;
      case Key::Back:  // always consumed: BOOT never pops the lock
        if (cursor_ > 0) {
          --cursor_;
        } else if (step_ == Step::Gate && !open_) {
          ui::nav::home();  // not back to the module's screen these are in front of
        } else if (step_ != Step::Unlock) {
          ui::nav::pop();  // cancel the task
        }
        return true;
      default: return false;
    }
  }

  // The wait after wrong PINs counts down.
  bool onTick() override { return checksPin() && waitText() != drawnWait_; }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    const int n = count();
    Rect r = body;

    const Rect top = r.takeTop(kTopH);
    drawPadlock(c, top.x + (top.w - kPadlockCols * kCell) / 2,
                top.y + (top.h - kPadlockRows * kCell) / 2);

    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const std::string digit = "DIGIT " + std::to_string(cursor_ + 1);
    drawSectionHeader(c, r.takeTop(kSectionH), "PIN",
                      step_ == Step::New || step_ == Step::Entry
                          ? digit + " \xC2\xB7 " + std::to_string(kMinDigits) + " TO " +
                                std::to_string(kMaxDigits)
                          : digit + "/" + std::to_string(n));
    const Rect foot = r.takeBottom(kSectionH);
    drawnWait_ = checksPin() ? waitText() : std::string();
    if (!drawnWait_.empty()) {
      drawNotice(c, foot, "WRONG PIN: " + drawnWait_);
    } else if (!notice_.empty()) {
      drawNotice(c, foot, notice_);
    }

    // The wheels, centred in what's left.
    const int w = std::min(kWheelMaxW, (r.w - 2 * kPadX - (n - 1) * kWheelGap) / n);
    int x = r.x + (r.w - (n * w + (n - 1) * kWheelGap)) / 2;
    const int upY = r.y + (r.h - kColumnH) / 2;
    const int aboveY = upY + kArrowH + 6;
    const int windowY = aboveY + kNeighbourH + 8;
    const int belowY = windowY + kWindowH + 8;
    const int downY = belowY + kNeighbourH + 6;
    for (int i = 0; i < n; ++i, x += w + kWheelGap) {
      const Rect window{x, windowY, w, kWindowH};
      if (i == cursor_) {
        const int d = digits_[i];
        fill(c, window, kInk);
        drawText(c, window, digitText(d), kDigit, Align::Center, kPaper);
        drawText(c, {x, aboveY, w, kNeighbourH}, digitText(d - 1), kNeighbour, Align::Center);
        drawText(c, {x, belowY, w, kNeighbourH}, digitText(d + 1), kNeighbour, Align::Center);
        drawText(c, {x, upY, w, kArrowH}, glyph::kUp, kArrow, Align::Center);
        drawText(c, {x, downY, w, kArrowH}, glyph::kDown, kArrow, Align::Center);
      } else if (i < cursor_) {  // entered: masked
        frame(c, window, kRule);
        fill(c, {x + (w - 18) / 2, window.centerY() - 9, 18, 18}, kInk);
      } else if (i < required()) {  // still to come
        frame(c, window, kRule);
        fill(c, {x + (w - 24) / 2, window.centerY() - 2, 24, 4}, kInk);
      } else {  // a new PIN may stop before it
        dashedFrame(c, window);
      }
    }
  }

 private:
  // Wheels drawn, and how many of them the PIN needs.
  int count() const {
    switch (step_) {
      case Step::New:
      case Step::Entry: return kMaxDigits;
      case Step::Confirm: return static_cast<int>(newPin_.size());
      default: return std::min(kMaxDigits, std::max(1, settings::pinLength()));
    }
  }
  int required() const {
    return step_ == Step::New || step_ == Step::Entry ? kMinDigits : count();
  }
  // These wheels are checked against the PIN (so misses count and waits apply).
  bool checksPin() const {
    return step_ == Step::Unlock || step_ == Step::Gate || step_ == Step::Current;
  }
  const char *cancelVerb() const { return step_ == Step::Gate && !open_ ? "HOME" : "CANCEL"; }
  bool last() const { return cursor_ == count() - 1; }
  const char *lastVerb() const {
    switch (step_) {
      case Step::Current: return task_ == PinTask::Clear ? "CLEAR" : "OK";
      case Step::Confirm: return "SAVE";
      case Step::Gate: return "OPEN";
      default: return "UNLOCK";  // the lock, and Entry
    }
  }

  // After a first PIN: where this build asks for it (pin_places.h).
  static std::string setMessage() {
    using namespace pinplaces;
    if (asks(kPowerOn)) return "PIN SET: ASKED AT EVERY START";
    if (asks(kSettings) && asks(kFiles)) return "PIN SET: FOR SYSTEM AND FILES";
    if (asks(kSettings)) return "PIN SET: FOR SYSTEM";
    if (asks(kFiles)) return "PIN SET: FOR FILES";
    return "PIN SET: THIS BUILD NEVER ASKS";
  }

  std::string entered(int length) const {
    std::string s;
    for (int i = 0; i < length; ++i) s += static_cast<char>('0' + digits_[i]);
    return s;
  }

  void reset() {
    std::fill(digits_, digits_ + kMaxDigits, 0);
    cursor_ = 0;
  }

  void chooseNew(int length) {
    if (step_ == Step::Entry) {  // someone else checks it
      finish(entered(length));
      return;
    }
    newPin_ = entered(length);
    step_ = Step::Confirm;
    reset();
  }

  void finish(const std::string &message) {
    if (done_) done_(message);
    ui::nav::pop();
  }

  void check() {
    const std::string pin = entered(count());
    switch (step_) {
      case Step::Unlock:
        if (settings::pinMatches(pin)) {
          log_i("lock: opened");
          matched();
          settings::unlocked();
          openUp(Guarded::Settings);  // the lock is the PIN for the guarded modules too
          openUp(Guarded::Files);
          openUp(Guarded::Agent);
          pinEntered(pin);
          ui::nav::pop();
          return;
        }
        break;
      case Step::Gate:
        if (settings::pinMatches(pin)) {
          log_i("lock: %s opened", labelOf(module_));
          matched();
          settings::unlocked();
          openUp(module_);
          if (module_ == Guarded::Agent) pinEntered(pin);
          if (open_) {
            ui::nav::replace(open_());
          } else {
            ui::nav::pop();  // back to the module's screen
          }
          return;
        }
        break;
      case Step::Current:
        if (settings::pinMatches(pin)) {
          matched();
          if (task_ == PinTask::Clear) {
            settings::clearPin();
            finish("PIN CLEARED: NO LOCK");
          } else {
            step_ = Step::New;
            reset();
          }
          return;
        }
        break;
      case Step::Confirm:
        if (pin == newPin_) {
          const bool changed = settings::hasPin();
          settings::setPin(newPin_);
          finish(changed ? "PIN CHANGED" : setMessage());
        } else {
          step_ = Step::New;
          newPin_.clear();
          reset();
          notice_ = "THE TWO DIDN'T MATCH: AGAIN";
        }
        return;
      case Step::New:
      case Step::Entry: return;  // chooseNew()
    }
    missed();
    reset();
    notice_ = "WRONG PIN";
  }

  Step step_ = Step::Unlock;
  PinTask task_ = PinTask::Set;
  std::function<void(const std::string &)> done_;
  Guarded module_ = Guarded::Files;                    // Gate: whose tag
  std::function<std::unique_ptr<ui::Screen>()> open_;  // Gate: its screen; empty: in front of it
  std::string tag_;     // Entry: whose tag
  std::string newPin_;  // NEW PIN's digits, waiting for NEW PIN AGAIN
  int digits_[kMaxDigits] = {};
  int cursor_ = 0;
  std::string notice_;     // after a miss; cleared by the next key
  std::string drawnWait_;  // waitText() as last drawn
};

// The wheels in front of the module's screen on top: it hasn't been in use for a minute.
void regate(Guarded m) {
  log_i("lock: %s not in use for a minute, asking again", labelOf(m));
  guardOf(m).open = false;  // before the push: guardedLeft() mustn't count it as use
  ui::nav::push(std::make_unique<PinScreen>(m, nullptr));
}

}  // namespace

void lockNow() {
  if (!pinplaces::asks(pinplaces::kPowerOn) || !settings::hasPin() || ui::nav::locked()) return;
  ui::nav::push(std::make_unique<PinScreen>());
}

void pollLock() {
  const int minutes = pinplaces::asks(pinplaces::kPowerOn) ? settings::lockMinutes() : 0;
  if (minutes > 0 && settings::hasPin() && !ui::nav::locked() &&
      input::idleMs() >= static_cast<uint32_t>(minutes) * 60000u && !audio::ringing() &&
      !audio::recording()) {
    log_i("lock: %d min without a key", minutes);
    lockNow();
    return;
  }
  // A guarded module's screen on top a minute without a key (its keys stamp it).
  if (onTop >= 0) {
    const Guarded m = static_cast<Guarded>(onTop);
    if (applies(m) && !fresh(m)) regate(m);
  }
}

void onPinEntered(void (*listener)(const std::string &pin)) { pinListener = listener; }

std::unique_ptr<ui::Screen> makePinEntry(std::string tag,
                                         std::function<void(const std::string &)> entered) {
  return std::make_unique<PinScreen>(std::move(tag), std::move(entered));
}

std::unique_ptr<ui::Screen> makePinTask(PinTask task,
                                        std::function<void(const std::string &)> done) {
  return std::make_unique<PinScreen>(task, std::move(done));
}

void openGuarded(Guarded module, std::function<std::unique_ptr<ui::Screen>()> open,
                 bool replace) {
  std::unique_ptr<ui::Screen> screen;
  if (!applies(module) || fresh(module)) {
    openUp(module);
    screen = open();
  } else {
    screen = std::make_unique<PinScreen>(module, std::move(open));
  }
  if (replace) {
    ui::nav::replace(std::move(screen));
  } else {
    ui::nav::push(std::move(screen));
  }
}

void guardedOnTop(Guarded module) {
  onTop = static_cast<int>(module);
  if (!applies(module) || fresh(module)) {
    openUp(module);
  } else {
    regate(module);
  }
}

void forgetGuard(Guarded module) { guardOf(module).open = false; }

void guardedInUse(Guarded module) {
  if (guardOf(module).open) guardOf(module).ms = millis();
}

void guardedLeft(Guarded module) {
  guardedInUse(module);
  if (onTop == static_cast<int>(module)) onTop = -1;
}

}  // namespace apps
