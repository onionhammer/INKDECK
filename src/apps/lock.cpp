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

// What the wheels are for: opening the lock, opening a module (openGuarded()), or one step of a
// PinTask.
enum class Step : uint8_t { Unlock, Gate, Current, New, Confirm };

class PinScreen : public ui::Screen {
 public:
  PinScreen() = default;  // the lock
  PinScreen(PinTask task, std::function<void(const std::string &)> done)
      : step_(settings::hasPin() ? Step::Current : Step::New), task_(task), done_(std::move(done)) {}
  PinScreen(const char *module, std::function<std::unique_ptr<ui::Screen>()> open)
      : step_(Step::Gate), module_(module), open_(std::move(open)) {}

  std::string tag() const override {
    switch (step_) {
      case Step::Unlock: return "LOCKED";
      case Step::Gate: return moduleTag(module_);
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
    out[n++] = {ui::glyph::kDial, "DIGIT"};
    if (step_ == Step::New) {
      const bool lastWheel = cursor_ == kMaxDigits - 1;
      out[n++] = {ui::glyph::kPress, lastWheel ? "DONE" : "NEXT"};
      if (cursor_ >= kMinDigits - 1 && !lastWheel) out[n++] = {ui::glyph::kHoldPress, "DONE"};
    } else {
      out[n++] = {ui::glyph::kPress, last() ? lastVerb() : "NEXT"};
    }
    if (cursor_ > 0) {
      out[n++] = {"BOOT", "BACK"};
    } else if (step_ != Step::Unlock) {
      out[n++] = {"BOOT", "CANCEL"};
    }
    return n;
  }

  // A ring sheet or an off screen over the wheels starts the PIN again.
  void onEnter() override { reset(); }

  bool onKey(Key key) override {
    if (key == Key::SelectLong) {  // a new PIN of fewer than eight digits
      if (step_ != Step::New || cursor_ < kMinDigits - 1) return false;
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
        if (step_ == Step::New) {
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
        } else if (step_ != Step::Unlock) {
          ui::nav::pop();  // cancel the task
        }
        return true;
      default: return false;
    }
  }

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
                      step_ == Step::New
                          ? digit + " \xC2\xB7 " + std::to_string(kMinDigits) + " TO " +
                                std::to_string(kMaxDigits)
                          : digit + "/" + std::to_string(n));
    const Rect foot = r.takeBottom(kSectionH);
    if (!notice_.empty()) drawNotice(c, foot, notice_);

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
      case Step::New: return kMaxDigits;
      case Step::Confirm: return static_cast<int>(newPin_.size());
      default: return std::min(kMaxDigits, std::max(1, settings::pinLength()));
    }
  }
  int required() const { return step_ == Step::New ? kMinDigits : count(); }
  bool last() const { return cursor_ == count() - 1; }
  const char *lastVerb() const {
    switch (step_) {
      case Step::Current: return task_ == PinTask::Clear ? "CLEAR" : "OK";
      case Step::Confirm: return "SAVE";
      case Step::Gate: return "OPEN";
      default: return "UNLOCK";
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
          settings::unlocked();
          ui::nav::pop();
          return;
        }
        break;
      case Step::Gate:
        if (settings::pinMatches(pin)) {
          log_i("lock: %s opened", module_);
          settings::unlocked();
          ui::nav::replace(open_());
          return;
        }
        break;
      case Step::Current:
        if (settings::pinMatches(pin)) {
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
      case Step::New: return;  // chooseNew()
    }
    ++misses_;
    log_w("lock: wrong PIN (%d on this screen)", misses_);
    reset();
    notice_ = "WRONG PIN";
  }

  Step step_ = Step::Unlock;
  PinTask task_ = PinTask::Set;
  std::function<void(const std::string &)> done_;
  const char *module_ = nullptr;                      // Gate: whose tag, and what it opens
  std::function<std::unique_ptr<ui::Screen>()> open_;
  std::string newPin_;  // NEW PIN's digits, waiting for NEW PIN AGAIN
  int digits_[kMaxDigits] = {};
  int cursor_ = 0;
  int misses_ = 0;
  std::string notice_;  // after a miss; cleared by the next key
};

// When each Guarded module's screens were last on top.
struct LastUse {
  bool ever = false;
  uint32_t ms = 0;
};
LastUse lastUse[2];  // by Guarded

constexpr uint32_t kGuardGraceMs = 60 * 1000;  // openGuarded(): no PIN within a minute of use

}  // namespace

void lockNow() {
  if (!pinplaces::asks(pinplaces::kPowerOn) || !settings::hasPin() || ui::nav::locked()) return;
  ui::nav::push(std::make_unique<PinScreen>());
}

void pollLock() {
  if (!pinplaces::asks(pinplaces::kPowerOn)) return;
  const int minutes = settings::lockMinutes();
  if (minutes <= 0 || !settings::hasPin() || ui::nav::locked()) return;
  if (input::idleMs() < static_cast<uint32_t>(minutes) * 60000u) return;
  if (audio::ringing() || audio::recording()) return;
  log_i("lock: %d min without a key", minutes);
  lockNow();
}

std::unique_ptr<ui::Screen> makePinTask(PinTask task,
                                        std::function<void(const std::string &)> done) {
  return std::make_unique<PinScreen>(task, std::move(done));
}

void guardedInUse(Guarded module) {
  LastUse &use = lastUse[static_cast<int>(module)];
  use.ever = true;
  use.ms = millis();
}

void openGuarded(Guarded module, std::function<std::unique_ptr<ui::Screen>()> open) {
  const bool settingsGate = module == Guarded::Settings;
  const LastUse &use = lastUse[static_cast<int>(module)];
  const bool fresh = use.ever && millis() - use.ms < kGuardGraceMs;
  if (fresh || !settings::hasPin() ||
      !pinplaces::asks(settingsGate ? pinplaces::kSettings : pinplaces::kFiles)) {
    ui::nav::push(open());
    return;
  }
  ui::nav::push(std::make_unique<PinScreen>(settingsGate ? "SYSTEM" : "FILES", std::move(open)));
}

}  // namespace apps
