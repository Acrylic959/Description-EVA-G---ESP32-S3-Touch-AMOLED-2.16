#pragma once
/* ===========================================================================
 * sim/stubs/SensorQMI8658.hpp — QMI8658 替身：把 sim 主程序喂进来的
 * "合成加速度"原样返回，用来驱动 GMeter 的完整信号链。
 * =========================================================================== */
#include <stdint.h>
#include "Wire.h"

void  simSetAccel(float x, float y, float z);   /* 由 sim_main 每个仿真步调用 */
void  simGetAccel(float &x, float &y, float &z);

class SensorQMI8658 {
 public:
  enum AccelerometerRange { ACC_RANGE_2G, ACC_RANGE_4G, ACC_RANGE_8G, ACC_RANGE_16G };
  enum AccelerometerODR { ACC_ODR_1000Hz = 1000, ACC_ODR_500Hz = 500, ACC_ODR_250Hz = 250 };
  enum LpfMode { LPF_MODE_0, LPF_MODE_1, LPF_MODE_2, LPF_MODE_3 };

  bool begin(TwoWire &w, uint8_t addr = 0x6A, int sda = -1, int scl = -1) {
    (void)w; (void)addr; (void)sda; (void)scl;
    return true;
  }
  bool configAccelerometer(AccelerometerRange r, AccelerometerODR odr, LpfMode lpf) {
    (void)r; (void)odr; (void)lpf;
    return true;
  }
  bool enableAccelerometer() { return true; }
  bool getDataReady() { return true; }
  bool getAccelerometer(float &x, float &y, float &z) {
    simGetAccel(x, y, z);
    return true;
  }
};
