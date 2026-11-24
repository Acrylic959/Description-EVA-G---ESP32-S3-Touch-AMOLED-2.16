#include "pro_mode.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>

#include "hud.h"

namespace {

const int16_t CH_X0 = 24, CH_X1 = 456, CH_Y0 = 326, CH_Y1 = 446;
const int16_t BAR_X0 = 112, BAR_X1 = 452, BAR_CEN = 282, BAR_HALF = 170, BAR_H = 20;

/* 曲线纵轴映射：0 -> 底边，CHART_FULL_SCALE -> 顶边 */
inline int16_t chY(float v) {
  float k = v / CHART_FULL_SCALE;
  if (k < 0.f) k = 0.f;
  if (k > 1.f) k = 1.f;
  return (int16_t)(CH_Y1 - k * (CH_Y1 - CH_Y0));
}

/* 数值 -> 颜色：青 → 琥珀 → 红 */
inline uint16_t valueColor(float v) {
  if (v <= 0.25f) return lerp565(COL_GREEN, COL_TEAL, v / 0.25f);
  if (v <= 0.45f) return lerp565(COL_TEAL, COL_AMBER, (v - 0.25f) / 0.20f);
  if (v <= HIGH_G_LIMIT) return COL_AMBER;
  float k = (v - HIGH_G_LIMIT) / 0.35f;
  return lerp565(COL_AMBER, COL_RED, k > 1.f ? 1.f : k);
}

}  // namespace

void ProMode::reset() {
  _bars[0] = _bars[1] = _bars[2] = 0.f;
  _magS = 0.f;
  _peakS = 0.f;
}

void ProMode::update(const GSnapshot &g, float dt) {
  if (dt > 0.05f) dt = 0.05f;
  float k = 1.f - expf(-dt / 0.06f);
  _bars[0] += (g.lat  - _bars[0]) * k;
  _bars[1] += (g.lon  - _bars[1]) * k;
  _bars[2] += (g.vert - _bars[2]) * k;
  _magS += (g.mag - _magS) * (1.f - expf(-dt / 0.05f));
  _peakS = g.peak;
}

/* ------------------------------------------------------------------ */
void ProMode::draw(CanvasGFX *c, const GSnapshot &g) {
  c->fillRect(0, 0, 480, 480, COL_PRO_BG);

  /* 顶部渐变亮线 */
  for (int16_t x = 0; x < 480; x += 3) {
    c->fillRect(x, 0, 3, 2, lerp565(COL_TEAL, COL_VIOLET, (float)x / 480.f));
  }

  drawHeader(c, g);
  drawBigValue(c, g);

  /* 说明文字 + 分隔线 */
  hudCNC(c, _bigCx, 168, 16, COL_DIM, "合成加速度");
  c->fillRect(0, 192, 480, 1, COL_PRO_GRID);

  drawBars(c, g);

  hudCN(c, 24, 296, 24, COL_DIM, "G值曲线");
  hudCNR(c, 452, 296, 24, COL_DIM, "120点 · 12秒");

  drawChart(c, g);

  /* 报警：横幅常亮（底色呼吸）+ 红框闪烁 */
  if (g.mag > HIGH_G_LIMIT) {
    bool on = ((millis() / 220) & 1);
    uint16_t bg = on ? lerp565(COL_PRO_BG, COL_RED, 0.34f)
                     : lerp565(COL_PRO_BG, COL_RED, 0.12f);
    hudPanel(c, 150, 16, 180, 42, 12, bg, COL_RED);
    hudCNC(c, 240, 21, 32, COL_RED, "高G报警");
    drawAlarm(c, on);
  }
}

/* ------------------------------------------------------------------ */
void ProMode::drawHeader(CanvasGFX *c, const GSnapshot &g) {
  if (g.mag <= HIGH_G_LIMIT) hudCN(c, 18, 6, 24, COL_DIM, "G值表");

  char b[16];
  snprintf(b, sizeof(b), "%.2f", _peakS);
  int16_t pw = hudTextW(3, b);
  hudTextR(c, 462, 8, 3, COL_AMBER, b);
  hudCNR(c, 462 - pw - 10, 8, 24, lerp565(COL_DIM, COL_AMBER, 0.35f), "最大");
}

void ProMode::drawBigValue(CanvasGFX *c, const GSnapshot &g) {
  const int16_t H_ = 78;
  float v = g.mag > 9.99f ? 9.99f : g.mag;
  uint16_t on = valueColor(v);
  uint16_t off = 0;   /* 不画灭段：只要亮段，读数最干净 */

  char b[16];
  snprintf(b, sizeof(b), "%.2f", v);
  int16_t w = ssMeasure(H_, b);
  /* 数值 + 单位作为一组居中 */
  int16_t gw = hudCNW(16, "G");
  int16_t x = (int16_t)((480 - (w + 10 + gw)) / 2);
  _bigCx = (int16_t)(x + w / 2);
  int16_t ty = (g.mag > HIGH_G_LIMIT) ? 70 : 64;   /* 报警横幅在上，数字让开一点 */
  ssNumber(c, x, ty, H_, v, 2, on, off);
  hudCN(c, (int16_t)(x + w + 10), (int16_t)(ty + H_ / 2 - 8), 16, COL_TEXT, "G");
}

void ProMode::drawBars(CanvasGFX *c, const GSnapshot &g) {
  hudCN(c, 24, 184, 24, COL_DIM, "三轴分量");
  hudCNR(c, 452, 190, 16, COL_DIM, "满量程 ±1G");

  {
    /* 三条都按自己的绝对值取色：绿 → 青 → 琥珀 → 红，超过 0.8G 自然变红 */
    float a0 = fabsf(_bars[0]), a1 = fabsf(_bars[1]), a2 = fabsf(_bars[2]);
    drawAxisBar(c, 214, "侧向", _bars[0], valueColor(a0));
    drawAxisBar(c, 242, "纵向", _bars[1], valueColor(a1));
    drawAxisBar(c, 270, "竖直", _bars[2], valueColor(a2));
  }

  (void)g;
}

void ProMode::drawAxisBar(CanvasGFX *c, int16_t y, const char *label,
                          float v, uint16_t col) {
  hudCN(c, 22, y + 2, 16, COL_TEXT, label);
  char b[12];
  snprintf(b, sizeof(b), "%+.2f", v);
  hudTextR(c, 106, y + 6, 1, COL_DIM, b);

  c->drawRect(BAR_X0, y - 1, BAR_X1 - BAR_X0, BAR_H + 1, COL_PRO_GRID);

  /* ±0.5g 刻度 */
  c->fillRect(BAR_CEN - BAR_HALF / 2, y + BAR_H - 4, 1, 4, COL_PRO_GRID);
  c->fillRect(BAR_CEN + BAR_HALF / 2, y + BAR_H - 4, 1, 4, COL_PRO_GRID);

  float k = v / BAR_FULL_SCALE;
  if (k > 0.98f) k = 0.98f;
  if (k < -0.98f) k = -0.98f;
  int16_t len = (int16_t)(BAR_HALF * fabsf(k));
  if (len > 0) {
    if (v > 0) c->fillRect(BAR_CEN + 1, y, len, BAR_H, col);
    else       c->fillRect(BAR_CEN - 1 - len, y, len, BAR_H, col);
  }

  /* 中心零位 */
  c->fillRect(BAR_CEN - 1, y, 3, BAR_H, lerp565(COL_PRO_GRID, COL_TEXT, 0.35f));
}

/* ------------------------------------------------------------------ */
void ProMode::drawChart(CanvasGFX *c, const GSnapshot &g) {
  c->fillRect(CH_X0, CH_Y0, CH_X1 - CH_X0 + 1, CH_Y1 - CH_Y0 + 1, COL_PRO_PANEL);

  /* 横向网格：0.4 / 0.8 / 1.2 */
  for (float v = 0.4f; v < CHART_FULL_SCALE + 0.01f; v += 0.4f) {
    c->fillRect(CH_X0, chY(v), CH_X1 - CH_X0 + 1, 1, COL_PRO_GRID);
  }
  /* 纵向网格：每 2 秒 */
  for (int i = 1; i < 6; i++) {
    int16_t x = CH_X0 + (CH_X1 - CH_X0) * i / 6;
    c->fillRect(x, CH_Y0, 1, CH_Y1 - CH_Y0 + 1, COL_PRO_GRID);
  }

  /* 曲线（面积填充 + 亮线） */
  int n = g.curveCount;
  int span = CH_X1 - CH_X0;
  if (n >= 2) {
    for (int px = 0; px <= span; px++) {
      float t = (float)px * (float)(n - 1) / (float)span;
      int i0 = (int)t;
      if (i0 > n - 1) i0 = n - 1;
      int i1 = (i0 + 1 < n) ? i0 + 1 : n - 1;
      float fr = t - (float)i0;
      float v = g.curve[i0] * (1.f - fr) + g.curve[i1] * fr;
      int16_t y = chY(v);
      if (y > CH_Y1) y = CH_Y1;
      uint16_t col = valueColor(v);
      int16_t x = CH_X0 + px;
      if (CH_Y1 - y > 1) {
        c->fillRect(x, y + 2, 1, CH_Y1 - y - 1, lerp565(COL_PRO_PANEL, col, 0.30f));
      }
      c->fillRect(x, y, 1, 2, col);
    }
  }

  /* 0.8G 报警参考线（红色虚线） */
  int16_t y08 = chY(HIGH_G_LIMIT);
  for (int16_t x = CH_X0; x < CH_X1; x += 14) {
    c->fillRect(x, y08 - 1, 8, 2, COL_RED);
  }

  /* 历史最大 G 标记线 */
  if (g.peak > 0.05f && g.peak <= CHART_FULL_SCALE) {
    int16_t yp = chY(g.peak);
    for (int16_t x = CH_X0; x < CH_X1; x += 20) {
      c->fillRect(x, yp, 6, 1, COL_AMBER);
    }
  }

  c->drawRect(CH_X0, CH_Y0, CH_X1 - CH_X0 + 1, CH_Y1 - CH_Y0 + 1, COL_PRO_GRID);

  int16_t ly = CH_Y1 + 4;
  hudCN(c, CH_X0, ly, 16, COL_DIM, "-12秒");
  hudCNR(c, CH_X1, ly, 16, COL_DIM, "现在");

  /* 图例：红虚线 = 0.8 报警线，琥珀虚线 = 历史最大值线 */
  int16_t lx = 166;
  c->fillRect(lx, ly + 7, 12, 2, COL_RED);
  hudCN(c, lx + 17, ly, 16, COL_DIM, "0.8报警");
  int16_t x2 = (int16_t)(lx + 17 + hudCNW(16, "0.8报警") + 12);
  c->fillRect(x2, ly + 7, 12, 2, COL_AMBER);
  hudCN(c, x2 + 17, ly, 16, COL_DIM, "最大");
}

void ProMode::drawAlarm(CanvasGFX *c, bool on) {
  if (!on) return;
  c->fillRect(0, 0, 480, 3, COL_RED);
  c->fillRect(0, 477, 480, 3, COL_RED);
  c->fillRect(0, 0, 3, 480, COL_RED);
  c->fillRect(477, 0, 3, 480, COL_RED);
}
