#include "input.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>

#include "board_pins.h"

namespace input {
namespace {

constexpr uint32_t kTickMs = 5;
constexpr uint8_t kDebounceTicks = 3;  // ~15 ms, like the vendor's MultiButton setup
constexpr uint32_t kLongPressMs = 1000;  // docs/ui-design.md: HOLD = ~1 s
constexpr uint32_t kRepeatDelayMs = 500;
constexpr uint32_t kRepeatEveryMs = 200;
constexpr uint32_t kFastAfterMs = 1700;  // repeats after this page through lists (6 rows first)
constexpr uint8_t kFastFlag = 0x80;      // or'd into a queued key
constexpr uint32_t kPowerMaxMs = 2000;  // a longer PWR hold is heading for the 4 s forced off

struct Button {
  int pin;
  Key shortKey;
  Key longKey;
  bool autoRepeat;  // true: fire on press + repeat; false: fire on release, or long on hold
  bool activeHigh;  // PWR_KEY_SENSE; the others are active low
  // state
  bool down = false;
  uint8_t integrator = 0;
  uint32_t heldMs = 0;
  bool longFired = false;
};

Button buttons[] = {
    {board::BTN_UP, Key::Up, Key::Up, true, false},
    {board::BTN_DOWN, Key::Down, Key::Down, true, false},
    {board::BTN_PRESS, Key::Select, Key::SelectLong, false, false},
    {board::BTN_BOOT, Key::Back, Key::BackLong, false, false},
    {board::PWR_KEY_SENSE, Key::Power, Key::Power, false, true},
};

QueueHandle_t queue = nullptr;
esp_timer_handle_t timer = nullptr;

std::atomic<uint32_t> lastKeyMs{0};
bool lastFast = false;  // only touched by the task calling wait()

void emit(Key key, bool fast = false) {
  lastKeyMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  const uint8_t item = static_cast<uint8_t>(key) | (fast ? kFastFlag : 0);
  xQueueSend(queue, &item, 0);
}

bool isPressed(const Button &b) {
  return gpio_get_level(static_cast<gpio_num_t>(b.pin)) == (b.activeHigh ? 1 : 0);
}

void tick(void *) {
  for (auto &b : buttons) {
    const bool pressed = isPressed(b);
    if (pressed != b.down) {
      if (++b.integrator < kDebounceTicks) continue;
      b.integrator = 0;
      b.down = pressed;
      if (pressed) {
        b.heldMs = 0;
        b.longFired = false;
        if (b.autoRepeat) emit(b.shortKey);
      } else if (!b.autoRepeat && !b.longFired) {
        emit(b.shortKey);
      }
      continue;
    }
    b.integrator = 0;
    if (!b.down) continue;

    b.heldMs += kTickMs;
    if (b.autoRepeat) {
      if (!b.longFired && b.heldMs >= kRepeatDelayMs &&
          (b.heldMs - kRepeatDelayMs) % kRepeatEveryMs == 0) {
        emit(b.shortKey, b.heldMs >= kFastAfterMs);
      }
    } else if (b.shortKey == Key::Power) {
      if (b.heldMs > kPowerMaxMs) b.longFired = true;  // released later: no key
    } else if (!b.longFired && b.heldMs >= kLongPressMs) {
      b.longFired = true;
      emit(b.longKey);
    }
  }
}

}  // namespace

const char *name(Key key) {
  switch (key) {
    case Key::Up: return "Up";
    case Key::Down: return "Down";
    case Key::Select: return "Select";
    case Key::SelectLong: return "SelectLong";
    case Key::Back: return "Back";
    case Key::BackLong: return "BackLong";
    case Key::Power: return "Power";
  }
  return "?";
}

void begin() {
  for (auto &b : buttons) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << b.pin;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;  // rotary has external 10k too; harmless
    gpio_config(&cfg);
  }
  vTaskDelay(pdMS_TO_TICKS(2));  // let the pull-ups settle before reading the starting levels
  for (auto &b : buttons) {
    b.down = isPressed(b);
    b.longFired = b.down;  // held since power-on: its release is not a press
    b.heldMs = b.down ? kPowerMaxMs + 1 : 0;
  }
  lastKeyMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  queue = xQueueCreate(32, sizeof(uint8_t));
  const esp_timer_create_args_t args = {
      .callback = tick,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "buttons",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&args, &timer);
  esp_timer_start_periodic(timer, kTickMs * 1000);
}

bool wait(Key &key, uint32_t timeoutMs) {
  uint8_t item = 0;
  if (xQueueReceive(queue, &item, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    lastFast = false;
    return false;
  }
  key = static_cast<Key>(item & ~kFastFlag);
  lastFast = (item & kFastFlag) != 0;
  return true;
}

bool lastKeyFast() { return lastFast; }

void inject(Key key, bool fast) { emit(key, fast); }

bool anyDown() {
  for (const auto &b : buttons) {
    if (b.down || isPressed(b)) return true;
  }
  return false;
}

bool pending() { return queue && uxQueueMessagesWaiting(queue) > 0; }

uint32_t idleMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000) - lastKeyMs.load();
}

}  // namespace input
