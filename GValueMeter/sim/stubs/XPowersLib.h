#pragma once
/* sim 用的 XPowersLib 替身 */
#include <stdint.h>
#include "Wire.h"

#define AXP2101_SLAVE_ADDRESS 0x34

class XPowersPMU {
 public:
  bool begin(TwoWire &w, uint8_t addr = AXP2101_SLAVE_ADDRESS, int sda = -1, int scl = -1) {
    (void)w; (void)addr; (void)sda; (void)scl;
    return true;
  }
  void enableBattDetection() {}
  void enableBattVoltageMeasure() {}
  void enableTemperatureMeasure() {}
  void enableVbusVoltageMeasure() {}
  void enableSystemVoltageMeasure() {}
};
