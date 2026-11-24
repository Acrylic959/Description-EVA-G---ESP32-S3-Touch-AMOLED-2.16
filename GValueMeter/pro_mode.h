#pragma once
/* ===========================================================================
 *  pro_mode.h — 专业 G 值模式
 *  顶部：实时 |G| + 历史最大 |G|
 *  中部：大号七段 |G|、三轴分量条（±1g，中心零位）
 *  底部：最近 120 点（约 12s）G 值曲线 + 0.8G 报警参考线
 *  G > 0.8 时：红色闪烁描边 + “高G报警”横幅
 * =========================================================================== */
#include "canvas_gfx.h"
#include "gmeter.h"

class ProMode {
 public:
  void reset();
  void update(const GSnapshot &g, float dt);
  void draw(CanvasGFX *cv, const GSnapshot &g);

 private:
  void drawHeader(CanvasGFX *c, const GSnapshot &g);
  void drawBigValue(CanvasGFX *c, const GSnapshot &g);
  void drawAxisBar(CanvasGFX *c, int16_t y, const char *label, float v, uint16_t col);
  void drawBars(CanvasGFX *c, const GSnapshot &g);
  void drawChart(CanvasGFX *c, const GSnapshot &g);
  void drawAlarm(CanvasGFX *c, bool on);

  float _bars[3] = {0, 0, 0};
  float _magS = 0.f;
  float _peakS = 0.f;
  int16_t _bigCx = 232;   /* 大号数字的水平中心，说明文字跟着它对齐 */
};
