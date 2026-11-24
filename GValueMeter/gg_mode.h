#pragma once
/* ===========================================================================
 *  gg_mode.h — 第三页：赛车式 G-G 圆盘（摩擦圆）
 *  圆内一点表示当前合成方向，边缘 = 1.0G；点会留下轨迹；> 0.8G 红框闪。
 * =========================================================================== */
#include <Arduino.h>

#include "canvas_gfx.h"
#include "gmeter.h"

class GgMode {
 public:
  static const int TRAIL_N = 300;      /* 25Hz × 12s */

  GgMode() { reset(); }
  void reset();
  void update(const GSnapshot &g, float dt);
  void draw(CanvasGFX *c, const GSnapshot &g);

 private:
  void pushTrail(float x, float y);

  float _tx[TRAIL_N];
  float _ty[TRAIL_N];
  int   _head;
  int   _count;
  float _acc;
  float _t;
  float _alarmHold;   /* 报警迟滞：过 0.80G 后继续亮 0.45s */
};
