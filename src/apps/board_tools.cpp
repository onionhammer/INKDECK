#include "apps/board_tools.h"

#include "features.h"

#if INKDECK_AGENT

#include <Arduino.h>
#include <cJSON.h>
#include <dirent.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "alarms.h"
#include "apps/clock.h"
#include "apps/files.h"
#include "apps/music.h"
#include "apps/note.h"
#include "apps/podcasts.h"
#include "apps/reader.h"
#include "apps/sensors.h"
#include "apps/weather.h"
#include "audio.h"
#include "dirlist.h"
#include "flashfs.h"
#include "forecast.h"
#include "input.h"
#include "paths.h"
#include "pmic.h"
#include "readstate.h"
#include "sdcard.h"
#include "sensor_hub.h"
#include "text.h"
#include "ui/nav.h"
#include "units.h"
#include "agent_client.h"

namespace apps {
namespace {

// What the hello announces: each tool's name, what it does (for the model) and its input.
constexpr const char *kDefinitions = R"json([
{"name":"find_books","description":"Lists the books (EPUBs) on the INKDECK, the e-paper board the user is talking through: the path to open each with, and how far it has been read. With a query, only the books whose path holds every word of it; if none does, all of them.","input_schema":{"type":"object","properties":{"query":{"type":"string","description":"Words from the title or the author, e.g. \"dune herbert\""}}}},
{"name":"open_book","description":"Opens a book on the board where it was left, once your reply has finished playing (the agent sheet closes). Get the path from find_books. Keep the reply short: say you're opening it.","input_schema":{"type":"object","properties":{"path":{"type":"string","description":"A path find_books gave"}},"required":["path"]}},
{"name":"weather","description":"The weather where the board is, from the forecast it keeps for its Weather screen: now, the next 12 hours and the coming days (temperature, sky, rain chance and amount, wind, sunrise and sunset). Use it for any question about the weather here; search the web only for somewhere else.","input_schema":{"type":"object","properties":{}}},
{"name":"board_status","description":"The board now: local date and time, battery, the room's temperature and humidity, volume, what's on screen, what's playing, its three alarms and the last book read.","input_schema":{"type":"object","properties":{}}},
{"name":"set_volume","description":"Sets the board's speaker volume.","input_schema":{"type":"object","properties":{"volume":{"type":"integer","minimum":0,"maximum":100}},"required":["volume"]}},
{"name":"set_alarm","description":"Sets one of the board's three alarms, or turns one off (on: false, with its slot or time). Without a slot it takes the one already at that time, else one that's off. board_status lists them.","input_schema":{"type":"object","properties":{"time":{"type":"string","description":"Local time, 24-hour HH:MM"},"repeat":{"type":"string","enum":["once","weekdays","weekend","every_day"],"description":"Default once"},"on":{"type":"boolean","description":"Default true"},"slot":{"type":"integer","minimum":1,"maximum":3}}}},
{"name":"show","description":"Opens a screen on the board once your reply has finished playing (the agent sheet closes): home, clock, weather (the forecast), music (the player), podcasts, sensors (temperature and humidity history), reader (the last book, where it was left), or note (the last note show_note put up, where it was left). Only when the user asks to see or open one: to answer a question, use the tool that reads the answer (weather, board_status) and say it.","input_schema":{"type":"object","properties":{"screen":{"type":"string","enum":["home","clock","weather","music","podcasts","sensors","reader","note"]}},"required":["screen"]}},
{"name":"show_note","description":"Puts text on the board's screen for the user to read and page through, once your reply has finished playing (the agent sheet closes), and keeps it in the board's notes. Use it when the user asks to see something, or when the answer is a recipe, the steps of a how-to, a list or anything else too long to follow by ear: put the whole answer here, and say only a sentence or two of it and that it's on the screen. Otherwise just answer aloud.","input_schema":{"type":"object","properties":{"title":{"type":"string","description":"Short, e.g. \"Banana bread\". A note with the same title is replaced"},"text":{"type":"string","description":"The whole answer in Markdown, without the title: # headings, - bullets, 1. numbered steps, - [ ] checklist items, **bold**, *italic*. Tables, links and images come out as plain text. Up to 16 KB; more is cut off"}},"required":["title","text"]}},
{"name":"read_note","description":"The note on the board's screen, or the last one shown: its title and text. Use it when the user asks about what's on the screen (\"double it\", \"what's step 4?\") and it isn't in this conversation.","input_schema":{"type":"object","properties":{}}}
])json";

constexpr int kMaxDepth = 3;         // folders below the books folder
constexpr int kMaxVisited = 3000;    // entries looked at, at most
constexpr size_t kMaxBooks = 400;
constexpr size_t kMaxListing = 6000;  // bytes of a listing

struct Call {
  uint32_t id;
  std::string name;
  cJSON *input;  // owned: run() deletes it
  uint32_t turn;
};

struct Result {
  std::string text;
  bool error = false;
};

SemaphoreHandle_t lock = nullptr;  // guards `calls`
std::vector<Call> calls;

// A screen to open once the turn it came in has been said (UI task only). The last call wins.
struct Change {
  uint32_t turn;
  std::function<void()> open;
};
std::unique_ptr<Change> change;

// The agent task's tool_call: queued for the UI task, which wakes for it.
void onCall(uint32_t id, const std::string &name, cJSON *input, uint32_t turn) {
  xSemaphoreTake(lock, portMAX_DELAY);
  calls.push_back({id, name, input, turn});
  xSemaphoreGive(lock);
  input::wake();
}

Result fail(const std::string &why) { return {why, true}; }

std::string text(const cJSON *in, const char *name) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(in, name);
  return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

using text::lower;


std::string twoDigits(int n) {
  char buf[4];
  snprintf(buf, sizeof buf, "%02d", n);
  return buf;
}

// --- find_books, open_book -------------------------------------------------------------------

void collectBooks(const std::string &dir, int depth, std::vector<std::string> &out,
                  int &visited) {
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  while (dirent *e = readdir(d)) {
    if (++visited > kMaxVisited || out.size() >= kMaxBooks) break;
    if (e->d_name[0] == '.') continue;
    const std::string path = dir + "/" + e->d_name;
    if (e->d_type == DT_DIR) {
      if (depth < kMaxDepth) collectBooks(path, depth + 1, out, visited);
    } else if (DirList::isEpub(e->d_name)) {
      out.push_back(path);
    }
  }
  closedir(d);
}

// Every word of `query` is in `path` (both lower case).
bool matches(const std::string &path, const std::string &query) {
  size_t pos = 0;
  while (pos < query.size()) {
    const size_t end = std::min(query.find(' ', pos), query.size());
    if (end > pos && path.find(query.substr(pos, end - pos)) == std::string::npos) return false;
    pos = end + 1;
  }
  return true;
}

std::string listing(const std::vector<std::string> &books) {
  std::string out;
  size_t shown = 0;
  for (const std::string &path : books) {
    if (out.size() > kMaxListing) break;
    out += path;
    readstate::Position pos;
    if (readstate::load(path, pos)) out += " (read " + std::to_string(pos.percent) + "%)";
    out += "\n";
    ++shown;
  }
  if (shown < books.size()) {
    out += "... and " + std::to_string(books.size() - shown) + " more: a query narrows it\n";
  }
  return out;
}

Result findBooks(const cJSON *in) {
  if (!sdcard::mounted()) return fail("there's no card in the board");
  const std::string root = sdFolder("books");  // the card's root if there's no such folder
  std::vector<std::string> books;
  int visited = 0;
  collectBooks(root, 0, books, visited);
  std::sort(books.begin(), books.end(), [](const std::string &a, const std::string &b) {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
  });
  if (books.empty()) return {"There are no books (EPUB files) in " + displayPath(root) + "."};

  const std::string query = lower(text(in, "query"));
  std::vector<std::string> found;
  for (const std::string &path : books) {
    if (matches(lower(path.substr(root.size())), query)) found.push_back(path);
  }
  std::string out;
  if (found.empty()) {
    out = "No book matches \"" + query + "\". All " + std::to_string(books.size()) +
          " books:\n" + listing(books);
  } else {
    out = listing(found);
  }
  std::string lastPath, lastTitle;
  readstate::Position pos;
  if (readstate::last(lastPath, lastTitle, pos)) {
    out += "Last read: " + lastTitle + " (" + lastPath + ")";
  }
  return {out};
}

// After nav::home(), from the root.
void push(std::unique_ptr<ui::Screen> screen) {
  if (screen) ui::nav::push(std::move(screen));
}

void later(uint32_t turn, std::function<void()> open) {
  change.reset(new Change{turn, std::move(open)});
}

Result openBook(const cJSON *in, uint32_t turn) {
  const std::string path = text(in, "path");
  const bool onBoard = path.rfind(std::string(sdcard::kMountPoint) + "/", 0) == 0 ||
                       path.rfind(std::string(flashfs::kMountPoint) + "/", 0) == 0;
  if (!onBoard || path.find("/../") != std::string::npos) {
    return fail("not a path on the board: give one find_books listed");
  }
  if (!DirList::isEpub(path)) return fail("not a book (EPUB)");
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return fail("there's no such book: call find_books");
  later(turn, [path] {
    std::string err;
    std::unique_ptr<ui::Screen> reader = makeReader(path, err);
    if (!reader) log_w("agent: couldn't open %s: %s", path.c_str(), err.c_str());
    push(std::move(reader));
  });
  return {"\"" + paths::stem(path) + "\" opens when your reply has finished playing."};
}

// --- board_status ----------------------------------------------------------------------------

std::string alarmLine(int slot) {
  const alarms::Alarm a = alarms::get(slot);
  std::string line = std::to_string(slot + 1) + ": " + twoDigits(a.hour) + ":" +
                     twoDigits(a.minute) + " " + alarms::repeatLabel(a.days) +
                     (a.on ? ", on" : ", off");
  if (a.on) {
    const time_t at = alarms::ringAfter(a, time(nullptr));
    tm t;
    localtime_r(&at, &t);
    char buf[24];
    strftime(buf, sizeof buf, "%a %d %b %H:%M", &t);
    line += std::string(" (next ") + buf + ")";
  }
  return line;
}

Result boardStatus() {
  std::string out;
  const time_t now = time(nullptr);
  tm t;
  localtime_r(&now, &t);
  if (t.tm_year + 1900 >= 2024) {
    char buf[48];
    strftime(buf, sizeof buf, "%A %d %B %Y, %H:%M", &t);
    out += std::string("Local time: ") + buf + "\n";
  } else {
    out += "Local time: unknown (the clock isn't set)\n";
  }

  const pmic::Status p = pmic::status();
  out += "Power: ";
  if (!p.ok) {
    out += "unknown";
  } else if (p.battery) {
    out += "battery " + std::to_string(p.percent) + "%" +
           (p.charging ? ", charging" : p.vbus ? ", on USB" : ", not plugged in");
  } else {
    out += "USB, no battery";
  }
  out += "\n";

  const shtc3::Reading env = sensors::env();
  if (env.ok) {
    char buf[64];
    snprintf(buf, sizeof buf, "Room: %.1f%s, %.0f%% humidity\n", units::temperature(env.tempC),
             units::temperatureUnit(), env.rh);
    out += buf;
  }

  const audio::Status a = audio::status();
  out += "Volume: " + std::to_string(a.volume) + " of 100\n";
  if (a.state != audio::State::Stopped) {
    out += std::string(a.state == audio::State::Paused ? "Paused: " : "Playing: ") + a.title;
    if (!a.artist.empty()) out += " by " + a.artist;
    out += "\n";
  }

  if (const Note *note = noteOnScreen()) {
    out += "On screen: the note \"" + note->title + "\" (read_note has its text)\n";
  } else if (const ui::Screen *screen = ui::nav::base()) {
    const std::string context = screen->context();
    out += "On screen: " + screen->tag() + (context.empty() ? "" : " - " + context) + "\n";
  }

  out += "Alarms:";
  for (int i = 0; i < alarms::kCount; ++i) out += "\n  " + alarmLine(i);
  out += "\n";

  std::string path, title;
  readstate::Position pos;
  if (readstate::last(path, title, pos)) {
    out += "Last book: " + title + ", " + std::to_string(pos.percent) + "% read (" + path + ")";
  }
  return {out};
}

// --- weather ---------------------------------------------------------------------------------

std::string localTime(time_t t, const char *format = "%H:%M") {
  tm local;
  localtime_r(&t, &local);
  char buf[32];
  strftime(buf, sizeof buf, format, &local);
  return buf;
}

std::string temp(float celsius) {
  char buf[16];
  snprintf(buf, sizeof buf, "%.0f%s", units::temperature(celsius), units::temperatureUnit());
  return buf;
}

std::string wind(float kmh, uint16_t degrees) {
  char buf[32];
  snprintf(buf, sizeof buf, "%.0f %s from the %s", units::windSpeed(kmh), units::windUnit(),
           weather::compass(degrees));
  return lower(buf);
}

std::string rainChance(uint8_t percent) {
  return percent == weather::kUnknown ? "" : ", " + std::to_string(percent) + "% chance of rain";
}

Result readWeather() {
  if (!weather::configured()) return fail("the board has no location set for a forecast");
  std::unique_ptr<weather::Forecast> f(new weather::Forecast);  // 2 KB: not on the stack
  if (!weather::get(*f)) return fail("the board hasn't fetched a forecast yet");
  const time_t now = time(nullptr);
  char where[48];
  snprintf(where, sizeof where, "%.2f, %.2f", f->lat, f->lon);
  std::string out = std::string("Forecast for ") + where + " (Open-Meteo), fetched " +
                    localTime(f->fetched, "%a %d %b %H:%M");
  if (now - f->fetched > 3 * 3600) out += " (old: the board hasn't been online since)";
  out += ".\n";

  if (const weather::Hour *h = weather::at(*f, now)) {
    out += "Now: " + temp(h->tempC) + " (feels like " + temp(h->feelsC) + "), " +
           lower(weather::condition(h->code)) + ", wind " + wind(h->windKmh, h->windDeg) +
           ", humidity " + std::to_string(h->rh) + "%" + rainChance(h->rain) + "\n";
  }

  const weather::Hour *hours[12];
  const int hourCount = weather::hoursAfter(*f, now, hours, 12);
  if (hourCount > 0) out += "Next hours:\n";
  for (int i = 0; i < hourCount; ++i) {
    const weather::Hour &h = *hours[i];
    out += "  " + localTime(h.time) + " " + temp(h.tempC) + ", " + lower(weather::condition(h.code)) +
           rainChance(h.rain) + "\n";
  }

  const weather::Day *days[weather::kDays];
  const int dayCount = weather::daysFrom(*f, now, days, weather::kDays);
  if (dayCount > 0) out += "Days:\n";
  for (int i = 0; i < dayCount; ++i) {
    const weather::Day &d = *days[i];
    char amount[24] = "";
    if (d.precipMm > 0.05f) {
      snprintf(amount, sizeof amount, ", %.*f %s", units::precipitationDecimals(),
               units::precipitation(d.precipMm), lower(units::precipitationUnit()).c_str());
    }
    out += "  " + localTime(d.date, "%a %d %b") + ": " + temp(d.loC) + " to " + temp(d.hiC) + ", " +
           lower(weather::condition(d.code)) + rainChance(d.rain) + amount + ", wind up to " +
           wind(d.windMaxKmh, d.windDeg);
    if (i == 0 && d.sunrise && d.sunset) {
      out += ", sunrise " + localTime(d.sunrise) + ", sunset " + localTime(d.sunset);
    }
    out += "\n";
  }
  return {out};
}

// --- set_volume, set_alarm -------------------------------------------------------------------

Result setVolume(const cJSON *in) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(in, "volume");
  if (!cJSON_IsNumber(v)) return fail("volume (0 to 100) is required");
  const int want = std::max(0, std::min(100, static_cast<int>(v->valuedouble)));
  audio::changeVolume(want - audio::status().volume);
  return {"Volume is " + std::to_string(audio::status().volume) + "."};
}

Result setAlarm(const cJSON *in) {
  const cJSON *onItem = cJSON_GetObjectItemCaseSensitive(in, "on");
  const bool on = !cJSON_IsBool(onItem) || cJSON_IsTrue(onItem);
  const std::string when = text(in, "time");
  int hour = -1, minute = -1;
  if (!when.empty() && (siscanf(when.c_str(), "%d:%d", &hour, &minute) != 2 || hour < 0 ||
                        hour > 23 || minute < 0 || minute > 59)) {
    return fail("time must be HH:MM, 24-hour");
  }
  const cJSON *slotItem = cJSON_GetObjectItemCaseSensitive(in, "slot");
  int slot = cJSON_IsNumber(slotItem) ? static_cast<int>(slotItem->valuedouble) - 1 : -1;
  if (cJSON_IsNumber(slotItem) && (slot < 0 || slot >= alarms::kCount)) {
    return fail("slot is 1, 2 or 3");
  }
  if (slot < 0 && hour >= 0) {  // the alarm already at that time
    for (int i = 0; i < alarms::kCount && slot < 0; ++i) {
      const alarms::Alarm a = alarms::get(i);
      if (a.hour == hour && a.minute == minute) slot = i;
    }
  }

  if (!on) {
    if (slot < 0) return fail("say which alarm to turn off: its slot or its time");
    alarms::Alarm a = alarms::get(slot);
    a.on = false;
    alarms::set(slot, a);
    return {"Alarm " + alarmLine(slot)};
  }
  if (hour < 0) return fail("time is required");
  for (int i = 0; i < alarms::kCount && slot < 0; ++i) {
    if (!alarms::get(i).on) slot = i;
  }
  if (slot < 0) return fail("all three alarms are on: name the slot to replace (board_status)");

  const std::string repeat = text(in, "repeat");
  uint8_t days = alarms::kOnce;
  if (repeat == "weekdays") {
    days = alarms::kWeekdays;
  } else if (repeat == "weekend") {
    days = alarms::kWeekend;
  } else if (repeat == "every_day") {
    days = alarms::kEveryDay;
  } else if (!repeat.empty() && repeat != "once") {
    return fail("repeat is once, weekdays, weekend or every_day");
  }
  alarms::set(slot, {static_cast<uint8_t>(hour), static_cast<uint8_t>(minute), days, true});
  return {"Alarm " + alarmLine(slot)};
}

// --- show ------------------------------------------------------------------------------------

Result show(const cJSON *in, uint32_t turn) {
  const std::string screen = text(in, "screen");
  std::string what = "The " + screen + " screen";
  std::function<void()> open;
  if (screen == "home") {
    open = [] {};
  } else if (screen == "clock") {
    open = [] { push(makeClock()); };
  } else if (screen == "weather") {
    open = [] { push(makeWeather()); };
  } else if (screen == "music") {
    open = [] { push(makeMusic()); };
  } else if (screen == "podcasts") {
    open = [] { push(makePodcasts()); };
  } else if (screen == "sensors") {
    open = [] { push(makeSensors()); };
  } else if (screen == "reader") {
    std::string path, title;
    readstate::Position pos;
    if (!readstate::last(path, title, pos)) return fail("no book has been read yet");
    open = [] { push(resumeLastBook()); };
  } else if (screen == "note") {
    Note note;
    if (!lastNote(note)) return fail("no note has been put up yet (show_note does that)");
    what = "The note \"" + note.title + "\"";
    open = [] {
      Note last;
      if (lastNote(last)) push(makeNote(std::move(last), false, false));
    };
  } else {
    return fail("screen is one of home, clock, weather, music, podcasts, sensors, reader, note");
  }
  later(turn, std::move(open));
  return {what + " opens when your reply has finished playing."};
}

// --- show_note, read_note --------------------------------------------------------------------

Result showNote(const cJSON *in, uint32_t turn) {
  Note note;
  note.title = text(in, "title");
  note.text = text(in, "text");
  if (note.text.find_first_not_of(" \t\r\n") == std::string::npos) {
    return fail("text is required: the whole answer, in Markdown");
  }
  std::string err;
  const bool kept = saveNote(note, err);  // cuts it to 16 KB first
  if (!kept) log_w("agent: a note not kept: %s", err.c_str());
  std::string out = "\"" + note.title + "\" goes on the screen when your reply has finished playing";
  out += kept ? ", and is kept in the board's notes." : ". It couldn't be kept: " + err + ".";
  if (note.cut) out += " It was longer than 16 KB, so it's cut off there.";
  later(turn, [note = std::move(note)] { push(makeNote(note, false, true)); });
  return {out};
}

Result readNoteTool() {
  Note last;
  const Note *note = noteOnScreen();
  if (!note) {
    if (!lastNote(last)) return fail("there's no note: none has been put up yet");
    note = &last;
  }
  std::string out = "Note \"" + note->title + "\"";
  tm made;
  localtime_r(&note->made, &made);
  if (made.tm_year + 1900 >= 2024) out += ", made " + localTime(note->made, "%a %d %b %H:%M");
  out += noteOnScreen() ? ", on the screen now" : ", not on the screen now";
  out += ":\n\n";
  // A note from the card can be longer than an answer may be (a frame holds 64 KB).
  if (note->text.size() > kMaxNoteBytes) {
    size_t at = kMaxNoteBytes;
    while (at > 0 && (static_cast<uint8_t>(note->text[at]) & 0xC0) == 0x80) --at;
    out += note->text.substr(0, at) + "\n\n[... the rest is cut off here]";
  } else {
    out += note->text;
  }
  return {out};
}

Result run(const Call &call) {
  cJSON *in = call.input;
  if (!cJSON_IsObject(in)) {
    cJSON_Delete(in);
    return fail("the input must be a JSON object");
  }
  Result r;
  if (call.name == "find_books") {
    r = findBooks(in);
  } else if (call.name == "open_book") {
    r = openBook(in, call.turn);
  } else if (call.name == "board_status") {
    r = boardStatus();
  } else if (call.name == "weather") {
    r = readWeather();
  } else if (call.name == "set_volume") {
    r = setVolume(in);
  } else if (call.name == "set_alarm") {
    r = setAlarm(in);
  } else if (call.name == "show") {
    r = show(in, call.turn);
  } else if (call.name == "show_note") {
    r = showNote(in, call.turn);
  } else if (call.name == "read_note") {
    r = readNoteTool();
  } else {
    r = fail("this board has no tool named " + call.name);
  }
  cJSON_Delete(in);
  return r;
}

// Opens the screen a tool asked for, once its turn's reply has played. A turn cut short (talking
// again, the sheet closed, the connection lost) drops it.
void applyChange() {
  if (!change) return;
  const agent::Status s = agent::status();
  if (s.turn != change->turn || s.state == agent::State::Off || s.state == agent::State::Error) {
    log_i("agent: a screen change dropped (the turn was cut short)");
    change.reset();
    return;
  }
  if (s.state != agent::State::Ready || s.mic) return;
  const std::function<void()> open = std::move(change->open);
  change.reset();
  ui::nav::home();  // closes the agent sheet, and with it the session
  open();
}

}  // namespace

void beginBoardTools() {
  if (lock) return;
  lock = xSemaphoreCreateMutex();
  agent::setTools(kDefinitions, onCall);
}

void pollBoardTools() {
  if (!lock) return;
  std::vector<Call> ready;
  xSemaphoreTake(lock, portMAX_DELAY);
  ready.swap(calls);
  xSemaphoreGive(lock);
  for (const Call &call : ready) {
    const uint32_t t0 = millis();
    const Result r = run(call);
    log_i("agent: tool %s -> %s%s (%u ms)", call.name.c_str(), r.error ? "error: " : "",
          r.text.substr(0, 120).c_str(), static_cast<unsigned>(millis() - t0));
    agent::toolResult(call.id, r.text, r.error);
  }
  applyChange();
}

}  // namespace apps

#else  // no agent in this build

namespace apps {
void beginBoardTools() {}
void pollBoardTools() {}
}  // namespace apps

#endif  // INKDECK_AGENT
