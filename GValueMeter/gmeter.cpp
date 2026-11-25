#include "gmeter.h"

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include "qmi8658_min.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

static Preferences   s_prefs;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------ */
bool GMeter::begin() {
  s_prefs.begin("gmeter", false);

  /* 旧格式（无签名）里可能存着上一版的方向设置，直接用默认值覆盖一次，
   * 否则改了 DEF_*_SIGN 也不会生效。 */
  uint8_t sig = s_prefs.getUChar("sig", 0);
  uint8_t ax  = s_prefs.getUChar("ax", 0xFF);
  if (sig == 0xA8 && ax != 0xFF) {
    _latIdx  = ax & 0x03;
    _lonIdx  = (ax >> 2) & 0x03;
    _vertIdx = (ax >> 4) & 0x03;
    _lonSign = (ax & 0x40) ? -1 : 1;
    _latSign = (ax & 0x80) ? -1 : 1;
    if (_latIdx > 2) _latIdx = DEF_LAT_AXIS;
    if (_lonIdx > 2) _lonIdx = DEF_LON_AXIS;
    if (_vertIdx > 2) _vertIdx = DEF_VERT_AXIS;
  } else {
    _latIdx = DEF_LAT_AXIS; _lonIdx = DEF_LON_AXIS; _vertIdx = DEF_VERT_AXIS;
    _latSign = DEF_LAT_SIGN; _lonSign = DEF_LON_SIGN;
    s_prefs.putUChar("sig", 0xA8);
    saveAxis();
  }
  _peak = s_prefs.getFloat("peak", 0.f);
  _autoAxis = s_prefs.getBool("auto", true);

  /* 板上 SA0 通常拉低 => 0x6A；个别批次为 0x6B，两个都试一次 */
  _imuOk = qmiBegin(IIC_SDA, IIC_SCL);
  if (!_imuOk) return false;

  _zeroReq = true;
  _calN = 0;
  _unitKnown = false;
  return true;
}

void GMeter::requestZero() {
  _calN = 0;
  _calSum[0] = _calSum[1] = _calSum[2] = 0.f;
  _calSq[0] = _calSq[1] = _calSq[2] = 0.f;
  _zeroReq = true;
}

void GMeter::resetPeak() {
  _peak = 0.f;
  for (int i = 0; i < 4; i++) _dirPeak[i] = 0.f;
  s_prefs.putFloat("peak", 0.f);
}

void GMeter::setActive(bool on) {
  _active = on;
  if (on) {
    /* 恢复时重置时间基准，避免把暂停时长算进滤波器 */
    _nowMs = millis();
    _lastCurveMs = millis();
  }
}

void GMeter::setAxis(uint8_t lat, uint8_t lon, uint8_t vert, int8_t latSign, int8_t lonSign) {
  if (lat > 2 || lon > 2 || vert > 2) return;
  _latIdx = lat; _lonIdx = lon; _vertIdx = vert;
  _latSign = latSign < 0 ? -1 : 1;
  _lonSign = lonSign < 0 ? -1 : 1;
  _autoAxis = false;
  s_prefs.begin("gmeter", false);
  s_prefs.putUChar("sig", 0xA8);
  s_prefs.putBool("auto", false);
  saveAxis();
}

void GMeter::setAutoAxis(bool on) {
  _autoAxis = on;
  s_prefs.begin("gmeter", false);
  s_prefs.putBool("auto", on);
  s_prefs.putUChar("sig", 0xA8);
  saveAxis();
}

void GMeter::getAxis(uint8_t &lat, uint8_t &lon, uint8_t &vert, int8_t &latSign, int8_t &lonSign) const {
  lat = _latIdx; lon = _lonIdx; vert = _vertIdx;
  latSign = _latSign; lonSign = _lonSign;
}

void GMeter::saveAxis() {
  uint8_t ax = (_latIdx & 3) | ((_lonIdx & 3) << 2) | ((_vertIdx & 3) << 4);
  if (_lonSign < 0) ax |= 0x40;
  if (_latSign < 0) ax |= 0x80;
  s_prefs.putUChar("ax", ax);
}

/* ------------------------------------------------------------------ */
void GMeter::taskStep() {
  if (!_imuOk || !_active) return;

  uint32_t now = millis();

  if (!qmiDataReady()) return;
  float rx, ry, rz;
  if (!qmiReadAccelMG(rx, ry, rz)) return;

  /* ---- 1) 单位自适应：静止时 |a| 应≈1g（g 单位）或 ≈1000（mg 单位） ---- */
  if (!_unitKnown) {
    _calSum[0] += rx; _calSum[1] += ry; _calSum[2] += rz;
    _calN++;
    if (_calN >= 24) {
      float mx = _calSum[0] / _calN, my = _calSum[1] / _calN, mz = _calSum[2] / _calN;
      float m = sqrtf(mx * mx + my * my + mz * mz);
      _unit = (m > 150.f && m < 9000.f) ? 0.001f : 1.f;
      _unitKnown = true;
      _calN = 0;
      _calSum[0] = _calSum[1] = _calSum[2] = 0.f;
    }
    return;
  }

  float v[3] = {rx * _unit, ry * _unit, rz * _unit};
  /* ---- 2) 零点采集（重力矢量） ---- */
  if (_zeroReq) {
    for (int i = 0; i < 3; i++) {
      _calSum[i] += v[i];
      _calSq[i]  += v[i] * v[i];
    }
    _calN++;
    if (_calN >= ZERO_SAMPLES) {
      float varSum = 0.f;
      for (int i = 0; i < 3; i++) {
        float mean = _calSum[i] / _calN;
        _g0[i] = mean;
        float var = _calSq[i] / _calN - mean * mean;
        if (var > 0) varSum += var;
      }
      _noise = sqrtf(varSum / 3.f);            // 静止噪声（各轴标准差 RMS）
      /* 重力估计从零点采样均值出发，之后逐帧低通跟住 */
      _grav[0] = _g0[0]; _grav[1] = _g0[1]; _grav[2] = _g0[2];
      _gmag = sqrtf(_g0[0] * _g0[0] + _g0[1] * _g0[1] + _g0[2] * _g0[2]);
      if (_gmag < 0.75f) _gmag = 0.75f;
      if (_gmag > 1.25f) _gmag = 1.25f;
      _f[0] = _f[1] = _f[2] = 0.f;
      _fast[0] = _fast[1] = _fast[2] = 0.f;
      _mag = _rawMag = 0.f;
      _vertP2P = 0.f;
      _vhIdx = 0; _vhFull = false;
      for (int i = 0; i < VH; i++) _vh[i] = 0.f;
      _curve[_head] = 0.f;
      _calN = 0;
      _calSum[0] = _calSum[1] = _calSum[2] = 0.f;
      _calSq[0] = _calSq[1] = _calSq[2] = 0.f;
      _zeroReq = false;
      _calibrated = true;
      _lastSaveMs = now;
      _nowMs = now;
    }
    return;
  }

  /* ---- 3) 动态加速度 ---- */
  float dt = (now - _nowMs) * 0.001f;
  _nowMs = now;
  if (dt <= 0.f) dt = 0.001f;
  if (dt > 0.2f) dt = 0.2f;

  /* ---- 3) 动态加速度：重力方向实时估计后整条减掉 ----
   * 只减一次固定零点的话，设备换个摆放角度（屏立着 ↔ 平放）重力就会串到
   * 别的轴上：静态读数 0.7G、圆球被顶在下半圆、轴向看起来"调换"。
   * 这里用一阶低通跟住重力方向并单位化：
   *   rd = v·u（沿重力的分量，静止 ≈ 1）
   *   d  = v - rd·u（动态加速度，静止恒为 0，任意姿态都不串轴）
   *   vert = 1 - rd（竖直动态，向上为正） */
  float aG = 1.f - expf(-dt / 2.0f);
  _grav[0] += (v[0] - _grav[0]) * aG;
  _grav[1] += (v[1] - _grav[1]) * aG;
  _grav[2] += (v[2] - _grav[2]) * aG;
  float gl = sqrtf(_grav[0] * _grav[0] + _grav[1] * _grav[1] + _grav[2] * _grav[2]);
  if (gl < 0.25f) {                       /* 估计还没起来：退回固定零点 */
    _grav[0] = _g0[0]; _grav[1] = _g0[1]; _grav[2] = _g0[2];
    gl = sqrtf(_g0[0] * _g0[0] + _g0[1] * _g0[1] + _g0[2] * _g0[2]);
    if (gl < 0.25f) gl = 1.f;
  }
  float u0 = _grav[0] / gl, u1 = _grav[1] / gl, u2 = _grav[2] / gl;

  /* 重力模长也跟住（这块 IMU 原始 |g| 大约 0.87，用它做静止基准，
   * 竖直条才不会一直顶着一个 0.13 的常数偏置） */
  _gmag += (gl - _gmag) * aG;
  if (_gmag < 0.75f) _gmag = 0.75f;
  if (_gmag > 1.25f) _gmag = 1.25f;

  float rd = v[0] * u0 + v[1] * u1 + v[2] * u2;
  float d[3] = {v[0] - rd * u0, v[1] - rd * u1, v[2] - rd * u2};
  float vertDyn = _gmag - rd;

  float lat, lon, vert;
  if (_autoAxis) {
    /* u 更像 Y 轴 → 屏幕立着：纵向取 Z；否则屏幕平放：纵向取 Y */
    bool upright = fabsf(u1) >= fabsf(u2);
    lat  = d[_latIdx] * (float)_latSign;
    lon  = (upright ? d[2] : d[1]) * (float)_lonSign;
    vert = vertDyn;
  } else {
    lat  = pick(d, _latIdx) * (float)_latSign;
    lon  = pick(d, _lonIdx) * (float)_lonSign;
    vert = vertDyn;
  }

  float aSlow = 1.f - expf(-dt / ACC_FILTER_TAU);
  float aFast = 1.f - expf(-dt / PEAK_TAU);

  _f[0] += (lat  - _f[0]) * aSlow;
  _f[1] += (lon  - _f[1]) * aSlow;
  _f[2] += (vert - _f[2]) * aSlow;

  _fast[0] += (lat  - _fast[0]) * aFast;
  _fast[1] += (lon  - _fast[1]) * aFast;
  _fast[2] += (vert - _fast[2]) * aFast;

  _mag = sqrtf(_f[0] * _f[0] + _f[1] * _f[1] + _f[2] * _f[2]);
  _rawMag = sqrtf(_fast[0] * _fast[0] + _fast[1] * _fast[1] + _fast[2] * _fast[2]);

  if (_rawMag > _peak) {
    _peak = _rawMag;
    if (now - _lastSaveMs > 3000) {          // 降低 flash 擦写频率
      s_prefs.putFloat("peak", _peak);
      _lastSaveMs = now;
    }
  }

  /* ---- 3b) 四向峰值（按车况定义，不是按圆点位置） ----
   *  本机轴向实测：左转时 lat<0，右转 lat>0；刹车 lon<0，加速 lon>0。
   *  （表情/圆点按"惯性反向"绘制，与这里的符号相反，见 pet_mode / gg_mode） */
  if (-_f[1] > _dirPeak[0]) _dirPeak[0] = -_f[1];   /* 刹车 */
  if ( _f[1] > _dirPeak[1]) _dirPeak[1] =  _f[1];   /* 加速 */
  if (-_f[0] > _dirPeak[2]) _dirPeak[2] = -_f[0];   /* 左转 */
  if ( _f[0] > _dirPeak[3]) _dirPeak[3] =  _f[0];   /* 右转 */

  /* ---- 4) 颠簸 / 减速带检测：竖直分量 240ms 峰峰值 ---- */
  _vh[_vhIdx] = _f[2];
  _vhIdx = (_vhIdx + 1) % VH;
  if (_vhIdx == 0) _vhFull = true;
  int n = _vhFull ? VH : _vhIdx;
  float vmin = 1e9f, vmax = -1e9f;
  for (int i = 0; i < n; i++) {
    if (_vh[i] < vmin) vmin = _vh[i];
    if (_vh[i] > vmax) vmax = _vh[i];
  }
  _vertP2P = (n > 0) ? (vmax - vmin) : 0.f;

  if (_vertP2P > PET_BUMP_P2P && (now - _lastBumpMs) > PET_BUMP_CD_MS) {
    _lastBumpMs = now;
    _bumpSeq++;
  }

  /* ---- 5) 曲线（10Hz） ---- */
  if (now - _lastCurveMs >= CURVE_PERIOD_MS) {
    _lastCurveMs = now;
    _curve[_head] = _mag;
    _head = (_head + 1) % CURVE_N;
    if (_count < CURVE_N) _count++;
  }

  _samples++;
}

/* ------------------------------------------------------------------ */
void GMeter::snapshot(GSnapshot &o) {
  portENTER_CRITICAL(&s_mux);

  o.lat  = _f[0];
  o.lon  = _f[1];
  o.vert = _f[2];
  o.mag  = _mag;
  o.rawMag = _rawMag;
  o.peak = _peak;
  for (int i = 0; i < 4; i++) o.dirPeak[i] = _dirPeak[i];
  o.vertP2P = _vertP2P;
  o.noise = _noise;
  o.imuOk = _imuOk;
  o.calibrated = _calibrated;
  o.calibrating = _zeroReq;
  o.calProgress = _zeroReq ? (float)_calN / (float)ZERO_SAMPLES : 1.f;
  o.bumpSeq = _bumpSeq;
  o.sampleSeq = _samples;

  o.curveCount = _count;
  int start = (_head - _count + CURVE_N) % CURVE_N;
  for (int i = 0; i < _count; i++) {
    o.curve[i] = _curve[(start + i) % CURVE_N];
  }

  portEXIT_CRITICAL(&s_mux);
}
