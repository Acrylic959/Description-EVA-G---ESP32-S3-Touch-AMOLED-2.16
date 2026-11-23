#include "display.h"
#include "pin_config.h"

// ============================================================
// CO5300 QSPI 显示驱动 + Canvas 双缓冲
// 基于微雪官方 01_HelloWorld / 04_LVGL_QMI8658_ui 初始化序列
// ============================================================

Arduino_CO5300 *gDisplay = nullptr;
Arduino_Canvas  *gCanvas  = nullptr;

static Arduino_ESP32QSPI *s_bus = nullptr;

bool displayBegin() {
  // QSPI 总线: CS, SCK, SDIO0~3
  s_bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

  // CO5300 驱动: RST, rotation=0, 480x480
  gDisplay = new Arduino_CO5300(
    s_bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);

  if (!gDisplay->begin()) {
    Serial.println("[DISP] CO5300 begin FAILED");
    return false;
  }

  // 亮度
  gDisplay->setBrightness(200);
  // 设置显示方向/扫描参数 (微雪官方配置)
  s_bus->writeC8D8(0x36, 0xA0);

  // 创建 Canvas 帧缓冲 (480x480x2 ≈ 460KB, 需开启 PSRAM)
  gCanvas = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, gDisplay, 0, 0, 0);
  // 传入 GFX_NOSKIP 之外的跳过标记: Arduino_Canvas::begin 的第2帧缓冲
  // 注意: 物理屏已经 begin 过, 这里传 GFX_SKIP_OUTPUT_BEGIN 避免重复初始化
  if (!gCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("[DISP] Canvas alloc FAILED (check PSRAM)");
    return false;
  }
  gCanvas->fillScreen(C_BG);

  Serial.printf("[DISP] OK, canvas %dx%d\n", LCD_WIDTH, LCD_HEIGHT);
  return true;
}

void displayFlush() {
  if (gCanvas) gCanvas->flush();
}

void displaySetBrightness(uint8_t level) {
  if (gDisplay) gDisplay->setBrightness(level);
}
