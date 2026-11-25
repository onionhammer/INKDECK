// Stand-ins for framework code the linker would otherwise pull in for nothing.
//
// The Arduino core, ESP-IDF and the C library come prebuilt with every feature on, and the
// linker follows every reference: one call this firmware never makes can bring in a whole
// subsystem. A definition here is linked first (src/ comes before the libraries), so the
// library's own object file is never extracted. Each entry says what it keeps out and why the
// board never gets there. The sizes are firmware.bin's, each measured by adding the stub alone
// (2026-11-24); together they are 58.7 KB.
//
// A stub that fails, rather than doing the real thing, is only for code nothing reaches, and
// "the firmware never uses that feature" isn't enough to know it: the framework's own start-up
// may. lwIP's DHCP server looked like one (3.6 KB, for a soft-AP the board never runs), but the
// Arduino core creates the AP's interface along with the station's whenever Wi-Fi starts, and
// with a dhcps_new() that returned nothing esp_netif asserted there, at boot. Read the caller
// before adding one, then boot the result.
//
// Not here: a __wrap_log_printf to bypass ESP Insights' log hook (2.2 KB). The framework links
// with --wrap=log_printf, and GNU ld moves a wrapper's symbol twice when the object that defines
// it also calls the wrapped function, which every LTO partition of src/ does: the wrapper ends
// up pointing into the middle of other code (scripts/check_calls.py). No __wrap_ of anything
// src/ calls can be defined in src/.
//
// When the framework is updated: a stub whose library object gets linked for another reason
// fails the build with "multiple definition" (a symbol the object also defines is now wanted:
// add it here, or drop the stub), and one whose name the framework changed stops saving
// anything, so check firmware.bin's size after an update. `used` keeps LTO from dropping them
// before the linker has seen the libraries' references.
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <chip-debug-report.h>
#include <esp32-hal-i2c-slave.h>
#include <esp_err.h>
#include <mbedtls/ssl.h>

#include "features.h"

#define STUB __attribute__((used))

extern "C" {

// --- The C library --------------------------------------------------------------------------

// newlib's strftime() formats its numbers with sniprintf(), the integer-only printf, and nothing
// else in the image uses that engine: a second vfprintf for one caller. The full one is linked
// anyway, and prints an integer the same. 9.8 KB.
STUB int sniprintf(char *buffer, size_t size, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(buffer, size, format, args);
  va_end(args);
  return n;
}

#if INKDECK_TAILNET
// siscanf() is the integer-only scanner, which the firmware calls to keep the float-capable one
// out (AGENTS.md gotcha 10). MicroLink's sscanf() calls link that one regardless, so with the
// tailnet built in the integer one is the second copy, and siscanf() goes to the one that's
// there. 7.9 KB. Without the tailnet nothing links the float scanner, and siscanf() is newlib's.
STUB int siscanf(const char *text, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int n = vsscanf(text, format, args);
  va_end(args);
  return n;
}
#endif

// --- The Arduino core -----------------------------------------------------------------------

// Wire.cpp refers to the I2C slave driver for TwoWire::begin(address). The board is the bus's
// master and nothing else (pmic, rtc, the sensors, the codec). 7.0 KB.
STUB esp_err_t i2cSlaveAttachCallbacks(uint8_t, i2c_slave_request_cb_t, i2c_slave_receive_cb_t,
                                       void *) {
  return ESP_ERR_NOT_SUPPORTED;
}
STUB esp_err_t i2cSlaveInit(uint8_t, int, int, uint16_t, uint32_t, size_t, size_t) {
  return ESP_ERR_NOT_SUPPORTED;
}
STUB esp_err_t i2cSlaveDeinit(uint8_t) { return ESP_OK; }
STUB size_t i2cSlaveWrite(uint8_t, const uint8_t *, uint32_t, uint32_t) { return 0; }

// --- ESP-IDF --------------------------------------------------------------------------------

// esp_netif's lwIP glue calls these for a PPP interface (a cellular modem), and they bring in
// lwIP's PPP stack: LCP, IPCP, PAP, VJ compression. The board's only interfaces are Wi-Fi and
// the tailnet's WireGuard one, so nothing reaches them. 21.6 KB. The types are esp_netif's own
// (esp_netif_lwip_ppp.h, not installed with the framework): pointers and an enum.
STUB void *esp_netif_new_ppp(void *, const void *) { return nullptr; }
STUB esp_err_t esp_netif_start_ppp(void *) { return ESP_ERR_NOT_SUPPORTED; }
STUB esp_err_t esp_netif_stop_ppp(void *) { return ESP_ERR_NOT_SUPPORTED; }
STUB void esp_netif_destroy_ppp(void *) {}
STUB esp_err_t esp_netif_ppp_set_default_netif(void *) { return ESP_ERR_NOT_SUPPORTED; }
STUB esp_err_t esp_netif_ppp_set_auth_internal(void *, int, const char *, const char *) {
  return ESP_ERR_NOT_SUPPORTED;
}
STUB esp_err_t esp_netif_lwip_ppp_input(void *, void *, size_t, void *) {
  return ESP_ERR_NOT_SUPPORTED;
}

// mbedTLS is built with the TLS server side, and its handshake dispatcher refers to it. The
// board is only ever a TLS client: its own servers (Wi-Fi Drive, the updater) are plain HTTP on
// the LAN and the tailnet, and the agent's link is Noise. 8.0 KB.
STUB int mbedtls_ssl_handshake_server_step(mbedtls_ssl_context *) {
  return MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE;
}

}  // extern "C"

#if CORE_DEBUG_LEVEL < 4
// The core's loopTask() prints a chip report around setup() when shouldPrintChipDebugReport()
// says so, which this firmware never does (at CORE_DEBUG_LEVEL 4 and up it always prints, and
// these step aside). Empty, they keep chip-debug-report.cpp out. 4.4 KB.
STUB void printBeforeSetupInfo(void) {}
STUB void printAfterSetupInfo(void) {}
#endif
