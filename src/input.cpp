#include "input.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "board_pins.h"

namespace input {
namespace {

constexpr uint32_t kTickMs = 5;
constexpr uint8_t kDebounceTicks = 3;  // ~15 ms, like the vendor's MultiButton setup
constexpr uint32_t kLongPressMs = 1000;  // docs/ui-design.md: HOLD = ~1 s
constexpr uint32_t kRepeatDelayMs = 500;
constexpr uint32_t kRepeatEveryMs = 200;

struct Button {
  int pin;
  Key shortKey;
  Key longKey;
  bool autoRepeat;  // true: fire on press + repeat; false: fire on release, or long on hold
  // state
  bool down = false;
  uint8_t integrator = 0;
  uint32_t heldMs = 0;
  bool longFired = false;
};

Button buttons[] = {
    {board::BTN_UP, Key::Up, Key::Up, true},
    {board::BTN_DOWN, Key::Down, Key::Down, true},
    {board::BTN_PRESS, Key::Select, Key::SelectLong, false},
    {board::BTN_BOOT, Key::Back, Key::BackLong, false},
};

QueueHandle_t queue = nullptr;
esp_timer_handle_t timer = nullptr;

void emit(Key key) { xQueueSend(queue, &key, 0); }

void tick(void *) {
  for (auto &b : buttons) {
    const bool pressed = gpio_get_level(static_cast<gpio_num_t>(b.pin)) == 0;  // active low
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
      if (b.heldMs >= kRepeatDelayMs && (b.heldMs - kRepeatDelayMs) % kRepeatEveryMs == 0) {
        emit(b.shortKey);
      }
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
  }
  return "?";
}

void begin() {
  for (const auto &b : buttons) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << b.pin;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;  // rotary has external 10k too; harmless
    gpio_config(&cfg);
  }
  queue = xQueueCreate(32, sizeof(Key));
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
  return xQueueReceive(queue, &key, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void inject(Key key) { emit(key); }

}  // namespace input
