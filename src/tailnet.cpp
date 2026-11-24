#include "tailnet.h"

#include "features.h"
#include "settings.h"

#if INKDECK_TAILNET  // the rest of the file, but the stubs at its end (features.h)

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <lwip/api.h>
#include <lwip/ip_addr.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#include "microlink.h"
#include "ml_inkdeck.h"  // lib/microlink/port: wake a peer, ask whether its session is up
#include "pmic.h"
#include "text.h"
#include "wlan.h"

namespace tailnet {
namespace {

// After the last user lets go, the board stays on the tailnet this long: the agent sheet opened
// again, or the next screen of a share, finds it there instead of joining again.
constexpr uint32_t kLingerMs = 60000;
// How often reach() asks again for a handshake that hasn't come (MicroLink's TCP API does the
// same).
constexpr uint32_t kWakeAgainMs = 5000;

// NVS: "keyid", the auth key the identity is from; "derp", the relay region the board calls home;
// "lan", a "<host>\t<ip>,<ip>" line per host with the machine's LAN addresses (connectLan()).
constexpr const char *kNamespace = "tailnet";
// How long the first join of a boot may take to join again from another relay region.
constexpr uint32_t kRegionRejoinMs = 15000;
// The USB watcher (begin()): how often it looks at the power source (pmic::status() caches it
// for 5 s anyway), and how long it waits after a join that failed: a minute, or 15 for a key the
// control plane refused, which won't change until a new one is on the card.
constexpr uint32_t kUsbPollMs = 5000;
constexpr uint32_t kUsbRetryMs = 60000;
constexpr uint32_t kUsbRefusedRetryMs = 15 * 60000;
constexpr int kMaxLanHosts = 8;      // remembered in "lan"; the oldest goes
constexpr int kMaxLanAddresses = 4;  // kept, and tried at once, per host

std::mutex joinMutex;   // held through a join and a stop, so a second caller waits for it
std::mutex stateMutex;  // guards what follows
microlink_t *ml = nullptr;  // made at the first join, kept for the boot: started and stopped
bool running = false;       // started and not stopped since
bool wifi = false;          // the tailnet's own wlan::connect()
int users = 0;              // hold()s that succeeded, not yet released
uint32_t generation = 0;    // changes with every hold and release: a linger that sees another
                            // one has been overtaken
std::string lastError;      // why the last join failed; "" once one succeeds
bool regionSettled = false; // this boot's first join has checked the relay region

// ml while it runs, for the resolver hook and the status calls, which take no lock. The handle
// itself is never freed, so a stale copy is harmless.
std::atomic<microlink_t *> live{nullptr};

// MicroLink keeps the config's string pointers, so they live here for the boot.
std::string authKey;
std::string deviceName;

// --- Helpers -----------------------------------------------------------------------------------

// The machines' LAN addresses (connectLan()), newest first, as NVS keeps them.
std::mutex lanMutex;  // guards what follows
std::vector<std::pair<std::string, std::string>> lanHosts;  // host, "a.b.c.d,e.f.g.h"
bool lanLoaded = false;

// The task asking the LAN's DNS (lanDns()): the resolver hook leaves its lookups alone.
std::mutex lanDnsMutex;
std::atomic<TaskHandle_t> lanLookupTask{nullptr};

using text::lower;

bool endsWith(const std::string &s, const char *tail) {
  const size_t n = strlen(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// A dotted IPv4 address in host byte order; 0 if `s` isn't one.
uint32_t parseIp(const std::string &s) {
  unsigned a, b, c, d;
  char extra;
  if (siscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return 0;
  if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
  return a << 24 | b << 16 | c << 8 | d;
}

std::string ipText(uint32_t ip) {  // host byte order
  char buf[16];
  snprintf(buf, sizeof buf, "%u.%u.%u.%u", static_cast<unsigned>(ip >> 24),
           static_cast<unsigned>(ip >> 16 & 0xFF), static_cast<unsigned>(ip >> 8 & 0xFF),
           static_cast<unsigned>(ip & 0xFF));
  return buf;
}

// A name as the tailnet's list has it: no trailing dot. "" if it can't be one.
std::string bareName(const char *name) {
  std::string s = name ? name : "";
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s.size() < 64 ? s : std::string();  // microlink_peer_info_t's hostname[64]
}

// Under lanMutex.
void loadLan() {
  if (lanLoaded) return;
  lanLoaded = true;
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return;
  const std::string text = prefs.isKey("lan") ? prefs.getString("lan", "").c_str() : "";
  prefs.end();
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    const std::string line = text.substr(pos, nl - pos);
    const size_t tab = line.find('\t');
    if (tab != std::string::npos) lanHosts.push_back({line.substr(0, tab), line.substr(tab + 1)});
    pos = nl + 1;
  }
}

// Under lanMutex.
void saveLan() {
  std::string text;
  for (const auto &[host, addrs] : lanHosts) text += host + "\t" + addrs + "\n";
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    prefs.putString("lan", text.c_str());
    prefs.end();
  }
}

// The key a host is remembered by: what the settings name, lower case, no trailing dot.
std::string lanKey(const std::string &host) { return lower(bareName(host.c_str())); }

// How likely a private address is the machine's LAN one, lower first: on the board's own /16,
// then 192.168/16, 10/8, and 172.16/12 last, where Docker puts its bridges (a server lists
// several, before its LAN address: found on the board 2026-11-21).
int lanRank(uint32_t ip, uint32_t own) {
  const int range = (ip >> 16) == 0xC0A8 ? 1 : (ip >> 24) == 10 ? 2 : 3;
  return (own && (ip >> 16) == (own >> 16) ? 0 : range * 2) + ((ip >> 20) == 0xAC1);
}

// The address the LAN's own DNS gives a name, in host byte order; 0 for none, an address, a
// MagicDNS name (the LAN's DNS can't know it) or a tailnet address.
uint32_t lanDns(const std::string &host) {
  const std::string name = bareName(host.c_str());
  if (name.empty() || parseIp(name) || endsWith(lower(name), ".ts.net")) return 0;
  std::lock_guard<std::mutex> g(lanDnsMutex);
  lanLookupTask = xTaskGetCurrentTaskHandle();
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  const int rc = getaddrinfo(name.c_str(), nullptr, &hints, &res);
  lanLookupTask = nullptr;
  if (rc != 0 || !res) return 0;
  const uint32_t ip = ntohl(reinterpret_cast<const sockaddr_in *>(res->ai_addr)->sin_addr.s_addr);
  freeaddrinfo(res);
  return (ip & 0xFFC00000u) == 0x64400000u ? 0 : ip;
}

// Keeps the LAN addresses the map gives for the peer at vpnIp, if they changed.
void rememberLan(microlink_t *m, const std::string &host, uint32_t vpnIp) {
  uint32_t found[8];  // all a peer can have (ML_MAX_ENDPOINTS)
  const int n = ml_inkdeck_peer_lan_addresses(m, vpnIp, found, 8);
  const std::string key = lanKey(host);
  if (n == 0 || key.empty()) return;
  const uint32_t own = ntohl(WiFi.localIP());
  std::stable_sort(found, found + n,
                   [own](uint32_t a, uint32_t b) { return lanRank(a, own) < lanRank(b, own); });
  std::string addrs;
  for (int i = 0; i < std::min(n, kMaxLanAddresses); ++i) addrs += (i ? "," : "") + ipText(found[i]);
  std::lock_guard<std::mutex> g(lanMutex);
  loadLan();
  const auto known = std::find_if(lanHosts.begin(), lanHosts.end(),
                                  [&](const auto &h) { return h.first == key; });
  if (known != lanHosts.end() && known->second == addrs && known == lanHosts.begin()) return;
  if (known != lanHosts.end()) lanHosts.erase(known);
  lanHosts.insert(lanHosts.begin(), {key, addrs});
  if (static_cast<int>(lanHosts.size()) > kMaxLanHosts) lanHosts.resize(kMaxLanHosts);
  saveLan();
  log_i("tailnet: %s's LAN addresses: %s", key.c_str(), addrs.c_str());
}

// Which auth key the board's identity came from, without keeping the key itself twice: FNV-1a.
std::string keyId(const std::string &key) {
  char buf[9];
  snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(text::fnv1a(key)));
  return buf;
}

// A different auth key from the one the board joined with: the board joins again as a new
// machine, so its old keys go first (microlink_factory_reset() wants that before init).
void forgetIdentityIfNewKey() {
  const std::string id = keyId(authKey);
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  const std::string had = prefs.isKey("keyid") ? prefs.getString("keyid", "").c_str() : "";
  if (had != id) {
    if (!had.empty()) log_i("tailnet: a new auth key: the board joins as a new machine");
    microlink_factory_reset();
    prefs.putString("keyid", id.c_str());
  }
  prefs.end();
}

uint16_t storedRegion() {
  Preferences prefs;
  uint16_t r = 0;
  if (prefs.begin(kNamespace, true)) {  // read-only: fails until the first join creates it
    r = prefs.getUShort("derp", 0);
    prefs.end();
  }
  return r;
}

void storeRegion(uint16_t r) {
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    if (prefs.getUShort("derp", 0) != r) prefs.putUShort("derp", r);
    prefs.end();
  }
}

// Under joinMutex.
microlink_t *create() {
  authKey = settings::tailscaleKey();
  deviceName = settings::tailscaleName();
  forgetIdentityIfNewKey();
  // The relay region the last boot settled on (settleRegion()); upstream's 9 the first time.
  if (const uint16_t r = storedRegion()) ml_inkdeck_derp_region = r;
  microlink_config_t cfg = {};
  cfg.auth_key = authKey.c_str();
  cfg.device_name = deviceName.c_str();
  cfg.enable_derp = cfg.enable_stun = cfg.enable_disco = true;
  // The agent daemon by address: keep it a WireGuard slot on a tailnet with more machines than
  // there are slots (CONFIG_ML_MAX_PEERS).
  cfg.priority_peer_ip = parseIp(settings::agentHost());
  // tailscale_exit_node = on: the board offers itself as an exit node (MicroLink adds ::/0, as
  // Tailscale wants both, but forwards IPv4 only: port/microlink_kconfig.h). Read here, so like
  // the name it changes at the next boot: the instance is kept for this one.
  if (settings::tailscaleExitNode()) cfg.advertise_routes = "0.0.0.0/0";
  return microlink_init(&cfg);
}

// Leaves the tailnet and lets the radio go. Under joinMutex.
void stopNow() {
  microlink_t *m;
  {
    std::lock_guard<std::mutex> g(stateMutex);
    m = running ? ml : nullptr;
    running = false;
    live = nullptr;
  }
  if (m) {
    const esp_err_t err = microlink_stop(m);
    if (err != ESP_OK) {
      // A task still in a network call: the next start stops it again first.
      log_w("tailnet: stop: %s", esp_err_to_name(err));
    } else {
      log_i("tailnet: left");
    }
  }
  if (wifi) {
    wifi = false;
    wlan::disconnect();
  }
}

// The board forwards the tailnet's internet traffic now (an exit node, approved or not).
bool exitNode() {
  microlink_t *m = live;
  return m && microlink_is_connected(m) && ml_inkdeck_exit_node(m);
}

// On USB power the board holds the tailnet; unplugged, it lets go (and leaves kLingerMs later).
// An exit node on USB also keeps Wi-Fi's modem sleep off: forwarded traffic is everything another
// machine sends, and modem sleep slows transfers several times over (ota.cpp, webdav.cpp). Those
// and the agent client turn it back on when they finish; the next look here turns it off again.
void usbTask(void *) {
  bool held = false;
  bool awake = false;                 // modem sleep off for the exit node
  uint32_t waitFrom = 0, waitMs = 0;  // after a failed join
  for (;;) {
    const pmic::Status p = pmic::status();
    const bool onUsb = p.ok && p.vbus;
    if (onUsb && !held && millis() - waitFrom >= waitMs) {
      std::string error;
      held = hold(30000, error);
      if (held) {
        log_i("tailnet: on USB: staying on the tailnet");
        waitMs = 0;
      } else {
        waitFrom = millis();
        waitMs = error.find("KEY") != std::string::npos ? kUsbRefusedRetryMs : kUsbRetryMs;
        log_w("tailnet: on USB, but %s; trying again in %lu min", error.c_str(),
              static_cast<unsigned long>(waitMs / 60000));
      }
    } else if (!onUsb && held) {
      held = false;
      if (awake) {
        awake = false;
        WiFi.setSleep(true);
      }
      release();
      log_i("tailnet: off USB: leaving the tailnet unless something else needs it");
    } else if (!onUsb) {
      waitMs = 0;  // plugging in again tries at once
    }
    if (held && exitNode() && WiFi.getSleep() != WIFI_PS_NONE) {
      if (!awake) log_i("tailnet: an exit node on USB: Wi-Fi modem sleep off");
      awake = true;
      WiFi.setSleep(false);
    }
    vTaskDelay(pdMS_TO_TICKS(kUsbPollMs));
  }
}

const char *stateError(microlink_state_t s) {
  switch (s) {
    case ML_STATE_AUTH_FAILED: return "TAILSCALE KEY REFUSED";
    case ML_STATE_NEEDS_LOGIN: return "TAILSCALE WANTS A NEW KEY";  // a used one-off key
    default: return nullptr;
  }
}

// Waits for MicroLink to be on the tailnet, or to say it won't be, until timeoutMs after t0.
microlink_state_t waitForJoin(uint32_t t0, uint32_t timeoutMs) {
  microlink_state_t state = microlink_get_state(ml);
  while (state != ML_STATE_CONNECTED && !stateError(state) && millis() - t0 < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(100));
    state = microlink_get_state(ml);
  }
  return state;
}

// The relay region the board's machines can hear it on: the agent host's, if that's on the
// tailnet, else the one most machines use. 0 if the map doesn't say.
uint16_t wantedRegion() {
  const std::string host = settings::agentHost();
  uint32_t ip = parseIp(host);
  if (!ip && routes(host)) ip = microlink_resolve(ml, bareName(host.c_str()).c_str());
  if (const uint16_t r = ml_inkdeck_peer_region(ml, ip)) return r;
  return ml_inkdeck_common_region(ml);
}

// The first join of a boot, once on the tailnet: moves the board's relay home to where its
// machines are, and joins again from there. MicroLink keeps one relay connection, to its home
// region, and relays to every peer through it, so a peer homed elsewhere never hears a relayed
// packet; until a direct path turns up there's no session (the board in upstream's default
// Dallas, the agent host in Chicago: found on the board 2026-11-21). The region is kept in NVS,
// so later boots start there. Under joinMutex, with nobody else on the tailnet. Returns the state
// after it.
microlink_state_t settleRegion(microlink_state_t state, uint32_t t0, uint32_t timeoutMs) {
  if (regionSettled || state != ML_STATE_CONNECTED || !ml_inkdeck_derp_auto()) return state;
  regionSettled = true;
  const uint16_t want = wantedRegion();
  const uint16_t have = ml_inkdeck_home_region(ml);
  if (!want) return state;
  storeRegion(want);
  if (want == have) return state;
  log_i("tailnet: relay region %u, where the board's machines are, not %u: joining again from there",
        static_cast<unsigned>(want), static_cast<unsigned>(have));
  ml_inkdeck_derp_region = want;
  microlink_stop(ml);
  // The instance keeps the home region it had: without this the new one is never read.
  ml_inkdeck_forget_home_region(ml);
  if (microlink_start(ml) != ESP_OK) return ML_STATE_ERROR;
  return waitForJoin(t0, std::max(timeoutMs, millis() - t0 + kRegionRejoinMs));
}

// Leaves, unless someone has held the tailnet since the release that asked (`arg`, its
// generation).
void leaveUnlessHeld(void *arg) {
  const uint32_t gen = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
  std::lock_guard<std::mutex> join(joinMutex);
  bool leave;
  {
    std::lock_guard<std::mutex> g(stateMutex);
    leave = users == 0 && generation == gen;
  }
  vTaskDelete(nullptr);
  if (leave) stopNow();
}

// The last user's linger: leaves kLingerMs on, unless someone held the tailnet meanwhile.
void lingerTask(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(kLingerMs));
  leaveUnlessHeld(arg);
  vTaskDelete(nullptr);
}

// release(false): leaves now, in a task of its own, since stopping MicroLink can take seconds.
void leaveTask(void *arg) {
  leaveUnlessHeld(arg);
}

// join()'s hold, made in a task of its own.
std::mutex backgroundMutex;  // guards what follows
int backgroundWanted = 0;    // join()s not yet matched by a leave()
bool backgroundJoining = false;
bool backgroundHeld = false;  // its hold() succeeded and hasn't been released

void backgroundTask(void *) {
  std::string error;
  const bool on = hold(30000, error);
  bool keep = false;
  {
    std::lock_guard<std::mutex> g(backgroundMutex);
    backgroundJoining = false;
    keep = on && backgroundWanted > 0;
    backgroundHeld = keep;
  }
  if (on && !keep) release();  // left while it joined
  vTaskDelete(nullptr);
}

}  // namespace

bool configured() { return settings::hasTailscale(); }

void begin() {
  static bool started = false;
  if (started || !configured()) return;
  started = xTaskCreate(usbTask, "tailnet-usb", 4096, nullptr, 1, nullptr) == pdPASS;
}

bool routes(const std::string &host) {
  if (host.empty() || !configured()) return false;
  if (const uint32_t ip = parseIp(host)) return (ip & 0xFFC00000u) == 0x64400000u;  // 100.64/10
  const std::string h = lower(bareName(host.c_str()));
  return !h.empty() && (endsWith(h, ".ts.net") || h.find('.') == std::string::npos);
}

bool hold(uint32_t timeoutMs, std::string &error) {
  if (!configured()) {
    error = "NO TAILSCALE KEY";
    return false;
  }
  const uint32_t t0 = millis();
  std::lock_guard<std::mutex> join(joinMutex);
  {
    std::lock_guard<std::mutex> g(stateMutex);
    if (running && microlink_is_connected(ml)) {
      ++users;
      ++generation;
      return true;
    }
  }
  if (!wifi) {
    if (!wlan::connect(timeoutMs, error)) return false;
    wifi = true;
  }
  if (!ml) ml = create();
  if (!ml) {
    error = "TAILSCALE DIDN'T START";
    stopNow();
    return false;
  }
  if (!running) {
    esp_err_t err = microlink_start(ml);
    if (err == ESP_ERR_INVALID_STATE) {  // the last stop timed out: finish it, then start
      microlink_stop(ml);
      err = microlink_start(ml);
    }
    if (err != ESP_OK) {
      log_w("tailnet: start: %s; internal heap %u free", esp_err_to_name(err),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
      error = "TAILSCALE DIDN'T START";
      stopNow();
      return false;
    }
    std::lock_guard<std::mutex> g(stateMutex);
    running = true;
    live = ml;
  }
  microlink_state_t state = waitForJoin(t0, timeoutMs);
  bool alone;
  {
    std::lock_guard<std::mutex> g(stateMutex);
    alone = users == 0;
  }
  if (alone) state = settleRegion(state, t0, timeoutMs);
  const bool on = state == ML_STATE_CONNECTED;
  bool first = false, leave = false;
  {
    std::lock_guard<std::mutex> g(stateMutex);
    if (on) {
      lastError.clear();
      first = users++ == 0;
      ++generation;
    } else {
      error = stateError(state) ? stateError(state) : "TAILNET TIMED OUT";
      lastError = error;
      leave = users == 0;  // nobody else is on it: don't keep trying
    }
  }
  if (first) {
    log_i("tailnet: on as %s (%s) in %lu ms, %d machines; internal heap %u free", name().c_str(),
          ipText(microlink_get_vpn_ip(ml)).c_str(), static_cast<unsigned long>(millis() - t0),
          microlink_get_peer_count(ml),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  } else if (!on) {
    log_w("tailnet: %s after %lu ms (state %d)", error.c_str(),
          static_cast<unsigned long>(millis() - t0), static_cast<int>(state));
  }
  if (leave) stopNow();
  return on;
}

void release(bool linger) {
  uint32_t gen;
  {
    std::lock_guard<std::mutex> g(stateMutex);
    if (users <= 0) return;
    gen = ++generation;
    if (--users > 0) return;
  }
  if (xTaskCreate(linger ? lingerTask : leaveTask, "tailnet-off", 3072,
                  reinterpret_cast<void *>(gen), 1, nullptr) != pdPASS) {
    std::lock_guard<std::mutex> join(joinMutex);  // no task for it: leave now, from this one
    stopNow();
  }
}

void join() {
  if (!configured()) return;
  std::lock_guard<std::mutex> g(backgroundMutex);
  if (++backgroundWanted > 1 || backgroundJoining || backgroundHeld) return;
  backgroundJoining = true;
  if (xTaskCreate(backgroundTask, "tailnet-join", 4096, nullptr, 1, nullptr) != pdPASS) {
    backgroundJoining = false;
    log_w("tailnet: no memory for the join task");
  }
}

void leave() {
  bool drop = false;
  {
    std::lock_guard<std::mutex> g(backgroundMutex);
    if (backgroundWanted == 0 || --backgroundWanted > 0) return;
    drop = backgroundHeld;
    backgroundHeld = false;
  }
  if (drop) release();
}

bool reach(const std::string &host, uint32_t timeoutMs) {
  microlink_t *m = live;
  if (!m || !microlink_is_connected(m)) return false;
  uint32_t ip = parseIp(host);
  if (!ip) ip = microlink_resolve(m, bareName(host.c_str()).c_str());
  if (!ip) return false;
  rememberLan(m, host, ip);  // for connectLan() next time
  if (ml_inkdeck_peer_is_up(m, ip)) return true;
  const uint32_t t0 = millis();
  uint32_t wokeAt = t0;
  ml_inkdeck_wake_peer(m, ip);
  while (millis() - t0 < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (ml_inkdeck_peer_is_up(m, ip)) {
      log_i("tailnet: session with %s up in %lu ms", host.c_str(),
            static_cast<unsigned long>(millis() - t0));
      return true;
    }
    if (millis() - wokeAt >= kWakeAgainMs) {
      wokeAt = millis();
      ml_inkdeck_wake_peer(m, ip);
    }
  }
  // Relayed packets reach only peers homed in the board's relay region (settleRegion()).
  log_w("tailnet: no session with %s after %lu ms (its relay region %u, the board's %u)",
        host.c_str(), static_cast<unsigned long>(timeoutMs),
        static_cast<unsigned>(ml_inkdeck_peer_region(m, ip)),
        static_cast<unsigned>(ml_inkdeck_home_region(m)));
  return false;
}

int connectLan(const std::string &host, uint16_t port, uint32_t timeoutMs, std::string *address) {
  std::vector<uint32_t> addrs;
  if (const uint32_t ip = lanDns(host)) addrs.push_back(ip);
  {
    std::lock_guard<std::mutex> g(lanMutex);
    loadLan();
    for (const auto &[key, list] : lanHosts) {
      if (key != lanKey(host)) continue;
      for (size_t pos = 0; pos < list.size();) {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        const uint32_t ip = parseIp(list.substr(pos, comma - pos));
        if (ip && std::find(addrs.begin(), addrs.end(), ip) == addrs.end()) addrs.push_back(ip);
        pos = comma + 1;
      }
    }
  }
  if (addrs.empty()) return -1;
  // A non-blocking connect to each; the first to complete wins.
  const uint32_t t0 = millis();
  std::vector<std::pair<int, uint32_t>> tries;  // socket, address
  int winner = -1;
  uint32_t winnerIp = 0;
  for (const uint32_t ip : addrs) {
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) continue;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0) {
      winner = fd;
      winnerIp = ip;
      break;
    }
    if (errno == EINPROGRESS) {
      tries.push_back({fd, ip});
    } else {
      ::close(fd);
    }
  }
  while (winner < 0 && !tries.empty() && millis() - t0 < timeoutMs) {
    fd_set w;
    FD_ZERO(&w);
    int top = -1;
    for (const auto &[fd, ip] : tries) {
      FD_SET(fd, &w);
      top = std::max(top, fd);
    }
    const uint32_t left = timeoutMs - (millis() - t0);
    timeval tv = {static_cast<time_t>(left / 1000), static_cast<suseconds_t>(left % 1000 * 1000)};
    if (select(top + 1, nullptr, &w, nullptr, &tv) <= 0) break;
    for (auto it = tries.begin(); it != tries.end();) {
      if (!FD_ISSET(it->first, &w)) {
        ++it;
        continue;
      }
      int err = 0;
      socklen_t len = sizeof err;
      getsockopt(it->first, SOL_SOCKET, SO_ERROR, &err, &len);
      if (err == 0 && winner < 0) {
        winner = it->first;
        winnerIp = it->second;
      } else {
        ::close(it->first);
      }
      it = tries.erase(it);
    }
  }
  for (const auto &[fd, ip] : tries) {
    if (fd != winner) ::close(fd);
  }
  const std::string key = lanKey(host);
  if (winner < 0) {
    log_i("tailnet: %s isn't on this LAN (%u addresses tried, %lu ms)", key.c_str(),
          static_cast<unsigned>(addrs.size()), static_cast<unsigned long>(millis() - t0));
    return -1;
  }
  fcntl(winner, F_SETFL, fcntl(winner, F_GETFL, 0) & ~O_NONBLOCK);
  log_i("tailnet: %s answered on the LAN at %s in %lu ms", key.c_str(), ipText(winnerIp).c_str(),
        static_cast<unsigned long>(millis() - t0));
  if (address) *address = ipText(winnerIp);
  return winner;
}

std::string address() {
  microlink_t *m = live;
  if (!m || !microlink_is_connected(m)) return "";
  const uint32_t ip = microlink_get_vpn_ip(m);
  return ip ? ipText(ip) : "";
}

std::string name() { return settings::tailscaleName(); }

std::string describe() {
  if (!configured()) return "not set";
  const std::string who = name();
  microlink_t *m = live;
  if (m) {
    if (microlink_is_connected(m)) {
      return "on \xC2\xB7 " + who + " \xC2\xB7 " + address() +
             (ml_inkdeck_exit_node(m) ? " \xC2\xB7 exit node" : "");
    }
    return "joining \xC2\xB7 " + who;
  }
  std::lock_guard<std::mutex> g(stateMutex);
  return (lastError.empty() ? std::string("off") : lastError) + " \xC2\xB7 " + who;
}

}  // namespace tailnet

// MagicDNS on the board: lwIP asks this before DNS for every getaddrinfo() (ESP-IDF's
// LWIP_HOOK_NETCONN_EXTERNAL_RESOLVE, whose default in liblwip.a is a weak one that declines).
// While the board is on the tailnet, a name on its list of machines (short, or the full
// *.ts.net one) answers with the machine's address. Anything else goes on to DNS.
extern "C" int lwip_hook_netconn_external_resolve(const char *name, ip_addr_t *addr,
                                                  u8_t addrtype, err_t *err) {
  microlink_t *m = tailnet::live;
  if (!m || !name || !addr || !err) return 0;
  if (xTaskGetCurrentTaskHandle() == tailnet::lanLookupTask) return 0;  // asking the LAN (lanDns())
  if (addrtype == NETCONN_DNS_IPV6) return 0;  // the tailnet's addresses here are IPv4
  if (!microlink_is_connected(m)) return 0;
  const std::string bare = tailnet::bareName(name);
  if (bare.empty()) return 0;
  const uint32_t ip = microlink_resolve(m, bare.c_str());
  if (!ip) return 0;
  IP_ADDR4(addr, ip >> 24, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF);
  *err = ERR_OK;
  return 1;
}

#else  // a build without the tailnet: nothing is on it, and MicroLink isn't linked

namespace tailnet {

bool configured() { return false; }
void begin() {}
bool routes(const std::string &) { return false; }
bool hold(uint32_t, std::string &error) {
  error = "NO TAILNET IN THIS BUILD";
  return false;
}
void release(bool) {}
void join() {}
void leave() {}
bool reach(const std::string &, uint32_t) { return false; }
int connectLan(const std::string &, uint16_t, uint32_t, std::string *) { return -1; }
std::string address() { return ""; }
std::string name() { return settings::tailscaleName(); }
std::string describe() { return "not in this build"; }

}  // namespace tailnet

#endif  // INKDECK_TAILNET
