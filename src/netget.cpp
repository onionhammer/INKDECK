#include "netget.h"

#include <Arduino.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <mbedtls/platform.h>

namespace net {
namespace {

constexpr int kMaxRedirects = 8;
constexpr int kTimeoutMs = 20000;

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

esp_http_client_handle_t handle(void *p) { return static_cast<esp_http_client_handle_t>(p); }

// Blocks of 512 bytes and up (record buffers, certificates) from PSRAM, small ones from internal
// RAM; either falls back to the other.
void *tlsCalloc(size_t n, size_t size) {
  constexpr uint32_t kInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  constexpr uint32_t kPsram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  const bool big = n * size >= 512;
  void *p = heap_caps_calloc(n, size, big ? kPsram : kInternal);
  return p ? p : heap_caps_calloc(n, size, big ? kInternal : kPsram);
}

void tlsFree(void *p) { heap_caps_free(p); }

}  // namespace

void begin() { mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree); }

bool Get::open(const std::string &url, std::string &error, int64_t from) {
  close();
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = kTimeoutMs;
  cfg.buffer_size = 4096;
  cfg.buffer_size_tx = 2048;  // tracking redirects make long request lines
  cfg.user_agent = "ESP32 podcast client/1.0";
  cfg.keep_alive_enable = true;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) {
    error = "NO MEMORY FOR HTTP";
    return false;
  }
  client_ = c;
  if (from > 0) {
    char range[40];
    snprintf(range, sizeof range, "bytes=%lld-", static_cast<long long>(from));
    esp_http_client_set_header(c, "Range", range);
  }
  for (int hop = 0; hop <= kMaxRedirects; ++hop) {
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
      log_w("net: open failed (%s); internal heap %u free, largest block %u",
            esp_err_to_name(err), heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      error = "NO CONNECTION";
      close();
      return false;
    }
    length_ = esp_http_client_fetch_headers(c);
    status_ = esp_http_client_get_status_code(c);
    if (!isRedirect(status_)) break;
    if (hop == kMaxRedirects || esp_http_client_set_redirection(c) != ESP_OK) {
      error = "TOO MANY REDIRECTS";
      close();
      return false;
    }
    // A fresh connection for the next hop: it's often another host, and the body of the
    // redirect is never wanted.
    esp_http_client_close(c);
  }
  if (length_ < 0 || esp_http_client_is_chunked_response(c)) length_ = -1;
  if ((status_ >= 200 && status_ < 300) || status_ == 416) return true;
  log_w("net: HTTP %d", status_);
  error = "SERVER SAID " + std::to_string(status_);
  close();
  return false;
}

int Get::read(char *buf, int len) {
  if (!client_) return -1;
  const int n = esp_http_client_read(handle(client_), buf, len);
  if (n == 0 && !esp_http_client_is_complete_data_received(handle(client_)) && length_ > 0) {
    return -1;  // the connection ended early
  }
  return n;
}

void Get::close() {
  if (!client_) return;
  esp_http_client_close(handle(client_));
  esp_http_client_cleanup(handle(client_));
  client_ = nullptr;
}

}  // namespace net
