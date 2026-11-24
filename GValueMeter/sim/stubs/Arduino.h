#pragma once
/* ===========================================================================
 * sim/stubs/Arduino.h — 桌面模拟器用的极简 Arduino 核心替身
 * 只实现固件实际用到的接口，行为尽量贴近 ESP32-S3。
 * =========================================================================== */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define RGB565_BLACK 0x0000
#define RGB565_WHITE 0xFFFF

/* 模拟时钟 */
uint32_t millis();
void simAdvance(uint32_t ms);          /* sim 主循环推进时钟 */
void delay(uint32_t ms);
void pinMode(uint8_t pin, uint8_t mode);
int  digitalRead(uint8_t pin);
void digitalWrite(uint8_t pin, uint8_t val);
long random(long max);
long random(long min, long max);

class SimSerial {
 public:
  void begin(long) {}
  void printf(const char *fmt, ...);
  void print(const char *s) { printf("%s", s); }
  void println(const char *s) { printf("%s\n", s); }
  int  available() { return 0; }
  int  read() { return -1; }
};
extern SimSerial Serial;
