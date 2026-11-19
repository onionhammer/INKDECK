#include "apps/ble_devices.h"
#include "features.h"

#if INKDECK_BLE

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <array>

#include "apps/home.h"
#include "apps/radio_ui.h"
#include "ble_adv.h"
#include "ble_survey.h"

namespace apps {
namespace {

using blesurvey::Device;
using ui::Align;
using ui::Key;
using ui::Rect;
using ui::TextStyle;

constexpr TextStyle kStripLabel{&ui::fonts::kMono13ExtraBold, 0};
constexpr TextStyle kScale{&ui::fonts::kMono13Medium, 0};
constexpr TextStyle kRawType{&ui::fonts::kMono13ExtraBold, 0};
constexpr TextStyle kRawHex{&ui::fonts::kMono13Medium, 0};

constexpr int kStripH = 169;  // section, header included
constexpr uint32_t kRefreshMs = 5000;
constexpr uint32_t kResortMs = 15000;
constexpr int kRawLineH = 22;

struct Key6 {
  std::array<uint8_t, 6> addr{};
  uint8_t type = 0;
  bool operator==(const Key6 &o) const { return type == o.type && addr == o.addr; }
};
Key6 keyOf(const Device &d) {
  Key6 k;
  memcpy(k.addr.data(), d.addr, 6);
  k.type = d.addrType;
  return k;
}

struct Browse {
  std::vector<Key6> order;
  int index = 0;
};

enum class Sort : uint8_t { Signal, Kind, Name };

const char *sortName(Sort s) {
  switch (s) {
    case Sort::Signal: return "BY SIGNAL";
    case Sort::Kind: return "BY KIND";
    case Sort::Name: return "BY NAME";
  }
  return "";
}

std::string twoOf(int i, int n) {
  char buf[16];
  snprintf(buf, sizeof buf, "%02d/%02d", i, n);
  return buf;
}

std::string lower(const std::string &s) {
  std::string out = s;
  for (char &ch : out) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
  return out;
}

bleadv::Info infoOf(const Device &d) { return bleadv::decode(d.adv, d.advLen, d.rsp, d.rspLen); }

// A device's row title: its name, else its address.
std::string titleOf(const Device &d, const bleadv::Info &info) {
  return info.name.empty() ? blesurvey::addressText(d) : info.name;
}

std::string subtitleOf(const Device &d, const bleadv::Info &info) {
  const char *dot = " \xC2\xB7 ";
  std::string s = blesurvey::addressKind(d);
  if (d.connectable) s += std::string(dot) + "CONN";
  if (!info.what.empty()) {
    s += dot + info.what;
  } else if (d.advLen == 0 && d.rspLen == 0) {
    s += std::string(dot) + "NO DATA";
  }
  return s;
}

void sortDevices(std::vector<Device> &v, Sort s) {
  switch (s) {
    case Sort::Signal:
      std::stable_sort(v.begin(), v.end(),
                       [](const Device &a, const Device &b) { return a.rssi > b.rssi; });
      break;
    case Sort::Kind: {
      std::vector<std::pair<std::string, Device>> keyed;
      for (const Device &d : v) {
        std::string k = infoOf(d).kind;
        if (k.empty()) k = "\x7F";  // no kind: last
        keyed.emplace_back(k, d);
      }
      std::stable_sort(keyed.begin(), keyed.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first < b.first : a.second.rssi > b.second.rssi;
      });
      for (size_t i = 0; i < v.size(); ++i) v[i] = keyed[i].second;
      break;
    }
    case Sort::Name: {
      std::vector<std::pair<std::string, Device>> keyed;
      for (const Device &d : v) {
        const std::string name = infoOf(d).name;
        keyed.emplace_back(name.empty() ? "\x7F" + blesurvey::addressText(d) : lower(name), d);
      }
      std::stable_sort(keyed.begin(), keyed.end(),
                       [](const auto &a, const auto &b) { return a.first < b.first; });
      for (size_t i = 0; i < v.size(); ++i) v[i] = keyed[i].second;
      break;
    }
  }
}

// ---------------------------------------------------------------------------------------------

class BleDetail : public ui::Screen {
 public:
  BleDetail(std::shared_ptr<Browse> browse, const Device &d) : browse_(std::move(browse)) {
    show(d);
  }

  std::string tag() const override { return moduleTag("SENSORS"); }
  std::string context() const override {
    return "BLE \xC2\xB7 " +
           twoOf(browse_->index + 1, static_cast<int>(browse_->order.size()));
  }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "DEVICE"};
    out[1] = {ui::glyph::kPress, tone_ ? "TONE OFF" : "TONE"};
    out[2] = {"BOOT", "BACK"};
    return 3;
  }

  void onEnter() override { blesurvey::begin(); }
  void onLeave() override { blesurvey::end(); }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up:
      case Key::Down: return step(key == Key::Up ? -1 : 1);
      case Key::Select: tone_ = !tone_; return true;
      default: return false;
    }
  }

  bool onTick() override {
    Device now;
    if (blesurvey::device(dev_.addr, dev_.addrType, now)) {
      if (now.advLen != dev_.advLen || now.rspLen != dev_.rspLen ||
          memcmp(now.adv, dev_.adv, now.advLen) != 0 || memcmp(now.rsp, dev_.rsp, now.rspLen) != 0) {
        info_ = infoOf(now);
      }
      dev_ = now;
    }
    const uint32_t t = millis();
    bool changed = false;
    if (t - sampledAt_ >= radio::kSampleMs) {
      sampledAt_ = t;
      history_.push(heardWithin(3000) ? dev_.rssi : radio::History::kNone);
      changed = true;
    }
    if (tone_ && heardWithin(5000)) radio::beep(dev_.rssi);
    const std::string lost = lostText();
    const std::string seen = seenText();
    changed |= lost != lost_ || seen != seen_;
    lost_ = lost;
    seen_ = seen;
    return changed;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    Rect r = body;
    std::string name = info_.name;
    if (name.empty()) name = info_.what.empty() ? "(no name)" : info_.what;
    radio::drawIdentity(c, r.takeTop(radio::kIdentityH), "DEVICE",
                        dev_.connectable ? "CONNECTABLE" : "BROADCAST", name,
                        blesurvey::addressText(dev_), addressTag());

    const bool have = heardWithin(10000);
    const Rect signal = radio::takeSection(c, r, kRule + kSectionH + radio::kSignalH, "SIGNAL",
                                           have ? "LIVE \xC2\xB7 EVERY ADVERT" : lost_);
    std::string top, bottom;
    if (info_.txPower != 127) {
      top = "TX " + std::string(info_.txPower >= 0 ? "+" : "-") + std::to_string(abs(info_.txPower)) +
            " dBm";
      if (have) bottom = "PATH LOSS " + std::to_string(info_.txPower - dev_.rssi) + " dB";
    }
    radio::drawSignal(c, signal, have, dev_.rssi, top, bottom, history_);

    std::string every;
    if (dev_.gapMs > 0) every = "EVERY ~" + std::to_string(dev_.gapMs) + " ms";
    Rect details = radio::takeSection(c, r, kRule + kSectionH + 202, "ADVERTISING", every);
    details = details.inset(kPadX, 0);
    details.takeTop(4);
    for (const auto &kv : detailRows()) {
      if (details.h < radio::kKeyValueH) break;
      radio::drawKeyValue(c, details.takeTop(radio::kKeyValueH), kv.first, kv.second);
    }

    drawRaw(c, r);
  }

 private:
  void show(const Device &d) {
    dev_ = d;
    info_ = infoOf(d);
    history_.clear();
    history_.push(d.rssi);
    sampledAt_ = millis();
    lost_ = lostText();
    seen_ = seenText();
  }

  bool step(int delta) {
    const int count = static_cast<int>(browse_->order.size());
    const int next = browse_->index + delta;
    if (next < 0 || next >= count) return false;
    const Key6 &k = browse_->order[next];
    Device d;
    if (!blesurvey::device(k.addr.data(), k.type, d)) return false;  // gone since; stay
    browse_->index = next;
    show(d);
    return true;
  }

  bool heardWithin(uint32_t ms) const { return dev_.heardMs != 0 && millis() - dev_.heardMs < ms; }

  std::string lostText() const {
    if (heardWithin(10000) || dev_.heardMs == 0) return "";
    const uint32_t s = (millis() - dev_.heardMs) / 1000;
    if (s >= 120) return "NOT HEARD FOR " + std::to_string(s / 60) + " MIN";
    return "NOT HEARD FOR " + std::to_string(s / 5 * 5) + " S";
  }

  // "418 ADVERTS · FOR 4 MIN": the time in 10 s steps under a minute, so it doesn't redraw the
  // screen every second.
  std::string seenText() const {
    const uint32_t s = (dev_.heardMs - dev_.firstMs) / 1000;
    const std::string span =
        s >= 60 ? std::to_string(s / 60) + " MIN" : std::to_string(s / 10 * 10) + " S";
    return std::to_string(dev_.adverts) + (dev_.adverts == 1 ? " ADVERT" : " ADVERTS") +
           " \xC2\xB7 FOR " + span;
  }

  std::string addressTag() const {
    const std::string k = blesurvey::addressKind(dev_);
    if (k == "STATIC") return "RANDOM STATIC";
    if (k == "RPA") return "RANDOM PRIVATE";
    if (k == "NRPA") return "NON-RESOLVABLE";
    return k;
  }

  std::vector<std::pair<std::string, std::string>> detailRows() const {
    const bleadv::Info &i = info_;
    const char *dot = " \xC2\xB7 ";
    std::vector<std::pair<std::string, std::string>> rows;
    char hex[12];
    // KIND only when it adds something: not when it's the appearance, a service or the maker,
    // which have rows of their own.
    bool repeats = i.what == bleadv::appearanceName(i.appearance);
    for (const uint16_t u : i.services) {
      const char *n = bleadv::serviceName(u);
      repeats |= n && i.what == n;
    }
    const char *maker = bleadv::companyName(i.company);
    repeats |= maker && i.what == maker;
    if (!i.what.empty() && !repeats) rows.emplace_back("KIND", i.what);
    if (i.appearance > 0) {
      snprintf(hex, sizeof hex, "0x%04X", i.appearance);
      const std::string name = bleadv::appearanceName(i.appearance);
      rows.emplace_back("APPEARANCE", name.empty() ? hex : name + dot + hex);
    }
    if (!i.services.empty() || i.services128 > 0) {
      // Two lines at most: each service as "180D HEART RATE", the 128-bit ones counted.
      std::vector<std::string> parts;
      for (const uint16_t u : i.services) {
        snprintf(hex, sizeof hex, "%04X", u);
        const char *n = bleadv::serviceName(u);
        parts.push_back(n ? std::string(hex) + " " + n : hex);
      }
      if (i.services128 > 0) parts.push_back(std::to_string(i.services128) + " x 128-BIT");
      std::string line;
      int lines = 0;
      for (size_t p = 0; p < parts.size(); ++p) {
        const std::string next = line.empty() ? parts[p] : line + dot + parts[p];
        if (!line.empty() && next.size() > 30 && lines == 0) {
          rows.emplace_back("SERVICES", line);
          ++lines;
          line = parts[p];
        } else {
          line = next;
        }
      }
      if (!line.empty()) rows.emplace_back(lines == 0 ? "SERVICES" : "", line);
    }
    if (i.company >= 0) {
      snprintf(hex, sizeof hex, "0x%04X", i.company);
      const char *n = bleadv::companyName(i.company);
      rows.emplace_back("MAKER", n ? std::string(hex) + " " + n : hex);
    }
    if (i.flags >= 0) rows.emplace_back("FLAGS", bleadv::flagsText(i.flags));
    if (!i.name.empty()) {
      rows.emplace_back("NAME", std::string(i.nameComplete ? "COMPLETE" : "SHORTENED") + dot +
                                    (i.nameInResponse ? "IN SCAN RESPONSE" : "IN ADVERT"));
    }
    rows.emplace_back("SEEN", seen_);
    return rows;
  }

  // One AD structure a line: its label and its bytes (length and type included), the scan
  // response's after a 2 px rule.
  void drawRaw(ui::Canvas &c, Rect r) const {
    using namespace ui;
    std::string right = "ADV " + std::to_string(dev_.advLen) + " B";
    if (dev_.rspLen > 0) right += " \xC2\xB7 RSP " + std::to_string(dev_.rspLen) + " B";
    Rect body = radio::takeSection(c, r, r.h, "RAW", right);
    body = body.inset(kPadX, 0);
    body.takeTop(8);
    auto lines = [&](const uint8_t *payload, int len, bool ruleAfter) {
      const std::vector<bleadv::Field> fs = bleadv::fields(payload, len);
      for (size_t f = 0; f < fs.size(); ++f) {
        if (body.h < kRawLineH) return;
        const Rect line = body.takeTop(kRawLineH);
        std::string hex;
        char b[4];
        for (int k = 0; k < fs[f].len + 2; ++k) {
          snprintf(b, sizeof b, k ? " %02X" : "%02X", payload[fs[f].at + k]);
          hex += b;
        }
        drawText(c, {line.x, line.y, 64, line.h - 1}, bleadv::fieldLabel(fs[f].type), kRawType);
        drawText(c, {line.x + 64, line.y, line.w - 64, line.h - 1}, hex, kRawHex);
        if (ruleAfter && f + 1 == fs.size()) {
          fill(c, {line.x, line.bottom() - 2, line.w, 2}, kInk);
        } else {
          dashedH(c, line.x, line.bottom() - 1, line.w);
        }
      }
    };
    lines(dev_.adv, dev_.advLen, dev_.rspLen > 0);
    lines(dev_.rsp, dev_.rspLen, false);
    if (dev_.advLen == 0 && dev_.rspLen == 0) {
      drawText(c, body.takeTop(kRawLineH), "EMPTY ADVERT", style::kLabel);
    }
  }

  std::shared_ptr<Browse> browse_;
  Device dev_;
  bleadv::Info info_;
  radio::History history_;
  uint32_t sampledAt_ = 0;
  std::string lost_, seen_;
  bool tone_ = false;
};

// ---------------------------------------------------------------------------------------------

class BleList : public ui::Screen {
 public:
  BleList() : browse_(std::make_shared<Browse>()) {}

  std::string tag() const override { return moduleTag("SENSORS"); }
  std::string context() const override {
    if (devs_.empty() && !blesurvey::running()) return "BLE \xC2\xB7 STARTING\xE2\x80\xA6";
    return "BLE \xC2\xB7 " + std::to_string(devs_.size()) +
           (devs_.size() == 1 ? " DEVICE" : " DEVICES");
  }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "MOVE"};
    out[1] = {ui::glyph::kPress, "DETAIL"};
    out[2] = {ui::glyph::kHoldPress, "SORT"};
    out[3] = {"BOOT", "BACK"};
    return 4;
  }

  void onEnter() override {
    blesurvey::begin();
    if (!browse_->order.empty() && browse_->index < static_cast<int>(browse_->order.size())) {
      selected_ = browse_->order[browse_->index];
      haveSelected_ = true;
    }
    resort();
  }
  void onLeave() override { blesurvey::end(); }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up:
      case Key::Down:
        if (!cursor_.move(key == Key::Up ? -1 : 1)) return false;
        remember();
        return true;
      case Key::Select:
        if (devs_.empty()) return false;
        browse_->index = cursor_.cursor();
        ui::nav::push(std::make_unique<BleDetail>(browse_, devs_[cursor_.cursor()]));
        return true;
      case Key::SelectLong:
        sort_ = static_cast<Sort>((static_cast<int>(sort_) + 1) % 3);
        resort();
        return true;
      default: return false;
    }
  }

  bool onTick() override {
    const uint32_t now = millis();
    const bool wasRunning = running_;
    running_ = blesurvey::running();
    const std::string e = blesurvey::error();
    bool changed = running_ != wasRunning || e != error_;
    error_ = e;
    if (now - sortedAt_ >= kResortMs || (devs_.empty() && now - refreshedAt_ >= 1000)) {
      resort();
      return true;
    }
    if (now - refreshedAt_ >= kRefreshMs) changed |= refresh();
    return changed;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    Rect r = body;
    drawStrip(c, r.takeTop(kStripH));

    Rect list = r;
    hrule(c, list.x, list.y, list.w);
    list.takeTop(kRule);
    const Rect head = list.takeTop(kSectionH);
    if (devs_.empty() && !error_.empty()) {
      drawNotice(c, head, error_);
    } else {
      drawSectionHeader(c, head, "DEVICES",
                        devs_.empty() ? sortName(sort_)
                                      : std::string(sortName(sort_)) + " \xC2\xB7 " + cursor_.label());
    }
    if (devs_.empty()) {
      const char *msg = running_ ? "LISTENING\xE2\x80\xA6" : "STARTING BLUETOOTH\xE2\x80\xA6";
      drawText(c, {list.x, list.y, list.w, 80}, msg, style::kLabel, Align::Center);
      return;
    }
    const int perPage = list.h / kRowTallH;
    const int first = cursor_.pageStart(perPage);
    for (int i = first; i < static_cast<int>(devs_.size()) && i < first + perPage; ++i) {
      const Device &d = devs_[i];
      const bleadv::Info info = infoOf(d);
      radio::drawRow(c, {list.x, list.y + (i - first) * kRowTallH, list.w, kRowTallH},
                     titleOf(d, info), subtitleOf(d, info), info.kind, d.rssi,
                     i == cursor_.cursor());
    }
  }

 private:
  void remember() {
    if (devs_.empty()) return;
    selected_ = keyOf(devs_[cursor_.cursor()]);
    haveSelected_ = true;
  }

  void place() {
    int at = 0;
    if (haveSelected_) {
      for (size_t i = 0; i < devs_.size(); ++i) {
        if (keyOf(devs_[i]) == selected_) at = static_cast<int>(i);
      }
    }
    cursor_.reset(static_cast<int>(devs_.size()), at);
    browse_->order.clear();
    for (const Device &d : devs_) browse_->order.push_back(keyOf(d));
    remember();
  }

  // A new snapshot in a new order.
  void resort() {
    devs_ = blesurvey::devices();
    sortDevices(devs_, sort_);
    sortedAt_ = refreshedAt_ = millis();
    running_ = blesurvey::running();
    error_ = blesurvey::error();
    place();
  }

  // New signals and adverts in the same order; devices heard since go at the end, and ones gone
  // quiet stay until the next resort(). True if a row or the strip would look different.
  bool refresh() {
    refreshedAt_ = millis();
    const std::vector<Device> fresh = blesurvey::devices();
    bool changed = false;
    for (Device &d : devs_) {
      for (const Device &f : fresh) {
        if (!blesurvey::sameDevice(f, d.addr, d.addrType)) continue;
        changed |= f.rssi != d.rssi || f.advLen != d.advLen || f.rspLen != d.rspLen ||
                   f.connectable != d.connectable;
        d = f;
        break;
      }
    }
    for (const Device &f : fresh) {
      const bool known = std::any_of(devs_.begin(), devs_.end(), [&](const Device &d) {
        return blesurvey::sameDevice(d, f.addr, f.addrType);
      });
      if (!known) {
        devs_.push_back(f);
        changed = true;
      }
    }
    if (changed) place();
    return changed;
  }

  // Far to near, -100 to -30 dBm in 2 dB columns, each device an 8 px square stacked up its
  // column; the selected one ringed, with its name above on a dashed leader.
  void drawStrip(ui::Canvas &c, Rect r) const {
    using namespace ui;
    std::string right = devs_.empty() ? "" : "NEAREST " + radio::dbm(nearest()) + " dBm";
    drawSectionHeader(c, r.takeTop(kSectionH), "PROXIMITY", right);
    const Rect a{r.x + kPadX, r.y + 6, r.w - 2 * kPadX, 130};
    drawText(c, a.x, a.y + 12, "FAR", style::kLabel);
    drawText(c, a.right() - textWidth(style::kLabel, "NEAR"), a.y + 12, "NEAR", style::kLabel);
    fill(c, {a.x, a.y + 101, a.w, 2}, kInk);
    auto xOf = [&](int dbm) { return a.x + (std::clamp(dbm, -100, -30) + 100) * a.w / 70; };
    const int scaleBase = a.y + 120;
    drawText(c, a.x, scaleBase, radio::dbm(-100), kScale);
    for (const int level : {-80, -60, -40}) {
      const std::string t = radio::dbm(level);
      drawText(c, xOf(level) - textWidth(kScale, t) / 2, scaleBase, t, kScale);
    }
    drawText(c, a.right() - textWidth(kScale, "dBm"), scaleBase, "dBm", kScale);

    constexpr int kColumns = 35;
    constexpr int kStack = 5;
    int heights[kColumns] = {};
    int ringX = -1, ringY = 0;
    for (size_t i = 0; i < devs_.size(); ++i) {
      const int col = std::clamp((devs_[i].rssi + 100) / 2, 0, kColumns - 1);
      const int x = a.x + 4 + col * (a.w - 12) / (kColumns - 1);
      const int level = heights[col]++;
      if (level >= kStack) {
        if (level == kStack) {  // more than fit: a plus on top
          c.drawFastHLine(x + 1, a.y + 90 - kStack * 11 + 3, 6, kInk);
          c.drawFastVLine(x + 4, a.y + 90 - kStack * 11, 7, kInk);
        }
        continue;
      }
      const int y = a.y + 90 - level * 11;
      fill(c, {x, y, 8, 8}, kInk);
      if (static_cast<int>(i) == cursor_.cursor()) {
        ringX = x;
        ringY = y;
      }
    }
    if (ringX < 0) return;
    frame(c, {ringX - 3, ringY - 3, 14, 14}, 2);
    const bleadv::Info info = infoOf(devs_[cursor_.cursor()]);
    const std::string label = ellipsize(kStripLabel, titleOf(devs_[cursor_.cursor()], info), 220);
    const int w = textWidth(kStripLabel, label);
    const int cx = ringX + 4;
    drawText(c, std::clamp(cx - w / 2, a.x + 40, a.right() - 48 - w), a.y + 30, label,
             kStripLabel);
    for (int y = a.y + 36; y < ringY - 5; y += 4) c.drawFastVLine(cx, y, 2, kInk);
  }

  int nearest() const {
    int best = -127;
    for (const Device &d : devs_) best = std::max<int>(best, d.rssi);
    return best;
  }

  std::shared_ptr<Browse> browse_;
  std::vector<Device> devs_;
  ui::ListCursor cursor_;
  Sort sort_ = Sort::Signal;
  Key6 selected_;
  bool haveSelected_ = false;
  uint32_t sortedAt_ = 0, refreshedAt_ = 0;
  bool running_ = false;
  std::string error_;
};

}  // namespace

std::unique_ptr<ui::Screen> makeBleDevices() { return std::make_unique<BleList>(); }

}  // namespace apps

#endif  // INKDECK_BLE
