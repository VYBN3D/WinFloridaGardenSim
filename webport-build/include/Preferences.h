#pragma once
#include <Arduino.h>

// NVS shim backed by browser localStorage through the JS runtime.
class Preferences {
public:
  void begin(const char *name, bool ro = false) { (void)name; (void)ro; }
  void end() {}
  bool clear();
  bool remove(const char *key);
  size_t putUChar(const char *key, uint8_t v);
  uint8_t getUChar(const char *key, uint8_t def = 0);
  size_t putUShort(const char *key, uint16_t v);
  uint16_t getUShort(const char *key, uint16_t def = 0);
  size_t putUInt(const char *key, uint32_t v);
  uint32_t getUInt(const char *key, uint32_t def = 0);
  size_t putShort(const char *key, int16_t v);
  int16_t getShort(const char *key, int16_t def = 0);
  size_t putBool(const char *key, bool v);
  bool getBool(const char *key, bool def = false);
  size_t putString(const char *key, const char *v);
  size_t putString(const char *key, const String &v) { return putString(key, v.c_str()); }
  String getString(const char *key, const char *def = "");
  size_t putBytes(const char *key, const void *data, size_t len);
  size_t getBytes(const char *key, void *data, size_t len);
  size_t getBytesLength(const char *key);
};
