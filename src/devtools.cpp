#include "devtools.h"

#include <Arduino.h>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <sys/stat.h>

#include <algorithm>
#include <string>

#include "apps/agent_face.h"
#include "apps/news_edition.h"
#include "apps/idle.h"
#include "audio.h"
#include "board_pins.h"
#include "comms.h"
#include "epub.h"
#include "features.h"
#include "flashfs.h"
#include "forecast.h"
#include "input.h"
#include "news_library.h"
#include "pmic.h"
#include "podcast_library.h"
#include "qmi8658.h"
#include "rtc.h"
#include "screen.h"
#include "text.h"
#include "zip.h"

namespace devtools {
namespace {

// A path on the card, the onboard flash (mounted on demand) or a mounted network share (read-only;
// open it in Files first), or "" (with a message) for anything else.
String cardPath(const String &path) {
  if (path.indexOf("..") < 0) {
    if (path.startsWith("/sdcard/") || path.startsWith("/net/")) return path;
    if (path.startsWith("/ffat/") || path == "/ffat") {
      if (flashfs::mount()) return path;
      Serial.println("devtools: the onboard FAT partition won't mount");
      return "";
    }
  }
  Serial.println("devtools: paths must start with /sdcard/, /ffat/ or /net/");
  return "";
}

// "!cp SRC|DST": copies one file, e.g. a book from the card to the onboard flash.
void copyFile(const String &src, const String &dst) {
  FILE *in = fopen(src.c_str(), "rb");
  if (!in) {
    Serial.println("devtools: can't open the source");
    return;
  }
  FILE *out = fopen(dst.c_str(), "wb");
  if (!out) {
    fclose(in);
    Serial.println("devtools: can't create the destination (does its folder exist?)");
    return;
  }
  constexpr size_t kChunk = 32 * 1024;
  // RAM, not flash, as the source of every write (AGENTS.md, Gotchas).
  auto *buf = static_cast<uint8_t *>(heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM));
  size_t total = 0;
  bool ok = buf != nullptr;
  const uint32_t t0 = millis();
  while (ok) {
    const size_t n = fread(buf, 1, kChunk, in);
    if (n == 0) break;
    ok = fwrite(buf, 1, n, out) == n;
    total += n;
  }
  ok = ok && !ferror(in);
  fclose(in);
  ok = fclose(out) == 0 && ok;
  heap_caps_free(buf);
  if (!ok) remove(dst.c_str());
  Serial.printf("devtools: %s %u bytes in %lu ms\n", ok ? "copied" : "copy FAILED after",
                static_cast<unsigned>(total), millis() - t0);
}

// "!feed https://...", "!append /sdcard/x text", "!rm /sdcard/x", "!wxforget".
void lineCommand(const String &line) {
  if (line == "wom" || line.startsWith("wom ")) {
    // The picture viewer's nap wake (qmi8658::wakeOnMotion): each event toggles INT2.
    int seconds = 10, threshold = qmi8658::kWakeThresholdMg;
    siscanf(line.c_str() + 3, "%d %d", &seconds, &threshold);
    seconds = std::max(1, seconds);
    if (!qmi8658::wakeOnMotion(threshold)) {
      Serial.println("wom: arming failed");
      qmi8658::setEnabled(true);
      return;
    }
    int level = digitalRead(board::IMU_INT2);
    int events = 0;
    Serial.printf("wom: armed at %d mg, INT2 %d; move the board (%d s)\n", threshold, level,
                  seconds);
    const uint32_t start = millis();
    while (millis() - start < static_cast<uint32_t>(seconds) * 1000) {
      const int now = digitalRead(board::IMU_INT2);
      if (now != level) {
        level = now;
        Serial.printf("wom: event %d at %lu ms\n", ++events,
                      static_cast<unsigned long>(millis() - start));
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    qmi8658::setEnabled(true);
    Serial.printf("wom: %d events; IMU back to normal\n", events);
    return;
  }
  if (line == "wxforget") {
    weather::forget();
    return;
  }
  if (line.startsWith("wxmodels ")) {
    weather::compareModels(line.substring(9).c_str());
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
  if (line.startsWith("b64 ")) {
    const int space = line.indexOf(' ', 4);
    const String path = cardPath(space > 0 ? line.substring(4, space) : "");
    if (path.isEmpty()) return;
    const String data = line.substring(space + 1);
    std::string bytes(data.length() / 4 * 3 + 3, '\0');  // RAM, not flash (AGENTS.md, Gotchas)
    size_t n = 0;
    if (mbedtls_base64_decode(reinterpret_cast<unsigned char *>(&bytes[0]), bytes.size(), &n,
                              reinterpret_cast<const unsigned char *>(data.c_str()),
                              data.length()) != 0) {
      Serial.println("devtools: that isn't base64");
      return;
    }
    FILE *f = fopen(path.c_str(), "ab");
    if (!f) {
      Serial.println("devtools: can't open that file (does its folder exist?)");
      return;
    }
    const size_t wrote = fwrite(bytes.data(), 1, n, f);
    fclose(f);
    Serial.printf("devtools: appended %u bytes\n", static_cast<unsigned>(wrote));
    return;
  }
#if INKDECK_AGENT
  if (line == "agentloop") {
    // The voice paths without a network: 3 s of the mic into the capture ring, then the same
    // samples back out through the stream ring (16 kHz).
    static uint8_t chunk[1024];
    if (!audio::captureStart()) {
      Serial.println("agentloop: the mic won't start");
      return;
    }
    Serial.println("agentloop: speak for 3 s");
    std::string pcm;
    const uint32_t t0 = millis();
    while (millis() - t0 < 3000) {
      const size_t n = audio::captureRead(chunk, sizeof chunk, 50);
      pcm.append(reinterpret_cast<const char *>(chunk), n);
    }
    audio::captureStop();
    for (size_t n; (n = audio::captureRead(chunk, sizeof chunk, 0)) > 0;) {
      pcm.append(reinterpret_cast<const char *>(chunk), n);
    }
    int peak = 0;
    for (size_t i = 0; i + 1 < pcm.size(); i += 2) {
      const int16_t v = static_cast<int16_t>(static_cast<uint8_t>(pcm[i]) |
                                             static_cast<uint8_t>(pcm[i + 1]) << 8);
      peak = std::max(peak, abs(static_cast<int>(v)));
    }
    Serial.printf("agentloop: captured %u bytes, peak %d; playing\n",
                  static_cast<unsigned>(pcm.size()), peak);
    if (!audio::streamStart(audio::kRecordRate)) {
      Serial.println("agentloop: the output won't start");
      return;
    }
    for (size_t off = 0; off < pcm.size();) {
      const size_t n = audio::streamWrite(reinterpret_cast<const uint8_t *>(pcm.data()) + off,
                                          std::min<size_t>(1024, pcm.size() - off), 200);
      if (n == 0 && !audio::streaming()) break;
      off += n;
    }
    audio::streamEnd();
    const uint32_t t1 = millis();
    while (audio::streaming() && millis() - t1 < 6000) delay(20);
    Serial.printf("agentloop: done in %lu ms\n", static_cast<unsigned long>(millis() - t1));
    return;
  }
#endif
#if INKDECK_COMMS
  if (line == "comms") {
    comms::dump(Serial);
    return;
  }
  if (line == "commstest") {
    comms::selfTest(Serial);
    return;
  }
  if (line.startsWith("commsgroup")) {
    // A test group without the card (the card's mesh_group lines are the real way): kept in
    // NVS, on top of the card's groups. "!commsgroup -" removes it.
    String rest = line.substring(10);
    rest.trim();
    if (rest.isEmpty()) {
      Serial.println("commsgroup: !commsgroup <secret> [name] sets a test group (dev only: the card's "
                     "mesh_group lines are the real way); !commsgroup - removes it");
      return;
    }
    if (rest == "-") {
      comms::setTestGroup("", "");
      Serial.println("commsgroup: test group removed");
      return;
    }
    const int space = rest.indexOf(' ');
    const String secret = space < 0 ? rest : rest.substring(0, space);
    const String name = space < 0 ? String("TEST") : rest.substring(space + 1);
    if (secret.length() < 8) {
      Serial.println("commsgroup: the secret must be 8 characters or more");
      return;
    }
    comms::setTestGroup(secret.c_str(), name.c_str());
    Serial.printf("commsgroup: test group %s set\n", name.c_str());
    return;
  }
  if (line.startsWith("commschannel")) {
    // Pins the channel used off Wi-Fi, in place of mesh_channel (0: mesh_channel again).
    const int ch = line.substring(12).toInt();
    if (ch < 0 || ch > 13) {
      Serial.println("commschannel: 1 to 13, or 0 for mesh_channel");
      return;
    }
    comms::setTestChannel(ch);
    Serial.printf("commschannel: %s\n", ch ? ("channel " + String(ch)).c_str() : "mesh_channel");
    return;
  }
  if (line == "commsrelay on" || line == "commsrelay off") {
    comms::setRelayForOthers(line.endsWith("on"));
    Serial.printf("commsrelay: relay for others %s\n", comms::relayForOthers() ? "on" : "off");
    return;
  }
  if (line == "commsplay") {
    // PLAY LAST: the last call heard, again.
    if (!comms::playLast()) {
      Serial.printf("commsplay: %s\n", comms::status().error.c_str());
    } else {
      const comms::LastCall c = comms::lastCall();
      Serial.printf("commsplay: playing %s's call again, %lu ms\n", c.fromName.c_str(),
                    static_cast<unsigned long>(c.ms));
    }
    return;
  }
  if (line == "commsresend" || line == "commsresend old") {
    comms::resend(line.endsWith("old"));
    Serial.println("commsresend: sent; look at the other board's dropped counters (!comms)");
    return;
  }
  if (line == "commsping" || line.startsWith("commstalk")) {
    if (!comms::configured()) {
      Serial.println("comms: no group (a mesh_group line, an agent_token or !commsgroup)");
      return;
    }
    comms::hold();  // on battery the radio is off until something holds it
    for (int i = 0; i < 60 && !comms::listening(); ++i) delay(50);
    const int group = comms::activeGroup();
    if (line == "commsping") {
      const uint32_t id = comms::ping(group);
      if (!id) {
        Serial.println("commsping: not listening, or the group doesn't go over the air");
      } else {
        Serial.printf("commsping: ping %08lX to group %d; answers for 2 s\n",
                      static_cast<unsigned long>(id), group);
        delay(2000);
        const std::vector<comms::Pong> answers = comms::pongs();
        for (const comms::Pong &p : answers) {
          Serial.printf("commsping: %08lX %s: %lu ms round trip (%lu held there, so %ld ms on "
                        "the way), heard there %d dBm (%d hops), here %d dBm (%d hops)\n",
                        static_cast<unsigned long>(p.id), p.name.empty() ? "?" : p.name.c_str(),
                        static_cast<unsigned long>(p.rttMs), static_cast<unsigned long>(p.heldMs),
                        static_cast<long>(p.rttMs) - static_cast<long>(p.heldMs), p.rssiThere,
                        p.hopsThere, p.rssiHere, p.hopsBack);
        }
        Serial.printf("commsping: %u answers\n", static_cast<unsigned>(answers.size()));
      }
    } else {
      const int seconds = std::max(1, std::min(60, static_cast<int>(line.substring(9).toInt())));
      if (!comms::talkStart(group, 0)) {
        Serial.printf("commstalk: %s\n", comms::status().error.c_str());
      } else {
        Serial.printf("commstalk: talking to group %d for %d s\n", group, seconds);
        delay(seconds * 1000);
        comms::talkStop();
        delay(300);
        const std::vector<comms::Call> c = comms::calls(group, 1);
        if (!c.empty() && c[0].outgoing) {
          Serial.printf("commstalk: sent %lu ms; %d boards said they heard it (so far)\n",
                        static_cast<unsigned long>(c[0].ms), c[0].heardBy);
        } else {
          Serial.printf("commstalk: %s\n", comms::status().error.c_str());
        }
      }
    }
    comms::release();
    return;
  }
#endif
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
  if (line.startsWith("cp ")) {
    const String rest = line.substring(3);
    const int bar = rest.indexOf('|');
    const String src = cardPath(bar < 0 ? rest : rest.substring(0, bar));
    const String dst = bar < 0 ? String() : cardPath(rest.substring(bar + 1));
    if (bar < 0) Serial.println("devtools: usage !cp SRC|DST");
    if (!src.isEmpty() && !dst.isEmpty()) copyFile(src, dst);
    return;
  }
  if (line.startsWith("mkdir ")) {
    const String path = cardPath(line.substring(6));
    if (path.isEmpty()) return;
    Serial.println(mkdir(path.c_str(), 0775) == 0 ? "devtools: made" : "devtools: mkdir failed");
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
  if (line.startsWith("ls ")) {
    const String path = cardPath(line.substring(3));
    if (path.isEmpty()) return;
    DIR *dir = opendir(path.c_str());
    if (!dir) {
      Serial.println("devtools: can't open that folder");
      return;
    }
    while (const dirent *e = readdir(dir)) {
      const String full = path + "/" + e->d_name;
      struct stat st;
      const bool isDir = e->d_type == DT_DIR;
      const long size = !isDir && stat(full.c_str(), &st) == 0 ? static_cast<long>(st.st_size) : 0;
      Serial.printf("%10ld  %s%s\n", size, e->d_name, isDir ? "/" : "");
    }
    closedir(dir);
    Serial.println("devtools: end of listing");
    return;
  }
  if (line.startsWith("flow ")) {
    // The start of chapter N as the Reader gets it: ¶ paragraph, [IMG n] picture, ^ centred,
    // [PAGE] page break; styles dropped, typographic slots as '~'.
    const String rest = line.substring(5);
    const int bar = rest.indexOf('|');
    const String path = cardPath(bar < 0 ? rest : rest.substring(0, bar));
    if (path.isEmpty()) return;
    Epub book;
    std::string err, flowText;
    std::vector<Epub::Image> images;
    const size_t chapter = bar < 0 ? 0 : rest.substring(bar + 1).toInt();
    if (!book.open(path.c_str(), err) || !book.chapterFlow(chapter, flowText, images, err)) {
      Serial.printf("devtools: %s\n", err.c_str());
      return;
    }
    std::string shown;
    for (size_t i = 0; i < flowText.size() && i < 1200; ++i) {
      const char c = flowText[i];
      if (c == flow::kPara) {
        shown += "\xC2\xB6\n";
      } else if (c == flow::kImage && i + 1 < flowText.size()) {
        shown += "[IMG " + std::to_string(static_cast<uint8_t>(flowText[++i]) - flow::kImageBase) + "]";
      } else if (c == flow::kCenter) {
        shown += '^';
      } else if (c == flow::kPageBreak) {
        shown += "[PAGE]";
      } else if (c == flow::kSoftHyphen) {
        shown += "\xC2\xB7";  // a middle dot marks a soft hyphen
      } else if (!flow::isStyle(c)) {
        shown += static_cast<uint8_t>(c) < 0x80 ? c : '~';
      }
    }
    const auto softHyphens = std::count(flowText.begin(), flowText.end(), flow::kSoftHyphen);
    Serial.printf("%s\ndevtools: chapter %u of %u, %u bytes, %u pictures, %u soft hyphens\n",
                  shown.c_str(), static_cast<unsigned>(chapter),
                  static_cast<unsigned>(book.chapterCount()),
                  static_cast<unsigned>(flowText.size()), static_cast<unsigned>(images.size()),
                  static_cast<unsigned>(softHyphens));
    return;
  }
  if (line.startsWith("unzip ")) {
    const String rest = line.substring(6);
    const int bar = rest.indexOf('|');
    const String path = cardPath(bar < 0 ? rest : rest.substring(0, bar));
    if (path.isEmpty()) return;
    ZipFile zip;
    std::string err;
    if (!zip.open(path.c_str(), err)) {
      Serial.printf("devtools: %s\n", err.c_str());
      return;
    }
    if (bar < 0) {
      for (const ZipFile::Entry &e : zip.entries()) {
        Serial.printf("%8u  %s\n", static_cast<unsigned>(e.size), e.name.c_str());
      }
    } else {
      std::string data;
      if (!zip.read(rest.substring(bar + 1).c_str(), data, err)) {
        Serial.printf("devtools: %s\n", err.c_str());
        return;
      }
      Serial.write(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    }
    Serial.println("\ndevtools: end of file");
    return;
  }
  if (line.startsWith("rm ")) {
    const String path = cardPath(line.substring(3));
    if (path.isEmpty()) return;
    Serial.println(remove(path.c_str()) == 0 ? "devtools: removed" : "devtools: can't remove");
    return;
  }
  if (line.startsWith("newsfeed ")) {
    const String url = line.substring(9);
    mkdir(news::kRoot, 0777);
    FILE *f = fopen(news::kFeedsFile, "ab");
    if (!f) {
      Serial.println("devtools: can't open the news feeds file");
      return;
    }
    fprintf(f, "%s\n", url.c_str());
    fclose(f);
    news::load(false);
    Serial.printf("devtools: added a news feed (%u characters)\n", url.length());
    return;
  }
  if (line == "newssync") {
    news::sync();
    Serial.println("devtools: syncing the news feeds");
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
  bool bootHeld = false;  // 'B' holds BOOT (talk); the next 'B' lets it go
  for (;;) {
    while (Serial.available() > 0) {
      switch (Serial.read()) {
        case 'u': input::inject(Key::Up); break;
        case 'd': input::inject(Key::Down); break;
        case 'U': input::inject(Key::Up, true); break;
        case 'D': input::inject(Key::Down, true); break;
        case 'p': input::inject(Key::Select); break;
        case 'P': input::inject(Key::SelectLong); break;
        case 'b': input::inject(Key::Back); break;
        case 'B':
          bootHeld = !bootHeld;
          input::inject(bootHeld ? Key::BackLong : Key::BackLongUp);
          break;
        case 's': screen::dump(Serial); break;
        case '-': audio::changeVolume(-5); break;  // e.g. silence the speaker before a test
        case '+': audio::changeVolume(+5); break;
        case 'r': pmic::dump(Serial); break;
        case 'c': rtc::forget(); break;
        case 'o': input::inject(Key::Power); break;
        case 'z': apps::requestStandby(2); break;
        case 'k': apps::requestCloseBook(); break;
        case 'a': apps::requestAgentFace(); break;
        case 'e': apps::requestEdition(); break;
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
