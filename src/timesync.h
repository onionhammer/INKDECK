// SYNC TIME: join Wi-Fi, ask NTP for the time, store it in the RTC, Wi-Fi off. Runs on its own
// task so the UI keeps drawing; one sync at a time.
#pragma once
#include <time.h>

#include <string>

namespace timesync {

enum class Phase { Idle, Connecting, Asking, Done, Failed };

struct Status {
  Phase phase = Phase::Idle;
  std::string result;  // Done or Failed: "TIME SET · 14:32 CST · WAS 3 S SLOW", "WI-FI: …"
};

bool start();  // false if a sync is already running (it carries on)
Status status();

// When the clock was last set from NTP (kept in NVS); 0 if never.
time_t lastSync();

}  // namespace timesync
