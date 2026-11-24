#pragma once
/* 开机屏 / 提示条 / 校准浮层 —— 放这里是为了让桌面模拟器也能渲染它们 */
#include <stdio.h>

#include "canvas_gfx.h"
#include "hud.h"

/* 顶部渐变亮线（各屏通用） */
inline void uiTopLine(CanvasGFX *c) {
  for (int16_t x = 0; x < 480; x += 3) {
    c->fillRect(x, 0, 3, 2, lerp565(COL_TEAL, COL_VIOLET, (float)x / 480.f));
  }
}

/* 启动屏上的小脸：和宠物模式同一套画法（黑底上的白色线稿脸） */
inline void uiMiniFace(CanvasGFX *c, int16_t cx, int16_t cy, int16_t R) {
  const uint16_t W = RGB565(255, 255, 255);
  const uint16_t K = RGB565(0, 0, 0);
  int16_t eyeDX = (int16_t)(R * 0.44f);
  int16_t eyeRX = (int16_t)(R * 0.27f);
  int16_t eyeRY = (int16_t)(R * 0.33f);
  int16_t ey = (int16_t)(cy - R * 0.03f);
  for (int i = 0; i < 2; i++) {
    int16_t ex = (int16_t)(cx + (i ? eyeDX : -eyeDX));
    hudEllipseFill(c, ex, ey, eyeRX, eyeRY, W);
    hudEllipseFill(c, ex, ey, (int16_t)(eyeRX * 0.52f), (int16_t)(eyeRY * 0.52f), K);
    c->fillCircle((int16_t)(ex - eyeRX * 0.16f), (int16_t)(ey - eyeRY * 0.26f),
                  (int16_t)(eyeRX * 0.15f), W);
    c->fillCircle((int16_t)(ex + eyeRX * 0.18f), (int16_t)(ey + eyeRY * 0.22f),
                  (int16_t)(eyeRX * 0.07f), W);
  }
  hudEllipseFill(c, cx, (int16_t)(cy + R * 0.56f),
                 (int16_t)(R * 0.115f), (int16_t)(R * 0.050f), W);
}

/* 开机 / 校准进度屏 */
inline void uiBoot(CanvasGFX *c, const char *line, float prog) {
  c->fillRect(0, 0, 480, 480, RGB565(0, 0, 0));
  uiTopLine(c);

  /* 标题上方的呼吸光圈，呼应宠物模式的脸 */
  hudEllipseGrad(c, 240, 178, 190, 136,
                 lerp565(RGB565(0, 0, 0), COL_TEAL, 0.045f), RGB565(0, 0, 0));

  hudCNC(c, 240, 96, 32, COL_TEXT, "G值表");
  hudCNC(c, 240, 140, 16, COL_DIM, "车载加速度 · 宠物 / 专业双模式");
  uiMiniFace(c, 240, 244, 72);

  int16_t bx = 90, by = 344, bw = 300;
  c->drawRect(bx, by, bw, 12, COL_PRO_GRID);
  if (prog > 0.f) {
    float k = prog > 1.f ? 1.f : prog;
    c->fillRect(bx + 2, by + 2, (int16_t)((bw - 4) * k), 8, COL_TEAL);
  }
  if (line && line[0]) hudCNC(c, 240, 372, 16, COL_DIM, line);
}

/* 底部提示条 */
inline void uiNotice(CanvasGFX *c, const char *msg) {
  if (!msg || !msg[0]) return;
  int16_t w = hudCNW(16, msg) + 48;
  if (w < 200) w = 200;
  if (w > 420) w = 420;
  hudPanel(c, (int16_t)(240 - w / 2), 420, w, 42, 14, RGB565(17, 21, 34), COL_TEAL);
  hudCNC(c, 240, 433, 16, COL_TEAL, msg);
}

/* 校准中的进度浮层 */
inline void uiCalOverlay(CanvasGFX *c, float progress) {
  hudPanel(c, 90, 404, 300, 56, 16, RGB565(17, 21, 34), COL_AMBER);
  char b[40];
  snprintf(b, sizeof(b), "校准中 %d%%", (int)(progress * 100.f));
  hudCNC(c, 240, 412, 16, COL_AMBER, b);
  c->fillRect(104, 446, 272, 6, COL_PRO_GRID);
  c->fillRect(104, 446, (int16_t)(272.f * progress), 6, COL_AMBER);
}

/* 待机提示（专业页 / 圆盘页中央）：未开测时不显示 G 值，只提示如何开测 */
inline void uiStandbyHint(CanvasGFX *c, float t) {
  float k = 0.5f + 0.5f * sinf(t * 2.4f);
  hudPanel(c, 84, 198, 312, 68, 18, RGB565(17, 21, 34),
           lerp565(COL_PRO_GRID, COL_GREEN, 0.30f + 0.45f * k));
  hudCNC(c, 240, 208, 24, lerp565(COL_TEXT, COL_GREEN, 0.35f + 0.4f * k), "待机中");
  hudCNC(c, 240, 240, 16, COL_DIM, "长按 BOOT 开始测量");
}
