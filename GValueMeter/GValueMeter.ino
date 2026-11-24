/* ===========================================================================
 *  GValueMeter.ino — 车载 G 值表 / 宠物表情屏
 *  硬件：Waveshare ESP32-S3-Touch-AMOLED-2.16
 *        · 2.16" 480×480 AMOLED (CO5300, QSPI)
 *        · QMI8658 六轴 IMU
 *        · ES7210 双麦 + I2S
 *        · AXP2101 电源管理
 *
 *  两套界面：
 *    宠物模式  — 呼吸圆脸 + 眨眼高光 + 加速/减速/颠簸/转弯/听歌表情
 *    专业模式  — 大号 G 值 + 三轴分量条 + 120 点曲线 + 0.8G 报警
 *
 *  按键（GPIO18 侧面键 / GPIO0 BOOT 键，功能相同）：
 *    短按       切换界面
 *    双击       清除历史最大 G
 *    长按 1.2s  重新取重力零点（换车/换安装角度后用）
 * =========================================================================== */
#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "pin_config.h"
#include "canvas_gfx.h"
#include "hud.h"
#include "ui_screen.h"
#include "gmeter.h"
#include "mic_audio.h"
#include "pet_mode.h"
#include "pro_mode.h"
#include "touch.h"
#include "gg_mode.h"

/* ------------------------------ 全局对象 ------------------------------ */
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);

CanvasGFX canvas(LCD_WIDTH, LCD_HEIGHT);
XPowersPMU power;
GMeter gm;
MicAudio mic;
PetMode pet;
ProMode pro;
GgMode  gg;

uint8_t  g_mode = 0;                 // 0 = 宠物, 1 = 专业, 2 = G-G 圆盘
char     g_notice[40] = {0};
uint32_t g_noticeUntil = 0;
uint16_t g_fps = 0;

/* ------------------------------ 小工具 ------------------------------ */
static void setNotice(const char *s, uint32_t ms = UI_NOTICE_MS) {
  strncpy(g_notice, s, sizeof(g_notice) - 1);
  g_notice[sizeof(g_notice) - 1] = 0;
  g_noticeUntil = millis() + ms;
}

/* 三页循环：0 宠物 → 1 专业 → 2 G-G 圆盘 → 0 */
static void applyMode(uint8_t m, bool announce = true) {
  g_mode = (uint8_t)(m % 3);
  if (g_mode == 0) {
    pet.reset();
    if (announce) setNotice("宠物表情模式");
  } else if (g_mode == 1) {
    pro.reset();
    if (announce) setNotice("专业G值模式");
  } else {
    gg.reset();
    if (announce) setNotice("G-G圆盘");
  }
}

static void drawNotice(CanvasGFX *c, uint32_t now) {
  if (!g_notice[0] || now > g_noticeUntil) return;
  uiNotice(c, g_notice);
}

/* 开机 / 校准进度屏 */
static void bootScreen(const char *line, float prog) {
  uiBoot(&canvas, line, prog);
  canvasPresentAll(gfx, &canvas);
}

/* 校准时的进度浮层 */
static void drawCalOverlay(CanvasGFX *c, const GSnapshot &g) {
  if (!g.calibrating) return;
  uiCalOverlay(c, g.calProgress);
}

/* ------------------------------ 串口命令 ------------------------------ */
static bool findAxis(const char *s, const char *key, uint8_t &idx, int &sgn) {
  const char *p = strstr(s, key);
  if (!p) return false;
  p += strlen(key);
  if (*p != '=') return false;
  p++;
  sgn = 1;
  if (*p == '-') { sgn = -1; p++; }
  if (*p == 'x' || *p == 'X') idx = 0;
  else if (*p == 'y' || *p == 'Y') idx = 1;
  else if (*p == 'z' || *p == 'Z') idx = 2;
  else return false;
  return true;
}

static void printInfo() {
  uint8_t lat, lon, vert;
  int8_t ls;
  gm.getAxis(lat, lon, vert, ls);
  const char *nm[3] = {"X", "Y", "Z"};
  Serial.printf("[INFO] axis lat=%s lon=%s%s vert=%s | peak=%.2f g | fps=%u | mic=%s\r\n",
                nm[lat], (ls < 0 ? "-" : ""), nm[lon], nm[vert],
                gm.peak(), g_fps, mic.ok() ? "ok" : "off");
}

static void execCmd(char *cmd) {
  if (!strncmp(cmd, "cal", 3)) {
    gm.requestZero();
    g_notice[0] = 0;              // 进度交给校准浮层显示
    Serial.println("[CMD] zero start");
    return;
  }
  if (!strncmp(cmd, "reset", 5)) {
    gm.resetPeak();
    setNotice("峰值已清零");
    Serial.println("[CMD] peak cleared");
    return;
  }
  if (!strncmp(cmd, "info", 4)) { printInfo(); return; }
  if (!strncmp(cmd, "help", 4) || cmd[0] == '?') {
    Serial.println("cal | reset | info | mode pet|pro|gg | bright=0..255 | axis lat=x lon=-z vert=y");
    return;
  }
  if (!strncmp(cmd, "mode", 4)) {
    if (strstr(cmd, "pro"))      applyMode(1);
    else if (strstr(cmd, "pet")) applyMode(0);
    else if (strstr(cmd, "gg"))  applyMode(2);
    else                         applyMode((uint8_t)(g_mode + 1));
    return;
  }
  if (!strncmp(cmd, "bright", 6)) {
    const char *p = strchr(cmd, '=');
    if (p) {
      int v = atoi(p + 1);
      if (v < 5) v = 5;
      if (v > 255) v = 255;
      gfx->setBrightness((uint8_t)v);
      Serial.printf("[CMD] brightness=%d\r\n", v);
    }
    return;
  }
  if (!strncmp(cmd, "axis", 4)) {
    uint8_t lat, lon, vert, i;
    int8_t ls;
    int sg;
    gm.getAxis(lat, lon, vert, ls);
    bool any = false;
    if (findAxis(cmd, "lat", i, sg))  { lat = i; any = true; }
    if (findAxis(cmd, "lon", i, sg))  { lon = i; ls = (int8_t)sg; any = true; }
    if (findAxis(cmd, "vert", i, sg)) { vert = i; any = true; }
    if (any) {
      gm.setAxis(lat, lon, vert, ls);
      printInfo();
      setNotice("轴向已保存");
    } else {
      Serial.println("[CMD] 用法: axis lat=x lon=-z vert=y");
    }
    return;
  }
  Serial.println("[CMD] 未知命令，输入 help 查看");
}

static void handleSerial() {
  static char buf[96];
  static uint8_t len = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r' || ch == '\n') {
      if (len) { buf[len] = 0; execCmd(buf); len = 0; }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = ch;
    }
  }
}

/* ------------------------------ 触摸滑动 ------------------------------ */
/* 左滑 → 专业 G 值模式；右滑 → 宠物表情模式（与按键切换等价） */
static void handleSwipe() {
  int ev = touchPoll();
  if (ev == 0) return;
  if (ev < 0) {                       /* 左滑：下一页 */
    applyMode((uint8_t)(g_mode + 1));
  } else if (ev == 1) {               /* 右滑：上一页 */
    applyMode((uint8_t)((g_mode + 2) % 3));
  }
}

/* ------------------------- BOOT 键：重置水平面 ------------------------- */
/* 车身装歪了、换了安装角度，按一下 BOOT 就重新取一次重力零点（要求：停稳、尽量水平） */
static void updateBootKey(uint32_t now) {
  static bool down = false;
  static uint32_t downMs = 0;

  bool raw = (digitalRead(PIN_BOOT) == LOW);
  if (raw && !down) {
    down = true;
    downMs = now;
  } else if (!raw && down) {
    down = false;
    if (now - downMs >= 120) {            /* 防抖：短促误触不算 */
      gm.requestZero();
      setNotice("重新校准水平，请保持静止", 2200);
      Serial.println("[BOOT] 水平零点重取");
    }
  }
}

/* ------------------------------ 按键 ------------------------------ */
static void updateButtons(uint32_t now) {
  static bool pressed = false;
  static uint32_t downMs = 0, releaseMs = 0;
  static bool longDone = false;
  static uint8_t clicks = 0;

  bool raw = (digitalRead(PIN_KEY3) == LOW);   /* BOOT 键单独走 updateBootKey */

  if (raw && !pressed) {                       // 按下
    pressed = true;
    downMs = now;
    longDone = false;
  } else if (!raw && pressed) {                // 松开
    pressed = false;
    releaseMs = now;
    if (!longDone) {
      uint32_t held = now - downMs;
      if (held > 40) {
        clicks++;
        if (clicks >= 2) {
          clicks = 0;
          gm.resetPeak();
          setNotice("峰值已清零");
        }
      }
    }
  } else if (raw && pressed && !longDone) {    // 长按
    if (now - downMs > 1200) {
      longDone = true;
      gm.requestZero();
      g_notice[0] = 0;                         // 交给校准浮层
    }
  }

  /* 单击判定（松手后 350ms 内不再按下） */
  if (!raw && !pressed && clicks == 1 && (now - releaseMs) > 350) {
    clicks = 0;
    applyMode((uint8_t)(g_mode + 1));
  }
}

/* ------------------------------ IMU 任务 ------------------------------ */
static void imuTask(void *arg) {
  (void)arg;
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(IMU_PERIOD_MS);
  while (true) {
    gm.taskStep();
    vTaskDelayUntil(&last, period);
  }
}

/* ------------------------------ setup ------------------------------ */
void setup() {
  Serial.begin(115200);
  delay(150);

  Wire.begin(IIC_SDA, IIC_SCL, 400000);

  /* PMU：板上 ALDO/BLDO 默认已给屏、传感器供电，这里只做状态监测 */
  if (power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
  } else {
    Serial.println("[PMU] AXP2101 未找到（屏幕仍可用）");
  }

  if (!gfx->begin()) {
    Serial.println("[LCD] gfx->begin() failed");
  }
  bus->writeC8D8(0x36, LCD_MADCTL);
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(BRIGHTNESS_DEF);

  if (!canvas.begin()) {
    Serial.println("[MEM] PSRAM 帧缓冲分配失败，请确认板子 PSRAM=OPI 且已启用");
    gfx->setCursor(30, 200);
    gfx->setTextSize(2);
    gfx->setTextColor(COL_RED, COL_RED);
    gfx->println("PSRAM ALLOC FAILED");
    while (true) delay(200);
  }

  bootScreen("正在启动传感器…", 0.05f);

  if (!gm.begin()) {
    bootScreen("未找到 QMI8658", 0.2f);
    Serial.println("[IMU] QMI8658 未找到");
  } else {
    bootScreen("校准中，请保持车身水平静止", 0.35f);
  }

  if (!mic.begin()) {
    Serial.println("[MIC] 音频不可用，宠物表情仅由 G 值驱动");
  }
  bootScreen("就绪", 0.9f);
  delay(200);

  xTaskCreatePinnedToCore(imuTask, "imu", 3072, nullptr, 2, nullptr, 0);

  pinMode(PIN_KEY3, INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);

  touchBegin(&Wire);   /* 触摸芯片找不到就退回按键，不影响其它功能 */

  pet.reset();
  pro.reset();
  setNotice("左右滑动切换界面");
  Serial.println("[BOOT] G-Meter ready. 输入 help 查看串口命令");
  Serial.println("[BOOT] 侧面键短按=翻页 双击=清峰值 长按1.2s=取零点 | 左右滑动=翻页 | BOOT 键=重置水平面");
}

/* ------------------------------ loop ------------------------------ */
void loop() {
  uint32_t now = millis();

  updateBootKey(now);
  updateButtons(now);
  handleSwipe();
  handleSerial();

  static uint32_t lastFrame = 0, fpsT = 0;
  static uint16_t frameCnt = 0;

  if (now - lastFrame >= UI_FRAME_MS) {
    float dt = (now - lastFrame) * 0.001f;
    lastFrame = now;

    GSnapshot g;
    gm.snapshot(g);
    AudioSnapshot a;
    mic.snapshot(a);

    if (g_mode == 0) {
      pet.update(g, a, dt);
      pet.draw(&canvas, g, a);
    } else if (g_mode == 1) {
      pro.update(g, dt);
      pro.draw(&canvas, g);
    } else {
      gg.update(g, dt);
      gg.draw(&canvas, g);
    }

    drawCalOverlay(&canvas, g);
    drawNotice(&canvas, now);
    canvasPresentAll(gfx, &canvas);

    frameCnt++;
    if (now - fpsT >= 1000) {
      g_fps = frameCnt;
      frameCnt = 0;
      fpsT = now;
    }
  }

  /* 每秒打印一次运行状态（串口监视器可直接看 G 值与麦克风） */
  static uint32_t lastLog = 0;
  if (now - lastLog >= 1000) {
    lastLog = now;
    printInfo();
  }

  delay(1);
}
