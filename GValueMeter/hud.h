#pragma once
/* ===========================================================================
 *  hud.h — 配色 + 绘图原语 + 七段数字 + 小字号文字
 *  Arduino_GFX 没有弧线/半透明/矢量字，这里补齐本项目需要的那一套，
 *  全部用「粗线段（胶囊）+ 逐行椭圆填充」实现，写入 RAM 画布足够快。
 * =========================================================================== */
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------ 配色 ------------------------------ */
#define COL_PRO_BG      RGB565(6, 9, 16)
#define COL_PRO_PANEL   RGB565(13, 19, 32)
#define COL_PRO_GRID    RGB565(28, 40, 66)
#define COL_TEXT        RGB565(234, 240, 250)
#define COL_DIM         RGB565(104, 120, 150)
#define COL_TEAL        RGB565(56, 226, 200)
#define COL_CYAN        RGB565(64, 196, 255)
#define COL_AMBER       RGB565(255, 178, 54)
#define COL_RED         RGB565(255, 58, 78)
#define COL_VIOLET      RGB565(178, 124, 255)
#define COL_GREEN       RGB565(92, 232, 128)
#define COL_ORANGE      RGB565(255, 92, 24)
#define COL_PINK        RGB565(255, 96, 128)

#define COL_PET_BG      RGB565(9, 12, 22)
#define COL_PET_DOT     RGB565(28, 38, 62)
#define COL_FACE_TOP    RGB565(255, 236, 204)
#define COL_FACE_BOT    RGB565(246, 160, 102)
#define COL_INK         RGB565(40, 36, 58)
#define COL_CHEEK       RGB565(247, 136, 146)
#define COL_TONGUE      RGB565(238, 112, 128)
#define COL_TEETH       RGB565(255, 251, 244)
#define COL_SHADOW      RGB565(16, 24, 40)

/* --------------------------- 颜色工具 --------------------------- */
inline uint16_t lerp565(uint16_t a, uint16_t b, float t) {
  if (t <= 0.f) return a;
  if (t >= 1.f) return b;
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + (int)((br - ar) * t + 0.5f);
  int g = ag + (int)((bg - ag) * t + 0.5f);
  int bl = ab + (int)((bb - ab) * t + 0.5f);
  if (r < 0) r = 0; if (r > 31) r = 31;
  if (g < 0) g = 0; if (g > 63) g = 63;
  if (bl < 0) bl = 0; if (bl > 31) bl = 31;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}
/* 用底色把颜色"淡出"，模拟透明度（AMOLED 上很好用） */
inline uint16_t fade565(uint16_t c, uint16_t bg, float keep) {
  return lerp565(bg, c, keep);
}

/* --------------------------- 几何原语 --------------------------- */

/* 粗线段，两端圆头（胶囊）。t = 粗细 */
inline void hudCapsule(Arduino_GFX *d, float x0, float y0, float x1, float y1,
                       float t, uint16_t c) {
  if (t < 1.f) t = 1.f;
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  int16_t half = (int16_t)(t * 0.5f + 0.5f);
  if (len < 0.6f) {
    d->fillCircle((int16_t)lroundf(x0), (int16_t)lroundf(y0), half, c);
    return;
  }
  /* 按主轴逐列/逐行填充：相邻列(行)紧贴，斜线上不会留下棋盘格一样的缝 */
  if (fabsf(dx) >= fabsf(dy)) {
    int n = (int)fabsf(dx);
    if (n < 1) n = 1;
    for (int i = 0; i <= n; i++) {
      float u = (float)i / (float)n;
      int16_t px = (int16_t)lroundf(x0 + dx * u);
      int16_t py = (int16_t)lroundf(y0 + dy * u);
      d->fillRect(px, py - half, 1, half * 2 + 1, c);
    }
  } else {
    int n = (int)fabsf(dy);
    if (n < 1) n = 1;
    for (int i = 0; i <= n; i++) {
      float u = (float)i / (float)n;
      int16_t px = (int16_t)lroundf(x0 + dx * u);
      int16_t py = (int16_t)lroundf(y0 + dy * u);
      d->fillRect(px - half, py, half * 2 + 1, 1, c);
    }
  }
  /* 圆头 */
  d->fillCircle((int16_t)lroundf(x0), (int16_t)lroundf(y0), half, c);
  d->fillCircle((int16_t)lroundf(x1), (int16_t)lroundf(y1), half, c);
}

/* 折线（pts 用 x0,y0,x1,y1,... 排列） */
inline void hudPolyline(Arduino_GFX *d, const int16_t *pts, int nPts,
                        float t, uint16_t c) {
  for (int i = 0; i + 1 < nPts; i++) {
    hudCapsule(d, (float)pts[i * 2], (float)pts[i * 2 + 1],
               (float)pts[(i + 1) * 2], (float)pts[(i + 1) * 2 + 1], t, c);
  }
}

/* 椭圆弧（角度：0 = +X，正向为屏幕向下 / 顺时针）
 * 用"同心层 + 逐点描线"实现：比折线胶囊更平滑，圆弧边缘没有锯齿豁口。 */
inline void hudArc(Arduino_GFX *d, float cx, float cy, float rx, float ry,
                   float a0, float a1, float t, uint16_t c) {
  float sweep = a1 - a0;
  float rad = (rx > ry ? rx : ry);
  int steps = (int)(fabsf(sweep) * rad);      /* 约 1px 一个点 */
  if (steps < 24) steps = 24;
  if (steps > 720) steps = 720;
  /* 每 0.5px 加一层：比整像素分层更密，弧带里不会漏点 */
  for (float k = 0.f; k < t; k += 0.5f) {
    float sx = rx - k, sy = ry - k;
    if (sx < 1.f || sy < 1.f) break;
    for (int i = 0; i <= steps; i++) {
      float a = a0 + sweep * (float)i / (float)steps;
      int16_t px = (int16_t)lroundf(cx + sx * cosf(a));
      int16_t py = (int16_t)lroundf(cy + sy * sinf(a));
      d->writePixel(px, py, c);
    }
  }
}

/* 椭圆环 */
inline void hudRing(Arduino_GFX *d, float cx, float cy, float rx, float ry,
                    float t, uint16_t c) {
  hudArc(d, cx, cy, rx, ry, 0.f, 6.283185f, t, c);
}

/* 实心椭圆（逐行填充，比逐点画快得多） */
inline void hudEllipseFill(Arduino_GFX *d, int16_t cx, int16_t cy,
                           int16_t rx, int16_t ry, uint16_t c) {
  if (rx <= 0 || ry <= 0) return;
  for (int16_t y = -ry; y <= ry; y++) {
    float k = 1.f - (float)(y * y) / (float)(ry * ry);
    if (k <= 0.f) continue;
    int16_t w = (int16_t)(rx * sqrtf(k));
    d->fillRect(cx - w, cy + y, w * 2 + 1, 1, c);
  }
}

/* 垂直渐变的实心椭圆：上亮下暗，像有光从顶上打下来 */
inline void hudEllipseGrad(Arduino_GFX *d, int16_t cx, int16_t cy,
                           int16_t rx, int16_t ry, uint16_t cTop, uint16_t cBot) {
  if (rx <= 0 || ry <= 0) return;
  for (int16_t y = -ry; y <= ry; y++) {
    float k = 1.f - (float)(y * y) / (float)(ry * ry);
    if (k <= 0.f) continue;
    int16_t w = (int16_t)(rx * sqrtf(k));
    float g = (float)(y + ry) / (float)(2 * ry);   // 0=上 1=下
    d->fillRect(cx - w, cy + y, w * 2 + 1, 1, lerp565(cTop, cBot, g));
  }
}

/* 柔和的高光（多层缩小椭圆，避免颜色跳变太硬） */
inline void hudGloss(Arduino_GFX *d, int16_t cx, int16_t cy, int16_t rx, int16_t ry,
                     uint16_t base, float strength) {
  const int layers = 5;
  for (int i = 0; i < layers; i++) {
    float k = 1.f - (float)i / layers;          // 外->内
    uint16_t c = lerp565(base, RGB565(255, 255, 255), strength * (1.f - k) * 0.9f + 0.02f);
    hudEllipseGrad(d, cx + rx / 6, cy - ry / 5,
                   (int16_t)(rx * k), (int16_t)(ry * k), c, c);
  }
}

/* 圆角面板 */
/* 圆角边框：用弧+胶囊自己画，比库里的 drawRoundRect 更连续（库版在四角是单像素跳点） */
inline void hudRoundBorder(Arduino_GFX *d, int16_t x, int16_t y, int16_t w, int16_t h,
                           int16_t r, uint16_t c) {
  if (r < 2) { d->drawRect(x, y, w, h, c); return; }
  float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w - 1), y1 = (float)(y + h - 1);
  float rr = (float)r;
  hudCapsule(d, x0 + rr, y0, x1 - rr, y0, 2.f, c);
  hudCapsule(d, x0 + rr, y1, x1 - rr, y1, 2.f, c);
  hudCapsule(d, x0, y0 + rr, x0, y1 - rr, 2.f, c);
  hudCapsule(d, x1, y0 + rr, x1, y1 - rr, 2.f, c);
  hudArc(d, x0 + rr, y0 + rr, rr, rr, 3.14159f, 4.712389f, 2.f, c);   /* 左上 */
  hudArc(d, x1 - rr, y0 + rr, rr, rr, 4.712389f, 6.28318f, 2.f, c);   /* 右上 */
  hudArc(d, x1 - rr, y1 - rr, rr, rr, 0.f, 1.5707963f, 2.f, c);       /* 右下 */
  hudArc(d, x0 + rr, y1 - rr, rr, rr, 1.5707963f, 3.14159f, 2.f, c);  /* 左下 */
}

inline void hudPanel(Arduino_GFX *d, int16_t x, int16_t y, int16_t w, int16_t h,
                     int16_t r, uint16_t fill, uint16_t border) {
  /* 逐行算圆角内缩，几何上精确：不会出现缺角/毛刺/虚线圈 */
  if (r < 2) r = 2;
  if (r > h / 2) r = h / 2;
  if (r > w / 2) r = w / 2;
  for (int16_t yy = 0; yy < h; yy++) {
    float dy = 0.f;
    if (yy < r) dy = (float)r - (float)yy - 0.5f;
    else if (yy >= h - r) dy = (float)(yy - (h - r)) + 0.5f;
    float rr = (float)r;
    float inset = rr - sqrtf(fmaxf(0.f, rr * rr - dy * dy));
    int16_t ix = (int16_t)(inset + 0.5f);
    int16_t x0 = (int16_t)(x + ix);
    int16_t ww = (int16_t)(w - ix * 2);
    if (ww < 2) ww = 2;
    if (border != fill && (yy < 2 || yy >= h - 2)) {
      d->fillRect(x0, (int16_t)(y + yy), ww, 1, border);
      continue;
    }
    d->fillRect(x0, (int16_t)(y + yy), ww, 1, fill);
    if (border != fill) {
      d->fillRect(x0, (int16_t)(y + yy), 2, 1, border);
      d->fillRect((int16_t)(x0 + ww - 2), (int16_t)(y + yy), 2, 1, border);
    }
  }
}

/* --------------------------- 文字（内置 5x7，整数倍放大） --------------------------- */
inline int16_t hudTextW(uint8_t size, const char *s) {
  return (int16_t)strlen(s) * 6 * size;
}
inline void hudText(Arduino_GFX *d, int16_t x, int16_t y, uint8_t size,
                    uint16_t c, const char *s) {
  d->setFont(NULL);
  d->setTextSize(size);
  d->setTextColor(c, c);      // bg == fg => 透明
  d->setCursor(x, y);
  d->print(s);
}
inline void hudTextC(Arduino_GFX *d, int16_t cx, int16_t y, uint8_t size,
                     uint16_t c, const char *s) {
  hudText(d, cx - hudTextW(size, s) / 2, y, size, c, s);
}
inline void hudTextR(Arduino_GFX *d, int16_t rx, int16_t y, uint8_t size,
                     uint16_t c, const char *s) {
  hudText(d, rx - hudTextW(size, s), y, size, c, s);
}

/* --------------------------- 中文（点阵，16/32 两档） --------------------------- */
#include "cjk_font.h"

/* px = 16 或 32：汉字格子边长；ASCII 用内置 5x7 放大到同高 */
/** 字号档位：16 / 24 / 32，三档点阵都在字库里；ASCII 用 5×7 放大对齐行高 */
inline uint8_t cjkSize(uint8_t px) { return (px >= 32) ? 32 : ((px >= 22) ? 24 : 16); }
inline uint8_t cjkAsciiScale(uint8_t px) { return (px >= 22) ? 3 : 2; }

inline const CjkGlyph *cjkLookup(uint16_t cp) {
  int lo = 0, hi = CJK_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (CJK_TABLE[mid].cp == cp) return &CJK_TABLE[mid];
    if (CJK_TABLE[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

/* UTF-8 下一个码点 */
inline uint16_t utf8Next(const char **pp) {
  const char *p = *pp;
  uint16_t cp;
  if ((p[0] & 0x80) == 0) { cp = (uint16_t)p[0]; p += 1; }
  else if ((p[0] & 0xE0) == 0xC0) { cp = (uint16_t)(((p[0] & 0x1F) << 6) | (p[1] & 0x3F)); p += 2; }
  else if ((p[0] & 0xF0) == 0xE0) {
    cp = (uint16_t)(((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F)); p += 3;
  } else { cp = '?'; p += 1; }
  *pp = p;
  return cp;
}

/* 单个汉字：同行的连续点亮合并成一条 fillRect，减少调用次数 */
inline void hudCJK(Arduino_GFX *d, int16_t x, int16_t y, uint8_t px, uint16_t c, uint16_t cp) {
  const CjkGlyph *g = cjkLookup(cp);
  if (!g) return;
  int gs = cjkSize(px);                     /* 字形档位，决定行数与行距 */
  const uint8_t *bits = (gs == 32) ? g->p32 : (gs == 24 ? g->p24 : g->p16);
  int bpr = gs >> 3;
  for (int row = 0; row < gs; row++) {
    const uint8_t *line = bits + row * bpr;
    int start = -1;
    for (int col = 0; col <= gs; col++) {
      bool on = false;
      if (col < gs) on = ((line[col >> 3] >> (7 - (col & 7))) & 1) != 0;
      if (on && start < 0) start = col;
      else if (!on && start >= 0) {
        d->fillRect(x + start, y + row, col - start, 1, c);
        start = -1;
      }
    }
  }
}

/* 中文标点里的窄字符，只占半格 */
inline bool cjkNarrow(uint16_t cp) {
  switch (cp) {
    case 0x00B7: case 0x2026: case 0xFF0C: case 0xFF1A: case 0xFF1B:
    case 0x3001: case 0xFF65: return true;
    default: return false;
  }
}
inline int16_t cjkAdv(uint16_t cp, uint8_t px) {
  if (cp < 0x80) return (int16_t)(6 * cjkAsciiScale(px));
  return cjkNarrow(cp) ? (int16_t)((px + 1) / 2) : (int16_t)(px + 1);
}

/* 中文 + ASCII 混排宽度：汉字 px+1，ASCII 6*cjkAsciiScale */
inline int16_t hudCNW(uint8_t px, const char *s) {
  int16_t w = 0;
  const char *p = s;
  while (*p) {
    uint16_t cp = utf8Next(&p);
    w += cjkAdv(cp, px);
  }
  return w;
}

inline void hudCN(Arduino_GFX *d, int16_t x, int16_t y, uint8_t px,
                  uint16_t c, const char *s) {
  const char *p = s;
  int16_t cx = x;
  uint8_t as = cjkAsciiScale(px);
  for (; *p; ) {
    uint16_t cp = utf8Next(&p);
    if (cp < 0x80) {
      if (cp != ' ') {
        char one[2] = {(char)cp, 0};
        hudText(d, cx, (int16_t)(y + (px - 7 * as) / 2), as, c, one);
      }
      cx += (int16_t)(6 * as);
    } else if (cjkNarrow(cp)) {
      hudCJK(d, (int16_t)(cx - px / 4 - 1), y, px, c, cp);
      cx += (int16_t)((px + 1) / 2);
    } else {
      hudCJK(d, cx, y, px, c, cp);
      cx += (int16_t)(px + 1);
    }
  }
}

inline void hudCNC(Arduino_GFX *d, int16_t cx, int16_t y, uint8_t px,
                   uint16_t c, const char *s) {
  hudCN(d, cx - hudCNW(px, s) / 2, y, px, c, s);
}

inline void hudCNR(Arduino_GFX *d, int16_t rx, int16_t y, uint8_t px,
                   uint16_t c, const char *s) {
  hudCN(d, rx - hudCNW(px, s), y, px, c, s);
}

/* --------------------------- 七段数字（大字号 G 值） --------------------------- */
static const uint8_t SS_MAP[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                   0x6D, 0x7D, 0x07, 0x7F, 0x6F};
/* 位序：0=a(上) 1=b(右上) 2=c(右下) 3=d(下) 4=e(左下) 5=f(左上) 6=g(中) */
inline void ssSeg(Arduino_GFX *d, int16_t x, int16_t y, int16_t w, int16_t h,
                  int16_t t, uint8_t idx, uint16_t c) {
  float cy = h * 0.5f;
  float inset = t * 0.62f;
  switch (idx) {
    case 0: hudCapsule(d, x + inset, y, x + w - inset, y, t, c); break;
    case 1: hudCapsule(d, x + w, y + inset, x + w, y + cy - inset * 0.5f, t, c); break;
    case 2: hudCapsule(d, x + w, y + cy + inset * 0.5f, x + w, y + h - inset, t, c); break;
    case 3: hudCapsule(d, x + inset, y + h, x + w - inset, y + h, t, c); break;
    case 4: hudCapsule(d, x, y + cy + inset * 0.5f, x, y + h - inset, t, c); break;
    case 5: hudCapsule(d, x, y + inset, x, y + cy - inset * 0.5f, t, c); break;
    case 6: hudCapsule(d, x + inset, y + cy, x + w - inset, y + cy, t, c); break;
    default: break;
  }
}

inline void ssDigit(Arduino_GFX *d, int16_t x, int16_t y, int16_t w, int16_t h,
                    int16_t t, uint8_t dig, uint16_t on, uint16_t off) {
  uint8_t mask = SS_MAP[dig % 10];
  /* "1"：b+c 两段共线，直接画一根通条，避免中间出现接缝 */
  if ((dig % 10) == 1) {
    float inset = t * 0.62f;
    hudCapsule(d, x + w, y + inset, x + w, y + h - inset, t, on);
    return;
  }
  for (uint8_t i = 0; i < 7; i++) {
    if ((mask >> i) & 1) ssSeg(d, x, y, w, h, t, i, on);
    else if (off != 0x0000) ssSeg(d, x, y, w, h, t, i, off);
  }
}

/* 单个七段字符的宽度 */
inline int16_t ssCharW(char ch, int16_t h, int16_t t) {
  if (ch == '.') return (int16_t)(t * 1.7f);
  if (ch == '-') return (int16_t)(h * 0.32f);
  return (int16_t)(h * 0.55f);
}

inline int16_t ssMeasure(int16_t h, const char *s) {
  int16_t t = (int16_t)(h / 7); if (t < 3) t = 3;
  int16_t gap = (int16_t)(t + 2);   /* 数字间距：太窄时 1 会和后面的 5 粘成 6 */
  int16_t wsum = 0;
  for (int i = 0; s[i]; i++) {
    if (s[i] == '1') { wsum += (int16_t)(t * 1.35f) + gap; continue; }  /* "1" 只占一根条的宽 */
    wsum += ssCharW(s[i], h, t) + gap;
  }
  return wsum - gap;
}

/* 绘制七段字符串。返回字符串宽度。on=点亮色，off=灭段色(0=不画) */
inline int16_t ssDraw(Arduino_GFX *d, int16_t x, int16_t y, int16_t h,
                      const char *s, uint16_t on, uint16_t off) {
  int16_t t = (int16_t)(h / 7); if (t < 3) t = 3;
  int16_t gap = (int16_t)(t + 2);   /* 数字间距：太窄时 1 会和后面的 5 粘成 6 */
  int16_t cur = x;
  for (int i = 0; s[i]; i++) {
    char ch = s[i];
    if (ch >= '0' && ch <= '9') {
      int16_t w = (int16_t)(h * 0.55f);
      if (ch == '1') w = (int16_t)(t * 1.35f);   /* "1" 只占一根条的宽，别留大空档 */
      ssDigit(d, cur, y, w, h, t, (uint8_t)(ch - '0'), on, off);
      cur += w + gap;
    } else if (ch == '.') {
      int16_t r = (int16_t)(t * 0.62f);
      d->fillCircle(cur + r, y + h - r, r, on);
      cur += r * 2 + gap;
    } else if (ch == '-') {
      int16_t w = (int16_t)(h * 0.32f);
      hudCapsule(d, cur, y + h * 0.5f, cur + w, y + h * 0.5f, t, on);
      cur += w + gap;
    } else {
      cur += gap;
    }
  }
  return cur - x - gap;
}

/* 把浮点数画成七段（dec=小数位），返回总宽度 */
inline int16_t ssNumber(Arduino_GFX *d, int16_t x, int16_t y, int16_t h,
                        float v, uint8_t dec, uint16_t on, uint16_t off) {
  char buf[16];
  float av = fabsf(v);
  if (v < 0 && av >= 0.005f) snprintf(buf, sizeof(buf), "-%.*f", dec, av);
  else snprintf(buf, sizeof(buf), "%.*f", dec, av);
  /* 防止四舍五入溢出显示宽度（如 9.999 -> 10.00） */
  if (ssMeasure(h, buf) > (int16_t)(h * 3.3f)) {
    snprintf(buf, sizeof(buf), "%.1f", av);
    if (v < 0) snprintf(buf, sizeof(buf), "-%.1f", av);
  }
  return ssDraw(d, x, y, h, buf, on, off);
}

/* --------------------------- 装饰图形 --------------------------- */
/* 四角星（十字 + 斜十字），rot 为旋转角 */
inline void hudStar(Arduino_GFX *d, float cx, float cy, float r, float rot,
                    float t, uint16_t c) {
  for (int k = 0; k < 4; k++) {
    float a = rot + k * 1.5707963f;
    hudCapsule(d, cx, cy, cx + r * cosf(a), cy + r * sinf(a), t, c);
  }
  for (int k = 0; k < 4; k++) {
    float a = rot + 0.7853982f + k * 1.5707963f;
    hudCapsule(d, cx, cy, cx + r * 0.42f * cosf(a), cy + r * 0.42f * sinf(a), t * 0.8f, c);
  }
  d->fillCircle((int16_t)cx, (int16_t)cy, (int16_t)(t * 0.9f), c);
}

/* 四角星（✦）：两条菱形轴并集，线条干净不炸毛 */
inline void hudSparkle(Arduino_GFX *d, float cx, float cy, float r, float w, uint16_t c) {
  int r0 = (int)r, w0 = (int)w;
  for (int dx = -r0; dx <= r0; dx++) {
    int hh = (int)(w0 * (1.f - (float)(dx < 0 ? -dx : dx) / (float)r0));
    if (hh < 0) continue;
    d->fillRect((int16_t)(cx + dx), (int16_t)(cy - hh), 1, (int16_t)(hh * 2 + 1), c);
  }
  for (int dy = -r0; dy <= r0; dy++) {
    int hh = (int)(w0 * (1.f - (float)(dy < 0 ? -dy : dy) / (float)r0));
    if (hh < 0) continue;
    d->fillRect((int16_t)(cx - hh), (int16_t)(cy + dy), (int16_t)(hh * 2 + 1), 1, c);
  }
}

/* 八分音符 */
inline void hudNote(Arduino_GFX *d, float cx, float cy, float s, uint16_t c) {
  d->fillCircle((int16_t)cx, (int16_t)cy, (int16_t)(s * 0.42f), c);          // 符头
  hudCapsule(d, cx + s * 0.38f, cy - s * 0.05f, cx + s * 0.38f, cy - s * 1.05f, s * 0.16f, c); // 符干
  hudCapsule(d, cx + s * 0.38f, cy - s * 1.05f, cx + s * 0.95f, cy - s * 0.72f, s * 0.2f, c);  // 符尾
}

/* 感叹号 */
inline void hudBang(Arduino_GFX *d, float x, float yTop, float len, float t, uint16_t c) {
  hudCapsule(d, x, yTop, x, yTop + len, t, c);
  d->fillCircle((int16_t)x, (int16_t)(yTop + len + t * 1.6f), (int16_t)(t * 0.75f), c);
}
