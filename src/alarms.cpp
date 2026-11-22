#include "alarms.h"

#include <Preferences.h>
#include <string.h>

#include <mutex>

#include "power.h"
#include "timeutil.h"

namespace alarms {
namespace {

constexpr const char *kNamespace = "alarms";
constexpr const char *kKey = "slots";
constexpr const char *kSnoozeKey = "snooze";  // u64: until << 8 | slot
constexpr uint8_t kVersion = 1;

struct Record {  // the NVS blob; bump kVersion when the layout changes
  uint8_t version;
  uint8_t slot[kCount][4];  // hour, minute, days, on
};

// The first visit's slots: everyday times, all off, so nothing rings until it's asked to.
constexpr Alarm kDefaults[kCount] = {
    {7, 0, kWeekdays, false},
    {9, 0, kWeekend, false},
    {12, 0, kOnce, false},
};

constexpr uint8_t kRepeats[] = {kOnce,    kEveryDay, kWeekdays, kWeekend, 0x01, 0x02,
                                0x04,     0x08,      0x10,      0x20,     0x40};
constexpr int kRepeatCount = sizeof kRepeats / sizeof kRepeats[0];
constexpr const char *kDayNames[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

std::mutex mutex;  // the UI task edits; the alarm check reads
Alarm slots[kCount] = {kDefaults[0], kDefaults[1], kDefaults[2]};
time_t snoozeAt = 0;
int snoozeSlot = 0;

void saveSnooze() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.putULong64(kSnoozeKey, static_cast<uint64_t>(snoozeAt) << 8 | (snoozeSlot & 0xFF));
  prefs.end();
}

void save() {
  Record r = {};
  r.version = kVersion;
  for (int i = 0; i < kCount; ++i) {
    r.slot[i][0] = slots[i].hour;
    r.slot[i][1] = slots[i].minute;
    r.slot[i][2] = slots[i].days;
    r.slot[i][3] = slots[i].on;
  }
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.putBytes(kKey, &r, sizeof r);
  prefs.end();
}

}  // namespace

void begin() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  Record r;
  const bool ok = prefs.isKey(kKey) && prefs.getBytes(kKey, &r, sizeof r) == sizeof r &&
                  r.version == kVersion;
  const uint64_t snoozed = prefs.isKey(kSnoozeKey) ? prefs.getULong64(kSnoozeKey) : 0;
  prefs.end();
  std::lock_guard<std::mutex> lock(mutex);
  snoozeAt = static_cast<time_t>(snoozed >> 8);
  snoozeSlot = static_cast<int>(snoozed & 0xFF) % kCount;
  if (!ok) return;
  for (int i = 0; i < kCount; ++i) {
    const uint8_t *s = r.slot[i];
    if (s[0] > 23 || s[1] > 59 || s[2] > kEveryDay) continue;
    slots[i] = {s[0], s[1], s[2], s[3] != 0};
  }
}

Alarm get(int slot) {
  std::lock_guard<std::mutex> lock(mutex);
  return slots[slot % kCount];
}

void set(int slot, const Alarm &a) {
  std::lock_guard<std::mutex> lock(mutex);
  Alarm &s = slots[slot % kCount];
  if (s.hour == a.hour && s.minute == a.minute && s.days == a.days && s.on == a.on) return;
  s = a;
  save();
}

time_t ringAfter(const Alarm &a, time_t after) {
  tm day;
  localtime_r(&after, &day);
  // Eight days covers a weekly repeat whose only day is today but whose time has passed.
  for (int i = 0; i < 8; ++i) {
    tm d = day;
    d.tm_mday += i;
    d.tm_hour = 12;  // normalise the date at noon, clear of daylight-saving gaps
    d.tm_isdst = -1;
    mktime(&d);
    if (a.days != kOnce && !(a.days & (1 << timeutil::mondayIndex(d)))) continue;
    const time_t ring = timeutil::localAt(d, a.hour, a.minute);
    if (ring > after) return ring;
  }
  return 0;
}

time_t next(time_t after, int *slot) {
  time_t best = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (snoozeAt > after) {
      best = snoozeAt;
      if (slot) *slot = snoozeSlot;
    }
  }
  for (int i = 0; i < kCount; ++i) {
    const Alarm a = get(i);
    if (!a.on) continue;
    const time_t t = ringAfter(a, after);
    if (t && (!best || t < best)) {
      best = t;
      if (slot) *slot = i;
    }
  }
  return best;
}

int due(time_t since, time_t now) {
  int found = -1;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (snoozeAt > since && snoozeAt <= now) {
      found = snoozeSlot;
      snoozeAt = 0;
      saveSnooze();
    }
  }
  for (int i = 0; i < kCount; ++i) {
    Alarm a = get(i);
    if (!a.on) continue;
    const time_t t = ringAfter(a, since);
    if (!t || t > now) continue;
    if (a.days == kOnce) {
      a.on = false;
      set(i, a);
    }
    if (found < 0) found = i;
  }
  return found;
}

void snooze(int slot, time_t until) {
  std::lock_guard<std::mutex> lock(mutex);
  snoozeAt = until;
  snoozeSlot = slot % kCount;
  saveSnooze();
}

time_t snoozedUntil(int *slot) {
  std::lock_guard<std::mutex> lock(mutex);
  if (slot) *slot = snoozeSlot;
  return snoozeAt;
}

uint8_t stepRepeat(uint8_t days, int delta) {
  int i = 0;
  while (i < kRepeatCount && kRepeats[i] != days) ++i;
  if (i == kRepeatCount) i = 0;
  return kRepeats[((i + delta) % kRepeatCount + kRepeatCount) % kRepeatCount];
}

std::string repeatLabel(uint8_t days) {
  switch (days) {
    case kOnce: return "once";
    case kEveryDay: return "every day";
    case kWeekdays: return "Mon\xE2\x80\x93" "Fri";  // en dash
    case kWeekend: return "Sat\xE2\x80\x93" "Sun";
    default: break;
  }
  std::string out;
  for (int i = 0; i < 7; ++i) {
    if (!(days & (1 << i))) continue;
    if (!out.empty()) out += ", ";
    out += kDayNames[i];
  }
  return out;
}

time_t nextAtWake(time_t now) {
  const time_t armed = power::armedFor();
  return next(armed ? armed - 1 : now - 60);
}

}  // namespace alarms
