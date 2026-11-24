#include "touch.h"

#include "pin_config.h"

#define TOUCH_INT_PIN 11      /* CST9220 INT */
#define TOUCH_RST_PIN 40      /* CST9220 RST */

static TwoWire *s_wire = nullptr;
static uint8_t  s_addr = 0;
static bool     s_ok = false;

static bool     s_active = false;
static int16_t  s_x0 = 0, s_y0 = 0, s_xl = 0, s_yl = 0;
static uint32_t s_t0 = 0;
static uint32_t s_lastMs = 0;

static bool probe(uint8_t a) {
  s_wire->beginTransmission(a);
  return s_wire->endTransmission() == 0;
}

void touchBegin(TwoWire *w) {
  s_wire = w;
  pinMode(TOUCH_RST_PIN, OUTPUT);
  digitalWrite(TOUCH_RST_PIN, LOW);
  delay(12);
  digitalWrite(TOUCH_RST_PIN, HIGH);
  delay(60);
  pinMode(TOUCH_INT_PIN, INPUT_PULLUP);

  /* CST9220 常见地址 0x5A；同系列也有 0x15，两个都试一下 */
  if (probe(0x5A))      s_addr = 0x5A;
  else if (probe(0x15)) s_addr = 0x15;
  s_ok = (s_addr != 0);
  if (s_ok) Serial.printf("[TOUCH] CST9220 @0x%02X\n", s_addr);
  else      Serial.println("[TOUCH] 未找到触摸芯片，滑动换页不可用（按键仍可用）");
}

bool touchReady() { return s_ok; }

/* 读一帧：返回触点个数，x/y 是 12bit 坐标（0..479 直接可用） */
static int readPoint(int16_t *x, int16_t *y) {
  if (!s_ok) return 0;
  s_wire->beginTransmission(s_addr);
  s_wire->write((uint8_t)0x02);
  if (s_wire->endTransmission(false) != 0) return 0;
  if (s_wire->requestFrom(s_addr, (uint8_t)5) != 5) return 0;

  uint8_t n  = s_wire->read();
  uint8_t xh = s_wire->read();
  uint8_t xl = s_wire->read();
  uint8_t yh = s_wire->read();
  uint8_t yl = s_wire->read();

  if (n == 0 || n > 2) return 0;
  *x = (int16_t)(((xh & 0x0F) << 8) | xl);
  *y = (int16_t)(((yh & 0x0F) << 8) | yl);
  if (*x < 0 || *x > 480 || *y < 0 || *y > 480) return 0;
  return n;
}

int touchPoll() {
  uint32_t now = millis();
  if (now - s_lastMs < 12) return 0;    /* 12ms 采样，够识别手势又不抢 I2C */
  s_lastMs = now;

  int16_t x = 0, y = 0;
  int n = readPoint(&x, &y);

  if (n > 0) {
    if (!s_active) {
      s_active = true;
      s_x0 = x; s_y0 = y; s_t0 = now;
    }
    s_xl = x; s_yl = y;
    return 0;
  }

  if (!s_active) return 0;
  s_active = false;

  int16_t dx = s_xl - s_x0;
  int16_t dy = s_yl - s_y0;
  uint32_t dt = now - s_t0;
  int16_t adx = dx < 0 ? -dx : dx;
  int16_t ady = dy < 0 ? -dy : dy;

  if (dt > 1400) return 0;
  if (adx < 24 && ady < 24) return 2;                  /* 轻点 */
  if (adx < 90 || adx < ady * 2) return 0;             /* 不算横向滑动 */
  return (dx < 0) ? -1 : 1;                            /* 左滑=专业，右滑=宠物 */
}
