#include "devtools.h"

#include <Arduino.h>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <vector>

#include "apps/agent_face.h"
#include "apps/news_edition.h"
#include "apps/idle.h"
#include "audio.h"
#include "board_pins.h"
#include "comms.h"
#include "epub.h"
#include "es8311.h"
#include "features.h"
#include "flashfs.h"
#include "forecast.h"
#include "input.h"
#include "news_library.h"
#include "pmic.h"
#include "power.h"
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
  if (line.startsWith("commsreach")) {
    // "!commsreach N|on|off|-": the seconds between a sleeping board's polls in place of the
    // card's mesh_reachable line (the card is the real way), kept in NVS; "-" removes it.
    String rest = line.substring(10);
    rest.trim();
    if (rest == "-") {
      comms::setTestReach(-1);
    } else if (rest == "off") {
      comms::setTestReach(0);
    } else if (rest == "on") {
      comms::setTestReach(5);
    } else if (!rest.isEmpty()) {
      const int n = rest.toInt();
      if (n < 2 || n > 30) {
        Serial.println("commsreach: 2 to 30 (seconds), on, off, or - (the card's mesh_reachable)");
        return;
      }
      comms::setTestReach(n);
    }
    const int s = comms::reachInterval();
    Serial.printf("commsreach: %s%s\n", s ? ("every " + String(s) + " s").c_str() : "off",
                  comms::testReach() ? " (dev override; !commsreach - removes it)"
                                     : " (the card's mesh_reachable line)");
    return;
  }
  if (line.startsWith("reachtest")) {
    // "!reachtest pretend|sleep M|off": on USB, behave as on battery and, when the board would
    // power off, sleep reachable with a delay for the light sleep (pretend), or with real light
    // sleep for M minutes (sleep; the console goes meanwhile), then come on as after PWR.
    String rest = line.substring(9);
    rest.trim();
    if (rest == "pretend") {
      power::setReachTest(power::ReachTest::Pretend);
    } else if (rest.startsWith("sleep")) {
      const int m = rest.substring(5).toInt();
      power::setReachTest(power::ReachTest::Sleep, m > 0 ? m : 2);
    } else if (rest == "off") {
      power::setReachTest(power::ReachTest::Off);
    } else if (!rest.isEmpty()) {
      Serial.println("reachtest: pretend, sleep M (minutes), or off");
      return;
    }
    const power::ReachTest t = power::reachTest();
    Serial.printf("reachtest: %s\n", t == power::ReachTest::Pretend ? "pretend (on battery, the sleep a delay)"
                                     : t == power::ReachTest::Sleep  ? "sleep (on battery, real light sleep)"
                                                                     : "off");
    return;
  }
  if (line.startsWith("reachchan")) {
    // "!reachchan N [MISSES]": the next sleep polls on channel N, its channel check starting with
    // MISSES unanswered echoes (2: the next unanswered one sweeps).
    int ch = 0, misses = 0;
    siscanf(line.c_str(), "reachchan %d %d", &ch, &misses);
    comms::setReachChannel(ch, misses);
    Serial.printf("reachchan: the next sleep polls on channel %d, %d misses so far\n", ch, misses);
    return;
  }
  if (line == "reachlog" || line == "reachlog reset") {
    if (line.endsWith("reset")) comms::reachLogReset();
    comms::dumpReach(Serial);
    const pmic::Status p = pmic::status();
    Serial.printf("  battery now %d %%%s\n", p.percent, p.vbus ? ", on USB" : "");
    return;
  }
  if (line.startsWith("reachbench")) {
    // "!reachbench MODE N SLEEPMS LISTENMS" (comms.h), or "!reachbench" for the last report.
    char mode = 0;
    int n = 0;
    unsigned sleepMs = 0, listenMs = 0;
    if (siscanf(line.c_str(), "reachbench %c %d %u %u", &mode, &n, &sleepMs, &listenMs) == 4) {
      comms::reachBench(mode, n, sleepMs, listenMs, Serial);
    } else {
      comms::reachBenchShow(Serial);
    }
    return;
  }
  if (line.startsWith("commsmicmode")) {
    // How the mic is keyed, in place of the card's mesh_mic line (the card is the real way):
    // kept in NVS. "!commsmicmode -" removes it.
    String rest = line.substring(12);
    rest.trim();
    if (rest == "sticky" || rest == "push") {
      comms::setTestMicMode(true, rest == "push" ? comms::MicMode::Push : comms::MicMode::Sticky);
    } else if (rest == "-") {
      comms::setTestMicMode(false);
    } else if (!rest.isEmpty()) {
      Serial.println("commsmicmode: sticky, push, or - (the card's mesh_mic line again)");
      return;
    }
    Serial.printf("commsmicmode: %s%s\n",
                  comms::micMode() == comms::MicMode::Push ? "push" : "sticky",
                  comms::testMicMode() ? " (dev override; !commsmicmode - removes it)"
                                       : " (the card's mesh_mic line)");
    return;
  }
  if (line.startsWith("commsrange")) {
    // The rate this board sends at, in place of the card's mesh_range line (the card is the real
    // way): kept in NVS. "!commsrange -" removes it.
    String rest = line.substring(10);
    rest.trim();
    if (rest == "normal" || rest == "long") {
      comms::setTestRange(true, rest == "long" ? comms::Range::Long : comms::Range::Normal);
    } else if (rest == "-") {
      comms::setTestRange(false);
    } else if (!rest.isEmpty()) {
      Serial.println("commsrange: normal, long, or - (the card's mesh_range line again)");
      return;
    }
    Serial.printf("commsrange: %s%s\n",
                  comms::range() == comms::Range::Long ? "long (500 kbps)" : "normal (1 Mbps)",
                  comms::testRange() ? " (dev override; !commsrange - removes it)"
                                     : " (the card's mesh_range line)");
    return;
  }
  if (line.startsWith("commsdeaf")) {
    // "!commsdeaf ID": what this board's radio gets from that board is dropped, as if out of range
    // ("!commsdeaf off": none).
    String rest = line.substring(9);
    rest.trim();
    if (!rest.isEmpty()) comms::setDeaf(rest == "off" ? 0 : strtoul(rest.c_str(), nullptr, 16));
    if (comms::deaf()) {
      Serial.printf("commsdeaf: deaf to %08lX (!commsdeaf off ends it)\n",
                    static_cast<unsigned long>(comms::deaf()));
    } else {
      Serial.println("commsdeaf: deaf to nobody (!commsdeaf ID: as if that board were out of range)");
    }
    return;
  }
  if (line == "commsflood on" || line == "commsflood off") {
    comms::setFlood(line.endsWith("on"));
    Serial.printf("commsflood: %s\n", comms::flood()
                                          ? "every voice and message frame relayed, as before the rule"
                                          : "voice and message frames relayed only when needed");
    return;
  }
  if (line.startsWith("commsloss")) {
    // "!commsloss S": for S s (10), each board heard straight: its packets that came and that
    // didn't (gaps in its sequence numbers), and the gaps by length.
    const int sec = std::max(1, std::min(120, line.length() > 9 ? static_cast<int>(line.substring(9).toInt()) : 10));
    const std::vector<comms::LinkStat> a = comms::linkStats();
    delay(sec * 1000);
    const std::vector<comms::LinkStat> b = comms::linkStats();
    for (const comms::LinkStat &y : b) {
      comms::LinkStat x;
      for (const comms::LinkStat &z : a) {
        if (z.id == y.id && z.got <= y.got) x = z;
      }
      const uint32_t got = y.got - x.got, missed = y.missed >= x.missed ? y.missed - x.missed : 0;
      const uint32_t all = got + missed;
      Serial.printf("commsloss: %08lX in %d s: %lu came, %lu didn't: %lu.%lu%% lost (gaps 1: %lu, "
                    "2-3: %lu, 4-15: %lu, 16+: %lu), %d dBm\n",
                    static_cast<unsigned long>(y.id), sec, static_cast<unsigned long>(got),
                    static_cast<unsigned long>(missed),
                    static_cast<unsigned long>(all ? missed * 100 / all : 0),
                    static_cast<unsigned long>(all ? missed * 1000 / all % 10 : 0),
                    static_cast<unsigned long>(y.runs[0] - x.runs[0]),
                    static_cast<unsigned long>(y.runs[1] - x.runs[1]),
                    static_cast<unsigned long>(y.runs[2] - x.runs[2]),
                    static_cast<unsigned long>(y.runs[3] - x.runs[3]), y.rssi);
    }
    if (b.empty()) Serial.println("commsloss: no board heard straight");
    return;
  }
  if (line == "commsair" || line.startsWith("commsair ")) {
    // "!commsair [S]": the air survey, S seconds (5): how busy the channel is, by kind of frame.
    const int sec = line.length() > 8 ? line.substring(8).toInt() : 5;
    const comms::AirSurvey a = comms::airSurvey(static_cast<uint32_t>(sec > 0 ? sec : 5) * 1000);
    if (!a.ok) {
      Serial.printf("commsair: %s\n", a.error.c_str());
      return;
    }
    static const char *const kKinds[] = {"ESP-NOW", "data", "beacons", "other management",
                                         "control", "no payload (2-stream...)"};
    Serial.printf("commsair: channel %d, %lu ms: frames on the air %d.%d%% of the time:", a.channel,
                  static_cast<unsigned long>(a.ms), static_cast<int>(a.total * 100),
                  static_cast<int>(a.total * 1000) % 10);
    for (int k = 0; k < comms::AirSurvey::kKinds; ++k) {
      Serial.printf(" %s %d.%d%% (%lu),", kKinds[k], static_cast<int>(a.share[k] * 100),
                    static_cast<int>(a.share[k] * 1000) % 10,
                    static_cast<unsigned long>(a.frames[k]));
    }
    Serial.printf(" the strongest data frame %d dBm\n", a.loudestData);
    return;
  }
  if (line == "commslrrx on" || line == "commslrrx off") {
    // A test: the radio without the long-range protocol (off) hears no long-range packets.
    comms::longRangeRx(line.endsWith("on"));
    Serial.printf("commslrrx: the long-range protocol %s until a restart (a join follows)\n",
                  line.endsWith("on") ? "on" : "OFF");
    return;
  }
  if (line == "commsrelay on" || line == "commsrelay off") {
    comms::setRelayForOthers(line.endsWith("on"));
    Serial.printf("commsrelay: relay for others %s\n", comms::relayForOthers() ? "on" : "off");
    return;
  }
  if (line == "commsplay") {
    // PLAY LAST: the last 60 s heard, again.
    if (!comms::playLast()) {
      Serial.printf("commsplay: %s\n", comms::status().error.c_str());
    } else {
      const comms::LastCall c = comms::lastCall();
      Serial.printf("commsplay: playing the last %lu ms heard again (the last from %s)\n",
                    static_cast<unsigned long>(c.ms), c.fromName.c_str());
    }
    return;
  }
  if (line == "commsresend" || line == "commsresend old") {
    comms::resend(line.endsWith("old"));
    Serial.println("commsresend: sent; look at the other board's dropped counters (!comms)");
    return;
  }
  if (line == "commsring" || line.startsWith("commsring ")) {
    // Rings all of the active group, or one board of it by its id (hex, as !comms lists it).
    String rest = line.substring(9);
    rest.trim();
    const uint32_t target = rest.isEmpty() ? 0 : strtoul(rest.c_str(), nullptr, 16);
    const int group = comms::activeGroup();
    if (!comms::ringStart(group, target)) {
      Serial.printf("commsring: %s\n", comms::status().error.c_str());
    } else {
      Serial.printf("commsring: ringing %s in group %d (60 s at most; !comms shows it, "
                    "!commsringstop gives up)\n",
                    target ? rest.c_str() : "all", group);
    }
    return;
  }
  if (line == "commsringstop") {
    comms::ringStop();
    Serial.println("commsringstop: given up (or a finished ring cleared)");
    return;
  }
  if (line == "commsringvanish") {
    // Testing: the ring going stops sending ring packets, with no cancel, as if this board had
    // gone out of range; the boards it rang stop after 5 s.
    comms::ringVanish();
    Serial.println("commsringvanish: ring packets stop now, no cancel");
    return;
  }
  if (line == "commsanswer") {
    Serial.println(comms::answer() ? "commsanswer: answered" : "commsanswer: nothing rings");
    return;
  }
  if (line == "commsignore") {
    comms::ignoreRing();
    Serial.println("commsignore: it stops ringing here");
    return;
  }
  if (line == "commsjoin" || line == "commsleave") {
    const int group = comms::activeGroup();
    if (line == "commsleave") {
      comms::leave(group);
      Serial.printf("commsleave: out of group %d's conversation (its voice won't play here)\n",
                    group);
    } else if (!comms::join(group)) {
      Serial.printf("commsjoin: %s\n", comms::status().error.c_str());
    } else {
      Serial.printf("commsjoin: in group %d's conversation\n", group);
    }
    return;
  }
  if (line.startsWith("commsdropmsg")) {
    // Testing: every nth message frame that comes is dropped as if lost (0: none).
    const int n = std::max(0, static_cast<int>(line.substring(12).toInt()));
    comms::dropMessageFrames(n);
    Serial.printf("commsdropmsg: %s\n",
                  n ? ("every " + String(n) + "th message frame that comes is dropped").c_str()
                    : "none dropped");
    return;
  }
  if (line == "commsplaymsg" || line.startsWith("commsplaymsg ")) {
    // Plays a message kept: the newest, or #id as !comms lists them.
    const std::vector<comms::Message> ms = comms::messages(-1);
    String rest = line.substring(12);
    rest.trim();
    rest.replace("#", "");
    uint32_t id = rest.isEmpty() ? 0 : static_cast<uint32_t>(rest.toInt());
    if (!id) {
      for (const comms::Message &m : ms) {
        if (!m.receiving) {
          id = m.id;
          break;
        }
      }
    }
    if (!id || !comms::playMessage(id)) {
      Serial.printf("commsplaymsg: %s\n", id ? comms::status().error.c_str() : "no message kept");
    } else {
      for (const comms::Message &m : ms) {
        if (m.id != id) continue;
        Serial.printf("commsplaymsg: playing #%lu from %s, %lu ms, %s\n",
                      static_cast<unsigned long>(id), m.fromName.c_str(),
                      static_cast<unsigned long>(m.ms), m.complete ? "complete" : "INCOMPLETE");
      }
    }
    return;
  }
  if (line.startsWith("commsmsg")) {
    // Records N seconds and sends them as a message: for the ring going unanswered (5 s in, or
    // after NO ANSWER), else (testing) to all of the active group directly.
    const int seconds = std::max(1, std::min(60, static_cast<int>(line.substring(8).toInt())));
    comms::hold();
    for (int i = 0; i < 60 && !comms::listening(); ++i) delay(50);
    const comms::Dial d = comms::dial();
    const bool forRing = d.canLeaveMessage;
    if (!(forRing ? comms::messageStart() : comms::messageStart(comms::activeGroup(), 0))) {
      Serial.printf("commsmsg: %s\n", comms::status().error.c_str());
      comms::release();
      return;
    }
    // The mic opens in the comms task, after the cue: wait for it.
    for (int i = 0; i < 40 && comms::dial().state != comms::DialState::Recording; ++i) delay(50);
    if (comms::dial().state != comms::DialState::Recording) {
      Serial.printf("commsmsg: %s\n", comms::status().error.c_str());
      comms::release();
      return;
    }
    Serial.printf("commsmsg: recording %d s %s\n", seconds,
                  forRing ? "for the ring (it has stopped on the others)"
                          : "for all of the active group (no ring)");
    delay(seconds * 1000);
    comms::messageSend();
    delay(300);
    const comms::Dial after = comms::dial();
    Serial.printf("commsmsg: sent, %lu ms; !comms shows its checksum and the boards that have it "
                  "whole\n",
                  static_cast<unsigned long>(after.messageMs));
    comms::release();
    return;
  }
  // The mic (comms.h): this board's open mic, and the others' heard here.
  if (line.startsWith("commsbusy")) {
    // "!commsbusy alarm|rec|stop": something else takes the audio, as the Clock's alarm or the
    // Recorder would (for what the mic does meanwhile); stop: back to nothing.
    String rest = line.substring(9);
    rest.trim();
    if (rest == "alarm") {
      audio::ring(true);
    } else if (rest == "rec") {
      audio::record("/sdcard/commsbusy.wav");
    } else if (rest == "stop") {
      audio::ring(false);
      audio::stopRecording();
    } else {
      Serial.println("commsbusy: alarm, rec or stop");
      return;
    }
    Serial.printf("commsbusy: %s\n", rest.c_str());
    return;
  }
  if (line == "commsmicoff") {
    comms::micOff();
    Serial.println("commsmicoff: the mic goes off (its off goes out)");
    return;
  }
  if (line.startsWith("commsmicthr")) {
    String rest = line.substring(11);
    rest.trim();
    if (rest.isEmpty()) {
      Serial.printf("commsmicthr: %d dBFS (!commsmicthr -36 sets it, in 3 dB steps, -60 to 0)\n",
                    comms::micThreshold());
      return;
    }
    comms::setMicThreshold(rest.toInt());
    delay(50);
    Serial.printf("commsmicthr: %d dBFS\n", comms::micThreshold());
    return;
  }
  if (line.startsWith("commstone")) {
    // "!commstone S DB": a 1 kHz tone peaking at DB dBFS added to the mic's input for S s.
    String rest = line.substring(9);
    rest.trim();
    const int space = rest.indexOf(' ');
    const int seconds = std::max(1, std::min(600, static_cast<int>(rest.toInt())));
    const int db = space < 0 ? -20 : static_cast<int>(rest.substring(space + 1).toInt());
    if (!comms::mic().on) {
      Serial.println("commstone: the mic is off (!commsmic first)");
      return;
    }
    comms::micTone(seconds * 1000u, db);
    Serial.printf("commstone: a 1 kHz tone at %d dBFS for %d s added to the mic "
                  "(threshold %d dBFS)\n",
                  std::min(0, db), seconds, comms::mic().thresholdDb);
    return;
  }
  if (line == "commsvanish" || line == "commsvanish off") {
    const bool on = line == "commsvanish";
    comms::micVanish(on);
    Serial.printf("commsvanish: %s\n", on ? "the mic sends nothing from now, its off neither "
                                            "(the boards that hear it see LOST after 20 s)"
                                          : "the mic is back on air");
    return;
  }
  if (line == "commsack") {
    comms::ackLost();
    Serial.println("commsack: LOST's alarm silenced (if it sounded)");
    return;
  }
  if (line.startsWith("commslong")) {
    // "!commslong S": a mic heard counts as on long after S s, not 5 minutes (0: 5 minutes).
    const int seconds = std::max(0, static_cast<int>(line.substring(9).toInt()));
    comms::setLongMicMs(seconds * 1000u);
    Serial.printf("commslong: a mic on longer than %d s chirps once where it's heard as it goes "
                  "off\n",
                  seconds ? seconds : static_cast<int>(comms::kLongMicMs / 1000));
    return;
  }
  if (line == "commsecho" || line.startsWith("commsecho ")) {
    // "!commsecho [DB [MS]]": the loopback test: a 1 kHz tone at DB dBFS (-20) for MS ms (1000)
    // through the duplex path, and what the mic hears of it.
    String rest = line.substring(9);
    rest.trim();
    const int space = rest.indexOf(' ');
    const int db = rest.isEmpty() ? -20 : static_cast<int>(rest.toInt());
    const int ms = space < 0 ? 1000 : static_cast<int>(rest.substring(space + 1).toInt());
    comms::hold();
    for (int i = 0; i < 60 && !comms::listening(); ++i) delay(50);
    const comms::EchoTest e = comms::echoTest(static_cast<float>(std::min(0, db)), ms);
    comms::release();
    if (!e.ok) {
      Serial.printf("commsecho: %s\n", e.error.c_str());
      return;
    }
    Serial.printf("commsecho: VOL %d, played %d dBFS (DAC out), the mic heard %d dBFS (quiet %d): "
                  "coupling %+d dB, delay %d ms (the delay used from now on, until a restart)\n",
                  e.volume, static_cast<int>(lroundf(e.playedDb)), static_cast<int>(lroundf(e.micDb)),
                  static_cast<int>(lroundf(e.noiseDb)), static_cast<int>(lroundf(e.couplingDb)),
                  e.delayMs);
    return;
  }
  if (line == "commsmic" || line.startsWith("commsmic ")) {
    // "!commsmic [all|ID] [DB]": this board's mic on in the active group, to all of it or one
    // board (hex id, as !comms lists it), at DB dBFS (else the threshold kept).
    String rest = line.substring(8);
    rest.trim();
    uint32_t target = 0;
    while (!rest.isEmpty()) {
      const int space = rest.indexOf(' ');
      String word = space < 0 ? rest : rest.substring(0, space);
      rest = space < 0 ? String() : rest.substring(space + 1);
      rest.trim();
      if (word.startsWith("-") || word == "0") {
        comms::setMicThreshold(word.toInt());
      } else if (word != "all") {
        target = strtoul(word.c_str(), nullptr, 16);
      }
    }
    if (!comms::configured()) {
      Serial.println("comms: no group (a mesh_group line, an agent_token or !commsgroup)");
      return;
    }
    if (!comms::micOn(comms::activeGroup(), target)) {
      Serial.printf("commsmic: %s\n", comms::status().error.c_str());
      return;
    }
    delay(600);
    const comms::Mic m = comms::mic();
    Serial.printf("commsmic: the mic is %s in group %d to %s at %d dBFS (!comms shows it, "
                  "!commsmicoff switches it off)\n",
                  m.capturing ? "on" : m.paused.empty() ? "starting" : m.paused.c_str(),
                  comms::activeGroup(), target ? "one board" : "all", m.thresholdDb);
    if (!m.on && !comms::status().error.empty()) {
      Serial.printf("commsmic: %s\n", comms::status().error.c_str());
    }
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
      // "!commstalk N [board-id]": the mic on for N s to all of the group, or to one board (hex,
      // as !comms lists it), then off.
      String rest = line.substring(9);
      rest.trim();
      const int space = rest.indexOf(' ');
      const int seconds = std::max(1, std::min(600, static_cast<int>(rest.toInt())));
      const uint32_t board =
          space < 0 ? 0 : strtoul(rest.substring(space + 1).c_str(), nullptr, 16);
      if (!comms::micOn(group, board)) {
        Serial.printf("commstalk: %s\n", comms::status().error.c_str());
      } else {
        Serial.printf("commstalk: the mic on in group %d for %d s\n", group, seconds);
        delay(seconds * 1000);
        const comms::Mic m = comms::mic();
        comms::micOff();
        Serial.printf("commstalk: off; %lu sounds, %lu frames sent (%lu as echo)\n",
                      static_cast<unsigned long>(m.sounds), static_cast<unsigned long>(m.framesSent),
                      static_cast<unsigned long>(m.framesMuted));
      }
    }
    comms::release();
    return;
  }
#endif
  if (line == "quiet" || line == "quiet off") {
    // Testing only: 30 dB less at the DAC for every sound, minimum-volume ones included.
    const bool on = line == "quiet";
    es8311::setQuiet(on);
    audio::changeVolume(0);  // set again now if the codec is on
    Serial.printf("quiet: %s\n", on ? "on, 30 dB less for every sound until !quiet off or a reset"
                                     : "off");
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

// 6 KB: at 4 KB, `!comms` with the open mics' state and `!commstest` came within about 220 bytes
// of the end (2026-11-23).
void begin() { xTaskCreatePinnedToCore(task, "devtools", 6144, nullptr, 1, nullptr, 1); }

}  // namespace devtools
