#include "devtools.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <sys/stat.h>

#include <string>

#include "apps/idle.h"
#include "audio.h"
#include "forecast.h"
#include "input.h"
#include "pmic.h"
#include "podcast_library.h"
#include "qmi8658.h"
#include "rtc.h"
#include "screen.h"

namespace devtools {
namespace {

// A path on the card, or "" (with a message) for anything else.
String cardPath(const String &path) {
  if (path.startsWith("/sdcard/") && path.indexOf("..") < 0) return path;
  Serial.println("devtools: paths must start with /sdcard/");
  return "";
}

// "!feed https://...", "!append /sdcard/x text", "!rm /sdcard/x", "!wxforget".
void lineCommand(const String &line) {
  if (line == "wxforget") {
    weather::forget();
    return;
  }
  if (line.startsWith("append ")) {
    const int space = line.indexOf(' ', 7);
    const String path = cardPath(space > 0 ? line.substring(7, space) : "");
    if (path.isEmpty()) return;
    FILE *f = fopen(path.c_str(), "ab");
    if (!f) {
      Serial.println("devtools: can't open that file");
      return;
    }
    fprintf(f, "%s\n", line.substring(space + 1).c_str());
    fclose(f);
    Serial.println("devtools: appended");
    return;
  }
  if (line == "sdtest") {
    // Writes 64 KB of a pattern from internal RAM, from PSRAM and from flash, reads each back.
    constexpr size_t kSize = 64 * 1024;
    static const char kFlash[] = "flash-resident pattern 0123456789abcdefghijklmnopqrstuvwxyz\n";
#define SDTEST_64 "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/"
#define SDTEST_512 SDTEST_64 SDTEST_64 SDTEST_64 SDTEST_64 SDTEST_64 SDTEST_64 SDTEST_64 SDTEST_64
    static const char kFlashBig[] = SDTEST_512 SDTEST_512;  // 1 KB, written in one call
    uint8_t *internal = static_cast<uint8_t *>(heap_caps_malloc(kSize, MALLOC_CAP_INTERNAL));
    uint8_t *psram = static_cast<uint8_t *>(heap_caps_malloc(kSize, MALLOC_CAP_SPIRAM));
    uint8_t *check = static_cast<uint8_t *>(heap_caps_malloc(kSize, MALLOC_CAP_SPIRAM));
    if (!internal || !psram || !check) {
      Serial.println("sdtest: no memory");
    } else {
      for (size_t i = 0; i < kSize; ++i) internal[i] = psram[i] = static_cast<uint8_t>(i * 7 + 3);
      struct Case {
        const char *name;
        const uint8_t *src;
        size_t size;
        size_t chunk;
      } cases[] = {{"internal", internal, kSize, kSize},
                   {"psram", psram, kSize, kSize},
                   {"psram-16k", psram, kSize, 16 * 1024},
                   {"flash-1k", reinterpret_cast<const uint8_t *>(kFlashBig), sizeof kFlashBig - 1,
                    sizeof kFlashBig - 1},
                   {"flash", reinterpret_cast<const uint8_t *>(kFlash), sizeof kFlash - 1,
                    sizeof kFlash - 1}};
      for (const Case &c : cases) {
        const std::string path = std::string("/sdcard/.inkdeck/sdtest-") + c.name + ".bin";
        FILE *f = fopen(path.c_str(), "wb");
        size_t written = 0;
        if (f) {
          if (c.src == reinterpret_cast<const uint8_t *>(kFlash)) {
            for (int r = 0; r < 64; ++r) written += fwrite(c.src, 1, c.size, f);
          } else {
            for (size_t off = 0; off < c.size; off += c.chunk) written += fwrite(c.src + off, 1, c.chunk, f);
          }
          fclose(f);
        }
        f = fopen(path.c_str(), "rb");
        size_t bad = 0, firstBad = SIZE_MAX, got = 0;
        if (f) {
          got = fread(check, 1, kSize, f);
          fclose(f);
          for (size_t i = 0; i < got; ++i) {
            const uint8_t want = c.src == reinterpret_cast<const uint8_t *>(kFlash)
                                     ? c.src[i % c.size]
                                     : c.src[i];
            if (check[i] != want) {
              ++bad;
              if (firstBad == SIZE_MAX) firstBad = i;
            }
          }
        }
        Serial.printf("sdtest %-10s wrote %u read %u, %u bytes wrong (first at %d)\n", c.name,
                      static_cast<unsigned>(written), static_cast<unsigned>(got),
                      static_cast<unsigned>(bad), firstBad == SIZE_MAX ? -1 : static_cast<int>(firstBad));
        remove(path.c_str());
      }
    }
    heap_caps_free(internal);
    heap_caps_free(psram);
    heap_caps_free(check);
    return;
  }
  if (line.startsWith("cat ")) {
    const String path = cardPath(line.substring(4));
    if (path.isEmpty()) return;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
      Serial.println("devtools: can't open that file");
      return;
    }
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) Serial.write(reinterpret_cast<uint8_t *>(buf), n);
    fclose(f);
    Serial.println("\ndevtools: end of file");
    return;
  }
  if (line.startsWith("rm ")) {
    const String path = cardPath(line.substring(3));
    if (path.isEmpty()) return;
    Serial.println(remove(path.c_str()) == 0 ? "devtools: removed" : "devtools: can't remove");
    return;
  }
  if (line.startsWith("feed ")) {
    const String url = line.substring(5);
    mkdir(podcasts::kRoot, 0777);
    FILE *f = fopen(podcasts::kFeedsFile, "ab");
    if (!f) {
      Serial.println("devtools: can't open the feeds file");
      return;
    }
    fprintf(f, "%s\n", url.c_str());
    fclose(f);
    Serial.printf("devtools: added a feed (%u characters)\n", url.length());
    return;
  }
  Serial.println("devtools: unknown command");
}

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
        case 'c': rtc::forget(); break;
        case 'o': input::inject(Key::Power); break;
        case 'z': apps::requestStandby(2); break;
        case 'h':
          Serial.printf("heap: internal %u free (largest %u, lowest %u), DMA %u free (largest %u), "
                        "PSRAM %u free\n",
                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                        heap_caps_get_free_size(MALLOC_CAP_DMA),
                        heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
          break;
        case '!': {
          String line = Serial.readStringUntil('\n');
          line.trim();
          lineCommand(line);
          break;
        }
        case 'i': {
          const qmi8658::Reading m = qmi8658::read();
          Serial.printf("IMU chip acc %+.2f %+.2f %+.2f g, gyro %+.1f %+.1f %+.1f dps\n",
                        m.acc[0], m.acc[1], m.acc[2], m.gyro[0], m.gyro[1], m.gyro[2]);
          break;
        }
        default: break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

void begin() { xTaskCreatePinnedToCore(task, "devtools", 4096, nullptr, 1, nullptr, 1); }

}  // namespace devtools
