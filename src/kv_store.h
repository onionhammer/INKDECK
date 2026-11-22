// Small records in NVS (Preferences), for what an off face keeps between its RTC wakes: numbers,
// and fixed-size structs read back only when their size still matches. Named kv_store, not nvs,
// beside ESP-IDF's own nvs_* names.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace kv {

uint32_t loadU32(const char *ns, const char *key);  // 0 if it isn't there
// Writes only if it changes (NVS wears; the off faces save every minute).
void saveU32(const char *ns, const char *key, uint32_t v);

// The bytes at key into out, if there are exactly `size` of them.
bool loadBytes(const char *ns, const char *key, void *out, size_t size);
void saveBytes(const char *ns, const char *key, const void *v, size_t size);

template <typename T>
bool load(const char *ns, const char *key, T &out) {
  return loadBytes(ns, key, &out, sizeof(T));
}
template <typename T>
void save(const char *ns, const char *key, const T &v) {
  saveBytes(ns, key, &v, sizeof(T));
}

}  // namespace kv
