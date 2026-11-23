#include "touch.h"
#include "pin_config.h"
#include <Wire.h>
#include <TouchDrvCSTXXX.hpp>

// ============================================================
// CST9220 触摸驱动 + 滑动/点击手势
// 基于微雪官方 SensorLib: TouchDrvCST92xx
// ============================================================

static TouchDrvCST92xx touch;
static int16_t tx[2], ty[2];

// 回调
static void (*s_onSwipe)(int) = nullptr;
static void (*s_onTap)() = nullptr;

// 手势状态
static bool s_touching = false;
static int16_t s_startX = 0, s_startY = 0;
static unsigned long s_startMs = 0;

void IRAM_ATTR onTouchInt() {
  // 中断仅唤醒轮询, 由 touchLoop 读取坐标
}

bool touchBegin() {
  touch.setPins(TP_RST, TP_INT);
  if (!touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("[TOUCH] CST92xx not online!");
    return false;
  }
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setSwapXY(true);
  touch.setMirrorXY(true, false);

  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchInt, FALLING);

  Serial.print("[TOUCH] Model: ");
  Serial.println(touch.getModelName());
  return true;
}

void touchLoop() {
  uint8_t touched = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
  unsigned long now = millis();

  if (touched > 0) {
    int16_t px = tx[0], py = ty[0];
    if (!s_touching) {
      // 按下
      s_touching = true;
      s_startX = px;
      s_startY = py;
      s_startMs = now;
    } else {
      // 滑动中
      int16_t dx = px - s_startX;
      int16_t dy = py - s_startY;
      unsigned long dt = now - s_startMs;
      if (dt < SWIPE_MAX_MS && abs(dx) >= SWIPE_MIN_DIST && abs(dx) > abs(dy)) {
        // 触发滑动
        if (s_onSwipe) s_onSwipe(dx > 0 ? 1 : -1);
        s_touching = false;  // 防止重复触发
      }
    }
  } else {
    // 抬起
    if (s_touching) {
      unsigned long dt = now - s_startMs;
      if (dt < 250) {
        // 短按 = 点击
        if (s_onTap) s_onTap();
      }
    }
    s_touching = false;
  }
}

void touchOnSwipe(void (*cb)(int dir)) { s_onSwipe = cb; }
void touchOnTap(void (*cb)()) { s_onTap = cb; }
