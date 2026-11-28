// Board-shim: browser HAL for WinFlorida Garden Sim (S3 4" CYD build).
// The game sketch compiles UNCHANGED; this header fakes the Arduino/ESP32 core.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>
#include <time.h>

typedef uint8_t byte;
typedef bool boolean;

#define PROGMEM
#define F(s) (s)
#define PSTR(s) (s)
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#define pgm_read_word(addr) (*(const uint16_t *)(addr))
#define pgm_read_dword(addr) (*(const uint32_t *)(addr))
#define pgm_read_float(addr) (*(const float *)(addr))

#define PI 3.14159265358979f
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2
#define TFT_BL 45
#define TFT_BACKLIGHT_ON HIGH

#define DEPRECATED_ATTR

// Arduino helper macros (same semantics as the core)
#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif
#ifndef abs
#define abs(x) ((x)>0?(x):-(x))
#endif
#ifndef constrain
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))
#endif
#ifndef map
#define map(x,in_min,in_max,out_min,out_max) ((x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min)
#endif
#define transpose(a, b) { int16_t t = a; a = b; b = t; }

// --- timing ------------------------------------------------------------------
unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void delayMicroseconds(unsigned long us);
void yield();

// --- random ------------------------------------------------------------------
long random(long howbig);
long random(long howsmall, long howbig);
void randomSeed(unsigned long seed);

// --- GPIO / ADC stubs ---------------------------------------------------------
void pinMode(int pin, int mode);
void digitalWrite(int pin, int val);
int digitalRead(int pin);
unsigned long analogReadMilliVolts(int pin);
int analogRead(int pin);

// --- serial -------------------------------------------------------------------
struct SerialShim {
  void begin(unsigned long) {}
  operator bool() const { return true; }
  bool isConnected() const { return true; }
  void print(const char *s);
  void print(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); print(b); }
  void println(const char *s) { print(s); print("\n"); }
  void println(int v) { print(v); print("\n"); }
  void println() { print("\n"); }
  void printf(const char *fmt, ...);
};
extern SerialShim Serial;

// --- time ---------------------------------------------------------------------
time_t time(time_t *t);
struct tm *localtime(const time_t *t);

// --- strlcpy (BSD, for sketch convenience) -------------------------------------
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);

// --- minimal Arduino String ----------------------------------------------------
#ifndef portMUX_TYPE
typedef int portMUX_TYPE;
#endif
#define portMUX_INITIALIZER_UNLOCKED 0
struct ESPClass { void restart(); };
extern ESPClass ESP;
#define portMUX_INITIALIZER_UNLOCKED 0
class String {
  char *buf = nullptr;
  void ensure(size_t len);
public:
  String() {}
  String(const char *s);
  String(char c);
  String(int v);
  String(unsigned int v);
  String(long v);
  String(unsigned long v);
  String(const String &o);
  String &operator=(const String &o);
  String &operator=(const char *s);
  ~String() { delete[] buf; }
  unsigned int length() const { return buf ? (unsigned int)strlen(buf) : 0; }
  const char *c_str() const { return buf ? buf : ""; }
  operator const char *() const { return c_str(); }
  char charAt(unsigned int i) const { return buf ? buf[i] : 0; }
  char operator[](unsigned int i) const { return charAt(i); }
  bool startsWith(const String &s) const;
  bool endsWith(const String &s) const;
  void trim();
  void toCharArray(char *out, unsigned int len) const;
  String substring(unsigned int a, unsigned int b) const;
  int toInt() const { return atoi(c_str()); }
  String &operator+=(const String &o);
  String &operator+=(const char *s);
  String &operator+=(char c);
};
String operator+(const String &a, const String &b);
String operator+(const String &a, const char *b);
bool operator==(const String &a, const String &b);
bool operator==(const String &a, const char *b);
bool operator!=(const String &a, const String &b);
