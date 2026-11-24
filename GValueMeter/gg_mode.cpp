#include "gg_mode.h"

#include "pin_config.h"
#include "hud.h"

/* 圆盘几何：中心和半径。边缘刚好 1.0G。 */
static const float GG_CX = 240.f;
static const float GG_CY = 252.f;
static const float GG_R  = 152.f;

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

void GgMode::reset() {
  _head = _count = 0;
  _acc = 0.f;
  _t = 0.f;
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
  _acc += dt;
  if (_acc >= 0.04f) {                 /* 25Hz 记一个点，12 秒画满一圈 */
    _acc -= 0.04f;
    if (_acc > 0.04f) _acc = 0.f;
    float x = clamp1(g.lat);
#ifdef GG_BRAKE_UP
    float y = clamp1(GG_BRAKE_UP ? g.lon : -g.lon);   /* 刹车（LON 负）朝上 */
#else
    float y = clamp1(g.lon);
#endif
    pushTrail(x, y);
  }
}

void GgMode::draw(CanvasGFX *c, const GSnapshot &g) {
  c->fillRect(0, 0, 480, 480, KB);

  bool alarm = (g.mag > HIGH_G_LIMIT);
  uint16_t accent = ggAccent(g.mag);

  /* 屏边一圈极淡的绿光，和宠物页保持同一套底子 */
  hudRing(c, 240.f, 240.f, 238.f, 238.f, 2.f, fade565(COL_GREEN, KB, alarm ? 0.04f : 0.10f));

  /* ---- 顶部：标题 / 历史最大 ---- */
  hudCN(c, 20, 2, 24, lerp565(COL_DIM, COL_GREEN, 0.25f), "G-G 圆盘");
  char buf[24];
  snprintf(buf, sizeof(buf), "%.2f", g.peak);
  int16_t vw = (int16_t)(strlen(buf) * 12);            /* size2 每字符 12px */
  hudTextR(c, 458, 8, 2, COL_AMBER, buf);
  hudCN(c, 456, 8, 16, lerp565(COL_DIM, COL_AMBER, 0.4f), "G");
  hudCNR(c, 456 - vw - 14, 9, 16, COL_DIM, "最大");

  /* ---- 当前合成 G：大号读数 ---- */
  snprintf(buf, sizeof(buf), "%.2f", g.mag);
  hudTextC(c, 240, 30, 4, accent, buf);
  hudCN(c, 300, 34, 16, lerp565(COL_DIM, accent, 0.5f), "G");

  /* ---- 四向标签 ---- */
  uint16_t dim = lerp565(COL_PRO_PANEL, COL_DIM, 0.75f);
  hudCNC(c, 240, 72, 24, dim, "刹车");
  hudCNC(c, 240, 406, 24, dim, "加速");
  hudCNR(c, (int16_t)(GG_CX - GG_R - 10), 240, 24, dim, "左转");
  hudCN(c, (int16_t)(GG_CX + GG_R + 10), 240, 24, dim, "右转");

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
  float vx = clamp1(g.lat);
#ifdef GG_BRAKE_UP
  float vy = clamp1(GG_BRAKE_UP ? g.lon : -g.lon);
#else
  float vy = clamp1(g.lon);
#endif
  float px = GG_CX + vx * GG_R;
  float py = GG_CY + vy * GG_R;
  int16_t dr = 9 + (int16_t)(3.f * (g.mag > 1.f ? 1.f : g.mag));

  c->fillCircle((int16_t)px, (int16_t)py, dr + 11, fade565(accent, KB, alarm ? 0.16f : 0.10f));
  c->fillCircle((int16_t)px, (int16_t)py, dr + 6, fade565(accent, KB, alarm ? 0.30f : 0.22f));
  c->fillCircle((int16_t)px, (int16_t)py, dr + 3, fade565(accent, KB, alarm ? 0.55f : 0.35f));
  c->fillCircle((int16_t)px, (int16_t)py, dr, accent);
  c->fillCircle((int16_t)(px - dr * 0.35f), (int16_t)(py - dr * 0.38f), dr / 3,
                fade565(WH, accent, 0.55f));

  /* ---- 底部三格：侧向 / 纵向 / 竖直 ---- */
  struct Axis { const char *name; float v; };
  float a0 = g.lat, a1 = g.lon, a2 = g.vert;
  Axis axes[3] = {{"侧向", a0}, {"纵向", a1}, {"竖直", a2}};
  for (int i = 0; i < 3; i++) {
    int16_t bx = (int16_t)(6 + i * 160);
    hudPanel(c, bx, 428, 148, 46, 12, RGB565(8, 11, 18),
             lerp565(COL_PRO_PANEL, COL_DIM, 0.45f));
    hudCN(c, bx + 12, 430, 16, COL_DIM, axes[i].name);
    char vb[16];
    snprintf(vb, sizeof(vb), "%+.2f", axes[i].v);
    uint16_t vc = ggAccent(fabsf(axes[i].v));
    hudText(c, bx + 14, 450, 2, vc, vb);
    int16_t wv = (int16_t)(strlen(vb) * 12);
    hudCN(c, bx + 20 + wv, 451, 16, lerp565(COL_DIM, vc, 0.5f), "G");
  }

  /* ---- 报警：贴边红框闪 + 圆盘内一圈红色辉光（视频里那圈红光） ---- */
  if (alarm) {
    float ph = 0.5f + 0.5f * sinf(_t * 12.f);
    hudRoundBorder(c, 3, 3, 474, 474, 26, fade565(COL_RED, KB, 0.25f + 0.65f * ph));
    hudRoundBorder(c, 8, 8, 464, 464, 22, fade565(COL_RED, KB, 0.10f + 0.35f * ph));
    /* 圆盘内侧一圈平滑红色辉光：逐半径降低透明度，不会出现"珠链" */
    for (int k = 0; k <= 14; k++) {
      float al = (0.55f * ph + 0.22f) * (1.f - (float)k / 15.f);
      c->drawCircle((int16_t)GG_CX, (int16_t)GG_CY, (int16_t)(GG_R + k - 2), fade565(COL_RED, KB, al));
    }
  }
}
