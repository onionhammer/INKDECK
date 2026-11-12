#include "devtools.h"

#include <Arduino.h>

#include "audio.h"
#include "input.h"
#include "pmic.h"
#include "screen.h"

namespace devtools {
namespace {

void task(void *) {
  using input::Key;
  for (;;) {
    while (Serial.available() > 0) {
      switch (Serial.read()) {
        case 'u': input::inject(Key::Up); break;
        case 'd': input::inject(Key::Down); break;
        case 'p': input::inject(Key::Select); break;
        case 'P': input::inject(Key::SelectLong); break;
        case 'b': input::inject(Key::Back); break;
        case 'B': input::inject(Key::BackLong); break;
        case 's': screen::dump(Serial); break;
        case '-': audio::changeVolume(-5); break;  // e.g. silence the speaker before a test
        case '+': audio::changeVolume(+5); break;
        case 'r': pmic::dump(Serial); break;
        default: break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

void begin() { xTaskCreatePinnedToCore(task, "devtools", 4096, nullptr, 1, nullptr, 1); }

}  // namespace devtools
