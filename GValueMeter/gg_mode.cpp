#include "gg_mode.h"

#include "pin_config.h"
#include "hud.h"

/* 圆盘几何：中心和半径。边缘刚好 1.0G。
 * 上下各留 ~70px 给标题行 / 大号读数 / 四向峰值 / 底部三格。 */
static const float GG_CX = 240.f;
static const float GG_CY = 242.f;
static const float GG_R  = 136.f;

/* 安全边距：硬件圆角盖住屏幕四角，所有贴边文字向内缩 */
static const int16_t SAFE_L = 36;
static const int16_t SAFE_R = 444;

static const uint16_t WH  = RGB565(255, 255, 255);
static const uint16_t KB  = RGB565(0, 0, 0);

/* 连续渐变：0G 亮绿 → 0.5G 青 → 0.8G 琥珀 → 1G 以上红 */
static inline uint16_t ggAccent(float mag) {
  if (mag <= 0.35f) return lerp565(COL_GREEN, COL_TEAL, mag / 0.35f);
  if (mag <= 0.60f) return lerp565(COL_TEAL, COL_AMBER, (mag - 0.35f) / 0.25f);
  if (mag <= 0.85f) return lerp565(COL_AMBER, COL_ORANGE, (mag - 0.60f) / 0.25f);
  float k = (mag - 0.85f) / 0.55f;
  return lerp565(COL_ORANGE, COL_RED, k > 1.f ? 1.f : k);
}

/* 夹在 ±1G：超出边缘的点贴边显示，不画出圆外 */
static inline float clamp1(float v) { return v > 1.f ? 1.f : (v < -1.f ? -1.f : v); }

/* 圆点位置 = 惯性甩出去的方向（和赛车 G-G 表、宠物脸一致）：
 *   刹车（lon<0）→ 点向上；加速 → 向下
 *   左转（lat<0） → 点向右；右转 → 向左                                       */
static inline void ggMap(const GSnapshot &g, float &x, float &y) {
  x = clamp1(-g.lat);
#ifdef GG_BRAKE_UP
  y = clamp1(GG_BRAKE_UP ? g.lon : -g.lon);
#else
  y = clamp1(g.lon);
#endif
}

void GgMode::reset() {
  _head = _count = 0;
  _acc = 0.f;
  _t = 0.f;
  _alarmHold = 0.f;
}

void GgMode::pushTrail(float x, float y) {
  _tx[_head] = x;
  _ty[_head] = y;
  _head = (_head + 1) % TRAIL_N;
  if (_count < TRAIL_N) _count++;
}

void GgMode::update(const GSnapshot &g, float dt) {
  if (dt > 0.05f) dt = 0.05f;
  _t += dt;
  if (g.mag > HIGH_G_LIMIT) _alarmHold = 0.45f;
  else if (_alarmHold > 0.f) _alarmHold = fmaxf(0.f, _alarmHold - dt);
  _acc += dt;
  if (_acc >= 0.04f) {                 /* 25Hz 记一个点，12 秒画满一圈 */
    _acc -= 0.04f;
    if (_acc > 0.04f) _acc = 0.f;
    float x, y;
    ggMap(g, x, y);
    pushTrail(x, y);
  }
}

/* 四向峰值小读数：只留数字（不再跟 "G"），还没出现过的方向用暗色 0.00 占位 */
static void drawDirPeak(CanvasGFX *c, int16_t x, int16_t y, float v, bool rightAlign) {
  char b[12];
  snprintf(b, sizeof(b), "%.2f", v);
  uint16_t col = (v > 0.005f) ? ggAccent(v) : lerp565(COL_PRO_PANEL, COL_DIM, 0.55f);
  if (rightAlign) hudTextR(c, x, y, 2, col, b);
  else            hudText(c, x, y, 2, col, b);
}

void GgMode::draw(CanvasGFX *c, const GSnapshot &g) {
  c->fillRect(0, 0, 480, 480, KB);

  bool alarm = (_alarmHold > 0.f);
  uint16_t accent = ggAccent(g.mag);

  /* ---- 顶部：标题 / 历史最大（左右各内缩 36px，避开硬件圆角） ---- */
  hudCN(c, SAFE_L, 26, 24, lerp565(COL_DIM, COL_GREEN, 0.25f), "G-G 圆盘");
  char buf[24];
  snprintf(buf, sizeof(buf), "%.2f", g.peak);
  int16_t vw = (int16_t)(strlen(buf) * 12);            /* size2 每字符 12px */
  hudCN(c, (int16_t)(SAFE_R - 16), 32, 16, lerp565(COL_DIM, COL_AMBER, 0.4f), "G");
  hudTextR(c, (int16_t)(SAFE_R - 20), 32, 2, COL_AMBER, buf);
  hudCNR(c, (int16_t)(SAFE_R - 20 - vw - 12), 33, 16, COL_DIM, "最大");

  /* ---- 当前合成 G：大号读数 ---- */
  snprintf(buf, sizeof(buf), "%.2f", g.mag);
  hudTextC(c, 240, 28, 4, accent, buf);
  hudCN(c, 302, 33, 16, lerp565(COL_DIM, accent, 0.5f), "G");

  /* ---- 四向峰值（取代原来的 刹车/加速/左转/右转 文字标签） ---- */
  drawDirPeak(c, 208, 88, g.dirPeak[0], false);      /* 上：刹车峰值 */
  drawDirPeak(c, 208, 386, g.dirPeak[1], false);     /* 下：加速峰值 */
  drawDirPeak(c, (int16_t)(GG_CX - GG_R - 8), 234, g.dirPeak[2], true);    /* 左：左转峰值 */
  drawDirPeak(c, (int16_t)(GG_CX + GG_R + 6), 234, g.dirPeak[3], false);   /* 右：右转峰值 */

  /* ---- 圆盘：外圈 / 0.5G 圈 / 十字 / 对角 ---- */
  uint16_t ringCol = lerp565(COL_PRO_PANEL, COL_DIM, 0.95f);
  /* 外圈 1.0G：画三条紧邻的圆线，比逐点弧更实 */
  for (int k = -1; k <= 1; k++) {
    c->drawCircle((int16_t)GG_CX, (int16_t)GG_CY, (int16_t)(GG_R + k), ringCol);
  }
  c->drawCircle((int16_t)GG_CX, (int16_t)GG_CY, (int16_t)(GG_R * 0.5f),
                fade565(COL_DIM, KB, 0.40f));
  c->drawCircle((int16_t)GG_CX, (int16_t)GG_CY, (int16_t)(GG_R * 0.25f),
                fade565(COL_DIM, KB, 0.22f));

  /* 十字 + 两条淡对角（赛车仪表那种网格） */
  c->fillRect((int16_t)(GG_CX - GG_R), (int16_t)GG_CY, (int16_t)(GG_R * 2), 1,
              fade565(COL_DIM, KB, 0.30f));
  c->fillRect((int16_t)GG_CX, (int16_t)(GG_CY - GG_R), 1, (int16_t)(GG_R * 2),
              fade565(COL_DIM, KB, 0.30f));
  for (int s = -1; s <= 1; s += 2) {
    float dx = GG_R * 0.7071f * s, dy = GG_R * 0.7071f;
    hudCapsule(c, GG_CX - dx, GG_CY - dy, GG_CX + dx, GG_CY + dy, 1.f,
               fade565(COL_DIM, KB, 0.14f));
  }

  /* 刻度文字：0.5G / 1.0G */
  hudText(c, (int16_t)(GG_CX + GG_R * 0.5f + 6), (int16_t)(GG_CY - GG_R * 0.5f - 6), 1,
          fade565(COL_DIM, KB, 0.45f), "0.5G");
  hudText(c, (int16_t)(GG_CX + GG_R * 0.72f), (int16_t)(GG_CY - GG_R * 0.72f), 1,
          fade565(COL_DIM, KB, 0.42f), "1.0G");

  /* ---- 轨迹：旧点越暗，新点越亮 ---- */
  if (_count > 1) {
    int idx = _head - _count;
    if (idx < 0) idx += TRAIL_N;
    float lx = 0.f, ly = 0.f;
    bool has = false;
    for (int i = 0; i < _count; i++) {
      int k = (idx + i) % TRAIL_N;
      float px = GG_CX + _tx[k] * GG_R;
      float py = GG_CY + _ty[k] * GG_R;
      if (has) {
        float a = (float)i / (float)_count;         /* 0 = 最旧 */
        float al = 0.06f + 0.62f * a * a;
        float m2 = _tx[k] * _tx[k] + _ty[k] * _ty[k];
        float m = sqrtf(m2);                       /* 这一点的 G 大小，决定拖尾颜色 */
        hudCapsule(c, lx, ly, px, py, 2.5f, fade565(ggAccent(m), KB, al));
      }
      lx = px; ly = py; has = true;
    }
  }

  /* ---- 当前点：光晕 + 实心 ---- */
  float vx, vy;
  ggMap(g, vx, vy);
  float px = GG_CX + vx * GG_R;
  float py = GG_CY + vy * GG_R;
  int16_t dr = 9 + (int16_t)(3.f * (g.mag > 1.f ? 1.f : g.mag));

  c->fillCircle((int16_t)px, (int16_t)py, dr + 11, fade565(accent, KB, alarm ? 0.16f : 0.10f));
  c->fillCircle((int16_t)px, (int16_t)py, dr + 6, fade565(accent, KB, alarm ? 0.30f : 0.22f));
  c->fillCircle((int16_t)px, (int16_t)py, dr + 3, fade565(accent, KB, alarm ? 0.55f : 0.35f));
  c->fillCircle((int16_t)px, (int16_t)py, dr, accent);
  c->fillCircle((int16_t)(px - dr * 0.35f), (int16_t)(py - dr * 0.38f), dr / 3,
                fade565(WH, accent, 0.55f));

  /* ---- 底部三格：侧向 / 纵向 / 竖直（整组内缩到 36..444） ---- */
  struct Axis { const char *name; float v; };
  Axis axes[3] = {{"侧向", g.lat}, {"纵向", g.lon}, {"竖直", g.vert}};
  for (int i = 0; i < 3; i++) {
    int16_t bx = (int16_t)(SAFE_L + i * 138);
    hudPanel(c, bx, 408, 132, 42, 12, RGB565(8, 11, 18),
             lerp565(COL_PRO_PANEL, COL_DIM, 0.45f));
    hudCN(c, bx + 10, 410, 16, COL_DIM, axes[i].name);
    char vb[16];
    snprintf(vb, sizeof(vb), "%+.2f", axes[i].v);
    uint16_t vc = ggAccent(fabsf(axes[i].v));
    hudText(c, bx + 12, 428, 2, vc, vb);
    int16_t wv = (int16_t)(strlen(vb) * 12);
    hudCN(c, (int16_t)(bx + 18 + wv), 429, 16, lerp565(COL_DIM, vc, 0.5f), "G");
  }

  /* ---- 报警：贴边红框闪 + 圆盘内一圈红色辉光（视频里那圈红光） ---- */
  if (alarm) {
    float ph = 0.5f + 0.5f * sinf(_t * 12.f);
    hudGlowBorder(c, 16, 16, 448, 448, 40, COL_RED, KB, 0.30f + 0.62f * ph);
    /* 圆盘内侧一圈平滑红色辉光：逐半径降低透明度，不会出现"珠链" */
    /* 辉光向圆盘内侧收，别往圆外溢——圆盘外侧还有四向峰值文字 */
    for (int k = 0; k <= 12; k++) {
      float al = (0.55f * ph + 0.22f) * (1.f - (float)k / 13.f);
      c->drawCircle((int16_t)GG_CX, (int16_t)GG_CY, (int16_t)(GG_R - 2 - k), fade565(COL_RED, KB, al));
    }
  }
}
