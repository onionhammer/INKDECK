// Pieces shared by the Sensors screen's radio views: its scan panel (apps/sensors.h), the Wi-Fi
// screens (apps/wifi_networks.h) and the Bluetooth LE screens (apps/ble_devices.h). Signal bars,
// the dBm text, the live "// SIGNAL" section, two-line rows with the signal on the right,
// key/value lines and the name block a detail screen opens with. Sizes from the design canvas's
// "Sensors · Wi-Fi & Bluetooth LE scan" artboards.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "ui/components.h"

namespace apps {
namespace radio {

// Four bars, 3 px wide and 2 px apart, 4/7/10/14 px tall: filled up to the signal's level,
// outlined above it.
constexpr int kBarsW = 18;
int barsFor(int rssi);  // 1 to 4
void drawBars(ui::Canvas &c, int x, int bottom, int rssi, uint16_t color = ui::kInk);

const char *quality(int rssi);  // "STRONG", "GOOD", "FAIR", "WEAK", "FAINT"
std::string dbm(int rssi);      // "−48", with a real minus (U+2212)

// A detail screen's live signal: one sample every kSampleMs, the last kSlots of them (two
// minutes). kNone marks a sample period in which the device wasn't heard.
constexpr uint32_t kSampleMs = 2000;
class History {
 public:
  static constexpr int kSlots = 60;
  static constexpr int8_t kNone = -128;
  void clear() { v_.clear(); }
  void push(int8_t rssi);
  bool any() const;  // a real sample in it
  int min() const;   // of the real samples
  int max() const;
  const std::vector<int8_t> &samples() const { return v_; }
  bool operator==(const History &o) const { return v_ == o.v_; }

 private:
  std::vector<int8_t> v_;
};

// The "// SIGNAL" section's body (kSignalH): the reading in 64 px ("-48 dBm"; "--" without
// one), a right column (two lines; with `top` empty, the quality word over a 20-segment meter),
// "RSSI · 2 MIN" with the history's range, and its sparkline.
constexpr int kSignalH = 144;
void drawSignal(ui::Canvas &c, const ui::Rect &r, bool have, int rssi, const std::string &top,
                const std::string &bottom, const History &history);

// A two-line list row (kRowTallH): title, subtitle, an optional outlined tag, the bars and the
// dBm. Selected rows are inverted with a ▶, like ui::drawRow().
void drawRow(ui::Canvas &c, const ui::Rect &r, const std::string &title,
             const std::string &subtitle, const std::string &tag, int rssi, bool selected);

// A key/value line (kKeyValueH), System's STATE rows: the key in a 112 px column.
constexpr int kKeyValueH = 24;
void drawKeyValue(ui::Canvas &c, const ui::Rect &r, const std::string &key,
                  const std::string &value);

// A detail screen's opening section (kIdentityH, header included): the header with an inverted
// pill on its right, the name in 22 px, the address under it with an outlined tag on the right.
constexpr int kIdentityH = 102;
void drawIdentity(ui::Canvas &c, ui::Rect r, const std::string &header, const std::string &pill,
                  const std::string &name, const std::string &address, const std::string &tag);

// A section header with its 3 px rule above (every section but a screen's first).
ui::Rect takeSection(ui::Canvas &c, ui::Rect &r, int h, const std::string &label,
                     const std::string &right);

// The detail screens' ● TONE: a short beep whose pitch rises with the signal, once a second.
void beep(int rssi);

}  // namespace radio
}  // namespace apps
