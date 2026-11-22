#include "kv_store.h"

#include <Preferences.h>

namespace kv {

uint32_t loadU32(const char *ns, const char *key) {
  Preferences prefs;
  if (!prefs.begin(ns, true)) return 0;  // read-only fails until the namespace exists
  const uint32_t v = prefs.getUInt(key, 0);
  prefs.end();
  return v;
}

void saveU32(const char *ns, const char *key, uint32_t v) {
  Preferences prefs;
  if (!prefs.begin(ns, false)) return;
  if (prefs.getUInt(key, ~v) != v) prefs.putUInt(key, v);
  prefs.end();
}

bool loadBytes(const char *ns, const char *key, void *out, size_t size) {
  Preferences prefs;
  if (!prefs.begin(ns, true)) return false;
  const bool ok = prefs.getBytesLength(key) == size && prefs.getBytes(key, out, size) == size;
  prefs.end();
  return ok;
}

void saveBytes(const char *ns, const char *key, const void *v, size_t size) {
  Preferences prefs;
  if (!prefs.begin(ns, false)) return;
  prefs.putBytes(key, v, size);
  prefs.end();
}

}  // namespace kv
