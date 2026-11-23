#include "imu.h"
#include "pin_config.h"
#include <Wire.h>
#include <SensorQMI8658.hpp>
#include <math.h>

// ============================================================
// QMI8658 驱动 + G值计算
// 基于微雪官方 SensorLib: SensorQMI8658
// ============================================================

static SensorQMI8658 qmi;
static IMUdata acc;

// 零偏校准值
static float s_biasX = 0, s_biasY = 0, s_biasZ = 1.0f;

// 滤波后的G值
static float s_gx = 0, s_gy = 0, s_gz = 0;
static float s_gMax = 0;

// 减速带检测
static float s_lastGz = 0;
static unsigned long s_bumpUntilMs = 0;
static float s_bumpStrength = 0;

bool imuBegin() {
  Wire.begin(IIC_SDA, IIC_SCL);

  if (!qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("[IMU] Failed to find QMI8658!");
    return false;
  }

  // 4G量程, 1000Hz, 无额外LPF (与微雪官方示例一致, 兼容性最好)
  qmi.configAccelerometer(
    SensorQMI8658::ACC_RANGE_4G,
    SensorQMI8658::ACC_ODR_1000Hz,
    SensorQMI8658::LPF_MODE_0);
  qmi.enableAccelerometer();

  Serial.println("[IMU] QMI8658 OK");
  // 上电默认静止校准
  imuCalibrate();
  return true;
}

void imuCalibrate() {
  // 采样 60 次求平均作为零偏 (假设设备静止)
  const int N = 60;
  float sx = 0, sy = 0, sz = 0;
  int cnt = 0;
  unsigned long t0 = millis();
  while (cnt < N && millis() - t0 < 3000) {
    if (qmi.getDataReady()) {
      if (qmi.getAccelerometer(acc.x, acc.y, acc.z)) {
        sx += acc.x;
        sy += acc.y;
        sz += acc.z;
        cnt++;
      }
    }
    delay(5);
  }
  if (cnt > 0) {
    s_biasX = sx / cnt;
    s_biasY = sy / cnt;
    s_biasZ = sz / cnt;   // 保存重力所在轴的值
    Serial.printf("[IMU] Calibrated bias: x=%.3f y=%.3f z=%.3f\n",
                  s_biasX, s_biasY, s_biasZ);
  }
}

bool imuUpdate(GData &data) {
  if (!qmi.getDataReady()) return false;
  if (!qmi.getAccelerometer(acc.x, acc.y, acc.z)) return false;

  // 去除零偏/重力, 单位 g
  float rx = acc.x - s_biasX;
  float ry = acc.y - s_biasY;
  float rz = acc.z - s_biasZ;

  // 一阶低通滤波
  s_gx += G_FILTER_ALPHA * (rx - s_gx);
  s_gy += G_FILTER_ALPHA * (ry - s_gy);
  s_gz += G_FILTER_ALPHA * (rz - s_gz);

  // 合成平面 G (侧向 + 纵向)
  float total = sqrtf(s_gx * s_gx + s_gy * s_gy);
  if (total > s_gMax) s_gMax = total;

  // ---- 减速带/颠簸检测: 竖直方向短时突变 ----
  float dz = fabsf(s_gz - s_lastGz);
  if (dz > BUMP_THRESHOLD) {
    s_bumpStrength = dz;
    s_bumpUntilMs = millis() + 500;  // 表情持续 500ms
  }
  s_lastGz = s_gz;

  data.gx = s_gx;
  data.gy = s_gy;
  data.gz = s_gz;
  data.gTotal = total;
  data.gMax = s_gMax;
  return true;
}

void imuResetMax() {
  s_gMax = 0;
}

bool imuCheckBump() {
  if (millis() < s_bumpUntilMs) return true;
  return false;
}

float imuBumpStrength() {
  return s_bumpStrength;
}
