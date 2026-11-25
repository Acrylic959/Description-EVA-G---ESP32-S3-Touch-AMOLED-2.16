#pragma once
/* ===========================================================================
 *  gmeter.h — QMI8658 采样 / 重力零点校准 / 动态 G 解算 / 峰值 / 曲线 / 颠簸检测
 *
 *  设计要点：
 *   1) 上电或手动校准时把"静止重力矢量"记下来，之后所有输出都是
 *      相对该矢量的动态加速度 —— 这样无论板子怎么固定在车上，
 *      真正的重力都不会被当成 G 值（和 G-Tech 表的做法一致）。
 *   2) 采样在独立任务里跑（200Hz），UI 只读快照，互不干扰。
 *   3) 单位自适应：不同版本 SensorLib 返回 g 或 mg，启动时按静止
 *      |a|≈1.0 或 ≈1000 自动判定，避免单位错乱导致数字乱飞。
 * =========================================================================== */
#include <stdint.h>
#include "pin_config.h"

struct GSnapshot {
  float lat;        // 侧向（左/右）动态 g
  float lon;        // 纵向（加速+/刹车-）动态 g
  float vert;       // 竖直（颠簸）动态 g
  float mag;        // 合成 |G|（滤波后，用于显示/报警）
  float rawMag;     // 合成 |G|（未滤波，用于峰值）
  float peak;       // 历史最大 |G|
  float dirPeak[4]; // 四向峰值：0 刹车 1 加速 2 左转 3 右转（本次开测以来）
  float vertP2P;    // 最近 240ms 竖直峰峰值
  float noise;      // 静止噪声水平
  int   curveCount;
  float curve[CURVE_N];   // 旧 -> 新
  bool  imuOk;
  bool  calibrated;
  bool  calibrating;
  float calProgress;      // 0..1
  uint32_t bumpSeq;       // 颠簸事件计数（递增）
  uint32_t sampleSeq;
};

class GMeter {
 public:
  bool begin();
  void requestZero();                        // 请求重新取零点
  void resetPeak();                          // 清总峰值 + 四向峰值
  void setActive(bool on);                   // 待机：任务暂停采样（省电）
  bool imuOk() const { return _imuOk; }
  void taskStep();                            // IMU 任务里 200Hz 调用
  void snapshot(GSnapshot &out);              // UI 侧调用（临界区内拷贝）
  float peak() const { return _peak; }
  uint32_t sampleCount() const { return _samples; }

  /* 轴向映射：lat/lon/vert 取 0=X 1=Y 2=Z；latSign/lonSign 决定屏幕上的正方向
   * 约定：lat<0=左转、lon<0=刹车（惯性动效与四向峰值都按此计算） */
  void setAxis(uint8_t lat, uint8_t lon, uint8_t vert, int8_t latSign, int8_t lonSign);
  void getAxis(uint8_t &lat, uint8_t &lon, uint8_t &vert, int8_t &latSign, int8_t &lonSign) const;
  void saveAxis();
  void setAutoAxis(bool on);                 // 自动姿态（横放/竖放自动换轴向）
  bool autoAxis() const { return _autoAxis; }

 private:
  float pick(const float *v, uint8_t idx) const { return v[idx]; }

  bool     _imuOk = false;
  bool     _active = true;      /* false = 待机，taskStep 直接返回 */
  bool     _calibrated = false;
  bool     _zeroReq = true;
  bool     _unitKnown = false;
  float    _unit = 1.0f;

  uint8_t  _latIdx = DEF_LAT_AXIS;
  uint8_t  _lonIdx = DEF_LON_AXIS;
  uint8_t  _vertIdx = DEF_VERT_AXIS;
  int8_t   _latSign = DEF_LAT_SIGN;
  int8_t   _lonSign = DEF_LON_SIGN;

  /* 自动姿态：屏幕立着（Y 更竖）用 lon=Z；屏幕平放用 lon=Y、vert=Z。
   * 重力方向实时估计后整条减掉，横放/竖放/斜拿静态读数都归零。 */
  bool     _autoAxis = true;
  float    _grav[3] = {0, 0, 1};      // 重力方向（单位化前的原始矢量）
  float    _gmag = 0.9f;              // 重力模长估计（IMU 刻度可能不是 1.000）

  /* 校准累加 */
  int      _calN = 0;
  float    _calSum[3] = {0, 0, 0};
  float    _calSq[3] = {0, 0, 0};

  float    _g0[3] = {0, 0, 0};
  float    _f[3] = {0, 0, 0};        // 滤波后的动态分量
  float    _fast[3] = {0, 0, 0};
  float    _mag = 0, _rawMag = 0, _peak = 0;
  float    _dirPeak[4] = {0, 0, 0, 0};   /* 刹车 / 加速 / 左转 / 右转 */
  float    _noise = 0;

  /* 竖直历史（颠簸） */
  static const int VH = 48;
  float    _vh[VH] = {0};
  int      _vhIdx = 0;
  bool     _vhFull = false;
  uint32_t _lastBumpMs = 0;
  uint32_t _bumpSeq = 0;
  float    _vertP2P = 0;

  /* 曲线 */
  float    _curve[CURVE_N] = {0};
  int      _head = 0, _count = 0;
  uint32_t _lastCurveMs = 0;

  uint32_t _samples = 0;
  uint32_t _lastSaveMs = 0;
  uint32_t _nowMs = 0;
};
