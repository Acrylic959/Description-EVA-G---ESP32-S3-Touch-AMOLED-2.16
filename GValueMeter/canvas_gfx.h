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

/* 设备上帧缓冲直接给 SPI DMA 读（零拷贝推屏），SPI 高位先出，
 * 所以像素要在写入时就存成大端；桌面模拟器（-DGVM_SIM）走 GFX 自己的
 * 转换逻辑，保持小端。 */
#ifndef GVM_SIM
#define GVM_FB_BE 1
#endif

#ifndef GVM_FB_BE
static inline uint16_t cgStore(uint16_t c) { return c; }
#else
static inline uint16_t cgStore(uint16_t c) { return __builtin_bswap16(c); }
#endif

class CanvasGFX : public Arduino_GFX {
 public:
  CanvasGFX(int16_t w, int16_t h) : Arduino_GFX(w, h) {}

  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    (void)speed;
    _buf = allocBuf();
    return _buf != nullptr;
  }

  uint16_t *buffer() { return _buf; }
  size_t   bytes() const { return (size_t)_width * _height * 2; }

  /* 第二块 PSRAM 缓冲：一块在 DMA 推屏时，另一块用来画下一帧 */
  uint16_t *allocExtra() { return allocBuf(); }
  void setBuffer(uint16_t *b) { _buf = b; }

  void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override {
    _buf[(size_t)y * _width + x] = cgStore(color);
  }

  void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > _width)  w = _width  - x;
    if (y + h > _height) h = _height - y;
    if (w <= 0 || h <= 0) return;

    uint16_t c = cgStore(color);
    uint16_t *p = _buf + (size_t)y * _width + x;
    if (w == _width) {                 // 整行：一次填满
      uint32_t n = (uint32_t)w * h;
      uint32_t v = ((uint32_t)c << 16) | c;
      uint32_t *p32 = (uint32_t *)p;
      uint32_t pairs = n >> 1;
      for (uint32_t i = 0; i < pairs; i++) p32[i] = v;
      if (n & 1) p[n - 1] = c;
    } else {
      for (int16_t j = 0; j < h; j++) {
        uint16_t *row = p + (size_t)j * _width;
        for (int16_t i = 0; i < w; i++) row[i] = c;
      }
    }
  }

  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    if (x < 0 || x >= _width) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > _height) h = _height - y;
    if (h <= 0) return;
    uint16_t c = cgStore(color);
    uint16_t *p = _buf + (size_t)y * _width + x;
    for (int16_t j = 0; j < h; j++) p[(size_t)j * _width] = c;
  }

  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    if (y < 0 || y >= _height) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > _width) w = _width - x;
    if (w <= 0) return;
    uint16_t c = cgStore(color);
    uint16_t *p = _buf + (size_t)y * _width + x;
    for (int16_t i = 0; i < w; i++) p[i] = c;
  }

 private:
  /* 64 字节对齐：DMA 与缓存回写都要求行对齐 */
  uint16_t *allocBuf() {
    size_t bytes = (size_t)_width * _height * 2;
    uint16_t *b = (uint16_t *)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) b = (uint16_t *)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_8BIT);
    if (b) memset(b, 0, bytes);
    return b;
  }

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

/* 整帧同步推屏（桌面模拟器 / 无 panel_qspi 时使用） */
inline void canvasPresentAll(Arduino_GFX *disp, CanvasGFX *cv) {
  canvasPresent(disp, cv, 0, cv->height());
}
