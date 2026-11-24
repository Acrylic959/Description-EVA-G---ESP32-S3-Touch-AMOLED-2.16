#include "spectrum.h"

#include <math.h>

#include "hud.h"
#include "pin_config.h"

namespace {

/* 柱区几乎铺满整条屏宽：柱体只在 y=100..392 之间，远离四角圆角（圆角只吃 y<30 / y>450），
 * 所以左右可以开到 8 / 472，视觉上和屏幕左右边缘只剩一道极窄的缝。 */
const int16_t SX0  = 8;
const int16_t SX1  = 472;
const int16_t BASE = 392;   /* 基线（柱底） */
const int16_t TOP  = 100;   /* 最高柱顶 */
const int16_t GAP  = 4;     /* 柱间距 */

const uint16_t BG     = RGB565(3, 5, 9);
const uint16_t GRID   = RGB565(9, 14, 20);

/* 柱内纵向配色：底部青 → 中段绿 → 高处琥珀 → 顶部红 */
inline uint16_t specCol(float f) {
  if (f <= 0.40f) return lerp565(COL_TEAL, COL_GREEN, f / 0.40f);
  if (f <= 0.72f) return lerp565(COL_GREEN, COL_AMBER, (f - 0.40f) / 0.32f);
  return lerp565(COL_AMBER, COL_RED, (f - 0.72f) / 0.28f);
}

}  // namespace

void spectrumDraw(CanvasGFX *c, const float *bands, const float *pk,
                  float t, float level) {
  /* --- 背景：近黑 + 细网格 + 基线 --- */
  c->fillRect(0, 0, 480, 480, BG);
  for (int16_t y = BASE - 36; y > TOP - 10; y -= 36) {
    c->fillRect(SX0, y, SX1 - SX0, 1, GRID);
  }
  const float slot = (float)(SX1 - SX0 + 1) / (float)AUDIO_BANDS;
  for (int16_t i = 0; i < AUDIO_BANDS; i += 2) {
    int16_t x = (int16_t)(SX0 + slot * (float)i);
    c->fillRect(x, TOP - 8, 1, BASE - TOP + 8, GRID);
  }
  c->fillRect(SX0, BASE, SX1 - SX0, 1, lerp565(BG, COL_GREEN, 0.30f));

  /* --- 基线下方一条随音量呼吸的横向辉光（倒影区的底噪） --- */
  float glow = 0.06f + 0.22f * (level > 1.f ? 1.f : level);
  for (int16_t i = 0; i < 4; i++) {
    c->fillRect(SX0, BASE + 5 + i * 6, SX1 - SX0, 4,
                fade565(COL_TEAL, BG, glow - 0.045f * (float)i));
  }

  const int16_t bw = (int16_t)(slot - GAP);
  const float hmax = (float)(BASE - TOP);

  for (int i = 0; i < AUDIO_BANDS; i++) {
    float v = bands[i];
    if (v > 1.f) v = 1.f;
    if (v < 0.f) v = 0.f;

    int16_t x = (int16_t)(SX0 + slot * (float)i + GAP * 0.5f);
    float h = v * hmax;

    /* 柱体按 6px 一段上色，形成从底到顶的渐变 */
    int16_t nseg = (int16_t)(h / 6.f);
    for (int16_t s = 0; s < nseg; s++) {
      float f = (h > 12.f) ? ((float)s * 6.f / h) : 0.f;
      int16_t y = (int16_t)(BASE - (float)(s + 1) * 6.f);
      c->fillRect(x, y, bw, 6, specCol(f));
    }
    /* 还在长的那一小截，用顶端色收边 */
    float rem = h - (float)nseg * 6.f;
    if (rem > 0.5f) {
      c->fillRect(x, (int16_t)(BASE - h), bw, (int16_t)rem,
                  specCol(h / hmax));
    }

    /* 峰值帽：小白条，慢下落（pk 已在 mic 侧做了衰减） */
    float p = pk[i];
    if (p > 1.f) p = 1.f;
    if (p > 0.02f) {
      int16_t py = (int16_t)(BASE - p * hmax) - 5;
      if (py < TOP - 6) py = TOP - 6;
      c->fillRect(x, py, bw, 3, specCol(p * 0.85f + 0.1f));
    }

    /* 倒影：基线下方 22% 高度，暗一档 */
    int16_t mh = (int16_t)(h * 0.18f);
    for (int16_t s = 0; s < mh / 6 && s < 9; s++) {
      uint16_t col = fade565(specCol(0.15f), BG, 0.22f - 0.02f * (float)s);
      c->fillRect(x, (int16_t)(BASE + 3 + s * 6), bw, 4, col);
    }
  }

  (void)level;
  (void)t;
}
