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
  /* 电量：pct < 0 表示读不到（此时不画），chg = 正在充电 */
  void setBattery(int pct, bool chg) { _batPct = pct; _batChg = chg; }
  void draw(CanvasGFX *cv, const GSnapshot &g);
  /* 外部有浮层(校准进度等)盖过本页时调用：下一帧整屏重画，避免残留 */
  void requestFull() { _needFull = true; }

 private:
  void drawHeader(CanvasGFX *c, const GSnapshot &g);
  void drawBattery(CanvasGFX *c, int16_t x, int16_t y);
  void drawBigValue(CanvasGFX *c, const GSnapshot &g);
  void drawAxisBar(CanvasGFX *c, int16_t y, const char *label, float v, uint16_t col);
  void drawBars(CanvasGFX *c, const GSnapshot &g);
  void drawChart(CanvasGFX *c, const GSnapshot &g);

  float _bars[3] = {0, 0, 0};
  float _magS = 0.f;
  float _peakS = 0.f;
  int _batPct = -1;
  bool _batChg = false;
  int16_t _bigCx = 232;   /* 大号数字的水平中心，说明文字跟着它对齐 */
  bool _needFull = true;  /* 下一帧整屏重画（切页 / 报警状态变化时置位） */
  bool _alarmPrev = false;
  int16_t _bigX = 0, _bigY = 0, _bigW = 0, _bigH = 0;   /* 上一帧大数字的包围盒 */
  bool _bannerOn = false;
  uint32_t _chartMs = 0;                                /* 曲线重绘节流 */
  float _alarmHold = 0.f;   /* 报警保持：跨过 0.80G 后继续亮 0.45s，防止阈值抖动 */
};
