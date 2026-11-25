#include "apps/missed_ring.h"

#include <stdio.h>
#include <time.h>

#include <string>

#include "comms.h"
#include "features.h"
#include "rtc.h"
#include "timeutil.h"
#include "ui/components.h"
#include "ui/type.h"

namespace apps {

#if INKDECK_COMMS

namespace {

const std::string kDot = " \xC2\xB7 ";

// When the last one rang: 18:02 today, 22 NOV 18:02 before; "" without the time.
std::string whenRang(time_t at) {
  if (!at || !rtc::timeValid()) return "";
  const time_t now = time(nullptr);
  tm a, n;
  localtime_r(&at, &a);
  localtime_r(&now, &n);
  char buf[24];
  if (a.tm_year == n.tm_year && a.tm_yday == n.tm_yday) {
    snprintf(buf, sizeof buf, "%02d:%02d", a.tm_hour, a.tm_min);
  } else {
    snprintf(buf, sizeof buf, "%d %s %02d:%02d", a.tm_mday, timeutil::kMonthNames[a.tm_mon],
             a.tm_hour, a.tm_min);
  }
  return buf;
}

}  // namespace

bool missedRingShown() { return comms::missedRing().count > 0; }

int drawMissedRing(ui::Canvas &c, int x, int centerY, int h, int maxW, bool alignRight,
                   uint16_t color, bool rim) {
  using namespace ui;
  const comms::MissedRing m = comms::missedRing();
  if (m.count <= 0 || maxW <= 0) return 0;
  std::string name = m.name;
  if (name.empty()) {
    char b[16];
    snprintf(b, sizeof b, "INKDECK-%04X", static_cast<unsigned>(m.from & 0xFFFF));
    name = b;
  }
  const std::string when = whenRang(m.at);
  const std::string rang = name + " RANG" + (when.empty() ? "" : " " + when);
  const std::string missed =
      m.count == 1 ? "MISSED RING" : std::to_string(m.count) + " MISSED RINGS";
  // The longest that fits.
  const std::string tries[] = {
      missed + kDot + rang,
      m.count == 1 ? rang : rang + kDot + std::to_string(m.count) + " RINGS",
      rang,
      when.empty() ? missed : missed + " " + when,
  };
  constexpr int kPad = 8;
  const TextStyle &s = style::kStatusTag;
  std::string text;
  for (const std::string &t : tries) {
    if (tagWidth(t, s, kPad) <= maxW) {
      text = t;
      break;
    }
  }
  if (text.empty()) text = ellipsize(s, rang, maxW - 2 * kPad);
  const int w = tagWidth(text, s, kPad);
  const int left = alignRight ? x - w : x;
  if (rim) {
    constexpr int kRim = 4;
    fill(c, {left - kRim, centerY - h / 2 - kRim, w + 2 * kRim, h + 2 * kRim},
         color == kInk ? kPaper : kInk);
  }
  drawTag(c, left, centerY, h, text, s, TagKind::Inverted, kPad, color);
  return w;
}

#else

bool missedRingShown() { return false; }
int drawMissedRing(ui::Canvas &, int, int, int, int, bool, uint16_t, bool) { return 0; }

#endif

}  // namespace apps
