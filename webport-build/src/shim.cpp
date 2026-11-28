// Shim implementation: Arduino core stubs + faithful TFT_eSPI rasterization.
#include "Arduino.h"
#include "TFT_eSPI.h"
#include "Wire.h"
#include "Preferences.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include "HTTPClient.h"
#include "Update.h"
#include "esp_sleep.h"
#include "esp_now.h"
#include "driver/i2s_std.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <new>
void *operator new(size_t n) { return malloc(n); }
void *operator new[](size_t n) { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

// ===================== JS bridge helpers =====================
EM_JS(void, js_mark_dirty, (), { if (Module.gardenDirty !== undefined) Module.gardenDirty = true; });
EM_JS(void, js_sleep_screen, (int on), { if (Module.onDisplayPower) Module.onDisplayPower(!!on); });
EM_JS(void, js_audio_push, (int16_t *p, int frames), { Module.audioPush(p, frames); });
EM_JS(void, js_amp_enable, (int on), { if (Module.onAmp) Module.onAmp(!!on); });
EM_JS(double, js_audio_level, (), { return Module.audioLevel ? Module.audioLevel() : 1.0; });
EM_JS(int, js_touch_count, (), { return Module.touchCount() | 0; });
EM_JS(void, js_touch_point, (int *out), { Module.touchPanelPoint(out); });
EM_JS(int, js_boot_pressed, (), { return Module.bootPressed() ? 1 : 0; });
EM_JS(double, js_now, (), { return Date.now(); });
EM_JS(void, js_nvs_set, (const char *key, const char *val), {
  Module.nvsSet(UTF8ToString(key), UTF8ToString(val));
});
EM_JS(void, js_nvs_get, (const char *key, char *out, int maxlen), {
  stringToUTF8(Module.nvsGet(UTF8ToString(key)) || "", out, maxlen);
});
EM_JS(int, js_fetch_get, (const char *url, char *bodyOut, int bodyMax), {
  return Asyncify.handleSleep(function (wake) {
    Module.httpGet(UTF8ToString(url)).then(function (r) {
      if (r.status !== 200) { wake(0); return; }
      var body = r.body;
      if (body.length > bodyMax - 1) body = body.substring(0, bodyMax - 1);
      stringToUTF8(body, bodyOut, bodyMax);
      wake(r.status);
    }).catch(function () { wake(0); });
  });
});
EM_JS(void, js_reload, (), { if (Module.onReboot) Module.onReboot(); });

// ===================== timing =====================
unsigned long millis() { return (unsigned long)(js_now() - EM_ASM_DOUBLE({ return Module.t0; })); }
unsigned long micros() { return millis() * 1000; }
void delay(unsigned long ms) { emscripten_sleep(ms); }
void delayMicroseconds(unsigned long us) { if (us >= 1000) emscripten_sleep(us / 1000); }
void yield() {}

// ===================== random =====================
static unsigned long rngState = 0x12345678u;
void randomSeed(unsigned long seed) { rngState = seed ? seed : 0x12345678u; }
long random(long howbig) {
  if (howbig <= 0) return 0;
  rngState = rngState * 1103515245u + 12345u;
  return (long)(rngState % (unsigned long)howbig);
}
long random(long howsmall, long howbig) {
  if (howsmall >= howbig) return howsmall;
  long diff = howbig - howsmall;
  return random(diff) + howsmall;
}

// ===================== GPIO / ADC =====================
void pinMode(int, int) {}
static bool bootPinState = true; // true = HIGH (released)
void digitalWrite(int pin, int val) {
  if (pin == 0) bootPinState = val != LOW;
  else if (pin == 1) js_amp_enable(val == LOW); // FM8002E enable is active-low
}
int digitalRead(int pin) { return pin == 0 ? (js_boot_pressed() ? LOW : HIGH) : HIGH; }
unsigned long analogReadMilliVolts(int) { return 0; } // no battery on the web
int analogRead(int pin) { (void)pin; return (int)((long long)js_now() & 0xFFF); }

SerialShim Serial;
void SerialShim::print(const char *s) { EM_ASM({ console.log(UTF8ToString($0)); }, s); }
void SerialShim::printf(const char *fmt, ...) {
  char buf[256];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  print(buf);
}

time_t time(time_t *) { return (time_t)(js_now() / 1000.0); }
struct tm *localtime(const time_t *) { static struct tm t; return &t; }

size_t strlcpy(char *dst, const char *src, size_t size) {
  size_t len = strlen(src);
  if (size) {
    size_t n = len >= size ? size - 1 : len;
    memcpy(dst, src, n);
    dst[n] = 0;
  }
  return len;
}
size_t strlcat(char *dst, const char *src, size_t size) {
  size_t dl = strlen(dst);
  if (dl >= size) return dl + strlen(src);
  return dl + strlcpy(dst + dl, src, size - dl);
}

// ===================== String =====================
void String::ensure(size_t len) {
  if (!buf) { buf = new char[len + 1]; buf[0] = 0; }
  else if (strlen(buf) < len) { char *nb = new char[len + 1]; strcpy(nb, buf); delete[] buf; buf = nb; }
}
String::String(const char *s) { if (s && s[0]) { buf = new char[strlen(s) + 1]; strcpy(buf, s); } }
String::String(char c) { buf = new char[2]; buf[0] = c; buf[1] = 0; }
String::String(int v) { buf = new char[16]; snprintf(buf, 16, "%d", v); }
String::String(unsigned int v) { buf = new char[16]; snprintf(buf, 16, "%u", v); }
String::String(long v) { buf = new char[24]; snprintf(buf, 24, "%ld", v); }
String::String(unsigned long v) { buf = new char[24]; snprintf(buf, 24, "%lu", v); }
String::String(const String &o) { if (o.buf) { buf = new char[strlen(o.buf) + 1]; strcpy(buf, o.buf); } }
String &String::operator=(const String &o) { if (this != &o) { delete[] buf; buf = nullptr; if (o.buf) { buf = new char[strlen(o.buf) + 1]; strcpy(buf, o.buf); } } return *this; }
String &String::operator=(const char *s) { delete[] buf; buf = nullptr; if (s && s[0]) { buf = new char[strlen(s) + 1]; strcpy(buf, s); } return *this; }
bool String::startsWith(const String &s) const { return strncmp(c_str(), s.c_str(), s.length()) == 0; }
bool String::endsWith(const String &s) const { unsigned int l = length(), sl = s.length(); return l >= sl && strcmp(c_str() + l - sl, s.c_str()) == 0; }
void String::trim() {
  if (!buf) return;
  char *a = buf; while (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n') a++;
  char *e = buf + strlen(a); while (e > a && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
  *e = 0;
  if (a != buf) memmove(buf, a, strlen(a) + 1);
}
void String::toCharArray(char *out, unsigned int len) const { snprintf(out, len, "%s", c_str()); }
String String::substring(unsigned int a, unsigned int b) const {
  unsigned int l = length();
  if (a >= l) return String();
  if (b > l) b = l;
  char *t = new char[b - a + 1];
  memcpy(t, c_str() + a, b - a); t[b - a] = 0;
  String r(t); delete[] t; return r;
}
String &String::operator+=(const String &o) { ensure(length() + o.length()); strcat(buf, o.c_str()); return *this; }
String &String::operator+=(const char *s) { ensure(length() + strlen(s)); strcat(buf, s); return *this; }
String &String::operator+=(char c) { char t[2] = {c, 0}; return (*this += t); }
String operator+(const String &a, const String &b) { String r(a); r += b; return r; }
String operator+(const String &a, const char *b) { String r(a); r += b; return r; }
bool operator==(const String &a, const String &b) { return strcmp(a.c_str(), b.c_str()) == 0; }
bool operator==(const String &a, const char *b) { return strcmp(a.c_str(), b) == 0; }
bool operator!=(const String &a, const String &b) { return !(a == b); }

// ===================== TFT_eSPI-faithful rasterization =====================
void DisplayBuf::fillRect(int32_t x, int32_t y, int32_t w_, int32_t h_, uint32_t c) {
  if (w_ < 0) { x += w_; w_ = -w_; }
  if (h_ < 0) { y += h_; h_ = -h_; }
  for (int32_t yy = y; yy < y + h_; yy++)
    for (int32_t xx = x; xx < x + w_; xx++)
      put(xx, yy, (uint16_t)c);
  js_mark_dirty();
}
void DisplayBuf::drawFastHLine(int32_t x, int32_t y, int32_t len, uint32_t c) {
  if (len < 0) { x += len; len = -len; }
  fillRect(x, y, len, 1, c);
}
void DisplayBuf::drawFastVLine(int32_t x, int32_t y, int32_t len, uint32_t c) {
  if (len < 0) { y += len; len = -len; }
  fillRect(x, y, 1, len, c);
}
void DisplayBuf::drawPixel(int32_t x, int32_t y, uint32_t c) { put(x, y, (uint16_t)c); js_mark_dirty(); }
void DisplayBuf::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
  bool steep = abs(y1 - y0) > abs(x1 - x0);
  if (steep) { transpose(x0, y0); transpose(x1, y1); }
  if (x0 > x1) { transpose(x0, x1); transpose(y0, y1); }
  int32_t dx = x1 - x0, dy = abs(y1 - y0);
  int32_t err = dx >> 1, ystep = -1, xs = x0, dlen = 0;
  if (y0 < y1) ystep = 1;
  if (steep) {
    for (; x0 <= x1; x0++) {
      dlen++;
      err -= dy;
      if (err < 0) {
        if (dlen == 1) drawPixel(y0, xs, color);
        else drawFastVLine(y0, xs, dlen, color);
        dlen = 0;
        y0 += ystep; xs = x0 + 1;
        err += dx;
      }
    }
    if (dlen) drawFastVLine(y0, xs, dlen, color);
  } else {
    for (; x0 <= x1; x0++) {
      dlen++;
      err -= dy;
      if (err < 0) {
        if (dlen == 1) drawPixel(xs, y0, color);
        else drawFastHLine(xs, y0, dlen, color);
        dlen = 0;
        y0 += ystep; xs = x0 + 1;
        err += dx;
      }
    }
    if (dlen) drawFastHLine(xs, y0, dlen, color);
  }
  js_mark_dirty();
}
void DisplayBuf::drawRect(int32_t x, int32_t y, int32_t w_, int32_t h_, uint32_t c) {
  drawFastHLine(x, y, w_, c);
  drawFastHLine(x, y + h_ - 1, w_, c);
  drawFastVLine(x, y, h_, c);
  drawFastVLine(x + w_ - 1, y, h_, c);
}
void DisplayBuf::fillCircleHelper(int32_t x0, int32_t y0, int32_t r, uint8_t cornername, int32_t delta, uint32_t color) {
  int32_t f = 1 - r, ddF_x = 1, ddF_y = -r - r, y = 0;
  delta++;
  while (y < r) {
    if (f >= 0) {
      if (cornername & 0x1) drawFastHLine(x0 - y, y0 + r, y + y + delta, color);
      if (cornername & 0x2) drawFastHLine(x0 - y, y0 - r, y + y + delta, color);
      r--;
      ddF_y += 2;
      f += ddF_y;
    }
    y++;
    ddF_x += 2;
    f += ddF_x;
    if (cornername & 0x1) drawFastHLine(x0 - r, y0 + y, r + r + delta, color);
    if (cornername & 0x2) drawFastHLine(x0 - r, y0 - y, r + r + delta, color);
  }
}
void DisplayBuf::fillCircle(int32_t x0, int32_t y0, int32_t r, uint32_t color) {
  int32_t x = 0, dx = 1, dy = r + r, p = -(r >> 1);
  drawFastHLine(x0 - r, y0, dy + 1, color);
  while (x < r) {
    if (p >= 0) {
      drawFastHLine(x0 - x, y0 + r, dx, color);
      drawFastHLine(x0 - x, y0 - r, dx, color);
      dy -= 2;
      p -= dy;
      r--;
    }
    dx += 2;
    p += dx;
    x++;
    drawFastHLine(x0 - r, y0 + x, dy + 1, color);
    drawFastHLine(x0 - r, y0 - x, dy + 1, color);
  }
}
void DisplayBuf::drawCircleHelper(int32_t x0, int32_t y0, int32_t rr, uint8_t cornername, uint32_t color) {
  if (rr <= 0) return;
  int32_t f = 1 - rr, ddF_x = 1, ddF_y = -2 * rr, xe = 0, xs = 0, len = 0;
  do {
    while (f < 0) { ++xe; f += (ddF_x += 2); }
    f += (ddF_y += 2);
    if (xe - xs == 1) {
      if (cornername & 0x1) { drawPixel(x0 - xe, y0 - rr, color); drawPixel(x0 - rr, y0 - xe, color); }
      if (cornername & 0x2) { drawPixel(x0 + rr, y0 - xe, color); drawPixel(x0 + xs + 1, y0 - rr, color); }
      if (cornername & 0x4) { drawPixel(x0 + xs + 1, y0 + rr, color); drawPixel(x0 + rr, y0 + xs + 1, color); }
      if (cornername & 0x8) { drawPixel(x0 - rr, y0 + xs + 1, color); drawPixel(x0 - xe, y0 + rr, color); }
    } else {
      len = xe - xs++;
      if (cornername & 0x1) { drawFastHLine(x0 - xe, y0 - rr, len, color); drawFastVLine(x0 - rr, y0 - xe, len, color); }
      if (cornername & 0x2) { drawFastVLine(x0 + rr, y0 - xe, len, color); drawFastHLine(x0 + xs, y0 - rr, len, color); }
      if (cornername & 0x4) { drawFastHLine(x0 + xs, y0 + rr, len, color); drawFastVLine(x0 + rr, y0 + xs, len, color); }
      if (cornername & 0x8) { drawFastVLine(x0 - rr, y0 + xs, len, color); drawFastHLine(x0 - xe, y0 + rr, len, color); }
    }
    xs = xe;
  } while (xe < rr--);
}
void DisplayBuf::drawCircle(int32_t x0, int32_t y0, int32_t r, uint32_t color) {
  if (r <= 0) return;
  int32_t f = 1 - r, ddF_y = -2 * r, ddF_x = 1, xs = -1, xe = 0, len = 0;
  bool first = true;
  do {
    while (f < 0) { ++xe; f += (ddF_x += 2); }
    f += (ddF_y += 2);
    if (xe - xs > 1) {
      if (first) {
        len = 2 * (xe - xs) - 1;
        drawFastHLine(x0 - xe, y0 + r, len, color);
        drawFastHLine(x0 - xe, y0 - r, len, color);
        drawFastVLine(x0 + r, y0 - xe, len, color);
        drawFastVLine(x0 - r, y0 - xe, len, color);
        first = false;
      } else {
        len = 2 * (xe - xs) - 1;
        drawFastHLine(x0 - xe, y0 + r, len, color);
        drawFastVLine(x0 + r, y0 - xe, len, color);
        x0--;
        drawFastHLine(x0 - xe, y0 - r, len, color);
        drawFastVLine(x0 - r, y0 - xe, len, color);
        x0 += 2;
        len = 2 * (xe - xs) + 1;
        drawFastHLine(x0 - xe, y0 + r, len, color);
        drawFastVLine(x0 + r, y0 - xe, len, color);
        drawFastHLine(x0 - xe, y0 - r, len, color);
        drawFastVLine(x0 - r, y0 - xe, len, color);
      }
    } else {
      ++xs;
      if (first) {
        drawFastHLine(x0 - xe, y0 + r, 2, color);
        drawFastHLine(x0 - xe, y0 - r, 2, color);
        drawFastVLine(x0 + r, y0 - xe, 2, color);
        drawFastVLine(x0 - r, y0 - xe, 2, color);
        first = false;
      } else {
        drawFastHLine(x0 - xe, y0 + r, 2, color);
        drawFastVLine(x0 + r, y0 - xe, 2, color);
        drawFastHLine(x0 - xe, y0 - r, 2, color);
        drawFastVLine(x0 - r, y0 - xe, 2, color);
      }
    }
    xs = xe;
  } while (xe < r--);
}
void DisplayBuf::fillRoundRect(int32_t x, int32_t y, int32_t w_, int32_t h_, int32_t r, uint32_t color) {
  fillRect(x, y + r, w_, h_ - r - r, color);
  fillCircleHelper(x + r, y + h_ - r - 1, r, 1, w_ - r - r - 1, color);
  fillCircleHelper(x + r, y + r, r, 2, w_ - r - r - 1, color);
}
void DisplayBuf::drawRoundRect(int32_t x, int32_t y, int32_t w_, int32_t h_, int32_t r, uint32_t color) {
  drawFastHLine(x + r, y, w_ - r - r, color);
  drawFastHLine(x + r, y + h_ - 1, w_ - r - r, color);
  drawFastVLine(x, y + r, h_ - r - r, color);
  drawFastVLine(x + w_ - 1, y + r, h_ - r - r, color);
  drawCircleHelper(x + r, y + r, r, 1, color);
  drawCircleHelper(x + w_ - r - 1, y + r, r, 2, color);
  drawCircleHelper(x + w_ - r - 1, y + h_ - r - 1, r, 4, color);
  drawCircleHelper(x + r, y + h_ - r - 1, r, 8, color);
}
void DisplayBuf::fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color) {
  int32_t a, b, y, last;
  if (y0 > y1) { transpose(y0, y1); transpose(x0, x1); }
  if (y1 > y2) { transpose(y2, y1); transpose(x2, x1); }
  if (y0 > y1) { transpose(y0, y1); transpose(x0, x1); }
  if (y0 == y2) {
    a = b = x0;
    if (x1 < a) a = x1; else if (x1 > b) b = x1;
    if (x2 < a) a = x2; else if (x2 > b) b = x2;
    drawFastHLine(a, y0, b - a + 1, color);
    return;
  }
  int32_t dx01 = x1 - x0, dy01 = y1 - y0, dx02 = x2 - x0, dy02 = y2 - y0,
          dx12 = x2 - x1, dy12 = y2 - y1, sa = 0, sb = 0;
  if (y1 == y2) last = y1; else last = y1 - 1;
  for (y = y0; y <= last; y++) {
    a = x0 + sa / dy01;
    b = x0 + sb / dy02;
    sa += dx01;
    sb += dx02;
    if (a > b) transpose(a, b);
    drawFastHLine(a, y, b - a + 1, color);
  }
  sa = dx12 * (y - y1);
  sb = dx02 * (y - y0);
  for (; y <= y2; y++) {
    a = x1 + sa / dy12;
    b = x0 + sb / dy02;
    sa += dx12;
    sb += dx02;
    if (a > b) transpose(a, b);
    drawFastHLine(a, y, b - a + 1, color);
  }
}
void DisplayBuf::drawTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color) {
  drawLine(x0, y0, x1, y1, color);
  drawLine(x1, y1, x2, y2, color);
  drawLine(x2, y2, x0, y0, color);
}
void DisplayBuf::pushImage(int32_t x, int32_t y, int32_t w_, int32_t h_, const uint16_t *d) {
  for (int32_t r = 0; r < h_; r++)
    for (int32_t c2 = 0; c2 < w_; c2++)
      put(x + c2, y + r, d[r * w_ + c2]);
  js_mark_dirty();
}
void DisplayBuf::pushImage(int32_t x, int32_t y, int32_t w_, int32_t h_, const uint16_t *d, uint32_t key) {
  for (int32_t r = 0; r < h_; r++)
    for (int32_t c2 = 0; c2 < w_; c2++)
      if (d[r * w_ + c2] != key) put(x + c2, y + r, d[r * w_ + c2]);
  js_mark_dirty();
}

// ---- fonts ----
// glcdfont.c provides `font[]`; Font16.c provides widtbl_f16 + chrtbl_f16;
// Font32rle.c provides widtbl_f32 + chrtbl_f32. All compiled from the real
// library files, so glyphs are exact by construction.
extern "C" {
extern const unsigned char font[];
extern const unsigned char widtbl_f16[96];
extern const unsigned char *const chrtbl_f16[96];
extern const unsigned char widtbl_f32[96];
extern const unsigned char *const chrtbl_f32[96];
extern const unsigned char chr_hgt_f16_;
}
#define chr_hgt_f16 16
#define baseline_f16 13
#define chr_hgt_f32 26
#define baseline_f32 19

// Font 1 (5x7 GLCD), replicated from TFT_eSPI::drawChar
void DisplayBuf::drawCharPixelFont(int32_t x, int32_t y, uint16_t c, uint32_t color, uint32_t bg, uint8_t size) {
  if (c > 255) return;
  if (c > 175) c++;
  bool fillbg = (bg != color);
  for (int8_t i = 0; i < 6; i++) {
    uint8_t line = (i == 5) ? 0 : font[c * 5 + i];
    if (size == 1 && !fillbg) {
      for (int8_t j = 0; j < 8; j++) {
        if (line & 0x1) drawPixel(x + i, y + j, color);
        line >>= 1;
      }
    } else {
      for (int8_t j = 0; j < 8; j++) {
        if (line & 0x1) fillRect(x + (i * size), y + (j * size), size, size, color);
        else if (fillbg) fillRect(x + i * size, y + j * size, size, size, bg);
        line >>= 1;
      }
    }
  }
}

// Fonts 2/4 via the real glyph tables (TFT_eSPI drawChar path)
int16_t DisplayBuf::drawCharGlyph(int32_t x, int32_t y, uint16_t uniCode, uint8_t font) {
  if (!uniCode) return 0;
  if (font == 1) { drawCharPixelFont(x, y, uniCode, textcolor, textbgcolor, textsize); return 6 * textsize; }
  if ((font > 1) && (font < 9) && ((uniCode < 32) || (uniCode > 127))) return 0;
  int32_t width = 0, height = 0;
  const uint8_t *flash_address = nullptr;
  uniCode -= 32;
  if (font == 2) {
    flash_address = chrtbl_f16[uniCode];
    width = widtbl_f16[uniCode];
    height = chr_hgt_f16;
  } else if (font > 2 && font < 9 && font == 4) {
    flash_address = chrtbl_f32[uniCode];
    width = widtbl_f32[uniCode];
    height = chr_hgt_f32;
  } else return 0;

  int32_t w = width;
  int32_t pX, pY = y;
  uint8_t line;
  int32_t total = width * height * textsize; // pixels to emit

  if (font == 2) {
    w = w + 6;
    w = w / 8;
    for (int32_t i = 0; i < height; i++) {
      if (textcolor != textbgcolor) fillRect(x, pY, width * textsize, textsize, textbgcolor);
      for (int32_t k = 0; k < w; k++) {
        line = flash_address[w * i + k];
        if (line) {
          if (textsize == 1) {
            pX = x + k * 8;
            for (int b = 7; b >= 0; --b)
              if (line & (1 << b)) drawPixel(pX + (7 - b), pY, textcolor);
          } else {
            pX = x + k * 8 * textsize;
            for (int b = 7; b >= 0; --b)
              if (line & (1 << b)) fillRect(pX + (7 - b) * textsize, pY, textsize, textsize, textcolor);
          }
        }
      }
      pY += textsize;
    }
    return width * textsize;
  }

  // font 4: RLE decoding, replicated from TFT_eSPI (linear runs over the cell)
  int32_t pc = 0, pl = 0;
  uint16_t pcol = 0;
  bool pf = true;
  while (pc < total / textsize / textsize) {
    line = *flash_address++;
    if (line & 0x80) { pcol = textcolor; line &= 0x7F; pf = true; }
    else { pcol = textbgcolor; if (textcolor == textbgcolor) pf = false; }
    line++;
    int32_t px = pc % width;
    int32_t tx = x + textsize * px;
    int32_t py = pc / width;
    int32_t ty = y + textsize * py;
    pl = 0;
    pc += line;
    while (line--) {
      pl++;
      if ((px + pl) >= width) {
        if (pf) fillRect(tx, ty, pl * textsize, textsize, pcol);
        pl = 0;
        px = 0;
        tx = x;
        py++;
        ty += textsize;
      }
    }
    if (pl && pf) fillRect(tx, ty, pl * textsize, textsize, pcol);
  }
  return width * textsize;
}

int16_t DisplayBuf::textWidth(const char *s, uint8_t font) {
  int32_t str_width = 0;
  if (font > 1 && font < 9) {
    const uint8_t *wt = (font == 2) ? widtbl_f16 : widtbl_f32;
    while (*s) {
      uint16_t u = (uint8_t)*s++;
      if (u > 31 && u < 128) str_width += wt[u - 32];
      else str_width += wt[0];
    }
  } else {
    str_width = 6 * strlen(s);
  }
  return (int16_t)(str_width * textsize);
}

int16_t DisplayBuf::fontHeight(uint8_t font) {
  uint8_t h;
  if (font == 1) h = 8;
  else if (font == 2) h = chr_hgt_f16;
  else if (font == 4) h = chr_hgt_f32;
  else h = 8;
  return (int16_t)(h * textsize);
}

int16_t DisplayBuf::drawString(const char *s, int32_t poX, int32_t poY, uint8_t font) {
  int8_t savedSize = textsize;
  textsize = 1; // the game always draws with textsize 1; scaleR handles zoom
  (void)savedSize;
  uint8_t padding = 1;
  int16_t baseline = 0;
  uint16_t cwidth = textWidth(s, font);
  uint16_t cheight = fontHeight(font);
  if (font != 1) baseline = (font == 2 ? baseline_f16 : baseline_f32);

  if (textdatum || padX) {
    switch (textdatum) {
      case 1: poX -= cwidth / 2; padding += 1; break;              // TC
      case 2: poX -= cwidth; padding += 2; break;                  // TR
      case 3: poY -= cheight / 2; break;                           // ML
      case 4: poX -= cwidth / 2; poY -= cheight / 2; padding += 1; break; // MC
      case 5: poX -= cwidth; poY -= cheight / 2; padding += 2; break;     // MR
      case 6: poY -= cheight; break;                               // BL
      case 7: poX -= cwidth / 2; poY -= cheight; padding += 1; break;     // BC
      case 8: poX -= cwidth; poY -= cheight; padding += 2; break;         // BR
    }
  }
  int16_t sumX = 0;
  uint16_t len = strlen(s);
  uint16_t n = 0;
  while (n < len) {
    uint8_t u = (uint8_t)s[n++];
    sumX += drawCharGlyph(poX + sumX, poY, u, font);
  }
  if ((padX > cwidth) && (textcolor != textbgcolor)) {
    int16_t padXc = poX + cwidth;
    switch (padding) {
      case 1: fillRect(padXc, poY, padX - cwidth, cheight, textbgcolor); break;
      case 2:
        fillRect(padXc, poY, (padX - cwidth) >> 1, cheight, textbgcolor);
        padXc = poX - ((padX - cwidth) >> 1);
        fillRect(padXc, poY, (padX - cwidth) >> 1, cheight, textbgcolor);
        break;
      case 3:
        if (padXc > padX) padXc = padX;
        fillRect(poX + cwidth - padXc, poY, padXc - cwidth, cheight, textbgcolor);
        break;
    }
  }
  return sumX;
}
int16_t DisplayBuf::drawCentreString(const char *s, int32_t dX, int32_t poY, uint8_t font) {
  uint8_t temp = textdatum;
  textdatum = 1; // TC_DATUM
  int16_t r = drawString(s, dX, poY, font);
  textdatum = temp;
  return r;
}
int16_t DisplayBuf::drawRightString(const char *s, int32_t dX, int32_t poY, uint8_t font) {
  uint8_t temp = textdatum;
  textdatum = 2; // TR_DATUM
  int16_t r = drawString(s, dX, poY, font);
  textdatum = temp;
  return r;
}

// ---- sprite ----
static uint16_t spriteHeap[480 * 320];  // one sprite at a time (frame buffer or splash)
bool TFT_eSprite::createSprite(int32_t W, int32_t H) {
  if (W * H > 480 * 320) return false;
  buf = spriteHeap;
  setSize(W, H);
  fillRect(0, 0, W, H, 0);
  return true;
}
void TFT_eSprite::deleteSprite() { buf = nullptr; }
void TFT_eSprite::pushSprite(int32_t x, int32_t y) {
  if (!buf || !owner || !owner->buf) return;
  for (int32_t r = 0; r < h; r++)
    for (int32_t c2 = 0; c2 < w; c2++)
      owner->put(x + c2, y + r, buf[r * w + c2]);
  js_mark_dirty();
}

// ===================== Wire (FT6336G touch + ES8311 audio bus) =====================
TwoWire Wire;
static uint8_t wireAddr = 0, wireReg = 0;
static uint8_t wireTx[8];
static int wireTxLen = 0;
static uint8_t wireRx[16];
static int wireRxLen = 0, wireRxPos = 0;

void TwoWire::beginTransmission(uint8_t addr) { wireAddr = addr; wireTxLen = 0; }
bool TwoWire::endTransmission(bool) { if (wireTxLen) wireReg = wireTx[0]; return true; }
size_t TwoWire::write(uint8_t v) { if (wireTxLen < 8) wireTx[wireTxLen++] = v; return 1; }
uint8_t TwoWire::requestFrom(uint8_t addr, uint8_t n) {
  wireRxPos = 0;
  wireRxLen = 0;
  if (addr == 0x38) { // FT6336G
    if (wireReg == 0x02) { wireRx[wireRxLen++] = (uint8_t)(js_touch_count() & 0xFF); }
    else if (wireReg == 0x03) {
      int pt[2] = {0, 0};
      js_touch_point(pt);
      // Sketch mapping: rawX = 319 - panelY ; rawY = panelX (12-bit, big-endian)
      int rawX = 319 - pt[1];
      int rawY = pt[0];
      wireRx[wireRxLen++] = (uint8_t)(((rawX >> 8) & 0x0F) | 0x10);
      wireRx[wireRxLen++] = (uint8_t)(rawX & 0xFF);
      wireRx[wireRxLen++] = (uint8_t)(((rawY >> 8) & 0x0F) | 0x10);
      wireRx[wireRxLen++] = (uint8_t)(rawY & 0xFF);
    }
  }
  // addr 0x18 (ES8311): reads return 0 — the codec state is irrelevant here.
  return wireRxLen;
}
int TwoWire::available() { return wireRxLen - wireRxPos; }
int TwoWire::read() { return wireRxPos < wireRxLen ? wireRx[wireRxPos++] : 0; }

// ===================== Preferences (browser NVS) =====================
static char nvsScratch[512];
bool Preferences::clear() { EM_ASM({ Module.nvsClear(); }); return true; }
bool Preferences::remove(const char *key) { js_nvs_set(key, "\x01"); return true; }
size_t Preferences::putUChar(const char *key, uint8_t v) { snprintf(nvsScratch, sizeof(nvsScratch), "%u", v); js_nvs_set(key, nvsScratch); return 1; }
uint8_t Preferences::getUChar(const char *key, uint8_t def) { js_nvs_get(key, nvsScratch, sizeof(nvsScratch)); if (!nvsScratch[0]) return def; return (uint8_t)strtoul(nvsScratch, nullptr, 10); }
size_t Preferences::putUShort(const char *key, uint16_t v) { snprintf(nvsScratch, sizeof(nvsScratch), "%u", v); js_nvs_set(key, nvsScratch); return 2; }
uint16_t Preferences::getUShort(const char *key, uint16_t def) { js_nvs_get(key, nvsScratch, sizeof(nvsScratch)); if (!nvsScratch[0]) return def; return (uint16_t)strtoul(nvsScratch, nullptr, 10); }
size_t Preferences::putUInt(const char *key, uint32_t v) { snprintf(nvsScratch, sizeof(nvsScratch), "%u", v); js_nvs_set(key, nvsScratch); return 4; }
uint32_t Preferences::getUInt(const char *key, uint32_t def) { js_nvs_get(key, nvsScratch, sizeof(nvsScratch)); if (!nvsScratch[0]) return def; return (uint32_t)strtoul(nvsScratch, nullptr, 10); }
size_t Preferences::putShort(const char *key, int16_t v) { snprintf(nvsScratch, sizeof(nvsScratch), "%d", v); js_nvs_set(key, nvsScratch); return 2; }
int16_t Preferences::getShort(const char *key, int16_t def) { js_nvs_get(key, nvsScratch, sizeof(nvsScratch)); if (!nvsScratch[0]) return def; return (int16_t)strtol(nvsScratch, nullptr, 10); }
size_t Preferences::putBool(const char *key, bool v) { js_nvs_set(key, v ? "1" : "0"); return 1; }
bool Preferences::getBool(const char *key, bool def) { js_nvs_get(key, nvsScratch, sizeof(nvsScratch)); if (!nvsScratch[0]) return def; return nvsScratch[0] == '1'; }
size_t Preferences::putString(const char *key, const char *v) { js_nvs_set(key, v); return strlen(v); }
String Preferences::getString(const char *key, const char *def) { js_nvs_get(key, nvsScratch, sizeof(nvsScratch)); return String(nvsScratch[0] ? nvsScratch : def); }
size_t Preferences::putBytes(const char *key, const void *data, size_t len) {
  // hex-encode arrays so they survive JSON/localStorage round trips
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < len && 2 * i + 1 < sizeof(nvsScratch) - 2; ++i)
    snprintf(nvsScratch + 2 * i, 3, "%02x", p[i]);
  nvsScratch[2 * len] = 0;
  js_nvs_set(key, nvsScratch);
  return len;
}
size_t Preferences::getBytes(const char *key, void *data, size_t len) {
  js_nvs_get(key, nvsScratch, sizeof(nvsScratch));
  if (!nvsScratch[0]) return 0;
  uint8_t *out = (uint8_t *)data;
  size_t n = 0;
  for (const char *s = nvsScratch; s[0] && s[1] && n < len; s += 2, ++n) {
    char t[3] = {s[0], s[1], 0};
    out[n] = (uint8_t)strtoul(t, nullptr, 16);
  }
  return n;
}
size_t Preferences::getBytesLength(const char *key) {
  js_nvs_get(key, nvsScratch, sizeof(nvsScratch));
  return strlen(nvsScratch) / 2;
}

// ===================== WiFi / HTTP / Update =====================
WiFiClass WiFi;
int WiFiClass::status() { return WL_CONNECTED; } // the browser is the network
String WiFiClass::SSID(int) { return String("WEB"); }
int32_t WiFiClass::RSSI(int) { return -40; }
int WiFiClass::scanComplete() { return 0; } // no networks visible in a browser
void WiFiClass::scanNetworks(bool, bool, bool, uint32_t) {}
void WiFiClass::scanDelete() {}

static char httpUrl[256];
static bool httpBodyReady = false;
static char *httpBody = nullptr;
static size_t httpBodyLen = 0;
bool HTTPClient::begin(WiFiClientSecure &, const String &url) { return begin(url); }
bool HTTPClient::begin(const String &url) {
  strlcpy(httpUrl, url.c_str(), sizeof(httpUrl));
  httpBodyReady = false;
  return true;
}
int HTTPClient::GET() {
  static char bodyBuf[384 * 1024];
  int code = js_fetch_get(httpUrl, bodyBuf, sizeof(bodyBuf));
  if (code == 200) {
    httpBody = bodyBuf;
    httpBodyLen = strlen(bodyBuf);
    httpBodyReady = true;
  }
  return code;
}
String HTTPClient::getString() { return String(httpBodyReady ? httpBody : ""); }
int HTTPClient::getSize() { return httpBodyReady ? (int)httpBodyLen : -1; }
static WiFiClientSecure httpStream;
WiFiClientSecure *HTTPClient::getStreamPtr() { return &httpStream; }
void HTTPClient::end() {}
const uint8_t *WiFiClientSecure::streamData() const { return httpBodyReady ? (const uint8_t *)httpBody : nullptr; }
size_t WiFiClientSecure::streamSize() const { return httpBodyReady ? httpBodyLen : 0; }

UpdateClass Update;
bool UpdateClass::begin(size_t) { return true; }
size_t UpdateClass::writeStream(WiFiClientSecure &s) {
  // consume the real bytes; nothing to flash in a browser
  return s.streamSize();
}
bool UpdateClass::end(bool) { return true; }
void UpdateClass::abort() {}

// ===================== ESP sleep / ESP-NOW =====================
int esp_sleep_get_wakeup_cause() { return 0; }
void esp_sleep_enable_ext0_wakeup(int, int) {}
void esp_sleep_enable_timer_wakeup(unsigned long long) {}
void esp_sleep_disable_wakeup_source(int) {}
void esp_deep_sleep_start() {
  // No true power-down in a browser: park on the deep-sleep screen until the
  // page reloads (BOOT tap). Audio is already stopped by the sketch.
  for (;;) emscripten_sleep(200);
}
void esp_light_sleep_start() { emscripten_sleep(10); }
int esp_now_init() { return ESP_OK; }
int esp_now_deinit() { return ESP_OK; }
int esp_now_register_recv_cb(void (*)(const esp_now_recv_info *, const uint8_t *, int)) { return ESP_OK; }
int esp_now_send(const uint8_t *, const uint8_t *, size_t) { return ESP_FAIL; } // no peers in a browser
bool esp_now_is_peer_exist(const uint8_t *) { return false; }
int esp_now_add_peer(const esp_now_peer_info *) { return ESP_OK; }
int esp_wifi_set_ps(int) { return 0; }
int esp_wifi_set_channel(int, int) { return 0; }

// ===================== I2S audio =====================
int i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *handle, void *) {
  static int dummy;
  *handle = &dummy;
  return ESP_OK;
}
int i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t *) { return ESP_OK; }
int i2s_channel_enable(i2s_chan_handle_t) { return ESP_OK; }
int i2s_channel_disable(i2s_chan_handle_t) { return ESP_OK; }
int i2s_del_channel(i2s_chan_handle_t) { return ESP_OK; }
int i2s_channel_write(i2s_chan_handle_t, const void *data, size_t size, size_t *written, int) {
  js_audio_push((int16_t *)data, (int)(size / 4)); // stereo int16 = 4 bytes/frame
  *written = size;
  return ESP_OK;
}

ESPClass ESP;
void ESPClass::restart() { js_reload(); for (;;) emscripten_sleep(200); }

// ===================== display power / reboot hooks =====================
void shimDisplayCommand(uint8_t c) {
  // 0x28 = display off (sleep) — JS dims the canvas
  if (c == 0x28) js_sleep_screen(1);
  else if (c == 0x11) js_sleep_screen(0);
}
void shimDisplayInit() { js_sleep_screen(0); }

// ===================== entry point =====================
void setup();
void loop();
static uint16_t canvasHeap[480 * 320];
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t garden_fb_ptr() { return (uint32_t)(uintptr_t)canvasHeap; }
extern "C" EMSCRIPTEN_KEEPALIVE unsigned int garden_millis() { return (unsigned int)millis(); }
int main() {
  extern TFT_eSPI canvas;
  canvas.buf = canvasHeap;
  setup();
  for (;;) loop();
  return 0;
}
