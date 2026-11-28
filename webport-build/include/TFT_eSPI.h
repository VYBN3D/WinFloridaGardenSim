#pragma once
#include <Arduino.h>

// TFT constants the sketch uses
#define TFT_BLACK 0x0000
#define TFT_WHITE 0xFFFF
#define TFT_YELLOW 0xFFE0
#define TFT_RED 0xF800
#define TFT_GREEN 0x07E0

// Viewport helper constants replicated from TFT_eSPI
struct DisplayBuf {
  uint16_t *buf;
  int32_t w, h;
  uint16_t textcolor = 0xFFFF, textbgcolor = 0;
  uint8_t textfont = 1, textsize = 1;
  uint8_t textdatum = 0; // TL_DATUM
  bool padX = 0;
  bool invertFlag = false;
  bool inTransaction = false, lockTransaction = false, _vpOoB = false;
  int32_t _vpX = 0, _vpY = 0, _vpW, _vpH;
  int32_t _xDatum = 0, _yDatum = 0;

  void setSize(int32_t W, int32_t H) { w = W; h = H; _vpW = W; _vpH = H; }
  inline void put(int32_t x, int32_t y, uint16_t c) {
    if ((unsigned)x < (unsigned)w && (unsigned)y < (unsigned)h)
      buf[y * w + x] = c;
  }
  inline uint16_t get(int32_t x, int32_t y) {
    if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return 0;
    return buf[y * w + x];
  }

  void fillRect(int32_t x, int32_t y, int32_t w_, int32_t h_, uint32_t c);
  void fillScreen(uint32_t c) { fillRect(0, 0, w, h, c); }
  void drawFastHLine(int32_t x, int32_t y, int32_t len, uint32_t c);
  void drawFastVLine(int32_t x, int32_t y, int32_t len, uint32_t c);
  void drawPixel(int32_t x, int32_t y, uint32_t c);
  void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c);
  void drawRect(int32_t x, int32_t y, int32_t w_, int32_t h_, uint32_t c);
  void fillCircle(int32_t x0, int32_t y0, int32_t r, uint32_t c);
  void fillCircleHelper(int32_t x0, int32_t y0, int32_t r, uint8_t corners, int32_t delta, uint32_t c);
  void drawCircle(int32_t x0, int32_t y0, int32_t r, uint32_t c);
  void drawCircleHelper(int32_t x0, int32_t y0, int32_t r, uint8_t corners, uint32_t c);
  void fillRoundRect(int32_t x, int32_t y, int32_t w_, int32_t h_, int32_t r, uint32_t c);
  void drawRoundRect(int32_t x, int32_t y, int32_t w_, int32_t h_, int32_t r, uint32_t c);
  void fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t c);
  void drawTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t c);
  void pushBlock(uint32_t c, int32_t n);  // stream n pixels from cursor — not needed, unused

  // text
  void setTextColor(uint32_t c) { textcolor = c; textbgcolor = c; }
  void setTextColor(uint32_t c, uint32_t bg) { textcolor = c; textbgcolor = bg; }
  void setTextDatum(uint8_t d) { textdatum = d; }
  int16_t drawCharGlyph(int32_t x, int32_t y, uint16_t uniCode, uint8_t font);
  void drawCharPixelFont(int32_t x, int32_t y, uint16_t c, uint32_t color, uint32_t bg, uint8_t size);
  int16_t textWidth(const char *s, uint8_t font);
  int16_t fontHeight(uint8_t font);
  int16_t drawString(const char *s, int32_t x, int32_t y, uint8_t font);
  int16_t drawCentreString(const char *s, int32_t x, int32_t y, uint8_t font);
  int16_t drawRightString(const char *s, int32_t x, int32_t y, uint8_t font);
  void pushImage(int32_t x, int32_t y, int32_t w_, int32_t h_, const uint16_t *d);
  void pushImage(int32_t x, int32_t y, int32_t w_, int32_t h_, const uint16_t *d, uint32_t key);
  void invertDisplay(bool i) { invertFlag = i; }
  void writecommand(uint8_t c) { extern void shimDisplayCommand(uint8_t); shimDisplayCommand(c); }
  void init() { extern void shimDisplayInit(); shimDisplayInit(); }
  void setRotation(uint8_t r) { (void)r; }
  void setSwapBytes(bool b) { (void)b; }
  bool getSwapBytes() const { return false; }
};

class TFT_eSPI : public DisplayBuf {
public:
  TFT_eSPI() { setSize(480, 320); buf = nullptr; }
};

class TFT_eSprite : public DisplayBuf {
public:
  DisplayBuf *owner = nullptr;
  TFT_eSprite(DisplayBuf *o) : owner(o) {}
  bool createSprite(int32_t W, int32_t H);
  void deleteSprite();
  void fillSprite(uint32_t c) { fillRect(0, 0, w, h, c); }
  void setColorDepth(int8_t d) { (void)d; }
  void pushSprite(int32_t x, int32_t y);
};
