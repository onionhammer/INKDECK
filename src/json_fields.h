// Fields of a cJSON object, read leniently: a missing field or one of the wrong type gives the
// fallback (the agent client's messages, the board tools' inputs).
#pragma once
#include <cJSON.h>
#include <stdint.h>

#include <string>

namespace jsonf {

// A string field; "" otherwise.
inline std::string str(const cJSON *json, const char *name) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
  return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

// A number field as an int; fallback otherwise.
inline int integer(const cJSON *json, const char *name, int fallback = -1) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
  return cJSON_IsNumber(item) ? static_cast<int>(item->valuedouble) : fallback;
}

// Unix seconds; 0 for a missing, negative or non-number field.
inline uint32_t unixTime(const cJSON *json, const char *name) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
  return cJSON_IsNumber(item) && item->valuedouble > 0 ? static_cast<uint32_t>(item->valuedouble)
                                                       : 0;
}

inline bool flag(const cJSON *json, const char *name) {
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, name));
}

}  // namespace jsonf
