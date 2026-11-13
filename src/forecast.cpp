#include "forecast.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <math.h>
#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

#include "pmic.h"
#include "power.h"
#include "rtc.h"
#include "settings.h"  // the location
#include "wlan.h"

namespace weather {
namespace {

constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr int kHttpTimeoutMs = 15000;
constexpr size_t kMaxBody = 32 * 1024;  // a reply is about 6 KB
constexpr time_t kHour = 3600;
constexpr time_t kRetryAfter = 15 * 60;  // after a failed automatic sync

constexpr const char *kNamespace = "weather";
constexpr const char *kKey = "forecast";
constexpr uint32_t kVersion = 2;  // bump when Forecast changes

// The fields asked for. Times come back as Unix seconds; daily dates are midnight in the
// location's own time zone (timezone=auto).
constexpr const char *kQuery =
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,"
    "wind_speed_10m,wind_direction_10m,is_day"
    "&hourly=temperature_2m,apparent_temperature,relative_humidity_2m,"
    "precipitation_probability,weather_code,wind_speed_10m,wind_direction_10m,is_day"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,"
    "precipitation_probability_max,precipitation_sum,wind_speed_10m_max,"
    "wind_direction_10m_dominant"
    "&timezone=auto&timeformat=unixtime&forecast_days=7&forecast_hours=48";

std::mutex forecastMutex;  // guards cache and loaded
Forecast cache;
bool loaded = false;

std::atomic<Phase> phase{Phase::Idle};
std::mutex resultMutex;
std::string result;
time_t finishedAt = 0;
std::atomic<time_t> lastAttempt{0};  // RAM, so a reboot retries at once

struct Stored {
  uint32_t version;
  uint32_t size;
  Forecast forecast;
};

void loadLocked() {
  if (loaded) return;
  loaded = true;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  if (prefs.getBytesLength(kKey) == sizeof(Stored)) {
    auto stored = std::make_unique<Stored>();
    prefs.getBytes(kKey, stored.get(), sizeof(Stored));
    if (stored->version == kVersion && stored->size == sizeof(Forecast)) {
      cache = stored->forecast;
    }
  }
  prefs.end();
}

void store(const Forecast &f) {
  {
    std::lock_guard<std::mutex> lock(forecastMutex);
    cache = f;
    loaded = true;
  }
  auto stored = std::make_unique<Stored>();
  stored->version = kVersion;
  stored->size = sizeof(Forecast);
  stored->forecast = f;
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    if (prefs.putBytes(kKey, stored.get(), sizeof(Stored)) != sizeof(Stored)) {
      log_w("weather: couldn't save the forecast to NVS");
    }
    prefs.end();
  }
}

void finish(Phase p, std::string text) {
  {
    std::lock_guard<std::mutex> lock(resultMutex);
    result = std::move(text);
    finishedAt = time(nullptr);
  }
  phase = p;
}

// --- HTTP.

esp_err_t onEvent(esp_http_client_event_t *e) {
  if (e->event_id == HTTP_EVENT_ON_DATA) {
    auto *body = static_cast<std::string *>(e->user_data);
    if (body->size() + e->data_len <= kMaxBody) {
      body->append(static_cast<const char *>(e->data), e->data_len);
    }
  }
  return ESP_OK;
}

bool fetch(std::string &body, std::string &error) {
  char url[768];
  // Two decimals is about a kilometre: plenty for a forecast, and all the service needs.
  snprintf(url, sizeof url,
           "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f%s",
           settings::latitude(), settings::longitude(), kQuery);
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.event_handler = onEvent;
  cfg.user_data = &body;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = kHttpTimeoutMs;
  cfg.buffer_size_tx = 1024;  // the request line is about 600 bytes; the default is 512
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    error = "NO MEMORY FOR HTTPS";
    return false;
  }
  const esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (err != ESP_OK) {
    log_w("weather: request failed (%s)", esp_err_to_name(err));
    error = err == ESP_ERR_HTTP_CONNECT ? "NO ANSWER FROM OPEN-METEO" : "REQUEST FAILED";
    return false;
  }
  if (status != 200) {
    log_w("weather: HTTP %d: %.200s", status, body.c_str());
    error = "OPEN-METEO SAID " + std::to_string(status);
    return false;
  }
  return true;
}

// --- JSON.

// A field of an object: the value itself (index < 0) or element `index` of an array.
const cJSON *field(const cJSON *object, const char *key, int index) {
  const cJSON *node = cJSON_GetObjectItemCaseSensitive(object, key);
  return index < 0 ? node : cJSON_GetArrayItem(node, index);
}

double number(const cJSON *object, const char *key, int index, double fallback) {
  const cJSON *node = field(object, key, index);
  return cJSON_IsNumber(node) ? node->valuedouble : fallback;
}

uint8_t percent(const cJSON *object, const char *key, int index) {
  const double v = number(object, key, index, -1);
  return v < 0 ? kUnknown : static_cast<uint8_t>(lround(fmin(v, 100)));
}

Hour readHour(const cJSON *object, int index) {
  Hour h;
  h.time = static_cast<time_t>(number(object, "time", index, 0));
  h.tempC = static_cast<float>(number(object, "temperature_2m", index, NAN));
  h.feelsC = static_cast<float>(number(object, "apparent_temperature", index, NAN));
  h.windKmh = static_cast<float>(number(object, "wind_speed_10m", index, NAN));
  h.windDeg = static_cast<uint16_t>(lround(number(object, "wind_direction_10m", index, 0))) % 360;
  const uint8_t rh = percent(object, "relative_humidity_2m", index);
  h.rh = rh == kUnknown ? 0 : rh;
  h.rain = percent(object, "precipitation_probability", index);
  h.code = static_cast<uint8_t>(number(object, "weather_code", index, 0));
  h.day = number(object, "is_day", index, 1) != 0;
  return h;
}

Day readDay(const cJSON *object, int index) {
  Day d;
  d.date = static_cast<time_t>(number(object, "time", index, 0));
  d.loC = static_cast<float>(number(object, "temperature_2m_min", index, NAN));
  d.hiC = static_cast<float>(number(object, "temperature_2m_max", index, NAN));
  d.precipMm = static_cast<float>(number(object, "precipitation_sum", index, 0));
  d.windMaxKmh = static_cast<float>(number(object, "wind_speed_10m_max", index, NAN));
  d.windDeg =
      static_cast<uint16_t>(lround(number(object, "wind_direction_10m_dominant", index, 0))) % 360;
  d.rain = percent(object, "precipitation_probability_max", index);
  d.code = static_cast<uint8_t>(number(object, "weather_code", index, 0));
  d.sunrise = static_cast<time_t>(number(object, "sunrise", index, 0));
  d.sunset = static_cast<time_t>(number(object, "sunset", index, 0));
  return d;
}

bool parse(const std::string &body, Forecast &f, std::string &error) {
  cJSON *root = cJSON_ParseWithLength(body.data(), body.size());
  if (!root) {
    error = "UNREADABLE REPLY";
    return false;
  }
  const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
  const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
  const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
  bool ok = cJSON_IsObject(current) && cJSON_IsObject(hourly) && cJSON_IsObject(daily);
  if (ok) {
    f.current = readHour(current, -1);
    f.hourCount = 0;
    const int hours = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(hourly, "time"));
    for (int i = 0; i < hours && f.hourCount < kHours; ++i) f.hours[f.hourCount++] = readHour(hourly, i);
    f.dayCount = 0;
    const int days = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(daily, "time"));
    for (int i = 0; i < days && f.dayCount < kDays; ++i) f.days[f.dayCount++] = readDay(daily, i);
    ok = f.current.time != 0 && !isnan(f.current.tempC) && f.hourCount > 0 && f.dayCount > 0;
  }
  cJSON_Delete(root);
  if (!ok) error = "INCOMPLETE REPLY";
  return ok;
}

// --- The sync task.

void run(void *) {
  const uint32_t t0 = millis();
  std::string error;
  if (!wlan::connect(kConnectTimeoutMs, error)) {
    finish(Phase::Failed, "WI-FI: " + error);
    vTaskDelete(nullptr);
    return;
  }
  phase = Phase::Fetching;
  std::string body;
  body.reserve(8 * 1024);
  bool ok = fetch(body, error);
  wlan::disconnect();
  auto f = std::make_unique<Forecast>();
  if (ok) ok = parse(body, *f, error);
  if (!ok) {
    finish(Phase::Failed, error);
    vTaskDelete(nullptr);
    return;
  }
  f->fetched = time(nullptr);
  f->lat = static_cast<float>(settings::latitude());
  f->lon = static_cast<float>(settings::longitude());
  store(*f);
  tm local;
  localtime_r(&f->fetched, &local);
  char text[32];
  strftime(text, sizeof text, "UPDATED %H:%M", &local);
  log_i("weather: %d hours, %d days, %u bytes in %lu ms", f->hourCount, f->dayCount,
        static_cast<unsigned>(body.size()), static_cast<unsigned long>(millis() - t0));
  finish(Phase::Done, text);
  vTaskDelete(nullptr);
}

time_t floorHour(time_t t) { return t - t % kHour; }

// The forecast is for the location set now (a card can move it): within about a kilometre.
bool forHere(const Forecast &f) {
  return fabsf(f.lat - static_cast<float>(settings::latitude())) < 0.01f &&
         fabsf(f.lon - static_cast<float>(settings::longitude())) < 0.01f;
}

}  // namespace

bool configured() { return settings::hasLocation() && wlan::configured(); }

void forget() {
  {
    std::lock_guard<std::mutex> lock(forecastMutex);
    cache = Forecast{};
    loaded = true;
  }
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    prefs.remove(kKey);
    prefs.end();
  }
  log_w("weather: forecast forgotten");
}

bool get(Forecast &out) {
  std::lock_guard<std::mutex> lock(forecastMutex);
  loadLocked();
  if (!cache.fetched || !forHere(cache)) return false;
  out = cache;
  return true;
}

const Hour *at(const Forecast &f, time_t t) {
  if (!f.fetched) return nullptr;
  if (t - f.fetched < kHour) return &f.current;
  for (int i = 0; i < f.hourCount; ++i) {
    if (f.hours[i].time <= t && t < f.hours[i].time + kHour) return &f.hours[i];
  }
  return nullptr;
}

int hoursAfter(const Forecast &f, time_t t, const Hour *out[], int max) {
  int n = 0;
  for (int i = 0; i < f.hourCount && n < max; ++i) {
    if (f.hours[i].time > t) out[n++] = &f.hours[i];
  }
  return n;
}

int daysFrom(const Forecast &f, time_t t, const Day *out[], int max) {
  tm local;
  localtime_r(&t, &local);
  local.tm_hour = local.tm_min = local.tm_sec = 0;
  local.tm_isdst = -1;
  const time_t midnight = mktime(&local);
  int n = 0;
  for (int i = 0; i < f.dayCount && n < max; ++i) {
    // By its noon, so a time zone that differs a little from .env's still lands on its day.
    if (f.days[i].date + 12 * kHour >= midnight) out[n++] = &f.days[i];
  }
  return n;
}

const char *condition(uint8_t code) {
  switch (code) {
    case 0: return "CLEAR";
    case 1: return "MOSTLY CLEAR";
    case 2: return "PARTLY CLOUDY";
    case 3: return "OVERCAST";
    case 45: return "FOG";
    case 48: return "FREEZING FOG";
    case 51: return "LIGHT DRIZZLE";
    case 53: return "DRIZZLE";
    case 55: return "HEAVY DRIZZLE";
    case 56:
    case 57: return "FREEZING DRIZZLE";
    case 61: return "LIGHT RAIN";
    case 63: return "RAIN";
    case 65: return "HEAVY RAIN";
    case 66:
    case 67: return "FREEZING RAIN";
    case 71: return "LIGHT SNOW";
    case 73: return "SNOW";
    case 75: return "HEAVY SNOW";
    case 77: return "SNOW GRAINS";
    case 80: return "LIGHT SHOWERS";
    case 81: return "SHOWERS";
    case 82: return "HEAVY SHOWERS";
    case 85: return "SNOW SHOWERS";
    case 86: return "SNOW SQUALLS";
    case 95: return "THUNDERSTORM";
    case 96:
    case 99: return "STORM + HAIL";
    default: return "UNKNOWN";
  }
}

Sky sky(uint8_t code) {
  if (code <= 1) return Sky::Clear;
  if (code == 2) return Sky::Partly;
  if (code == 3) return Sky::Cloud;
  if (code == 45 || code == 48) return Sky::Fog;
  if ((code >= 51 && code <= 57) || (code >= 80 && code <= 82)) return Sky::Showers;
  if (code >= 61 && code <= 67) return Sky::Rain;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return Sky::Snow;
  if (code >= 95) return Sky::Storm;
  return Sky::Cloud;
}

const char *compass(uint16_t degrees) {
  static const char *const kPoints[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return kPoints[((degrees % 360) * 2 + 45) / 90 % 8];
}

bool start() {
  const Phase p = phase.load();
  if (p == Phase::Connecting || p == Phase::Fetching) return false;
  phase = Phase::Connecting;
  lastAttempt = time(nullptr);
  // TLS and cJSON want more stack than the time sync; the reply and the tree go on the heap.
  if (xTaskCreate(run, "weather", 10240, nullptr, 1, nullptr) != pdPASS) {
    finish(Phase::Failed, "NO MEMORY FOR THE SYNC TASK");
    return false;
  }
  return true;
}

Status status() {
  Status s;
  s.phase = phase.load();
  if (s.phase == Phase::Done || s.phase == Phase::Failed) {
    std::lock_guard<std::mutex> lock(resultMutex);
    s.result = result;
    s.at = finishedAt;
  }
  return s;
}

bool syncing() {
  const Phase p = phase.load();
  return p == Phase::Connecting || p == Phase::Fetching;
}

time_t nextSync() {
  if (!configured() || !rtc::timeValid()) return 0;
  time_t fetched;
  {
    std::lock_guard<std::mutex> lock(forecastMutex);
    loadLocked();
    fetched = forHere(cache) ? cache.fetched : 0;
  }
  // On the hour after the last forecast; one fetched in the last quarter of an hour skips the
  // next top of the hour.
  const time_t due = fetched ? floorHour(fetched + 15 * 60) + kHour : 0;
  const time_t attempt = lastAttempt.load();
  if (attempt > fetched && phase.load() == Phase::Failed) return std::max(due, attempt + kRetryAfter);
  return due ? due : time(nullptr);
}

void poll() {
  static uint32_t lastCheckMs = 0;
  if (lastCheckMs && millis() - lastCheckMs < 1000) return;
  lastCheckMs = millis();
  if (syncing()) return;
  const time_t due = nextSync();
  const time_t now = time(nullptr);
  if (!due || now < due) return;
  const pmic::Status p = pmic::status();
  if (p.ok && p.battery && !p.vbus && p.percent >= 0 && p.percent < power::kLowBatteryPercent) {
    // Count it as a failed attempt, so poll() looks again in kRetryAfter, not every second.
    lastAttempt = now;
    finish(Phase::Failed, "BATTERY LOW: NO AUTOMATIC SYNC");
    return;
  }
  log_i("weather: hourly sync");
  start();
}

}  // namespace weather
