#pragma once
/* sim 用 Wire 替身：这里没有真实 I2C 设备，写操作全部"成功"返回 */
#include <stdint.h>

class TwoWire {
 public:
  void begin(int sda = -1, int scl = -1, uint32_t freq = 100000) {
    (void)sda; (void)scl; (void)freq;
  }
  void beginTransmission(uint8_t addr) { _addr = addr; _n = 0; }
  size_t write(uint8_t v) { (void)v; return 1; }
  uint8_t endTransmission(bool stop = true) { (void)stop; return 0; }
  size_t requestFrom(uint8_t addr, uint8_t qty) { (void)addr; (void)qty; return 0; }
  int available() { return 0; }
  int read() { return 0; }

 private:
  uint8_t _addr = 0;
  uint8_t _n = 0;
};

extern TwoWire Wire;
