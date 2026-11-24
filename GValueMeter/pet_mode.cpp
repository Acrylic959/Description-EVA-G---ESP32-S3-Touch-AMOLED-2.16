#include "pet_mode.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>

#include "hud.h"

/* ============================ 状态初始化 ============================ */
void PetMode::reset() {
  _t = 0.f;
  _accT = _brkT = _turnT = _bumpT = _shakeT = 0.f;
  _turnDir = 1;
  _lastBumpSeq = 0;
  _look = _lookTarget = 0.f;
  _blinkWait = 2.5f;
  _blinkAnim = 0.f;
  _singLevel = 0.f;
  _yawnWait = 7.5f; _yawnT = 0.f; _yawnOpen = 0.f;
  _lookIdle = 0.f; _lookWait = 2.0f; _lvl = 0.f;
  _singOn = false; _singT = 0.f; _singWait = 0.f;
  _idleWait = 2.6f; _nodT = 0.f; _nodY = 0.f;
  _swayT = 0.f; _swayX = 0.f; _sneezeT = 0.f; _sneezeJ = 0.f;
  _lastBeat = 0;
  _beatT = 0.f;
  _eyes = E_IDLE;
  _mouth = M_SMILE;
  _blinkOpen = 1.f;
  _fx = _fy = _fvx = _fvy = 0.f;
  _pop = 0.f;
  _popWave = 0.f;
  _exprKey = 0;

  /* 背景星点：固定种子，每次开机一致 */
  uint32_t s = 0x1F2E3D4Cu;
  for (int i = 0; i < DOT_N; i++) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    _dotX[i] = 12 + (int16_t)(s % 456);
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    _dotY[i] = 12 + (int16_t)(s % 456);
    _dotR[i] = 1 + (uint8_t)((s >> 8) % 2);
    _dotP[i] = (float)((s >> 16) % 628) * 0.01f;
  }
}

/* ============================== 逻辑更新 ============================== */
void PetMode::update(const GSnapshot &g, const AudioSnapshot &a, float dt) {
  if (dt > 0.05f) dt = 0.05f;
  _t += dt;

  /* --- 事件锁存 --- */
  if (g.bumpSeq != _lastBumpSeq) {
    _lastBumpSeq = g.bumpSeq;
    _bumpT = 1.15f;
    _shakeT = 0.5f;
  }
  if (g.lon > PET_ACC_G) _accT = 0.85f;
  if (g.lon < -PET_BRK_G) _brkT = 0.95f;
  if (fabsf(g.lat) > PET_TURN_G) {
    _turnT = 0.55f;
    /* 惯性方向：左转（lat<0）身体被甩向右（+1），右转甩向左（-1）。
     * 速度线 / 视线都跟着"被甩出去"的那一侧走。 */
    _turnDir = (g.lat > 0.f) ? -1 : 1;
  }

  _accT  = fmaxf(0.f, _accT  - dt);
  _brkT  = fmaxf(0.f, _brkT  - dt);
  _turnT = fmaxf(0.f, _turnT - dt);
  _bumpT = fmaxf(0.f, _bumpT - dt);
  _shakeT = fmaxf(0.f, _shakeT - dt);

  /* --- 听歌：识别到音乐也只唱一小段，然后回去演别的小动作，画面更丰富 --- */
  if (a.music) {
    if (_singT > 0.f) {
      _singT -= dt;
      if (_singT <= 0.f) _singWait = 2.6f + (float)random(0, 3400) / 1000.f;
    } else if (_singWait > 0.f) {
      _singWait -= dt;
    } else {
      _singT = 1.8f + (float)random(0, 1400) / 1000.f;
    }
    _singOn = (_singT > 0.f);
  } else {
    _singT = 0.f; _singWait = 0.f; _singOn = false;
  }

  /* --- 待机小动作：点头 / 摇身 / 打喷嚏 轮流来，间隔 3.2~6.4s --- */
  if (_standby && _yawnT <= 0.f && _singT <= 0.f) {
    _idleWait -= dt;
    if (_idleWait <= 0.f) {
      _idleWait = 3.2f + (float)random(0, 3200) / 1000.f;
      int pick = (int)random(0, 3);
      if (pick == 0)      { _nodT = 1.5f; }
      else if (pick == 1) { _swayT = 2.6f; }
      else                { _sneezeT = 1.2f; }
    }
  } else {
    _idleWait = 3.2f;
  }
  if (_nodT > 0.f)    _nodT = fmaxf(0.f, _nodT - dt);
  if (_swayT > 0.f)   _swayT = fmaxf(0.f, _swayT - dt);
  if (_sneezeT > 0.f) _sneezeT = fmaxf(0.f, _sneezeT - dt);

  /* 点头：上下小幅点头；摇身：左右轻摆；打喷嚏：先缩后一记前冲 */
  _nodY = (_nodT > 0.f) ? 5.f * sinf((1.5f - _nodT) * 6.283185f / 0.75f) : 0.f;
  _swayX = (_swayT > 0.f) ? 9.f * sinf((2.6f - _swayT) * 6.283185f / 1.3f) : 0.f;
  {
    float u = (_sneezeT > 0.f) ? (1.2f - _sneezeT) / 1.2f : 0.f;
    _sneezeJ = 0.f;
    if (u > 0.10f && u < 0.40f) { float k = (u - 0.10f) / 0.30f; _sneezeJ = -7.f * k; }
    else if (u >= 0.40f && u < 0.70f) { float k = (u - 0.40f) / 0.30f; _sneezeJ = -7.f + 18.f * k; }
  }

  /* --- 眼睛（优先级：喷嚏 > 颠簸 > 刹车/高G > 加速 > 听歌 > 待机） --- */
  if (_bumpT > 0.f)                 _eyes = E_DIZZY;
  else if (_brkT > 0.f)             _eyes = E_SHOCK;
  else if (g.mag > HIGH_G_LIMIT)    _eyes = E_SHOCK;
  else if (_accT > 0.f)             _eyes = E_HAPPY;
  else if (_singOn)                 _eyes = E_HAPPY;
  else                              _eyes = E_IDLE;

  /* --- 视线：转向优先；没有转向时自己左顾右盼，比一直盯着正前方更像活的 --- */
  if (_turnT > 0.f) {
    _lookIdle = 0.f;
    _lookTarget = (float)_turnDir;
  } else {
    _lookWait -= dt;
    if (_lookWait <= 0.f) {
      _lookWait = 1.7f + (float)random(0, 2600) / 1000.f;
      _lookIdle = (float)((int)random(-100, 101)) / 100.f;   /* -1..1 */
    }
    if (_yawnOpen > 0.2f) _lookIdle = 0.f;
    _lookIdle += (0.f - _lookIdle) * (1.f - expf(-dt / 0.85f));
    _lookTarget = _lookIdle;
  }
  _look += (_lookTarget - _look) * (1.f - expf(-dt / 0.10f));

  /* --- 眨眼（仅待机）：0.26s 一次，闭 45% 时间、睁 55%，两头都走 smoothstep --- */
  if (_eyes == E_IDLE) {
    _blinkWait -= dt;
    if (_blinkWait <= 0.f) {
      _blinkWait = 3.2f + (float)(random(0, 2200)) / 1000.f;
      _blinkAnim = 1.f;
    }
    if (_blinkAnim > 0.f) {
      _blinkAnim = fmaxf(0.f, _blinkAnim - dt / 0.26f);
      float u = 1.f - _blinkAnim;                       /* 0 → 1 的进度 */
      if (u < 0.45f) {
        float k = u / 0.45f;
        _blinkOpen = 1.f - k * k * (3.f - 2.f * k);     /* 快速闭眼 */
      } else {
        float k = (u - 0.45f) / 0.55f;
        _blinkOpen = k * k * (3.f - 2.f * k);           /* 缓慢睁开 */
      }
    } else {
      _blinkOpen = 1.f;
    }
  } else {
    _blinkAnim = 0.f;
    _blinkOpen = 1.f;
  }
  /* 打哈欠时眼睛越闭越紧（和普通眨眼取更小的那个） */
  if (_yawnOpen > 0.f) _blinkOpen = fminf(_blinkOpen, 1.f - 0.95f * _yawnOpen);
  if (_sneezeT > 0.f)  _blinkOpen = fminf(_blinkOpen, 0.18f);

  /* --- 嘴型 --- */
  float target = _singOn ? a.level : 0.f;
  _singLevel += (target - _singLevel) * (1.f - expf(-dt / 0.045f));

  if (_yawnOpen > 0.25f)                   _mouth = M_O;
  else if (_bumpT > 0.f)                   _mouth = M_O;
  else if (_brkT > 0.f)                    _mouth = M_O;
  else if (_singOn && _singLevel > 0.10f)  _mouth = M_SING;
  else if (_accT > 0.f)                    _mouth = M_OPEN;
  else                                     _mouth = M_SMILE;

  /* --- 整体悬浮：目标是"惯性甩出去"的方向 × 增益，二阶弹簧追，外加很慢的"液体"漂移 ---
   *  刹车（lon<0）身体前冲 → 屏幕上方；左转（lat<0）身体右甩 → 屏幕右侧。 */
  {
    const float MAX_X = 74.f, MAX_Y = 58.f;
    float tx = -g.lat * 86.f + 4.0f * sinf(_t * 0.72f) + 2.6f * sinf(_t * 1.21f + 1.7f);
    float ty =  g.lon * 74.f + 3.2f * sinf(_t * 0.88f + 0.6f) + 2.0f * sinf(_t * 1.37f + 2.4f);
    if (tx >  MAX_X) tx =  MAX_X;  if (tx < -MAX_X) tx = -MAX_X;
    if (ty >  MAX_Y) ty =  MAX_Y;  if (ty < -MAX_Y) ty = -MAX_Y;

    /* x'' = k(目标 - x) - c x'：欠阻尼一点点，撞到目标会轻微回弹，像水里 */
    const float K = 10.5f, C = 4.2f;   /* 更软一点：回摆更大，像被甩出去 */
    _fvx += (K * (tx - _fx) - C * _fvx) * dt;
    _fvy += (K * (ty - _fy) - C * _fvy) * dt;
    _fx += _fvx * dt;
    _fy += _fvy * dt;
    if (_fx >  MAX_X + 3.f) { _fx =  MAX_X + 3.f; _fvx = 0.f; }
    if (_fx < -MAX_X - 3.f) { _fx = -MAX_X - 3.f; _fvx = 0.f; }
    if (_fy >  MAX_Y + 3.f) { _fy =  MAX_Y + 3.f; _fvy = 0.f; }
    if (_fy < -MAX_Y - 3.f) { _fy = -MAX_Y - 3.f; _fvy = 0.f; }
  }

  /* --- 表情弹跳：眼睛/嘴型一变就重新起跳，正弦 × 线性衰减 = 来回两下 --- */
  uint8_t key = (uint8_t)((int)_eyes * 4 + (int)_mouth);
  if (key != _exprKey) {
    _exprKey = key;
    _pop = 1.f;
  }
  if (_pop > 0.f) _pop = fmaxf(0.f, _pop - dt / 0.45f);
  _popWave = sinf(_pop * 6.283185f) * _pop;     /* 先过冲、再回落、再小过冲 */

  /* --- 待机打哈欠：每 6.5~12.5 秒一次，闭眼 + 大张嘴 + 头顶 Zzz --- */
  if (_yawnT > 0.f) _yawnT = fmaxf(0.f, _yawnT - dt);
  if (_standby && _yawnT <= 0.f) {
    _yawnWait -= dt;
    if (_yawnWait <= 0.f) {
      _yawnT = 2.4f;
      _yawnWait = 6.5f + (float)random(0, 6000) / 1000.f;
    }
  }
  if (_yawnT > 0.f) {
    float u = 1.f - _yawnT / 2.4f;                 /* 0 → 1 */
    _yawnOpen = sinf(3.141592f * u);               /* 张到最大再收回 */
  } else {
    _yawnOpen = 0.f;
    if (!_standby) _yawnWait = 6.5f;               /* 开测后重新计时 */
  }

  _lvl += (a.level - _lvl) * (1.f - expf(-dt / 0.09f));

  /* --- 节拍 --- */
  if (a.beatSeq != _lastBeat) {
    _lastBeat = a.beatSeq;
    _beatT = 0.f;
  }
  _beatT += dt;
}

/* ================================ 绘制 ================================ */
static const uint16_t WH = RGB565(255, 255, 255);   /* 眼白 */
static const uint16_t KB = RGB565(0, 0, 0);         /* 纯黑底 */

/* G 值 → 强调色（绿 / 琥珀 / 红，与参考视频的上限配色一致） */
static inline uint16_t gAccent(float mag) {
  if (mag > HIGH_G_LIMIT) return COL_RED;
  if (mag > 0.45f)        return COL_AMBER;
  return COL_GREEN;
}

void PetMode::draw(CanvasGFX *c, const GSnapshot &g, const AudioSnapshot &a) {
  drawBackground(c);
  drawTopBar(c, g, a);

  /* 呼吸 + 抖动 */
  /* 呼吸：相位再套一层正弦，吸气快、末尾略停，比纯正弦更像活物 */
  float ba = _t * 2.1f;
  float breathe = 1.f + 0.018f * sinf(ba + 0.38f * sinf(ba * 2.f));
  float ox = 0.f, oy = 0.f;
  if (_shakeT > 0.f) {
    float k = _shakeT / 0.5f;
    k = k * k * (1.6f - 0.6f * k);               /* 抖动衰减用曲线，收尾更自然 */
    ox = sinf(_t * 61.f) * 11.f * k;
    oy = cosf(_t * 44.f) * 9.f * k;
  }
  /* 悬浮偏移 34px，加上抖动；脸底部 258+30+172 ≈ 460，仍在屏内 */
  float R = 172.f * breathe;
  float cx = 240.f + ox + _fx + _swayX;
  float cy = 258.f + oy + _fy + 10.f + _nodY + _sneezeJ;

  drawEffectsBack(c, cx, cy, R);
  drawEyes(c, cx, cy, R, g.mag);
  drawMouth(c, cx, cy, R, g.mag);
  drawEffectsFront(c, cx, cy, R, g, a);
}

/* ------------------------------ 背景 ------------------------------ */
void PetMode::drawBackground(CanvasGFX *c) {
  c->fillRect(0, 0, 480, 480, KB);
  for (int i = 0; i < DOT_N; i += 3) {
    float tw = 0.5f + 0.5f * sinf(_t * 1.6f + _dotP[i]);
    c->fillCircle(_dotX[i], _dotY[i], 1, fade565(COL_GREEN, KB, 0.08f + 0.12f * tw));
  }
}

/* --------------------- 顶部：当前 / 最大 + 二维气泡 --------------------- */
void PetMode::drawTopBar(CanvasGFX *c, const GSnapshot &g, const AudioSnapshot &a) {
  /* 单击按键隐藏 G 值、或待机时：整块顶部面板与二维气泡都不画，只留一张干净的脸 */
  if (_hideG || _standby) return;

  uint16_t accent = gAccent(g.mag);

  hudPanel(c, 30, 24, 286, 70, 20, RGB565(8, 11, 18),
           lerp565(COL_PRO_PANEL, accent, 0.30f));

  char buf[24];
  if (_standby) {
    /* 待机：不显示 G 值，提示怎么开测 */
    hudCN(c, 30, 17, 24, COL_GREEN, "待机");
    hudCN(c, 30, 49, 16, COL_DIM, "长按 BOOT 开测");
  } else {
    hudCN(c, 46, 29, 24, COL_DIM, "当前");
    snprintf(buf, sizeof(buf), "%.2f", g.mag);
    hudTextR(c, 282, 29, 3, accent, buf);
    hudCN(c, 288, 36, 16, lerp565(COL_DIM, accent, 0.5f), "G");

    hudCN(c, 46, 59, 24, COL_DIM, "最大");
    snprintf(buf, sizeof(buf), "%.2f", g.peak);
    hudTextR(c, 282, 59, 3, COL_AMBER, buf);
    hudCN(c, 288, 66, 16, lerp565(COL_DIM, COL_AMBER, 0.5f), "G");
  }

  /* 右侧 2D 气泡：点表示"惯性甩出去"的方向（与大脸、圆盘同一约定） */
  float bx = 414.f, by = 64.f, br = 28.f;
  hudRing(c, bx, by, br, br, 1.5f, lerp565(COL_PRO_PANEL, COL_DIM, 0.55f));
  c->fillRect((int16_t)(bx - 6), (int16_t)by, 12, 1, lerp565(COL_PRO_PANEL, COL_DIM, 0.35f));
  c->fillRect((int16_t)bx, (int16_t)(by - 6), 1, 12, lerp565(COL_PRO_PANEL, COL_DIM, 0.35f));

  float lx = g.lat, ly = g.lon;
  if (lx > 1.f) lx = 1.f;  if (lx < -1.f) lx = -1.f;
  if (ly > 1.f) ly = 1.f;  if (ly < -1.f) ly = -1.f;
  float dx = bx - lx * (br - 7.f);      /* 左转 → 点向右 */
  float dy = by + ly * (br - 7.f);      /* 刹车 → 点向上 */
  c->fillCircle((int16_t)dx, (int16_t)dy, 9, fade565(accent, KB, 0.35f));
  c->fillCircle((int16_t)dx, (int16_t)dy, 5, accent);

  (void)a;
}

/* ------------------------------ 眼睛 ------------------------------ */
void PetMode::drawEyes(CanvasGFX *c, float cx, float cy, float R, float mag) {
  /* 贴边透视：脸被"挤"到屏幕边缘时，双眼间距收紧、整体略微缩一圈 */
  float kx = fabsf(_fx) / 74.f;  if (kx > 1.f) kx = 1.f;
  float ky = fabsf(_fy) / 58.f;  if (ky > 1.f) ky = 1.f;
  float ks = sqrtf(kx * kx + ky * ky);  if (ks > 1.f) ks = 1.f;
  float sqX  = 1.f - 0.26f * kx;          /* 横向挤压：两眼靠拢 */
  float sAll = 1.f - 0.13f * ks;          /* 整体略缩：像被推远 */

  float baseY = cy - R * 0.04f;
  float baseDX = R * 0.43f * sqX * sAll;
  float baseRX = R * 0.28f * sAll;
  float baseRY = R * 0.34f * sAll;
  float lookX = _look * R * 0.13f;

  /* 眼睛外面的辉光：常态偏粉，高 G 转琥珀/红（视频里的那层 bloom） */
  uint16_t glow = (mag > HIGH_G_LIMIT) ? COL_RED : (mag > 0.45f ? COL_AMBER : COL_PINK);

  for (int i = 0; i < 2; i++) {
    /* 转弯时左右眼被"压扁/拉长"，做出侧倾的立体感 */
    float skew = 1.f - 0.13f * _look * (i == 0 ? 1.f : -1.f);
    float eyeRX = baseRX * skew * (1.f + 0.07f * _popWave);
    float eyeRY = baseRY * (1.f + 0.10f * _popWave);
    float dy = (i == 0 ? -1.f : 1.f) * R * 0.022f * _look;
    float ex = cx + (i == 0 ? -baseDX : baseDX) + lookX;
    float eyeY = baseY + dy;

    for (int k = 0; k < 2 && _eyes == E_IDLE; k++) {   /* 辉光少一层，省 5~6ms */
      float s = 1.22f - k * 0.075f;   /* 保持调用顺序稳定，占位无实际作用 */
      hudEllipseFill(c, (int16_t)ex, (int16_t)eyeY,
                     (int16_t)(eyeRX * s), (int16_t)(eyeRY * s),
                     fade565(glow, KB, 0.06f + k * 0.06f));
    }

    switch (_eyes) {
      case E_IDLE: {
        float ry = eyeRY * (0.10f + 0.90f * _blinkOpen);
        if (ry < eyeRY * 0.22f) {
          /* 闭眼：压成一条胶囊，闭合瞬间正好是"首帧 → 尾帧"的中点 */
          hudCapsule(c, ex - eyeRX * 0.95f, eyeY, ex + eyeRX * 0.95f, eyeY,
                     (float)(10.f + 5.f * _blinkOpen), WH);
        } else {
          hudEllipseFill(c, (int16_t)ex, (int16_t)eyeY, (int16_t)eyeRX, (int16_t)ry, WH);
          /* 瞳孔随睁眼比例缩放，闭合过程中不会"跳" */
          float k = _blinkOpen;
          hudEllipseFill(c, (int16_t)(ex + lookX * 0.55f), (int16_t)eyeY,
                         (int16_t)(eyeRX * 0.52f * k), (int16_t)(ry * 0.52f), KB);
          if (k > 0.35f) {
            c->fillCircle((int16_t)(ex - eyeRX * 0.16f + lookX * 0.55f),
                          (int16_t)(eyeY - ry * 0.26f), (int16_t)(eyeRX * 0.15f * k), WH);
            c->fillCircle((int16_t)(ex + eyeRX * 0.18f + lookX * 0.55f),
                          (int16_t)(eyeY + ry * 0.22f), (int16_t)(eyeRX * 0.07f * k), WH);
          }
        }
        break;
      }
      case E_HAPPY: {
        /* ∩ ∩：白色平滑粗弧线（视频里哼歌/兴奋时的笑眼） */
        int16_t pts[42];
        for (int k2 = 0; k2 < 21; k2++) {
          float u = (k2 - 10) / 10.f;
          pts[k2 * 2]     = (int16_t)(ex + u * eyeRX * 0.95f);
          pts[k2 * 2 + 1] = (int16_t)(eyeY + eyeRY * 0.42f - (1.f - u * u) * eyeRY * 0.92f);
        }
        hudPolyline(c, pts, 21, 15.f + 2.2f * _popWave, WH);
        break;
      }
      case E_SHOCK: {
        /* > < ：向鼻梁挤的惊慌眼 */
        int16_t pts[6];
        float sx = (i == 0) ? 1.f : -1.f;
        float hw = eyeRX * 0.62f, hh = eyeRY * 0.72f;
        pts[0] = (int16_t)(ex - sx * hw);  pts[1] = (int16_t)(eyeY - hh);
        pts[2] = (int16_t)(ex + sx * hw);  pts[3] = (int16_t)(eyeY);
        pts[4] = (int16_t)(ex - sx * hw);  pts[5] = (int16_t)(eyeY + hh);
        hudPolyline(c, pts, 3, 13.f, WH);
        break;
      }
      case E_DIZZY: {
        hudCapsule(c, ex - eyeRX * 0.80f, eyeY - eyeRY * 0.72f,
                   ex + eyeRX * 0.80f, eyeY + eyeRY * 0.72f, 12.f, WH);
        hudCapsule(c, ex - eyeRX * 0.80f, eyeY + eyeRY * 0.72f,
                   ex + eyeRX * 0.80f, eyeY - eyeRY * 0.72f, 12.f, WH);
        break;
      }
    }
  }
}

/* ------------------------------ 嘴 ------------------------------ */
void PetMode::drawMouth(CanvasGFX *c, float cx, float cy, float R, float mag) {
  /* 纵向挤压：脸贴上下边缘时，嘴离眼睛更近（五官间距变小） */
  float ky = fabsf(_fy) / 58.f;  if (ky > 1.f) ky = 1.f;
  float ks = fabsf(_fx) / 74.f;  if (ks > 1.f) ks = 1.f;
  float sAll = 1.f - 0.13f * sqrtf(ky * ky + ks * ks + 1e-6f); if (sAll < 0.86f) sAll = 0.86f;
  float my = cy + R * 0.55f * (1.f - 0.30f * ky) * sAll;
  R = R * sAll;
  uint16_t orange = (mag > HIGH_G_LIMIT) ? COL_RED : COL_ORANGE;

  /* 打哈欠：一张慢慢张大、再合上的椭圆嘴，里头露出暗红的舌根 */
  if (_yawnOpen > 0.15f) {
    float w = R * (0.11f + 0.20f * _yawnOpen);
    float h = R * (0.05f + 0.26f * _yawnOpen);
    hudEllipseFill(c, (int16_t)cx, (int16_t)my, (int16_t)(w * 1.25f), (int16_t)(h * 1.25f),
                   fade565(orange, KB, 0.10f));
    hudEllipseFill(c, (int16_t)cx, (int16_t)my, (int16_t)w, (int16_t)h, orange);
    hudEllipseFill(c, (int16_t)cx, (int16_t)(my + h * 0.45f),
                   (int16_t)(w * 0.75f), (int16_t)(h * 0.45f), fade565(COL_RED, KB, 0.55f));
    return;
  }

  switch (_mouth) {
    case M_SMILE:
      hudEllipseFill(c, (int16_t)cx, (int16_t)my,
                     (int16_t)(R * 0.115f * (1.f + 0.10f * _popWave)),
                     (int16_t)(R * 0.050f * (1.f - 0.30f * _popWave)), WH);
      break;
    case M_OPEN: {
      float rr = R * 0.15f * (1.f + 0.22f * _popWave);
      hudEllipseFill(c, (int16_t)cx, (int16_t)my, (int16_t)(rr * 1.3f), (int16_t)(rr * 1.3f),
                     fade565(orange, KB, 0.09f));
      hudEllipseFill(c, (int16_t)cx, (int16_t)my, (int16_t)rr, (int16_t)rr, orange);
      break;
    }
    case M_O: {
      float rr = R * (0.085f + (mag > 1.f ? 1.f : mag) * 0.10f) * (1.f + 0.14f * _popWave);
      hudEllipseFill(c, (int16_t)cx, (int16_t)my, (int16_t)(rr * 1.35f), (int16_t)(rr * 1.35f),
                     fade565(orange, KB, 0.10f));
      hudEllipseFill(c, (int16_t)cx, (int16_t)my, (int16_t)rr, (int16_t)rr, orange);
      break;
    }
    case M_SING: {
      float rr = R * 0.16f + _singLevel * R * 0.14f;
      hudEllipseFill(c, (int16_t)cx, (int16_t)my, (int16_t)(rr * 1.25f), (int16_t)(rr * 1.25f),
                     fade565(COL_ORANGE, KB, 0.06f));
      hudEllipseFill(c, (int16_t)cx, (int16_t)my, (int16_t)rr, (int16_t)rr, COL_ORANGE);
      break;
    }
  }
}

/* --------------------------- 特效：底层 --------------------------- */
void PetMode::drawEffectsBack(CanvasGFX *c, float cx, float cy, float R) {
  /* 加速/转弯的绿色跳动速度线已按要求删除，改由整脸位移来表达惯性 */
  (void)c; (void)cx; (void)cy; (void)R;
}

/* --------------------------- 特效：顶层 --------------------------- */
void PetMode::drawEffectsFront(CanvasGFX *c, float cx, float cy, float R,
                               const GSnapshot &g, const AudioSnapshot &a) {
  /* 减速带 / 颠簸：旋转星星 */
  if (_bumpT > 0.2f) {
    float k = (_bumpT - 0.2f) / 0.95f;
    if (k > 1.f) k = 1.f;
    for (int i = 0; i < 4; i++) {
      float ang = _t * 5.2f + 0.7853982f + i * 1.5707963f;
      float rr = R * 1.10f;
      float sx = cx + cosf(ang) * rr;
      float sy = cy + sinf(ang) * rr * 0.70f;
      float rr2 = 24.f + 6.f * sinf(_t * 9.f + i);
      hudSparkle(c, sx, sy, rr2 * 1.35f, 3.2f, fade565(COL_AMBER, KB, 0.16f + 0.24f * k));
      hudSparkle(c, sx, sy, rr2, 4.0f, fade565(COL_AMBER, KB, 0.45f + 0.55f * k));
    }
  }

  /* 刹车 / 高 G：感叹号 */
  float bangK = (_brkT > 0.30f) ? _brkT / 0.95f : 0.f;
  if (g.mag > HIGH_G_LIMIT) bangK = fmaxf(bangK, 0.65f + 0.35f * sinf(_t * 18.f));
  if (bangK > 0.05f) {
    if (bangK > 1.f) bangK = 1.f;
    float len = 22.f + 4.f * sinf(_t * 14.f);
    hudBang(c, cx, 100.f, len, 11.f, fade565(COL_RED, KB, 0.30f + 0.70f * bangK));
  }

  /* 打哈欠：头顶飘出三个越来越大的 Z */
  if (_yawnOpen > 0.20f) {
    for (int i = 0; i < 3; i++) {
      float ph = fmodf(_t * 0.55f + (float)i * 0.33f, 1.f);
      float zx = cx + R * 0.55f + (float)i * 15.f;
      float zy = cy - R * 0.68f - ph * 66.f;
      float sz = 6.f + (float)i * 5.f;
      int16_t zp[8] = {
        (int16_t)(zx - sz), (int16_t)zy,
        (int16_t)(zx + sz), (int16_t)zy,
        (int16_t)(zx - sz), (int16_t)(zy + sz),
        (int16_t)(zx + sz), (int16_t)(zy + sz)
      };
      hudPolyline(c, zp, 4, 3.f, fade565(COL_PINK, KB, 0.30f + 0.55f * (1.f - ph)));
    }
  }

  /* 开心 / 听歌：脸颊两点腮红 */
  float blush = (_accT > 0.f) ? 0.85f : (_singOn ? 0.55f : 0.f);
  if (blush > 0.05f) {
    for (int i = 0; i < 2; i++) {
      float bx = cx + (i ? 1.f : -1.f) * R * 0.60f;
      float by = cy + R * 0.22f;
      hudEllipseFill(c, (int16_t)bx, (int16_t)by,
                     (int16_t)(R * 0.13f), (int16_t)(R * 0.065f),
                     fade565(COL_PINK, KB, 0.26f * blush));
    }
  }

  /* 高 G：太阳穴上一滴慢慢滑下的汗 */
  if (g.mag > HIGH_G_LIMIT) {
    float k = (g.mag - HIGH_G_LIMIT) / 0.5f;
    if (k > 1.f) k = 1.f;
    float sx = cx + R * 0.84f;
    float sy = cy - R * 0.30f + 8.f * sinf(_t * 2.6f);
    hudEllipseFill(c, (int16_t)sx, (int16_t)(sy + 4.f), (int16_t)6, (int16_t)10,
                   fade565(COL_TEAL, KB, 0.35f + 0.5f * k));
  }

  /* 听歌（只在唱歌那一段）：音符洒在脸周围往上飘 */
  if (_singOn) {
    for (int i = 0; i < 3; i++) {
      float ph = fmodf(_t * 0.5f + i * 0.34f, 1.f);
      float ang = -1.1f + i * 1.1f;
      float nx = cx + cosf(ang) * R * 1.02f + sinf(_t * 2.f + i) * 6.f;
      float ny = 350.f - ph * 250.f;
      hudNote(c, nx, ny, 17.f, fade565(WH, KB, 0.30f + 0.70f * (1.f - ph)));
    }
  }
}
