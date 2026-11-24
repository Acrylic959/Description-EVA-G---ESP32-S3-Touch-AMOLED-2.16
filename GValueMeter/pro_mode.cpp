#include "pro_mode.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>

#include "hud.h"

namespace {

/* 硬件圆角会盖住屏幕四角：所有贴边元素左右各内缩到 [36, 444] */
const int16_t CH_X0 = 30, CH_X1 = 450, CH_Y0 = 314, CH_Y1 = 418;
const int16_t BAR_X0 = 104, BAR_X1 = 450, BAR_CEN = 277, BAR_HALF = 173, BAR_H = 20;

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
  _needFull = true;
  _alarmPrev = false;
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
  bool alarmNow = (g.mag > HIGH_G_LIMIT);
  /* 整屏填充要 21ms；日常只清两块"会变"的区域，省下的时间换成帧率 */
  if (_needFull || alarmNow != _alarmPrev) {
    c->fillRect(0, 0, 480, 480, COL_PRO_BG);
    _needFull = false;
  } else {
    c->fillRect(0, 0, 480, 46, COL_PRO_BG);      /* 表头：数字宽度会变，必须清，否则残影重叠 */
    c->fillRect(40, 44, 400, 146, COL_PRO_BG);   /* 大数字 / 单位 G / 报警横幅 */
    c->fillRect(0, 190, 480, 118, COL_PRO_BG);   /* 满量程标签 + 三条分量条 + 分隔线 */
  }
  _alarmPrev = alarmNow;

  drawHeader(c, g);
  drawBigValue(c, g);

  /* 分隔线 */
  c->fillRect(0, 192, 480, 1, COL_PRO_GRID);

  drawBars(c, g);

  hudCN(c, 30, 284, 24, COL_DIM, "G值曲线");

  drawChart(c, g);

  /* 报警：横幅放在大数字与分量条之间（不压任何别的元素），红框走单层辉光 */
  if (alarmNow) {
    float ph = 0.5f + 0.5f * sinf(millis() * 0.0055f);   /* 平滑呼吸（原来那条整屏红框已按反馈删除） */
    uint16_t bg = lerp565(COL_PRO_BG, COL_RED, 0.18f + 0.22f * ph);
    hudPanel(c, 152, 150, 176, 34, 14, bg, lerp565(COL_RED, COL_PRO_BG, 0.35f + 0.5f * ph));
    hudGlowBorder(c, 152, 150, 176, 34, 14, COL_RED, bg, 0.18f + 0.30f * ph);
    hudCNC(c, 240, 158, 24, lerp565(COL_RED, COL_TEXT, 0.15f + 0.35f * ph), "高G报警");
  }
}

/* ------------------------------------------------------------------ */
void ProMode::drawHeader(CanvasGFX *c, const GSnapshot &g) {
  (void)g;
  hudCN(c, 30, 22, 24, COL_DIM, "G值表");
  /* 电量：放在标题与"最大"之间 */
  if (_batPct >= 0) drawBattery(c, 112, 25);

  char b[16];
  snprintf(b, sizeof(b), "%.2f", _peakS);
  int16_t pw = hudTextW(3, b);
  hudTextR(c, 450, 24, 3, COL_AMBER, b);
  hudCNR(c, 440 - pw - 10, 24, 24, lerp565(COL_DIM, COL_AMBER, 0.35f), "最大");
}

/* 电池图标 + 百分比：绿 >50% / 琥珀 20~50% / 红 <20%（充电时青色 + 闪电） */
void ProMode::drawBattery(CanvasGFX *c, int16_t x, int16_t y) {
  const int16_t bw = 22, bh = 13;
  int pct = _batPct;
  if (pct > 100) pct = 100;
  if (pct < 0)   pct = 0;

  uint16_t col = (pct >= 50) ? COL_GREEN : (pct >= 20 ? COL_AMBER : COL_RED);
  if (_batChg) col = COL_TEAL;
  uint16_t shell = lerp565(COL_PRO_BG, COL_DIM, 0.85f);   /* 外壳用暗灰绿，和电量条分得开 */

  /* 外壳（1px 描边）+ 右侧正极小尾巴 */
  c->fillRect(x, y, bw, bh, shell);
  c->fillRect(x + 1, y + 1, bw - 2, bh - 2, COL_PRO_BG);
  c->fillRect(x + bw, y + 4, 2, bh - 8, shell);

  /* 电量条：内区 (bw-4)×(bh-4)，贴左生长，剩 1px 缝隙 */
  int16_t iw = bw - 4, ih = bh - 4;
  int16_t fw = (int16_t)((iw * pct + 50) / 100);
  if (pct > 0 && fw < 1) fw = 1;
  if (fw > 0) c->fillRect(x + 2, y + 2, fw, ih, col);

  /* 充电：叠一个闪电 */
  if (_batChg) {
    int16_t mx = x + bw / 2, my = y + 1;
    c->fillRect(mx - 3, my + 2, 7, 4, COL_PRO_BG);
    c->fillRect(mx - 1, my + 5, 6, 3, COL_PRO_BG);
    c->fillRect(mx + 1, my + 8, 4, 3, COL_PRO_BG);
    hudCapsule(c, (float)(mx + 2), (float)(my + 1), (float)(mx - 2), (float)(my + 6), 2.f, COL_TEXT);
    hudCapsule(c, (float)(mx - 2), (float)(my + 6), (float)(mx + 1), (float)(my + 11), 2.f, COL_TEXT);
  }

  /* 百分比 */
  char b[8];
  snprintf(b, sizeof(b), "%d%%", pct);
  hudText(c, x + bw + 7, y + 1, 2, col, b);
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
  int16_t ty = 62;
  ssNumber(c, x, ty, H_, v, 2, on, off);
  hudCN(c, (int16_t)(x + w + 10), (int16_t)(ty + H_ / 2 - 8), 16, COL_TEXT, "G");
}

void ProMode::drawBars(CanvasGFX *c, const GSnapshot &g) {
  hudCNR(c, 450, 170, 16, COL_DIM, "±1G 满量程");

  {
    /* 三条都按自己的绝对值取色：绿 → 青 → 琥珀 → 红，超过 0.8G 自然变红 */
    float a0 = fabsf(_bars[0]), a1 = fabsf(_bars[1]), a2 = fabsf(_bars[2]);
    drawAxisBar(c, 206, "侧向", _bars[0], valueColor(a0));
    drawAxisBar(c, 232, "纵向", _bars[1], valueColor(a1));
    drawAxisBar(c, 258, "竖直", _bars[2], valueColor(a2));
  }

  (void)g;
}

void ProMode::drawAxisBar(CanvasGFX *c, int16_t y, const char *label,
                          float v, uint16_t col) {
  hudCN(c, 30, y + 2, 16, COL_TEXT, label);
  char b[12];
  snprintf(b, sizeof(b), "%+.2f", v);
  hudTextR(c, 96, y + 6, 1, COL_DIM, b);

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
    for (int px = 0; px <= span; px += 2) {
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
        c->fillRect(x, y + 2, 2, CH_Y1 - y - 1, lerp565(COL_PRO_PANEL, col, 0.30f));
      }
      c->fillRect(x, y, 2, 2, col);
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
  /* 面板外圈一圈极淡辉光，让曲线块从背景里浮出来 */
  c->drawRect(CH_X0 - 1, CH_Y0 - 1, CH_X1 - CH_X0 + 3, CH_Y1 - CH_Y0 + 3,
              fade565(COL_TEAL, COL_PRO_BG, 0.22f));

  int16_t ly = CH_Y1 + 6;              /* 图例压在安全线以内（y<450），不再被圆角切 */
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


