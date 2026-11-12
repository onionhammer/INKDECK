# Network and persistence: Wi-Fi provisioning, SNTP, HTTP services, NVS, SPIFFS, components

Vendor code at commit `9b12d40`. Paths are relative to `ESP-IDF/08_ESP32-S3_e-Paper-3.97/`. All
networking is in the IDF factory app. There is no Arduino networking example. In the snippets, comments
starting `//>` are mine. Everything else is upstream text, trimmed with `// ...`.

## Contents

1. [Upstream file map](#1-upstream-file-map)
2. [Boot and Wi-Fi on/off flow](#2-boot-and-wi-fi-onoff-flow)
3. [Provisioning: SoftAP + captive portal (esp-wifi-connect)](#3-provisioning-softap--captive-portal-esp-wifi-connect)
4. [Station connect](#4-station-connect)
5. [Time sync (SNTP → RTC) and time zones](#5-time-sync-sntp--rtc-and-time-zones)
6. [HTTP services: lunar calendar, weather, IP geolocation](#6-http-services-lunar-calendar-weather-ip-geolocation)
7. [NVS map](#7-nvs-map)
8. [SPIFFS and file caches](#8-spiffs-and-file-caches)
9. [Kconfig, partitions, managed components](#9-kconfig-partitions-managed-components)
10. [Porting notes](#10-porting-notes)
11. [Licensing](#11-licensing)

## 1. Upstream file map

Base URL: `https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/`

| What | Link |
| --- | --- |
| esp-wifi-connect README (NVS keys, usage) | [README.md#L42-L74](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/esp-wifi-connect/README.md#L42-L74) |
| Component manifest (v2.4.3, MIT, upstream repo) | [idf_component.yml#L1-L10](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/esp-wifi-connect/idf_component.yml#L1-L10) |
| SSID list in NVS (load/save/add) | [ssid_manager.cc#L7-L104](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/esp-wifi-connect/ssid_manager.cc#L7-L104) |
| SoftAP setup, SSID naming, advanced NVS keys | [wifi_configuration_ap.cc#L101-L199](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/esp-wifi-connect/wifi_configuration_ap.cc#L101-L199) |
| Web server endpoints, captive-portal redirects | [wifi_configuration_ap.cc#L201-L630](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/esp-wifi-connect/wifi_configuration_ap.cc#L201-L630) |
| Test-connect + save, AP event handler, Stop | [wifi_configuration_ap.cc#L632-L820](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/esp-wifi-connect/wifi_configuration_ap.cc#L632-L820) |
| Wildcard DNS server | [dns_server.cc#L16-L75](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/esp-wifi-connect/dns_server.cc#L16-L75) |
| Portal page JS (`/submit`) / done page (`/reboot`) | [wifi_configuration.html#L465-L490](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/esp-wifi-connect/assets/wifi_configuration.html#L465-L490), [wifi_configuration_done.html#L55-L81](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/esp-wifi-connect/assets/wifi_configuration_done.html#L55-L81) |
| STA: scan → match saved SSIDs → connect/retry | [wifi_station.cc#L24-L271](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/esp-wifi-connect/wifi_station.cc#L24-L271) |
| Unused alternative (`espwifi_Init`, never called) | [esp_wifi_bsp.c#L48-L174](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/esp_wifi_bsp/esp_wifi_bsp.c#L48-L174) |
| Wi-Fi enable flag, on/off, clear-and-reprovision | [page_network.cc#L48-L218](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_network/page_network.cc#L48-L218) |
| `page_network_init` / `_init_main` / `_init_mode` | [page_network.cc#L221-L570](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_network/page_network.cc#L221-L570) |
| Network menu key handling | [page_network.cc#L575-L662](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_network/page_network.cc#L575-L662) |
| Boot: NVS init, SPIFFS, mode flag, Wi-Fi flag | [main.cc#L438-L547](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L438-L547) |
| TZ table, NVS defines, lunar URL, SNTP | [page_clock.cc#L34-L135](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L34-L135) |
| TZ + sensor-cache NVS, `apply_timezone`, `page_clock_init` | [page_clock.cc#L172-L339](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L172-L339) |
| Clock/calendar RTC-wake modes (NTP resync) | [page_clock.cc#L454-L715](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L454-L715) |
| Lunar API fetch, month cache save/load | [page_clock.cc#L717-L1009](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L717-L1009), [#L1269-L1325](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L1269-L1325) |
| Pinned root CA for the lunar API | [api_root_cert.pem](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/api_root_cert.pem#L1-L31) |
| Weather/geo URLs (**API key on L57**), SPIFFS mount, city-code + NVS | [page_weather.cc#L36-L201](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_weather/page_weather.cc#L36-L201) |
| Weather fetch + JSON parse | [page_weather.cc#L204-L422](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_weather/page_weather.cc#L204-L422) |
| City lookup, AMap IP location, wake schedule, weather pages | [page_weather.cc#L425-L869](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_weather/page_weather.cc#L425-L869) |
| Weather icon lookup (SD → SPIFFS) | [page_weather.cc#L1002-L1054](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_weather/page_weather.cc#L1002-L1054) |
| Other NVS users | [pcf85063_bsp.c#L18-L35](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/pcf85063_bsp/pcf85063_bsp.c#L18-L35), [page_alarm.cc#L150-L167](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_alarm/page_alarm.cc#L150-L167), [page_fiction.cc#L108-L166](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_fiction/page_fiction.cc#L108-L166) |
| Component deps, SPIFFS image, cert embed | [main/CMakeLists.txt#L23-L52](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/CMakeLists.txt#L23-L52), [idf_component.yml#L1-L18](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/idf_component.yml#L1-L18), [dependencies.lock](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/dependencies.lock#L1-L46) |
| Kconfig menu, partition table | [Kconfig.projbuild#L1-L134](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/Kconfig.projbuild#L1-L134), [partitions.csv#L1-L6](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/partitions.csv#L1-L6) |

## 2. Boot and Wi-Fi on/off flow

`app_main`: `nvs_flash_init()` (plain `ESP_ERROR_CHECK`, no erase fallback) → SD → I2C → `spiffs_init()` → RTC →
PMIC → `load_mode_enable_from_nvs()`. `if(RTC_INT && mode!=0)`, where `RTC_INT` is `gpio_get_level(RTC_INT_PIN)`
and the pin reads HIGH when the alarm is *not* asserted: a user power-on resets mode to 0 in NVS. After an RTC-alarm
wake, mode 1/2/3 (clock/calendar/weather) runs a one-shot task that ends in `axp_pwr_off()`. In normal mode (0):

```cpp
wifi_enable = load_wifi_enable_from_nvs();          //> NVS "wifi_state"/"enable", default 0 = off
if (wifi_enable) {
    // ...
    EPD_Clear();
    vTaskDelay(pdMS_TO_TICKS(500));
    page_network_init_main();
} else {
    // ...
    ESP_ERROR_CHECK(safe_wifi_stop());
    ESP_ERROR_CHECK(safe_wifi_deinit());
}
```

So **Wi-Fi is off out of the box**. The Network menu (`page_handle_network_key_event`) offers, when off:
"turn on" → `wifi_set_enable(true)` (saves flag=1, then `page_network_init()`). When on: "turn off" →
`WifiStation::Stop()`, `esp_wifi_stop()`, destroy the `WIFI_STA_DEF` netif. "Clear Wi-Fi" →
`page_network_config()`: `SsidManager::Clear()`, show a message, 2 s, `esp_restart()`.

`page_network_init*()` all do: create the default event loop and `esp_netif_init()` once (tolerating
`ESP_ERR_INVALID_STATE`), register an `IP_EVENT_STA_GOT_IP` logger, `nvs_flash_init()` again (erasing on
`NO_FREE_PAGES`/`NEW_VERSION_FOUND`, which wipes every namespace), then branch:

- **No saved SSIDs** → provisioning AP (§3). `_init_main` calls `ap.SetSsidPrefix("ESP32")`. `page_network_init`
  (the menu path) doesn't, so its SSID would be `-XXXX` unless `_init_main` ran earlier in the same boot (inferred
  from `GetSsid()`'s format string). The loop then waits for `WIFI_CONFIG_DONE_BIT` or BOOT long-press (code 23 →
  turn Wi-Fi off + restart).
- **Saved SSIDs** → `WifiStation::GetInstance().Start()`, then poll `esp_wifi_sta_get_ap_info()` once per second
  for up to 31 s, then poll for a non-zero IP for up to 30 s (`_init_mode`: 10 s). On success `_init`/`_init_main`
  call `page_clock_init()` (NTP → RTC). On failure they show a message and return. The app doesn't retry NTP later,
  but `WifiStation` keeps rescanning and reconnecting in the background (§4).

## 3. Provisioning: SoftAP + captive portal (esp-wifi-connect)

The vendored copy of `78/esp-wifi-connect` v2.4.3 adds a `SetOnConfigDone()` hook (inferred to be a
Waveshare addition; the README doesn't mention it).

```cpp
std::string WifiConfigurationAp::GetSsid() {
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));        //> (non-P4 branch)
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X", ssid_prefix_.c_str(), mac[4], mac[5]);
    return std::string(ssid);
}
void WifiConfigurationAp::StartAccessPoint() {
    ESP_ERROR_CHECK(esp_netif_init());
    ap_netif_ = esp_netif_create_default_wifi_ap();
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif_);
    esp_netif_set_ip_info(ap_netif_, &ip_info);
    esp_netif_dhcps_start(ap_netif_);
    dns_server_.Start(ip_info.gw);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // ...
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    // ... then reads NVS "wifi": ota_url (str), max_tx_power (i8), remember_bssid (u8, default true)
}
```

So the AP is **open (no password), SSID `ESP32-XXXX`** (last two SoftAP MAC bytes in hex), at 192.168.4.1, in
APSTA mode so it can scan and test credentials while serving the page. `Start()` also scans right away and
rescans 10 s after each `SCAN_DONE` (`esp_timer_start_once(…, 10 * 1000000)`). The DNS task answers *every*
query with 192.168.4.1. Default UI language is `zh-CN` (`language_`). The page also has en, zh-TW and ja strings.

| Endpoint | Behaviour |
| --- | --- |
| `GET /` | Embedded `wifi_configuration.html` |
| `GET /scan` | JSON `[{"ssid","rssi","authmode"}…]` from the last scan |
| `GET /saved/list`, `/saved/set_default?index=n`, `/saved/delete?index=n` | Manage stored SSIDs |
| `POST /submit` | JSON `{"ssid","password"}` (≤1024 B). `ConnectToWifi()` tries STA for up to 10 s, disconnects, then `Save()` → `SsidManager::AddSsid()`. Replies `{"success":true}` or `{"success":false,"error":"无法连接到 WiFi"}` |
| `GET /done.html` | 3 s countdown, then JS `fetch('/reboot', {method:'POST'})` |
| `POST /reboot` | Replies, then a task waits 200 ms, `httpd_stop`, 100 ms, `esp_restart()` |
| `GET /advanced/config`, `POST /advanced/submit` | Read/write `ota_url`, `max_tx_power`, `remember_bssid` in NVS "wifi". **Only this handler calls `on_config_done_`** |
| `/hotspot-detect.html`, `/generate_204`, `/mobile/status.php`, `/check_network_status.txt`, `/ncsi.txt`, `/fwlink/`, `/connectivity-check.html`, `/success.txt`, `/portal.html`, `/library/test/success.html` | `302` → `http://192.168.4.1/?lang=<language_>` |

The normal path (enter SSID → success → done page) ends in a **reboot**, not the callback. On the next boot
`wifi_state.enable` is already 1 (saved before provisioning started), so `page_network_init_main()` takes the
saved-SSID branch. `HTTPD_DEFAULT_CONFIG` with `max_uri_handlers = 24` and wildcard matching. The vendor
`sdkconfig` raises `CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048` (phones send long captive-portal headers; inferred reason).
`StartSmartConfig()` (ESPTouch) exists but the app never calls it.

Credential storage (`ssid_manager.cc`): NVS namespace **`wifi`**, keys `ssid`, `ssid1`…`ssid9` and `password`,
`password1`…`password9` (strings, ≤32 / ≤64 chars), max 10 entries, newest inserted at index 0, an existing SSID
gets its password overwritten, and when the list is full the last entry is dropped. Passwords are plain-text NVS
strings (`# CONFIG_NVS_ENCRYPTION is not set`).

## 4. Station connect

```cpp
void WifiStation::Start() {
    ESP_ERROR_CHECK(esp_netif_init());
    // ... register WIFI_EVENT/ANY and IP_EVENT_STA_GOT_IP handlers
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }
    // ... esp_timer "WiFiScanTimer" -> esp_wifi_scan_start(nullptr, false)
}
```

`STA_START` → async scan. On `SCAN_DONE` it sorts APs by RSSI and queues every AP whose SSID is in the saved
list, then `StartConnect()` sets `ssid`/`password` (plus `bssid`/`channel` when `remember_bssid`) and calls
`esp_wifi_connect()`. `STA_DISCONNECTED`: up to `MAX_RECONNECT_COUNT` (5) reconnects, then the next queued
AP, then `esp_timer_start_once(timer_handle_, 10 * 1000)`. That is **10 ms, not 10 s** (esp_timer takes µs, and
the AP class uses `10 * 1000000`), so with no matching AP it rescans almost continuously. `GOT_IP` sets
`WIFI_EVENT_CONNECTED` and clears the queue. `WaitForConnected()`/`OnConnected()` exist, but the app polls
`esp_wifi_sta_get_ap_info()` instead. "Connected" in the app (`wifi_is_connected()`) means *associated*, not
*has IP*. Callers check the IP separately.

## 5. Time sync (SNTP → RTC) and time zones

```cpp
int get_time_from_ntp(struct tm *t) {
    esp_sntp_stop(); // Stop first to prevent repeated initialization
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp1.aliyun.com");
    esp_sntp_init();
    // ... up to 10 x (time(&now); localtime_r(&now, &timeinfo); if tm_year > 2020-1900 -> ok) with 500 ms delays
    esp_sntp_stop();
}
void page_clock_init(void) {
    int timezone_idx = load_timezone_index_from_nvs();
    apply_timezone(timezone_idx);                       //> setenv("TZ", tz_str, 1); tzset();
    // ... up to 30 attempts: get_time_from_ntp -> tm_to_timedata -> PCF85063_SetTime(rtc_time)
}
```

- The single server is `ntp1.aliyun.com` (Alibaba Cloud). Each attempt waits at most 5 s, but the vendor
  sdkconfig has `CONFIG_LWIP_SNTP_STARTUP_DELAY=y` with a max of 5000 ms, so one attempt can expire before the
  first request goes out (inferred). Worst case `page_clock_init` blocks for about 150 s (30 × 5 s).
- **The RTC holds local time.** `tm_to_timedata` stores `tm_year + 1900 − 2000`, month 1-based, `tm_wday`.
- TZ table: 25 entries "UTC-12"…"UTC+12" with POSIX strings (inverted sign, e.g. `{"UTC+8","CST-8",8,"东八区"}`,
  `{"UTC+9","JST-9",…}`). The default index is 20 (UTC+8). There is no DST support.
- **Bug: NVS namespace for the TZ index.** `#define NVS_NAMESPACE "clock_config"` is followed two lines later by
  `#define NVS_NAMESPACE "clock_th"`. The second definition wins, so `timezone_idx` (i32) actually lives in
  namespace **`clock_th`** next to the `th_data` blob.
- `page_calendar_show_mode()` hard-codes `setenv("TZ", "CST-8", 1)` before NTP and ignores the saved zone.
- Resync points: boot with Wi-Fi on; clock page forced refresh; **clock mode** (RTC wake every minute) only at 08:00
  via `page_network_init_mode()`; **calendar mode** on each wake that isn't an alarm (or at 00:00); **weather mode**
  after each weather fetch attempt (`page_clock_init()` inside `if(page_network_init_mode())`).

## 6. HTTP services: lunar calendar, weather, IP geolocation

All three are **China-only free services** with Chinese-language payloads. Treat them as replaceable.

| Service | Request | Transport | Used for |
| --- | --- | --- | --- |
| Lunar calendar | `GET https://api.mu-jie.cc/lunar?date=YYYY-MM-DD`, headers `User-Agent: Mozilla/5.0`, `Accept: application/json`, `Referer: https://api.mu-jie.cc/`, `timeout_ms = 3000` | HTTPS, `cert_pem` = embedded `api_root_cert.pem` (**ISRG Root X1**, valid to 2035-06-04) | Calendar page: one request per day of month (≤31, up to 6 tries each), cached per month |
| Weather | `GET http://t.weather.sojson.com/api/weather/city/<sojson city code>` | Plain HTTP, `esp_http_client_perform` + event handler into a 16 KB PSRAM buffer | Weather page/mode |
| IP geolocation | `GET http://restapi.amap.com/v3/ip?key=…` | Plain HTTP. **An AMap (Gaode) web-service key is hard-coded in `AMAP_IP_URL`, page_weather.cc L57** (not reproduced here) | Locate the router's public IP → province/city |

Lunar JSON: reads `data.{lunarDate, festival, lunarFestival, IMonthCn, IDayCn, gzYear, gzMonth, gzDay, ncWeek,
Term, astro}` into `LunarInfo` (fixed `char[16|32]`). The month is saved as a JSON array to
`/sdcard/lunar/YYYY-MM.json`, or `/spiffs/lunar_YYYY-MM.json` if `/sdcard` isn't mounted. It's fetched only when the
file is missing and Wi-Fi is up, or on Function long-press. Worst case 31 × 6 × 3 s ≈ 9 min of blocking (inferred).

Weather flow: the AMap call is made **twice**. First `amap_ip_location_fetch_city_code()` gets `adcode`, which is
only used as a success gate. Then `amap_ip_location_fetch_city_code_by_name()` takes `province`/`city`, strips
suffixes (`省 市 区 县 盟 自治区 自治州 特别行政区`), and looks up `province,city,code` lines in
`/sdcard/city_code.txt`, falling back to `/spiffs/city_code.txt` (447 lines, e.g. `北京,朝阳,101010300`). The
sojson response fields used: `time`, `cityInfo.{city,parent}`, `data.{wendu,shidu,quality}` and
`data.forecast[0..3].{ymd,type,high,low,fx,fl,sunrise,sunset,aqi}`. `high`/`low` are parsed with
`sscanf(high, "高温 %d℃", …)` / `"低温 %d℃"`. Icons: `type` (e.g. 晴, 多云) → `Weather.txt` (`晴,qin` …) →
`/sdcard/Weather_img/Weather_img/<name>.bmp`, else `/spiffs/Weather_img/<name>.bmp`.

Schedule: the source comment says sojson updates at 03/08/13/19 h with a 1 h CDN cache, so the app refreshes when
the hour becomes **4, 9, 14 or 20** (page) or arms the RTC for the next of those hours (weather mode,
`Wake_up_time_setting_weather`). Alarms take precedence. The interactive page also refreshes on Function
long-press (12), then powers off after `Unattended_Time` (10) minutes idle with mode=3 saved. The 1-hour in-RAM
cache in `weather_fetch_and_show_cached` is effectively unused, because both callers pass `force_refresh = true`.

## 7. NVS map

Default `nvs` partition (0x6000 in the vendor table). Every access is open → get/set → commit → close, with no locking.
`nvs_mutex` is declared in `main.cc` but never created.

| Namespace | Key | Type | Meaning / owner |
| --- | --- | --- | --- |
| `wifi` | `ssid`, `ssid1`…`ssid9`, `password`, `password1`…`password9` | str | Saved networks, index 0 = preferred (esp-wifi-connect) |
| `wifi` | `ota_url` / `max_tx_power` / `remember_bssid` | str / i8 / u8 | Portal "advanced" tab. `remember_bssid` locks STA to the scanned BSSID+channel |
| `wifi_state` | `enable` | u8 | Wi-Fi on at boot (default 0) (`page_network.cc`) |
| `mode_state` | `mode` | u8 | 0 normal, 1 clock, 2 calendar, 3 weather: RTC-wake one-shot mode (`pcf85063_bsp.c`) |
| `alarm` | `alarms` | blob | `Alarm[6]` = `{u8 hour, u8 minute, u8 enabled}` ×6 (`page_alarm.cc`) |
| `clock_th` | `th_data` | blob | `Clock_TH` (date/time, battery %, temperature, humidity), the last drawn values (inferred purpose: partial redraws) |
| `clock_th` | `timezone_idx` | i32 | Index into the TZ table (meant for `clock_config`; see §5 bug) |
| `fiction_cfg` | `font_idx` | i32 | Reader font size (`page_fiction.cc`) |
| `weather` | `city_code` | str | `save/load_city_code_*` are defined but **never called** |

Also, `CONFIG_ESP_WIFI_NVS_ENABLED=y`. The provisioning AP uses the default `wifi_init_config_t`, so the IDF Wi-Fi
driver persists its own config in NVS too. `WifiStation` sets `cfg.nvs_enable = false`.

## 8. SPIFFS and file caches

```c
esp_vfs_spiffs_conf_t conf = { .base_path = "/spiffs", .partition_label = NULL,
                               .max_files = 5, .format_if_mount_failed = true };
```

`main/CMakeLists.txt` builds the `storage` partition (spiffs, 0xF0000) from `main/page_weather/` with
`spiffs_create_partition_image(storage ${spiffs_partition_dir} FLASH_IN_PROJECT)`. So the image holds
`city_code.txt`, `Weather_img/Weather.txt`, `Weather_img/*.bmp`, and (inferred, since the whole directory is packed)
`page_weather.cc/.h`. Runtime writes go to `/spiffs/lunar_YYYY-MM.json`. `CONFIG_SPIFFS_OBJ_NAME_LEN=32`. Other
caches live on SD: `/sdcard/lunar/*.json` and `/sdcard/GUI/clock_partial.bin` (raw framebuffer saved before power-off
in weather mode, then reloaded).

## 9. Kconfig, partitions, managed components

- `Kconfig.projbuild` menu "ESP32 S3 e-Paper-3.97" has only **Font Options** (`FONT12/16/18/24/28/36/48_EMBEDDED`,
  `Auxiliary_Font_EMBEDDED`, `FONT_ENABLE_TFCARD`, `FONT_ENABLE_SDCARD`, `FONT_DEBUG_ENABLED`) and **Image Resources**
  (`IMG_SOURCE_EMBEDDED` | `IMG_SOURCE_TFCARD`). Shipped `sdkconfig`: fonts 12/16/18/24 + auxiliary embedded, TF fonts
  on, images embedded. There are no network or audio options.
- `partitions.csv`: `nvs 0x6000`, `phy_init 0x1000`, `factory app 15M`, `storage spiffs 0xF0000`. No OTA.
- `main/idf_component.yml`: `espressif/esp_codec_dev: ^1.3.6`, `chmorgan/esp-audio-player: '*'`.
  `dependencies.lock` resolves esp_codec_dev **1.3.6**, esp-audio-player **1.0.7**, esp-libhelix-mp3 **1.0.3**, IDF **5.4.1**.
- Local components used by networking: `esp-wifi-connect` (requires `esp_timer esp_http_server esp_wifi nvs_flash json`)
  and IDF `esp_http_client`, `spiffs`. `esp_wifi_bsp` is in `REQUIRES` but is dead code (`espwifi_Init()` is never
  called). It has hard-coded STA test credentials, a WPA2 AP `ESP32_Config` with a hard-coded password, and a
  "read from NVS" comment that was never implemented.

## 10. Porting notes

- **Arduino equivalents.** STA: `WiFi.mode(WIFI_STA); WiFi.begin(ssid, pass)`, or `WiFiMulti` for the multi-SSID list.
  Portal: core `WebServer` + `DNSServer` (both in core 3.3.x) reproduce §3's endpoints and redirects. The core's
  `WiFiProv` (IDF `wifi_provisioning`, SoftAP/BLE with the Espressif app) and the third-party WiFiManager library
  (not in the core) are alternatives. Persistence: `Preferences` reads the same NVS, e.g. `prefs.begin("wifi", true);
  prefs.getString("ssid")`. Both tables start NVS at 0x9000 (Waveshare's CSV leaves offsets blank, so the IDF
  default; inferred), but ours is 0x5000 vs their 0x6000, so don't count on stock-firmware credentials surviving.
  Namespace/key names must be ≤15 chars. Time: `configTzTime("CST-8", "pool.ntp.org")` + `getLocalTime(&tm, timeout_ms)`, then write the RTC.
  HTTP: `HTTPClient` + `NetworkClientSecure::setCACert()` or IDF `esp_http_client` directly. JSON: IDF `cJSON` ships in
  the core (`json` component), or use ArduinoJson.
- **Storage.** Our partition table has no SPIFFS partition (`app3M_fat9M_16MB.csv`: nvs, otadata, 2×3 MB OTA, 9.9 MB
  FFat, coredump). Put `city_code.txt`/icons/caches on FFat or the SD card instead of `/spiffs`.
- **Don't copy:** the AMap key; the 10 ms rescan timer; `nvs_flash_erase()` on init errors outside `app_main`
  (it silently drops Wi-Fi creds and alarms); the duplicate `NVS_NAMESPACE`; the use-after-free `json_buf` log on
  page_weather.cc L291; `DnsServer::Stop()`, which only logs (socket and task keep running); open AP without a password.
- **Blocking.** Connect waits (≤61 s), NTP (≤150 s), the lunar month fetch (minutes) and the weather fetch all run on the
  UI task with the e-paper idle. In an RTC-wake cycle, budget the whole connect → fetch → shutdown on battery, and bail
  out early.
- **China-only services.** sojson weather and the city-code list cover mainland China. AMap IP location returns empty
  fields outside China. mu-jie lunar is a hobby API. `ntp1.aliyun.com` works globally but `pool.ntp.org` is the
  natural default. Replace with Open-Meteo / your own endpoint and hold an explicit location in config.
- **TLS.** Only the lunar call pins a CA. ISRG Root X1 expires in 2035. Prefer the core's certificate bundle.

## 11. Licensing

- Waveshare files (`page_network.cc`, `page_weather.cc`, `page_clock.cc`, `esp_wifi_bsp.c`, `main.cc`) have **no
  license header**. The repo has no LICENSE file.
- `components/esp-wifi-connect`: `idf_component.yml` declares `license: MIT`, `repository: https://github.com/78/esp-wifi-connect`,
  `version: 2.4.3`. There is no LICENSE file or source headers in the vendored copy. Waveshare modified it (`on_config_done_`).
- Managed components: see `audio.md` §10 (esp_codec_dev / esp-audio-player / libhelix, Apache-2.0 per registry metadata).
- `api_root_cert.pem` is the public ISRG Root X1 certificate.
