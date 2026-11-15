// Where the PIN (apps/lock.h) is asked, chosen at build time. Set in platformio.ini:
//   -DINKDECK_PIN_ON=power_on,settings,files    any of the three, comma-separated
//     power_on  the lock: every start, waking the desk face or cancelling a power-off, and
//               after lock_minutes without a key (SD:/inkdeck.ini)
//     settings  opening System from Home, unless it was on screen in the last minute
//     files     opening Files from Home, the same way
// Unset: all three. Empty: nowhere (System's PIN LOCK still asks before a change). Any other
// word stops the build. With no PIN set, nothing asks.
#pragma once

#ifndef INKDECK_PIN_ON
#define INKDECK_PIN_ON power_on, settings, files
#endif

namespace pinplaces {

enum Place : unsigned { kPowerOn = 1u << 0, kSettings = 1u << 1, kFiles = 1u << 2 };

namespace detail {

constexpr unsigned kUnknown = 1u << 31;

// The flag's words are bare tokens; this turns them into one string.
#define INKDECK_PIN_STR_(...) #__VA_ARGS__
#define INKDECK_PIN_STR(...) INKDECK_PIN_STR_(__VA_ARGS__)
inline constexpr char kSpec[] = INKDECK_PIN_STR(INKDECK_PIN_ON);
#undef INKDECK_PIN_STR
#undef INKDECK_PIN_STR_

// Quotes are separators too, so -DINKDECK_PIN_ON="files" works as well.
constexpr bool separator(char c) { return c == ',' || c == ' ' || c == '\t' || c == '"'; }

constexpr bool wordIs(const char *s, int n, const char *word) {
  int i = 0;
  for (; i < n && word[i]; ++i) {
    if (s[i] != word[i]) return false;
  }
  return i == n && !word[n];
}

constexpr unsigned parse(const char *s) {
  unsigned places = 0;
  while (*s) {
    if (separator(*s)) {
      ++s;
      continue;
    }
    int n = 0;
    while (s[n] && !separator(s[n])) ++n;
    if (wordIs(s, n, "power_on")) {
      places |= kPowerOn;
    } else if (wordIs(s, n, "settings")) {
      places |= kSettings;
    } else if (wordIs(s, n, "files")) {
      places |= kFiles;
    } else {
      places |= kUnknown;
    }
    s += n;
  }
  return places;
}

inline constexpr unsigned kPlaces = parse(kSpec);
static_assert(!(kPlaces & kUnknown),
              "INKDECK_PIN_ON: list any of power_on, settings, files, comma-separated");

}  // namespace detail

// true if the build asks for the PIN at `place`.
constexpr bool asks(Place place) { return (detail::kPlaces & place) != 0; }

}  // namespace pinplaces
