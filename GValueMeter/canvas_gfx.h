#pragma once
/* ===========================================================================
 *  canvas_gfx.h — 把 PSRAM 当作帧缓冲的"画布"
 *
 *  为什么不用 GFXcanvas16？
 *    480×480×2B = 460.8KB，远超 ESP32-S3 内部 DRAM 可分配空间，
 *    GFXcanvas16 / Arduino_Canvas 内部走 malloc() 会失败。
 *  本类直接继承 Arduino_GFX，把像素写进 heap_caps_malloc(PSRAM) 的缓冲区，
 *  因此可以直接复用它全部绘图原语（线、圆、圆角矩形、字体…），
 *  画完再整帧（或整条带）一次推给 CO5300，做到无闪烁。
 * =========================================================================== */
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <string.h>

class CanvasGFX : public Arduino_GFX {
 public:
  CanvasGFX(int16_t w, int16_t h) : Arduino_GFX(w, h) {}

  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    (void)speed;
    size_t bytes = (size_t)_width * _height * 2;
    _buf = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_buf) {  // PSRAM 没开时退化为内部 RAM（会失败，但便于定位问题）
      _buf = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    if (_buf) memset(_buf, 0, bytes);
    return _buf != nullptr;
  }

  uint16_t *buffer() { return _buf; }
  size_t   bytes() const { return (size_t)_width * _height * 2; }

  void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override {
    _buf[(size_t)y * _width + x] = color;
  }

  void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > _width)  w = _width  - x;
    if (y + h > _height) h = _height - y;
    if (w <= 0 || h <= 0) return;

    uint16_t *p = _buf + (size_t)y * _width + x;
    if (w == _width) {                 // 整行：一次填满
      uint32_t n = (uint32_t)w * h;
      uint32_t v = ((uint32_t)color << 16) | color;
      uint32_t *p32 = (uint32_t *)p;
      uint32_t pairs = n >> 1;
      for (uint32_t i = 0; i < pairs; i++) p32[i] = v;
      if (n & 1) p[n - 1] = color;
    } else {
      for (int16_t j = 0; j < h; j++) {
        uint16_t *row = p + (size_t)j * _width;
        for (int16_t i = 0; i < w; i++) row[i] = color;
      }
    }
  }

  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    if (x < 0 || x >= _width) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > _height) h = _height - y;
    if (h <= 0) return;
    uint16_t *p = _buf + (size_t)y * _width + x;
    for (int16_t j = 0; j < h; j++) p[(size_t)j * _width] = color;
  }

  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    if (y < 0 || y >= _height) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > _width) w = _width - x;
    if (w <= 0) return;
    uint16_t *p = _buf + (size_t)y * _width + x;
    for (int16_t i = 0; i < w; i++) p[i] = color;
  }

 private:
  uint16_t *_buf = nullptr;
};

/* 把画布的 [y0, y1) 行推到屏幕。
 * CO5300 在 16bit 模式下对奇地址敏感，这里把上下边界对齐到偶数。 */
inline void canvasPresent(Arduino_GFX *disp, CanvasGFX *cv, int16_t y0, int16_t y1) {
  if (y0 < 0) y0 = 0;
  if (y1 > cv->height()) y1 = cv->height();
  if (y0 & 1) y0--;
  if (y1 & 1) y1++;
  if (y0 < 0) y0 = 0;
  if (y1 > cv->height()) y1 = cv->height();
  if (y1 <= y0) return;
  disp->draw16bitRGBBitmap(0, y0, cv->buffer() + (size_t)y0 * cv->width(),
                           cv->width(), y1 - y0);
}

/* 快速推屏：一次开窗 + 整块 writePixels（走 QSPI 批量传输），
 * 相比 draw16bitRGBBitmap 的逐像素 writePixel，实测快 4~5 倍。 */
inline void canvasPresentFast(Arduino_CO5300 *disp, Arduino_DataBus *bus,
                              CanvasGFX *cv, int16_t y0, int16_t y1) {
  if (y0 < 0) y0 = 0;
  if (y1 > cv->height()) y1 = cv->height();
  if (y0 & 1) y0--;
  if (y1 & 1) y1++;
  if (y0 < 0) y0 = 0;
  if (y1 > cv->height()) y1 = cv->height();
  if (y1 <= y0) return;

  int16_t rows = y1 - y0;
  disp->startWrite();
  disp->writeAddrWindow(0, y0, cv->width(), (uint16_t)rows);
  bus->writePixels(cv->buffer() + (size_t)y0 * cv->width(),
                   (uint32_t)cv->width() * rows);
  disp->endWrite();
}

inline void canvasPresentAllFast(Arduino_CO5300 *disp, Arduino_DataBus *bus, CanvasGFX *cv) {
  canvasPresentFast(disp, bus, cv, 0, cv->height());
}
