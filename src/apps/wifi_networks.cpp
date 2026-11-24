#include "apps/wifi_networks.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <array>

#include "apps/home.h"
#include "apps/radio_ui.h"
#include "sorting.h"
#include "text.h"
#include "wifi_survey.h"

namespace apps {
namespace {

using ui::Align;
using ui::Key;
using ui::Rect;
using ui::TextStyle;
using wifisurvey::Network;

constexpr TextStyle kGrid{&ui::fonts::kMono13Medium, 0};        // chart scale
constexpr TextStyle kGridBold{&ui::fonts::kMono13Bold, 0};      // channel numbers
constexpr TextStyle kChartLabel{&ui::fonts::kMono13ExtraBold, 0};  // the selected network
constexpr TextStyle kAirRow{&ui::fonts::kMono14Bold, 0};

constexpr int kChartH = 217;  // section, header included

using Bssid = std::array<uint8_t, 6>;
Bssid keyOf(const Network &n) {
  Bssid b;
  memcpy(b.data(), n.bssid, 6);
  return b;
}

using Browse = radio::Browse<Bssid>;

enum class Sort : uint8_t { Signal, Channel, Name };

const char *sortName(Sort s) {
  switch (s) {
    case Sort::Signal: return "BY SIGNAL";
    case Sort::Channel: return "BY CHANNEL";
    case Sort::Name: return "BY NAME";
  }
  return "";
}

using text::lower;

void sortNetworks(std::vector<Network> &v, Sort s) {
  switch (s) {
    case Sort::Signal:
      sorting::stable(v.begin(), v.end(),
                       [](const Network &a, const Network &b) { return a.rssi > b.rssi; });
      break;
    case Sort::Channel:
      sorting::stable(v.begin(), v.end(), [](const Network &a, const Network &b) {
        return a.channel != b.channel ? a.channel < b.channel : a.rssi > b.rssi;
      });
      break;
    case Sort::Name:
      sorting::stable(v.begin(), v.end(), [](const Network &a, const Network &b) {
        if (a.ssid.empty() != b.ssid.empty()) return b.ssid.empty();  // hidden ones last
        const std::string la = lower(a.ssid), lb = lower(b.ssid);
        return la != lb ? la < lb : a.rssi > b.rssi;
      });
      break;
  }
}

std::string nameOf(const Network &n) { return n.ssid.empty() ? "(hidden)" : n.ssid; }


std::string subtitleOf(const Network &n) {
  const char *dot = " \xC2\xB7 ";
  std::string s;
  if (n.ssid.empty()) s = wifisurvey::bssidText(n.bssid) + dot;
  s += "CH " + std::to_string(n.channel);
  if (n.second != WIFI_SECOND_CHAN_NONE) s += std::string(dot) + "40 MHz";
  s += dot;
  s += wifisurvey::securityShort(n.auth);
  const char *gen = wifisurvey::generationName(n);
  if (*gen && !n.ssid.empty()) s += std::string(dot) + gen;
  return s;
}

// ---------------------------------------------------------------------------------------------
// Channel chart: 2.4 GHz channels across, signal up. Geometry from the artboard's 452 x 178 svg.

class Chart {
 public:
  explicit Chart(const Rect &area) : a_(area) {}
  int left() const { return a_.x + 34; }
  int right() const { return a_.right(); }
  int x(int ch) const { return std::clamp(a_.x + 34 + (ch + 1) * 418 / 16, left(), right() - 1); }
  int y(int rssi) const { return a_.y + 14 + (-30 - std::clamp(rssi, -95, -30)) * 132 / 65; }
  int base() const { return a_.y + 146; }

  void drawFrame(ui::Canvas &c, int firstCh, int lastCh) const {
    using namespace ui;
    for (const int level : {-40, -60, -80}) {
      dashedH(c, left(), y(level), right() - left());
      drawText(c, a_.x, y(level) + capHeight(kGrid) / 2, radio::dbm(level), kGrid);
    }
    fill(c, {left(), base(), right() - left(), 2}, kInk);
    const int labelBase = a_.y + 164;
    drawText(c, a_.x, labelBase, "CH", kGrid);
    for (int ch = firstCh; ch <= lastCh; ++ch) {
      const std::string t = std::to_string(ch);
      drawText(c, x(ch) - textWidth(kGridBold, t) / 2, labelBase, t, kGridBold);
    }
  }

  void drawNetwork(ui::Canvas &c, const Network &n, bool selected) const {
    using namespace ui;
    int lo, hi;
    wifisurvey::span(n, lo, hi);
    const int x0 = x(lo), x1 = x(hi), top = y(n.rssi), b = base();
    const int k = std::min(18, (x1 - x0) / 3);
    if (selected) {
      // 50 % dither under it, then a 2 px outline and its name above.
      for (int yy = top; yy < b; ++yy) {
        const int in = k * (b - yy) / std::max(1, b - top);
        for (int xx = x0 + in; xx <= x1 - in; ++xx) {
          if (((xx + yy) & 1) == 0) c.drawPixel(xx, yy, kInk);
        }
      }
      for (int d = 0; d < 2; ++d) {
        c.drawLine(x0 + d, b, x0 + k + d, top + d, kInk);
        c.drawLine(x0 + k, top + d, x1 - k, top + d, kInk);
        c.drawLine(x1 - k - d, top + d, x1 - d, b, kInk);
      }
      const std::string label = ellipsize(kChartLabel, nameOf(n), 200);
      const int w = textWidth(kChartLabel, label);
      const int lx = std::clamp((x0 + x1) / 2 - w / 2, left(), right() - w);
      drawText(c, lx, std::max(top - 7, a_.y + capHeight(kChartLabel)), label, kChartLabel);
      return;
    }
    c.drawLine(x0, b, x0 + k, top, kInk);
    c.drawLine(x0 + k, top, x1 - k, top, kInk);
    c.drawLine(x1 - k, top, x1, b, kInk);
  }

 private:
  Rect a_;
};

// ---------------------------------------------------------------------------------------------

class WifiDetail : public ui::Screen {
 public:
  WifiDetail(std::shared_ptr<Browse> browse, const Network &n) : browse_(std::move(browse)) {
    show(n);
  }

  std::string tag() const override { return moduleTag("SENSORS"); }
  std::string context() const override {
    return "WI-FI \xC2\xB7 " +
           browse_->position();
  }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "NETWORK"};
    out[1] = {ui::glyph::kPress, tone_ ? "TONE OFF" : "TONE"};
    out[2] = {"BOOT", "BACK"};
    return 3;
  }

  void onEnter() override {
    wifisurvey::begin();
    wifisurvey::track(net_.bssid, net_.channel);
  }
  void onLeave() override {
    wifisurvey::untrack();
    wifisurvey::end();
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up:
      case Key::Down: return step(key == Key::Up ? -1 : 1);
      case Key::Select: tone_ = !tone_; return true;
      default: return false;
    }
  }

  bool onTick() override {
    others_ = wifisurvey::networks();
    for (const Network &n : others_) {
      if (wifisurvey::sameBssid(n.bssid, net_.bssid)) {
        net_ = n;
        break;
      }
    }
    const uint32_t now = millis();
    bool changed = false;
    if (now - sampledAt_ >= radio::kSampleMs) {
      sampledAt_ = now;
      history_.push(heardWithin(3000) ? net_.rssi : radio::History::kNone);
      changed = true;
    }
    if (tone_ && heardWithin(5000)) radio::beep(net_.rssi);
    const std::string lost = lostText();
    changed |= lost != lost_;
    lost_ = lost;
    return changed;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    Rect r = body;
    radio::drawIdentity(c, r.takeTop(radio::kIdentityH), "NETWORK", net_.saved ? "SAVED" : "",
                        nameOf(net_), wifisurvey::bssidText(net_.bssid),
                        wifisurvey::generationName(net_));
    const bool have = heardWithin(10000);
    const std::string live = have ? "LIVE \xC2\xB7 CH " + std::to_string(net_.channel) + " EVERY 2 S"
                                  : lost_;
    const Rect signal =
        radio::takeSection(c, r, kRule + kSectionH + radio::kSignalH, "SIGNAL", live);
    radio::drawSignal(c, signal, have, net_.rssi, "", "", history_);

    Rect details = radio::takeSection(c, r, kRule + kSectionH + 202, "RADIO",
                                      wifisurvey::phyModes(net_));
    details = details.inset(kPadX, 0);
    details.takeTop(4);
    for (const auto &kv : radioRows()) {
      radio::drawKeyValue(c, details.takeTop(radio::kKeyValueH), kv.first, kv.second);
    }

    drawAir(c, r);
  }

 private:
  void show(const Network &n) {
    net_ = n;
    history_.clear();
    history_.push(n.rssi);  // from the scan that listed it
    sampledAt_ = millis();
    others_ = wifisurvey::networks();
    lost_ = lostText();
  }

  bool step(int delta) {
    const int next = browse_->step(delta);
    if (next < 0) return false;
    for (const Network &n : wifisurvey::networks()) {
      if (keyOf(n) != browse_->order[next]) continue;
      browse_->index = next;
      show(n);
      wifisurvey::track(net_.bssid, net_.channel);
      return true;
    }
    return false;  // it dropped out of the scans; stay
  }

  bool heardWithin(uint32_t ms) const {
    return net_.heardMs != 0 && millis() - net_.heardMs < ms;
  }

  // "NOT HEARD FOR 15 S" in 5 s steps once 10 s have gone by; "" while it's heard.
  std::string lostText() const {
    if (heardWithin(10000) || net_.heardMs == 0) return "";
    const uint32_t s = (millis() - net_.heardMs) / 1000;
    if (s >= 120) return "NOT HEARD FOR " + std::to_string(s / 60) + " MIN";
    return "NOT HEARD FOR " + std::to_string(s / 5 * 5) + " S";
  }

  std::vector<std::pair<std::string, std::string>> radioRows() const {
    const Network &n = net_;
    const char *dot = " \xC2\xB7 ";
    std::vector<std::pair<std::string, std::string>> rows;
    rows.emplace_back("CHANNEL", std::to_string(n.channel) + dot +
                                     std::to_string(wifisurvey::frequencyMhz(n.channel)) + " MHz");
    std::string width = "20 MHz";
    if (n.second == WIFI_SECOND_CHAN_ABOVE) {
      width = "40 MHz" + std::string(dot) + "CH " + std::to_string(n.channel) + " + " +
              std::to_string(n.channel + 4);
    } else if (n.second == WIFI_SECOND_CHAN_BELOW) {
      width = "40 MHz" + std::string(dot) + "CH " + std::to_string(n.channel) + " + " +
              std::to_string(n.channel - 4);
    }
    rows.emplace_back("WIDTH", width);
    rows.emplace_back("SECURITY", wifisurvey::securityLong(n.auth));
    if (n.auth != WIFI_AUTH_OPEN) {
      rows.emplace_back("CIPHER", std::string(wifisurvey::cipherName(n.pairwise)) + dot +
                                      "GROUP " + wifisurvey::cipherName(n.group));
    }
    rows.emplace_back("WPS", n.wps ? "ON" : "OFF");
    std::string country = "NOT ADVERTISED";
    if (n.country[0]) {
      country = n.country;
      if (n.countryCount > 0) {
        country += std::string(dot) + "CH " + std::to_string(n.countryFirst) + "\xE2\x80\x93" +
                   std::to_string(n.countryFirst + n.countryCount - 1);
      }
      if (n.countryMaxTx > 0) country += std::string(dot) + std::to_string(n.countryMaxTx) + " dBm";
    }
    rows.emplace_back("COUNTRY", country);
    if (n.ax) rows.emplace_back("BSS COLOR", n.bssColor >= 0 ? std::to_string(n.bssColor) : "OFF");
    std::string ftm = "NONE";
    if (n.ftmResponder && n.ftmInitiator) {
      ftm = "FTM RESPONDER + INITIATOR";
    } else if (n.ftmResponder) {
      ftm = "FTM RESPONDER";
    } else if (n.ftmInitiator) {
      ftm = "FTM INITIATOR";
    }
    rows.emplace_back("RANGING", ftm);
    return rows;
  }

  // The networks whose channels overlap this one's: on the same channel (they take turns with
  // it) or partly (they interfere).
  void drawAir(ui::Canvas &c, Rect r) const {
    using namespace ui;
    int lo, hi;
    wifisurvey::span(net_, lo, hi);
    std::vector<const Network *> air;
    for (const Network &o : others_) {
      if (wifisurvey::sameBssid(o.bssid, net_.bssid)) continue;
      int olo, ohi;
      wifisurvey::span(o, olo, ohi);
      if (olo < hi && lo < ohi) air.push_back(&o);
    }
    const std::string count = air.empty() ? "NONE"
                              : air.size() == 1
                                  ? "1 NETWORK"
                                  : std::to_string(air.size()) + " NETWORKS";
    Rect rows = radio::takeSection(c, r, r.h, "SHARES THE AIR", count);
    rows = rows.inset(kPadX, 0);
    rows.takeTop(2);
    if (air.empty()) {
      drawText(c, rows.takeTop(radio::kKeyValueH), "NOTHING ELSE ON THESE CHANNELS",
               style::kLabel);
      return;
    }
    for (const Network *o : air) {
      if (rows.h < radio::kKeyValueH) break;
      const Rect row = rows.takeTop(radio::kKeyValueH);
      const int w = row.w;
      drawText(c, {row.x, row.y, w - 76 - 40 - 40 - 8, row.h}, nameOf(*o), kAirRow);
      drawText(c, {row.x + w - 156, row.y, 76, row.h},
               o->channel == net_.channel ? "SAME CH" : "OVERLAP", style::kTag);
      drawText(c, {row.x + w - 80, row.y, 40, row.h}, std::to_string(o->channel), kAirRow,
               Align::Right);
      drawText(c, {row.x + w - 40, row.y, 40, row.h}, radio::dbm(o->rssi), kAirRow,
               Align::Right);
      dashedH(c, row.x, row.bottom() - 1, row.w);
    }
  }

  std::shared_ptr<Browse> browse_;
  Network net_;
  std::vector<Network> others_;
  radio::History history_;
  uint32_t sampledAt_ = 0;
  std::string lost_;
  bool tone_ = false;
};

// ---------------------------------------------------------------------------------------------

class WifiList : public ui::Screen {
 public:
  WifiList() : browse_(std::make_shared<Browse>()) {}

  std::string tag() const override { return moduleTag("SENSORS"); }
  std::string context() const override {
    if (nets_.empty() && wifisurvey::lastScanMs() == 0) return "WI-FI \xC2\xB7 SCANNING\xE2\x80\xA6";
    return "WI-FI \xC2\xB7 " + std::to_string(nets_.size()) +
           (nets_.size() == 1 ? " NETWORK" : " NETWORKS");
  }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "MOVE"};
    out[1] = {ui::glyph::kPress, "DETAIL"};
    out[2] = {ui::glyph::kHoldPress, "SORT"};
    out[3] = {"BOOT", "BACK"};
    return 4;
  }

  void onEnter() override {
    wifisurvey::begin();
    if (browse_->current(selected_)) haveSelected_ = true;  // where the detail ended
    reload();
  }
  void onLeave() override { wifisurvey::end(); }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up:
      case Key::Down:
        if (!cursor_.move(key == Key::Up ? -1 : 1)) return false;
        remember();
        return true;
      case Key::Select:
        if (nets_.empty()) return false;
        browse_->index = cursor_.cursor();
        ui::nav::push(std::make_unique<WifiDetail>(browse_, nets_[cursor_.cursor()]));
        return true;
      case Key::SelectLong:
        sort_ = static_cast<Sort>((static_cast<int>(sort_) + 1) % 3);
        reload();
        return true;
      default: return false;
    }
  }

  bool onTick() override {
    const uint32_t g = wifisurvey::generation();
    const std::string e = wifisurvey::error();
    if (g == gen_ && e == error_) return false;
    reload();
    return true;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    Rect r = body;
    drawChart(c, r.takeTop(kChartH));

    Rect list = r;
    hrule(c, list.x, list.y, list.w);
    list.takeTop(kRule);
    const Rect head = list.takeTop(kSectionH);
    if (nets_.empty() && !error_.empty()) {
      drawNotice(c, head, error_);
    } else {
      drawSectionHeader(c, head, "NETWORKS",
                        nets_.empty() ? sortName(sort_)
                                      : std::string(sortName(sort_)) + " \xC2\xB7 " + cursor_.label());
    }
    if (nets_.empty()) {
      const char *msg = wifisurvey::lastScanMs() == 0 ? "SCANNING\xE2\x80\xA6" : "NO NETWORKS IN RANGE";
      drawText(c, {list.x, list.y, list.w, 80}, msg, style::kLabel, Align::Center);
      return;
    }
    const int perPage = list.h / kRowTallH;
    const int first = cursor_.pageStart(perPage);
    for (int i = first; i < static_cast<int>(nets_.size()) && i < first + perPage; ++i) {
      const Network &n = nets_[i];
      radio::drawRow(c, {list.x, list.y + (i - first) * kRowTallH, list.w, kRowTallH}, nameOf(n),
                     subtitleOf(n), n.saved ? "SAVED" : "", n.rssi, i == cursor_.cursor());
    }
  }

 private:
  void remember() {
    if (nets_.empty()) return;
    selected_ = keyOf(nets_[cursor_.cursor()]);
    haveSelected_ = true;
  }

  // Takes the latest scan, sorts it and keeps the cursor on the network it was on.
  void reload() {
    gen_ = wifisurvey::generation();
    error_ = wifisurvey::error();
    nets_ = wifisurvey::networks();
    sortNetworks(nets_, sort_);
    cursor_.reset(static_cast<int>(nets_.size()),
                  browse_->reorder(nets_, keyOf, haveSelected_, selected_));
    remember();
  }

  void drawChart(ui::Canvas &c, Rect r) const {
    using namespace ui;
    std::string right = wifisurvey::lastScanMs() == 0 ? "SCANNING\xE2\x80\xA6" : "";
    if (!nets_.empty()) {
      int counts[16] = {};
      for (const Network &n : nets_) {
        if (n.channel < 16) ++counts[n.channel];
      }
      const int busiest = static_cast<int>(std::max_element(counts, counts + 16) - counts);
      right = "BUSIEST CH " + std::to_string(busiest) + " \xC2\xB7 " + std::to_string(counts[busiest]);
    }
    drawSectionHeader(c, r.takeTop(kSectionH), "CHANNELS \xC2\xB7 2.4 GHz", right);
    const Chart chart({r.x + kPadX, r.y + 6, r.w - 2 * kPadX, 178});
    int firstCh, lastCh;
    wifisurvey::channelRange(firstCh, lastCh);
    (void)firstCh;
    chart.drawFrame(c, 1, std::max(lastCh, 13));
    if (nets_.empty()) return;
    // Weakest first, the selected one last, on top.
    std::vector<int> order(nets_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    sorting::stable(order.begin(), order.end(),
                     [&](int a, int b) { return nets_[a].rssi < nets_[b].rssi; });
    for (const int i : order) {
      if (i != cursor_.cursor()) chart.drawNetwork(c, nets_[i], false);
    }
    chart.drawNetwork(c, nets_[cursor_.cursor()], true);
  }

  std::shared_ptr<Browse> browse_;
  std::vector<Network> nets_;
  ui::ListCursor cursor_;
  Sort sort_ = Sort::Signal;
  Bssid selected_{};
  bool haveSelected_ = false;
  uint32_t gen_ = 0;
  std::string error_;
};

}  // namespace

std::unique_ptr<ui::Screen> makeWifiNetworks() { return std::make_unique<WifiList>(); }

}  // namespace apps
