#pragma once
/* ===========================================================================
 * sim/stubs/Arduino_GFX_Library.h — 把 Arduino_GFX 的绘图原语实现到一块
 * 内存帧缓冲上（RGB565），供桌面模拟器渲染固件界面。
 * 接口名字/签名与 moononournation/GFX Library for Arduino 一致。
 * =========================================================================== */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "Arduino.h"
#include "font5x7.h"

#define GFX_NOT_DEFINED -1

class Arduino_GFX {
 public:
  Arduino_GFX(int16_t w, int16_t h) : _width(w), _height(h) {}
  virtual ~Arduino_GFX() {}

  virtual bool begin(int32_t speed = GFX_NOT_DEFINED) {
    (void)speed;
    _fb = (uint16_t *)calloc((size_t)_width * _height, sizeof(uint16_t));
    return _fb != nullptr;
  }

  int16_t width() const { return _width; }
  int16_t height() const { return _height; }

  /* 字体相关（sim 只有一套 5x7 点阵，size 为整数放大倍数） */
  void setFont(void *) {}
  void setTextSize(uint8_t s) { _textSize = s ? s : 1; }
  void setTextColor(uint16_t fg, uint16_t bg) { _fg = fg; _bg = bg; }
  void setTextColor(uint16_t fg) { _fg = fg; _bg = fg; }
  void setCursor(int16_t x, int16_t y) { _cx = x; _cy = y; }
  int16_t getCursorX() const { return _cx; }

  size_t print(const char *s) {
    int16_t x = _cx, y = _cy;
    for (const char *p = s; *p; p++) {
      uint8_t ch = (uint8_t)*p;
      if (ch > 127) ch = '?';
      for (int row = 0; row < 7; row++) {
        uint8_t bits = SIM_FONT[ch][row];
        for (int col = 0; col < 5; col++) {
          if (!((bits >> (4 - col)) & 1)) continue;
          if (_bg != _fg) {
            writeFillRectPreclipped(x + col * _textSize, y + row * _textSize,
                                    _textSize, _textSize, _bg);
          }
          writeFillRectPreclipped(x + col * _textSize, y + row * _textSize,
                                  _textSize, _textSize, _fg);
        }
      }
      x += 6 * _textSize;
    }
    _cx = x;
    return (size_t)(s ? strlen(s) : 0);
  }

  /* 像素级原语（子类可覆写，如 CanvasGFX） */
  virtual void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) {
    _fb[(size_t)y * _width + x] = color;
  }
  virtual void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > _width) w = _width - x;
    if (y + h > _height) h = _height - y;
    if (w <= 0 || h <= 0) return;
    for (int16_t j = 0; j < h; j++) {
      uint16_t *row = _fb + (size_t)(y + j) * _width + x;
      for (int16_t i = 0; i < w; i++) row[i] = color;
    }
  }
  virtual void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
    writeFillRectPreclipped(x, y, 1, h, color);
  }
  virtual void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
    writeFillRectPreclipped(x, y, w, 1, color);
  }

  void writePixel(int16_t x, int16_t y, uint16_t color) {
    if (x < 0 || y < 0 || x >= _width || y >= _height) return;
    writePixelPreclipped(x, y, color);
  }
  void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    writeFillRectPreclipped(x, y, w, h, color);
  }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    writeFillRectPreclipped(x, y, w, h, color);
  }
  void fillScreen(uint16_t color) { writeFillRectPreclipped(0, 0, _width, _height, color); }

  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    int16_t dx = (int16_t)abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int16_t dy = (int16_t)-abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy;
    while (true) {
      writePixel(x0, y0, color);
      if (x0 == x1 && y0 == y1) break;
      int16_t e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }

  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    writeFastHLine(x, y, w, color);
    writeFastHLine(x, y + h - 1, w, color);
    writeFastVLine(x, y, h, color);
    writeFastVLine(x + w - 1, y, h, color);
  }
  void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
    if (r <= 0) return;
    for (int16_t dy = -r; dy <= r; dy++) {
      int32_t k = (int32_t)r * r - (int32_t)dy * dy;
      if (k < 0) continue;
      int16_t w = (int16_t)sqrtf((float)k);
      writeFastHLine(x0 - w, y0 + dy, w * 2 + 1, color);
    }
  }
  void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
    int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
    writePixel(x0, y0 + r, color); writePixel(x0, y0 - r, color);
    writePixel(x0 + r, y0, color); writePixel(x0 - r, y0, color);
    while (x < y) {
      if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
      x++; ddF_x += 2; f += ddF_x;
      writePixel(x0 + x, y0 + y, color); writePixel(x0 - x, y0 + y, color);
      writePixel(x0 + x, y0 - y, color); writePixel(x0 - x, y0 - y, color);
      writePixel(x0 + y, y0 + x, color); writePixel(x0 - y, y0 + x, color);
      writePixel(x0 + y, y0 - x, color); writePixel(x0 - y, y0 - x, color);
    }
  }
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
    if (r <= 0 || r > w / 2 || r > h / 2) { fillRect(x, y, w, h, color); return; }
    fillRect(x + r, y, w - 2 * r, h, color);
    for (int16_t dy = -r; dy <= r; dy++) {
      int32_t k = (int32_t)r * r - (int32_t)dy * dy;
      if (k < 0) continue;
      int16_t dx = (int16_t)sqrtf((float)k);
      writeFastHLine(x + r - dx, y + r + dy, dx * 2 + 1, color);
      writeFastHLine(x + w - r - dx, y + r + dy, dx * 2 + 1, color);
    }
  }
  void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
    if (r <= 0) { drawRect(x, y, w, h, color); return; }
    writeFastHLine(x + r, y, w - 2 * r, color);
    writeFastHLine(x + r, y + h - 1, w - 2 * r, color);
    writeFastVLine(x, y + r, h - 2 * r, color);
    writeFastVLine(x + w - 1, y + r, h - 2 * r, color);
    int16_t cx1 = x + r, cy1 = y + r, cx2 = x + w - r - 1, cy2 = y + h - r - 1;
    for (int16_t a = 0; a <= r; a++) {   /* 四角用圆弧填充近似描边 */
      int32_t k = (int32_t)r * r - (int32_t)(r - a) * (int32_t)(r - a);
      if (k < 0) continue;
      int16_t dx = (int16_t)sqrtf((float)k);
      writePixel(cx1 - dx, cy1 - a, color);
      writePixel(cx2 + dx, cy1 - a, color);
      writePixel(cx1 - dx, cy2 + a, color);
      writePixel(cx2 + dx, cy2 + a, color);
    }
  }
  void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t *bitmap, int16_t w, int16_t h) {
    for (int16_t j = 0; j < h; j++) {
      for (int16_t i = 0; i < w; i++) {
        writePixel(x + i, y + j, bitmap[(size_t)j * w + i]);
      }
    }
  }
  void draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h) {
    draw16bitRGBBitmap(x, y, (uint16_t *)bitmap, w, h);
  }

  uint16_t *framebuffer() { return _fb; }

 protected:
  int16_t _width, _height;
  uint16_t *_fb = nullptr;
  uint16_t _fg = 0xFFFF, _bg = 0x0000;
  int16_t _cx = 0, _cy = 0;
  uint8_t _textSize = 1;
};
