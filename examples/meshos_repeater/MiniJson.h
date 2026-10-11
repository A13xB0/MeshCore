#pragma once

// Minimal readers for flat JSON objects - enough for ConfigSerializer files and
// the firmware manifest, without pulling in a JSON library. They find the first
// occurrence of "key" anywhere in the text, so only use them on simple objects.

#include <Arduino.h>

namespace minijson {

// Returns a pointer to the start of key's value, or NULL if the key is absent.
inline const char* findValue(const char* json, const char* key) {
  char quoted_key[40];
  snprintf(quoted_key, sizeof(quoted_key), "\"%s\"", key);
  const char* p = strstr(json, quoted_key);
  if (!p) return NULL;
  p = strchr(p + strlen(quoted_key), ':');
  if (!p) return NULL;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

// Copies a string value (no escape handling). False if absent or not a string.
inline bool getString(const char* json, const char* key, char* out, size_t out_size) {
  const char* p = findValue(json, key);
  if (!p || *p != '"') return false;
  p++;
  size_t n = 0;
  while (*p && *p != '"' && n < out_size - 1) out[n++] = *p++;
  out[n] = 0;
  return true;
}

// Reads an unsigned number value; 0 if absent.
inline uint32_t getUint(const char* json, const char* key) {
  const char* p = findValue(json, key);
  return p ? (uint32_t) strtoul(p, NULL, 10) : 0;
}

}  // namespace minijson
