#pragma once
/* ===========================================================================
 *  pet_mode.h — 宠物模式（表情机）
 *
 *  表情由「G 事件 + 麦克风」共同驱动，做了分层与优先级：
 *      眼睛 > 视线方向 > 嘴型 > 特效
 *  每个事件带保持时间（几十毫秒到 1 秒不等），避免数值抖动时表情乱闪。
 * =========================================================================== */
#include "canvas_gfx.h"
#include "gmeter.h"
#include "mic_audio.h"

class PetMode {
 public:
  void reset();
  void setStandby(bool on) { _standby = on; }   /* 待机：只播循环动画，不显示 G */
  void setHideG(bool on) { _hideG = on; }       /* 单击按键：宠物页隐藏/显示 G 值 */
  bool hideG() const { return _hideG; }
  void update(const GSnapshot &g, const AudioSnapshot &a, float dt);
  void draw(CanvasGFX *cv, const GSnapshot &g, const AudioSnapshot &a);

 private:
  enum Eyes  { E_IDLE, E_HAPPY, E_SHOCK, E_DIZZY };
  enum Mouth { M_SMILE, M_OPEN, M_O, M_SING };

  void drawBackground(CanvasGFX *c);
  void drawTopBar(CanvasGFX *c, const GSnapshot &g, const AudioSnapshot &a);
  void drawEyes(CanvasGFX *c, float cx, float cy, float R, float mag);
  void drawMouth(CanvasGFX *c, float cx, float cy, float R, float mag);
  void drawEffectsBack(CanvasGFX *c, float cx, float cy, float R);
  void drawEffectsFront(CanvasGFX *c, float cx, float cy, float R,
                        const GSnapshot &g, const AudioSnapshot &a);

  float _t = 0.f;
  bool  _standby = false;
  bool  _hideG = false;

  /* 事件保持时间 */
  float _accT = 0.f, _brkT = 0.f, _turnT = 0.f, _bumpT = 0.f, _shakeT = 0.f;
  int   _turnDir = 1;
  uint32_t _lastBumpSeq = 0;

  /* 视线 / 眨眼（_blinkOpen：1 = 全睁，0 = 闭合，走的是"闭→开"往返曲线） */
  float _look = 0.f, _lookTarget = 0.f;
  float _blinkWait = 2.6f, _blinkAnim = 0.f, _blinkOpen = 1.f;

  /* 表情切换的弹跳包络：1 → 0，正弦×衰减，做出首尾帧来回的观感 */
  float _pop = 0.f, _popWave = 0.f;
  uint8_t _exprKey = 0;

  /* 整体悬浮：脸在屏幕里像泡在液体里的浮子，二阶弹簧跟随 G 方向 */
  float _fx = 0.f, _fy = 0.f, _fvx = 0.f, _fvy = 0.f;

  /* 打哈欠（待机随机触发）：_yawnT 剩余时间，_yawnOpen 0..1 闭眼/张嘴强度 */
  float _yawnWait = 7.5f, _yawnT = 0.f, _yawnOpen = 0.f;

  /* 空闲视线游移：没有转向事件时自己左顾右盼 */
  float _lookIdle = 0.f, _lookWait = 2.0f;

  /* 听歌不是一直唱：唱一小段 → 冷却（这期间演别的小动作）→ 再唱 */
  bool  _singOn = false;
  float _singT = 0.f, _singWait = 0.f;

  /* 待机小动作：点头 / 摇身 / 打喷嚏，轮流来 */
  float _idleWait = 2.6f;
  float _nodT = 0.f, _nodY = 0.f;
  float _swayT = 0.f, _swayX = 0.f;
  float _sneezeT = 0.f, _sneezeJ = 0.f;

  /* 音量包络（驱动背景辉光与腮红的呼吸感） */
  float _lvl = 0.f;

  /* 嘴 */
  float _singLevel = 0.f;

  /* 节拍 */
  uint32_t _lastBeat = 0;
  float _beatT = 0.f;

  /* 背景星点 */
  static const int DOT_N = 26;
  int16_t _dotX[DOT_N], _dotY[DOT_N];
  uint8_t _dotR[DOT_N];
  float _dotP[DOT_N];

  Eyes  _eyes = E_IDLE;
  Mouth _mouth = M_SMILE;
};
