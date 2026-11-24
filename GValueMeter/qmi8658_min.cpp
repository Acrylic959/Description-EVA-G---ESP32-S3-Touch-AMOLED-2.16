#include "qmi8658_min.h"

#include <Wire.h>

/* ------------------------------ 寄存器 ------------------------------ */
#define QMI_WHO_AM_I   0x00
#define QMI_CTRL1      0x02      /* bit6 ADDR_AI（自增地址） */
#define QMI_CTRL2      0x03      /* bit6:4 aFS，bit3:0 aODR */
#define QMI_CTRL7      0x08      /* bit0 aEN 加速度计使能 */
#define QMI_STATUS0    0x2E      /* bit0 aDA 加速度数据就绪 */
#define QMI_AX_L       0x35      /* 0x35..0x3A = AX/AY/AZ 低高字节 */

/* ±4g：aFS=001；1000Hz：aODR=0011 */
#define QMI_CFG_CTRL2  ((0x01 << 4) | 0x03)
/* ±4g 量程满码 32768 对应 4000mg */
#define MG_PER_LSB     (4000.0f / 32768.0f)

static uint8_t s_addr = 0;

static bool wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool rd(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)s_addr, (int)n) != (int)n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.available() ? Wire.read() : 0xFF;
  return true;
}

uint8_t qmiAddr() { return s_addr; }

bool qmiBegin(uint8_t sda, uint8_t scl) {
  (void)sda;
  (void)scl;

  static const uint8_t kAddr[2] = {0x6A, 0x6B};
  uint8_t who = 0xFF;

  s_addr = 0;
  for (uint8_t i = 0; i < 2 && s_addr == 0; i++) {
    s_addr = kAddr[i];
    if (rd(QMI_WHO_AM_I, &who, 1) && who != 0x00 && who != 0xFF) {
      Serial.printf("[IMU] QMI8658 @0x%02X WHO_AM_I=0x%02X\r\n", s_addr, who);
      break;
    }
    s_addr = 0;
  }
  if (s_addr == 0) return false;

  /* 复位到已知状态：CTRL1 自增地址 + 小端；CTRL7 先全关 */
  if (!wr(QMI_CTRL7, 0x00)) return false;
  if (!wr(QMI_CTRL1, 0x40)) return false;
  if (!wr(0x04, 0x00)) return false;              /* CTRL3：陀螺不动 */
  if (!wr(QMI_CTRL2, QMI_CFG_CTRL2)) return false; /* 4g / 1000Hz */
  delay(10);
  if (!wr(QMI_CTRL7, 0x01)) return false;          /* 只开加速度计 */
  delay(50);
  return true;
}

bool qmiDataReady() {
  uint8_t st = 0;
  if (!rd(QMI_STATUS0, &st, 1)) return false;
  return (st & 0x01) != 0;
}

bool qmiReadAccelMG(float &x, float &y, float &z) {
  uint8_t b[6];
  if (!rd(QMI_AX_L, b, 6)) return false;
  int16_t ax = (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
  int16_t ay = (int16_t)((uint16_t)b[2] | ((uint16_t)b[3] << 8));
  int16_t az = (int16_t)((uint16_t)b[4] | ((uint16_t)b[5] << 8));
  x = (float)ax * MG_PER_LSB;
  y = (float)ay * MG_PER_LSB;
  z = (float)az * MG_PER_LSB;
  return true;
}
