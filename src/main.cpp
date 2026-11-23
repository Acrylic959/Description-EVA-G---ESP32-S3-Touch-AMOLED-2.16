// ============================================================
// EVA 车载 G 值表 固件  —  微雪 ESP32-S3-Touch-AMOLED-2.16
// ------------------------------------------------------------
// 功能 (复刻小红书「DIY车载小废物 / G值表」项目):
//   [宠物模式]  待机/加减速/减速带/转弯/听歌 表情 + 顶部当前/最大G
//   [专业模式]  当前G / 历史最大G / 三轴分量 / G曲线 / >0.8G红框报警
//   滑动切换模式, 单击重置最大G
// ============================================================

#include <Arduino.h>
#include "config.h"
#include "pin_config.h"
#include "display.h"
#include "imu.h"
#include "touch.h"
#include "audio.h"
#include <math.h>

// ---------------- 全局状态 ----------------
static AppMode  s_mode      = MODE_PET;    // 当前页面
static PetMood  s_mood      = MOOD_IDLE;   // 当前表情
static unsigned long s_moodUntil = 0;      // 表情保持到(ms)
static float    s_gHist[G_HISTORY_LEN] = {0}; // G值曲线历史
static int      s_histIdx   = 0;
static unsigned long s_lastHistMs = 0;
static GData    s_g;
static bool     s_alarm     = false;
static unsigned long s_lastFrame = 0;

// 宠物动画相位
static unsigned long s_animT = 0;

// ---------------- 手势回调 ----------------
void onSwipe(int dir) {
  // 左滑 -> 专业模式, 右滑 -> 宠物模式
  s_mode = (dir > 0) ? MODE_PRO : MODE_PET;
}

void onTap() {
  imuResetMax();
}

// ============================================================
// 绘制辅助
// ============================================================
static void drawCenterText(int cx, int cy, const char *txt, uint8_t size, uint16_t color) {
  int16_t x, y;
  uint16_t w, h;
  gCanvas->setTextSize(size);
  gCanvas->getTextBounds(txt, 0, 0, &x, &y, &w, &h);
  gCanvas->setCursor(cx - w / 2 - x, cy - h / 2 - y);
  gCanvas->setTextColor(color);
  gCanvas->print(txt);
}

// 报警红框
static void drawAlarmFrame() {
  if (!s_alarm) return;
  // 闪烁: 亮暗交替
  bool on = (millis() / 200) % 2 == 0;
  uint16_t c = on ? C_RED : C_RED_D;
  for (int i = 0; i < 8; i++) {
    gCanvas->drawRect(0 + i, 0 + i, LCD_WIDTH - 2 * i, LCD_HEIGHT - 2 * i, c);
  }
  gCanvas->fillRect(0, 0, LCD_WIDTH, 40, c);
  gCanvas->fillRect(0, LCD_HEIGHT - 40, LCD_WIDTH, 40, c);
  gCanvas->setTextSize(4);
  gCanvas->setTextColor(C_WHITE);
  gCanvas->setCursor(60, 8);
  gCanvas->print("! HIGH G !");
  gCanvas->setCursor(60, LCD_HEIGHT - 36);
  gCanvas->print("! HIGH G !");
}

// ============================================================
// 宠物表情绘制 (矢量绘制, 程序化动画)
// ============================================================

// 待机呼吸的圆形宠物
static void drawPetIdle(int cx, int cy, float t) {
  int r = 96 + (int)(3 * sinf(t));           // 呼吸缩放
  uint16_t body = C_PURPLE_L;

  // 光晕
  for (int i = 3; i > 0; i--) {
    gCanvas->drawCircle(cx, cy, r + i * 6, C_GREEN_D);
  }
  gCanvas->fillCircle(cx, cy, r, body);
  gCanvas->drawCircle(cx, cy, r, C_GREEN);

  // 眼睛 (圆)
  int ey = cy - 18;
  gCanvas->fillCircle(cx - 34, ey, 10, C_BLACK);
  gCanvas->fillCircle(cx + 34, ey, 10, C_BLACK);
  // 眼睛高光
  gCanvas->fillCircle(cx - 31, ey - 3, 3, C_WHITE);
  gCanvas->fillCircle(cx + 37, ey - 3, 3, C_WHITE);

  // 嘴巴: 微笑弧
  for (int a = 20; a <= 160; a += 8) {
    float rad = a * PI / 180;
    int mx = cx + (int)(28 * cosf(rad));
    int my = cy + 18 + (int)(18 * sinf(rad));
    gCanvas->fillCircle(mx, my, 3, C_BLACK);
  }
  // 腮红
  gCanvas->fillCircle(cx - 58, cy + 8, 9, 0xF81F);
  gCanvas->fillCircle(cx + 58, cy + 8, 9, 0xF81F);
}

// 加速: 兴奋表情, ^眼 + 张嘴 + 速度线
static void drawPetAccel(int cx, int cy, float t) {
  gCanvas->fillCircle(cx, cy, 96, C_GREEN_D);
  gCanvas->drawCircle(cx, cy, 96, C_GREEN);
  // ^ ^ 眼
  for (int s = -1; s <= 1; s += 2) {
    int ex = cx + s * 34;
    gCanvas->drawLine(ex - 12, cy - 12, ex, cy - 26, C_BLACK);
    gCanvas->drawLine(ex, cy - 26, ex + 12, cy - 12, C_BLACK);
  }
  // 张大的嘴 (椭圆)
  gCanvas->fillEllipse(cx, cy + 24, 22, 28, C_BLACK);
  gCanvas->fillEllipse(cx, cy + 34, 14, 12, 0xF800);
  // 速度线
  for (int i = 0; i < 3; i++) {
    int off = (millis() / 40 + i * 30) % 90;
    gCanvas->drawFastHLine(cx - 130, cy - 50 + i * 50, 20 + off / 3, C_CYAN);
  }
}

// 刹车: 震惊表情, >< 眼 + o 嘴 + 后仰
static void drawPetBrake(int cx, int cy, float t) {
  gCanvas->fillCircle(cx, cy, 96, C_RED_D);
  gCanvas->drawCircle(cx, cy, 96, C_RED);
  // > < 眼
  gCanvas->drawLine(cx - 46, cy - 30, cx - 22, cy - 18, C_BLACK);
  gCanvas->drawLine(cx - 22, cy - 18, cx - 46, cy - 6, C_BLACK);
  gCanvas->drawLine(cx + 46, cy - 30, cx + 22, cy - 18, C_BLACK);
  gCanvas->drawLine(cx + 22, cy - 18, cx + 46, cy - 6, C_BLACK);
  // o 嘴
  gCanvas->fillCircle(cx, cy + 26, 14, C_BLACK);
  // 感叹号
  gCanvas->setTextSize(5);
  gCanvas->setTextColor(C_YELLOW);
  gCanvas->setCursor(cx - 8, cy - 130);
  gCanvas->print("!");
}

// 减速带/颠簸: 晕眩表情, X眼 + 抖动 + 星星
static void drawPetBump(int cx, int cy, float t) {
  int shake = (int)(6 * sinf(t * 6));
  cx += shake;
  gCanvas->fillCircle(cx, cy, 96, C_ORANGE);
  gCanvas->drawCircle(cx, cy, 96, C_YELLOW);
  // X X 眼
  for (int s = -1; s <= 1; s += 2) {
    int ex = cx + s * 34;
    gCanvas->drawLine(ex - 12, cy - 30, ex + 12, cy - 6, C_BLACK);
    gCanvas->drawLine(ex + 12, cy - 30, ex - 12, cy - 6, C_BLACK);
  }
  // 波浪嘴
  for (int x = -24; x < 24; x += 4) {
    int my = cy + 28 + (int)(5 * sinf(x * 0.3 + t * 4));
    gCanvas->fillCircle(cx + x, my, 3, C_BLACK);
  }
  // 旋转的星星
  for (int i = 0; i < 3; i++) {
    float a = t * 2 + i * 2.09;
    int sx = cx + (int)(120 * cosf(a));
    int sy = cy - 90 + (int)(18 * sinf(a));
    drawCenterText(sx, sy, "*", 2, C_YELLOW);
  }
}

// 转弯: 眼睛看向转弯方向 + 身体倾斜
static void drawPetTurn(int cx, int cy, float t, bool left) {
  int dir = left ? -1 : 1;
  gCanvas->fillCircle(cx, cy, 96, C_VIOLET);
  gCanvas->drawCircle(cx, cy, 96, C_PURPLE_L);
  // 眼睛偏向一侧
  int shift = dir * 10;
  gCanvas->fillCircle(cx - 34 + shift, cy - 18, 10, C_BLACK);
  gCanvas->fillCircle(cx + 34 + shift, cy - 18, 10, C_BLACK);
  // 嘴: 偏向一侧的笑
  gCanvas->drawLine(cx - 20, cy + 22, cx + 20 * dir, cy + 30, C_BLACK);
  // 转向箭头
  drawCenterText(cx + dir * 120, cy, left ? "<" : ">", 4, C_CYAN);
  // 侧倾的轮胎痕/动感线
  for (int i = 0; i < 3; i++) {
    int ox = cx - dir * (110 + i * 12);
    gCanvas->drawFastVLine(ox, cy - 30, 60, C_GRAY);
  }
}

// 听歌哼歌: 随音量张合的嘴 + 音符 + 闭眼
static void drawPetSing(int cx, int cy, float t, float level, bool beat) {
  gCanvas->fillCircle(cx, cy, 96, C_MINT);
  gCanvas->drawCircle(cx, cy, 96, C_WHITE);
  // 闭眼 (满足的弧)
  for (int s = -1; s <= 1; s += 2) {
    int ex = cx + s * 34;
    for (int a = 200; a <= 340; a += 10) {
      float rad = a * PI / 180;
      gCanvas->fillCircle(ex + (int)(14 * cosf(rad)), cy - 16 + (int)(10 * sinf(rad)), 2, C_BLACK);
    }
  }
  // 嘴: 音量驱动开合
  int mouthOpen = 6 + (int)(level * 30) + (beat ? 8 : 0);
  gCanvas->fillEllipse(cx, cy + 26, 18, mouthOpen, C_BLACK);
  gCanvas->fillEllipse(cx, cy + 26 + mouthOpen / 2, 10, mouthOpen / 2, 0xF800);
  // 跳动音符
  int nOff = (millis() / 25) % 60;
  drawCenterText(cx + 110, cy - 40 - nOff, "~", 4, C_PURPLE_L);
  drawCenterText(cx + 130, cy - 10 - nOff / 2, ".", 6, C_PURPLE_L);
}

// ============================================================
// 页面 1: 宠物模式
// ============================================================
static void drawPetMode() {
  float t = millis() / 1000.0f;

  // 背景装饰网格
  for (int x = 0; x < LCD_WIDTH; x += 40)
    gCanvas->drawFastVLine(x, 0, LCD_HEIGHT, 0x0810);
  for (int y = 0; y < LCD_HEIGHT; y += 40)
    gCanvas->drawFastHLine(0, y, LCD_WIDTH, 0x0810);

  // 表情
  int cx = LCD_WIDTH / 2, cy = LCD_HEIGHT / 2 + 10;
  switch (s_mood) {
    case MOOD_ACCEL: drawPetAccel(cx, cy, t); break;
    case MOOD_BRAKE: drawPetBrake(cx, cy, t); break;
    case MOOD_BUMP:  drawPetBump(cx, cy, t);  break;
    case MOOD_TURN_L: drawPetTurn(cx, cy, t, true); break;
    case MOOD_TURN_R: drawPetTurn(cx, cy, t, false); break;
    case MOOD_SING:  drawPetSing(cx, cy, t, audioLevel(), audioBeat()); break;
    default:         drawPetIdle(cx, cy, t); break;
  }

  // 顶部状态栏: 当前 / 最大 G
  gCanvas->fillRect(0, 0, LCD_WIDTH, 72, C_GREEN_D);
  gCanvas->setTextSize(2);
  gCanvas->setTextColor(C_WHITE);
  gCanvas->setCursor(16, 10);
  gCanvas->print("G");
  gCanvas->setCursor(16, 40);
  gCanvas->print("MAX");

  gCanvas->setTextSize(4);
  gCanvas->setTextColor(s_alarm ? C_RED : C_GREEN);
  gCanvas->setCursor(70, 6);
  gCanvas->printf("%.2f", s_g.gTotal);

  gCanvas->setTextColor(C_YELLOW);
  gCanvas->setCursor(70, 38);
  gCanvas->printf("%.2f", s_g.gMax);

  // 底部模式提示
  gCanvas->setTextSize(2);
  gCanvas->setTextColor(C_GRAY_LIGHT);
  gCanvas->setCursor(16, LCD_HEIGHT - 28);
  gCanvas->print("PET MODE");
}

// ============================================================
// 页面 2: 专业 G 值模式
// ============================================================
static void drawProMode() {
  // 大号当前 G
  gCanvas->setTextSize(2);
  gCanvas->setTextColor(C_GRAY_LIGHT);
  gCanvas->setCursor(20, 16);
  gCanvas->print("CURRENT G");

  gCanvas->setTextSize(10);
  gCanvas->setTextColor(s_alarm ? C_RED : (s_g.gTotal > 0.3f ? C_ORANGE : C_GREEN));
  gCanvas->setCursor(20, 40);
  gCanvas->printf("%.2f", s_g.gTotal);

  // 历史最大 G
  gCanvas->setTextSize(2);
  gCanvas->setTextColor(C_YELLOW);
  gCanvas->setCursor(300, 24);
  gCanvas->print("MAX");
  gCanvas->setTextSize(5);
  gCanvas->setCursor(300, 46);
  gCanvas->printf("%.2f", s_g.gMax);

  gCanvas->drawFastHLine(20, 130, LCD_WIDTH - 40, C_GRAY);

  // 三轴分量条 (X 侧向 / Y 纵向 / Z 竖直)
  const char *lbl[3] = {"X LAT", "Y LON", "Z VERT"};
  float val[3] = {s_g.gx, s_g.gy, s_g.gz};
  uint16_t col[3] = {C_CYAN, C_GREEN, C_PURPLE_L};
  int barX = 130, barW = 300;
  for (int i = 0; i < 3; i++) {
    int y = 150 + i * 46;
    gCanvas->setTextSize(2);
    gCanvas->setTextColor(col[i]);
    gCanvas->setCursor(20, y);
    gCanvas->print(lbl[i]);

    // 中心零位条
    gCanvas->drawRect(barX, y, barW, 20, C_GRAY);
    gCanvas->drawFastVLine(barX + barW / 2, y, 20, C_GRAY_LIGHT);
    // 数值条: 相对中心, 满量程 ±1g
    float v = constrain(val[i], -1.0f, 1.0f);
    int len = (int)(v * (barW / 2));
    if (len >= 0)
      gCanvas->fillRect(barX + barW / 2, y + 2, len, 16, col[i]);
    else
      gCanvas->fillRect(barX + barW / 2 + len, y + 2, -len, 16, col[i]);

    gCanvas->setTextSize(1);
    gCanvas->setTextColor(C_WHITE);
    gCanvas->setCursor(barX, y + 24);
    gCanvas->printf("%+.2f", val[i]);
  }

  // G 值曲线
  int gy0 = 320, gh = 130;
  gCanvas->drawRect(20, gy0, LCD_WIDTH - 40, gh, C_GRAY);
  // 0.8G 报警参考线
  int alarmY = gy0 + gh - (int)(ALARM_G * gh);
  gCanvas->drawFastHLine(20, alarmY, LCD_WIDTH - 40, C_RED_D);

  int n = G_HISTORY_LEN;
  int gw = LCD_WIDTH - 44;
  for (int i = 0; i < n - 1; i++) {
    int idx1 = (s_histIdx + i) % n;
    int idx2 = (s_histIdx + i + 1) % n;
    float v1 = constrain(s_gHist[idx1], 0.0f, 1.0f);
    float v2 = constrain(s_gHist[idx2], 0.0f, 1.0f);
    int x1 = 22 + (int)((float)i / (n - 1) * gw);
    int x2 = 22 + (int)((float)(i + 1) / (n - 1) * gw);
    int y1 = gy0 + gh - (int)(v1 * gh);
    int y2 = gy0 + gh - (int)(v2 * gh);
    uint16_t lc = (s_gHist[idx2] >= ALARM_G) ? C_RED : C_GREEN;
    gCanvas->drawLine(x1, y1, x2, y2, lc);
  }

  gCanvas->setTextSize(1);
  gCanvas->setTextColor(C_GRAY_LIGHT);
  gCanvas->setCursor(22, gy0 + 4);
  gCanvas->print("G CURVE  0.8G ALARM LINE");

  gCanvas->setTextSize(2);
  gCanvas->setTextColor(C_GRAY_LIGHT);
  gCanvas->setCursor(16, LCD_HEIGHT - 28);
  gCanvas->print("PRO MODE");
}

// ============================================================
// 根据 G 值更新宠物表情
// ============================================================
static void updateMood() {
  unsigned long now = millis();
  PetMood next = s_mood;

  if (imuCheckBump()) {
    next = MOOD_BUMP;
    s_moodUntil = now + 500;
  } else if (audioLevel() > 0.30f) {
    // 音量足够 -> 听歌哼歌 (优先级低于颠簸)
    next = MOOD_SING;
    s_moodUntil = now + 300;
  } else if (s_g.gy > ACCEL_THRESHOLD) {
    next = MOOD_ACCEL;
    s_moodUntil = now + 300;
  } else if (s_g.gy < BRAKE_THRESHOLD) {
    next = MOOD_BRAKE;
    s_moodUntil = now + 300;
  } else if (s_g.gx > TURN_THRESHOLD) {
    next = MOOD_TURN_R;
    s_moodUntil = now + 300;
  } else if (s_g.gx < -TURN_THRESHOLD) {
    next = MOOD_TURN_L;
    s_moodUntil = now + 300;
  } else if (now > s_moodUntil) {
    next = MOOD_IDLE;
  }
  s_mood = next;
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  Wire.begin(IIC_SDA, IIC_SCL);

  bool dispOk = displayBegin();
  Serial.printf("[SYS] Display: %s\n", dispOk ? "OK" : "FAIL");

  bool imuOk = imuBegin();
  Serial.printf("[SYS] IMU: %s\n", imuOk ? "OK" : "FAIL");

  bool touchOk = touchBegin();
  Serial.printf("[SYS] Touch: %s\n", touchOk ? "OK" : "FAIL");

  bool audioOk = audioBegin();
  Serial.printf("[SYS] Audio: %s\n", audioOk ? "OK" : "FAIL");

  touchOnSwipe(onSwipe);
  touchOnTap(onTap);

  // 启动画面
  gCanvas->fillScreen(C_BG);
  drawCenterText(LCD_WIDTH / 2, LCD_HEIGHT / 2 - 40, "EVA G-METER", 4, C_GREEN);
  drawCenterText(LCD_WIDTH / 2, LCD_HEIGHT / 2 + 30, "SWIPE TO START", 2, C_PURPLE_L);
  displayFlush();
  delay(1500);
}

void loop() {
  // 触摸手势
  touchLoop();

  // 音频采样 (非阻塞, 读到即返回; 若 I2S 无数据会短暂阻塞一帧)
  audioSample();

  // IMU 更新
  if (imuUpdate(s_g)) {
    // 报警判定
    s_alarm = (s_g.gTotal >= ALARM_G);

    // 曲线采样 (约 20Hz)
    unsigned long now = millis();
    if (now - s_lastHistMs >= 50) {
      s_gHist[s_histIdx] = s_g.gTotal;
      s_histIdx = (s_histIdx + 1) % G_HISTORY_LEN;
      s_lastHistMs = now;
    }

    // 更新表情
    if (s_mode == MODE_PET) updateMood();
  }

  // 限制刷新率 ~30fps
  if (millis() - s_lastFrame < 33) return;
  s_lastFrame = millis();

  gCanvas->fillScreen(C_BG);
  if (s_mode == MODE_PET)
    drawPetMode();
  else
    drawProMode();

  drawAlarmFrame();
  displayFlush();
}
