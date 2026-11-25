/* ===========================================================================
 *  GValueMeter.ino — 车载 G 值表 / 宠物表情屏
 *  硬件：Waveshare ESP32-S3-Touch-AMOLED-2.16
 *        · 2.16" 480×480 AMOLED (CO5300, QSPI 40MHz ×4)
 *        · QMI8658 六轴 IMU
 *        · ES7210 双麦 + I2S
 *        · AXP2101 电源管理
 *
 *  三套界面（侧面键短按循环）：
 *    宠物模式  — 呼吸圆脸 + 眨眼 + 加速/减速/颠簸/转弯/听歌表情
 *    专业模式  — 大号 G 值 + 三轴分量条 + 120 点曲线 + 0.8G 报警
 *    G-G 圆盘  — 赛车摩擦圆：圆点按惯性方向在圆内移动并留轨迹 + 四向峰值
 *
 *  两种状态：
 *    待机（上电默认）— 不采样 IMU、麦克风挂起、20fps，宠物只播循环动画，省电
 *    测量            — 200Hz 采样、零点校准、三页都显示真实 G 值
 *
 *  按键：
 *    BOOT 长按 1.5s   开测 / 停测（开测时自动重新取重力零点、清本次峰值）
 *    BOOT 短按        宠物页 = 隐藏 / 显示 G 值（干净的脸）；其他页测量中 = 重新取水平零点
 *    侧面键短按       切换界面
 *    侧面键双击       清除峰值
 *    侧面键长按 1.2s  重新取重力零点
 *
 *  推屏：panel_qspi.cpp 用 spi_master 队列事务做异步 DMA 推屏，两块 PSRAM
 *        帧缓冲交替，画下一帧的同时上一帧还在往屏上发。
 * =========================================================================== */
#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "pin_config.h"
#define GVM_PUSH_GFX 1   /* 稳定性优先：走 GFX 官方推屏，帧缓冲保持小端 */
#include "canvas_gfx.h"
#include "hud.h"
#include "ui_screen.h"
#include "gmeter.h"
#include "mic_audio.h"
#include "pet_mode.h"
#include "pro_mode.h"
#include "spectrum.h"
#include "gg_mode.h"
#include "panel_qspi.h"
#include <Preferences.h>

/* ------------------------------ 全局对象 ------------------------------ */
/* is_shared_interface = true：GFX 只在初始化 / 调亮度时短暂持有总线锁，
 * 平时把 SPI2 让给 panel_qspi 的异步 DMA 设备（否则 GFX 会永久独占总线，
 * 我们的队列事务会一直等锁 → 卡死在第一帧）。 */
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3, true);

Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);

CanvasGFX canvas(LCD_WIDTH, LCD_HEIGHT);
XPowersAXP2101 power;
GMeter gm;
MicAudio mic;
PetMode pet;
ProMode pro;
GgMode  gg;

uint8_t  g_mode = 0;                 // 0 = 宠物, 1 = 专业, 2 = G-G 圆盘
bool     g_measuring = false;        // 上电待机，长按 BOOT 开测
bool     g_hideG = false;            // 宠物页：单击 BOOT 隐藏 / 显示 G 值（存 NVS）
bool     g_pmuOk = false;
static bool g_spec = false;          /* 宠物页频谱柱开关（PWR 键） */
int      g_battPct = -1;             // -1 = 读不到
bool     g_battChg = false;
static uint32_t g_lcdTestStart = 0, g_lcdTestUntil = 0;   /* 屏体纯色自检窗口 */
char     g_notice[40] = {0};
uint32_t g_noticeUntil = 0;
uint16_t g_fps = 0;
static uint32_t g_drawUs = 0, g_waitUs = 0, g_dmaUs = 0;
static uint32_t g_perUs = 0, g_lastDraw = 0;  /* 绘制 / 等待推屏 / DMA 实际耗时 */
static uint16_t *bufA = nullptr, *bufB = nullptr;         /* 两块 PSRAM 帧缓冲 */
static TaskHandle_t g_imuTask = nullptr;

/* ---- 推屏任务：拷贝+DMA 跑在 core 1，与 core 0 的绘制并行 ---- */
struct GFrameJob { uint16_t *buf; };
static QueueHandle_t     g_pushQ    = nullptr;
static SemaphoreHandle_t g_pushDone = nullptr;
static bool              g_pushInFlight = false;

static void pushTask(void *arg) {
  (void)arg;
  GFrameJob job;
  for (;;) {
    if (xQueueReceive(g_pushQ, &job, portMAX_DELAY) != pdTRUE) continue;
    panelFlush();                      /* 上一帧残余（正常为 0） */
    panelSetWindow(0, 0, LCD_WIDTH, LCD_HEIGHT);
    panelPush(job.buf, (size_t)LCD_WIDTH * LCD_HEIGHT);
    panelFlush();                      /* 等这一帧真正发完，再允许复用该缓冲 */
    xSemaphoreGive(g_pushDone);
  }
}

/* 等上一次推屏彻底结束（含 DMA 排空） */
static void waitPushIdle() {
  if (!g_pushInFlight) return;
  xSemaphoreTake(g_pushDone, portMAX_DELAY);
  g_pushInFlight = false;
}

/* 把整帧交给推屏任务，立刻返回；下一帧的绘制与这次拷贝/DMA 重叠 */
static void pushFrameAsync(uint16_t *buf) {
  if (!g_pushQ) {                      /* 任务没起来时退化为同步推屏 */
    panelFlush();
    panelSetWindow(0, 0, LCD_WIDTH, LCD_HEIGHT);
    panelPush(buf, (size_t)LCD_WIDTH * LCD_HEIGHT);
    panelFlush();
    return;
  }
  GFrameJob job = { buf };
  xQueueSend(g_pushQ, &job, portMAX_DELAY);
  g_pushInFlight = true;
}
static volatile bool g_imuResync = false;

/* ------------------------------ 小工具 ------------------------------ */
static void saveHideG();

static void setNotice(const char *s, uint32_t ms = UI_NOTICE_MS) {
  strncpy(g_notice, s, sizeof(g_notice) - 1);
  g_notice[sizeof(g_notice) - 1] = 0;
  g_noticeUntil = millis() + ms;
}

/* 麦克风只在两处开：宠物页测量中、频谱模式（任何页面）。
 * 专业页 / G-G 圆盘页一律关麦，省 CPU、也避免误触发"听歌"表情。 */
static void syncMicActive() {
  bool need = g_spec || (g_mode == 0 && g_measuring);
  mic.setActive(need);
}

static void requestFullPush();   /* 下一帧整屏重推（切页后用） */

/* 三页循环：0 宠物 → 1 专业 → 2 G-G 圆盘 → 0 */
static void applyMode(uint8_t m, bool announce = true) {
  g_mode = (uint8_t)(m % 3);
  /* 模式切换不再弹提示框：切页本身就是反馈，浮层还会挡住内容 */
  (void)announce;
  requestFullPush();          /* 切页后整屏重推一次，面板不会残留上一页 */
  if (g_mode == 0) {
    pet.reset();
  } else if (g_mode == 1) {
    pro.reset();
  } else {
    gg.reset();
  }
  syncMicActive();
}

/* 开测 / 停测 */
static void setMeasuring(bool on) {
  if (on == g_measuring) return;
  g_measuring = on;
  pet.setStandby(!on);
  gm.setActive(on);
  syncMicActive();
  if (on) {
    gm.resetPeak();                 /* 峰值与四向峰值都是"本次开测以来" */
    gm.requestZero();
    g_imuResync = true;
    if (g_imuTask) vTaskResume(g_imuTask);
    g_notice[0] = 0;                /* 进度交给校准浮层显示 */
    Serial.println("[MODE] 测量开始：取零点中");
  } else {
    if (g_imuTask) vTaskSuspend(g_imuTask);
    setNotice("已停止测量", 1800);
    Serial.println("[MODE] 测量停止：待机");
  }
}

static void drawNotice(CanvasGFX *c, uint32_t now) {
  if (!g_notice[0] || now > g_noticeUntil) return;
  uiNotice(c, g_notice);
}

#ifdef GVM_PUSH_GFX
/* ---- 分带差分推屏：只把真正发生变化的横条推给面板 ----
 * GFX 的 draw16bitRGBBitmap 是逐像素做 RGB565 转换后送 SPI，全屏 460KB 要 ~73ms；
 * 待机/宠物页大部分区域是静止的，按 24 行一条比对哈希，只推脏条，
 * 点到脸、顶部面板、通知时才多推几条，帧率能翻一截，画面也不会闪。 */
#define PB_ROWS 48   /* 10 条带：窗口切换少一半，局部刷新更稳 */
#define PB_N    (LCD_HEIGHT / PB_ROWS)
static uint32_t g_bandHash[PB_N];
static bool     g_bandOk = false;
static bool     g_pushAll = true;      /* 面板刚初始化/复位后必须整屏重推 */
static uint16_t g_dirtyBands = 0;
static uint32_t g_hashUs = 0, g_bandPushUs = 0;   /* 分带比对/实际送屏耗时 */

static inline uint32_t bandHash(const uint32_t *p, int words) {
  uint32_t h = 0x811C9DC5u;
  for (int i = 0; i < words; i++) h = (h * 16777619u) ^ p[i];
  return h;
}

static bool g_fastPush = false;      /* 实测 writePixels 比逐像素还慢，默认关闭，留作对比 */

static void pushBand(uint16_t *buf, int b0, int b1) {
  int16_t y = (int16_t)(b0 * PB_ROWS);
  int16_t h = (int16_t)((b1 - b0) * PB_ROWS);
  if (!g_fastPush) {
    gfx->draw16bitRGBBitmap(0, y, buf + (size_t)y * LCD_WIDTH, LCD_WIDTH, h);
    return;
  }
  /* 关键提速点：GFX 的 draw16bitRGBBitmap 是"每个像素一次 write16"，
   * 480×480 要 23 万次总线调用；这里改成一次 writeAddrWindow + writePixels，
   * 由总线内部按 16384 像素一批打包 DMA，调用次数从 23 万降到 14 次。 */
  gfx->startWrite();
  gfx->writeAddrWindow(0, y, LCD_WIDTH, h);
  bus->writePixels(buf + (size_t)y * LCD_WIDTH, (uint32_t)LCD_WIDTH * h);
  gfx->endWrite();
}

static void requestFullPush() { g_pushAll = true; }

static void pushDirty(uint16_t *buf) {
  const int words = LCD_WIDTH * PB_ROWS / 2;
  const uint32_t *w = (const uint32_t *)buf;
  g_dirtyBands = 0;
  g_hashUs = 0;
  g_bandPushUs = 0;
  if (!g_bandOk || g_pushAll) {
    for (int b = 0; b < PB_N; b++)
      g_bandHash[b] = bandHash(w + (size_t)b * words, words);
    pushBand(buf, 0, PB_N);
    g_bandOk = true;
    g_pushAll = false;
    g_dirtyBands = PB_N;
    return;
  }
  int run = -1;
  uint32_t tHash = micros();
  uint32_t tPush2 = tHash;
  for (int b = 0; b < PB_N; b++) {
    uint32_t h = bandHash(w + (size_t)b * words, words);
    g_hashUs = micros() - tHash;
    tPush2 = micros();
    if (h != g_bandHash[b]) {
      g_bandHash[b] = h;
      g_dirtyBands++;
      if (run < 0) run = b;
    } else if (run >= 0) {
      pushBand(buf, run, b);
      run = -1;
      g_bandPushUs += micros() - tPush2;
    }
    tPush2 = micros();
  }
  if (run >= 0) { pushBand(buf, run, PB_N); g_bandPushUs += micros() - tPush2; }
}
#endif

/* 开机 / 校准进度屏（同步推屏） */
static void bootScreen(const char *line, float prog) {
  uiBoot(&canvas, line, prog);
#ifdef GVM_PUSH_GFX
  g_pushAll = true;
  pushDirty(canvas.buffer());
#else
  waitPushIdle();
  panelFlush();
  panelSetWindow(0, 0, LCD_WIDTH, LCD_HEIGHT);
  panelPush(canvas.buffer(), (size_t)LCD_WIDTH * LCD_HEIGHT);
  panelFlush();
#endif
}

/* 校准时的进度浮层 */
static void drawCalOverlay(CanvasGFX *c, const GSnapshot &g) {
  if (!g.calibrating) return;
  uiCalOverlay(c, g.calProgress);
}

/* 待机时把快照清零：不显示任何 G 值，宠物只播自己的循环动画 */
static void zeroSnapshot(GSnapshot &g) {
  g.lat = g.lon = g.vert = g.mag = g.rawMag = g.peak = g.vertP2P = 0.f;
  for (int i = 0; i < 4; i++) g.dirPeak[i] = 0.f;
  g.curveCount = 0;
  g.bumpSeq = 0;
  g.calibrating = false;
  g.calProgress = 1.f;
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
  int8_t las, ls;
  gm.getAxis(lat, lon, vert, las, ls);
  const char *nm[3] = {"X", "Y", "Z"};
  GSnapshot g;
  gm.snapshot(g);
  Serial.printf("[INFO] %s | axis lat=%s%s lon=%s%s vert=%s | peak=%.2f 4dir[%.2f %.2f %.2f %.2f] | fps=%u draw=%ums wait=%ums dma=%ums | mic=%s\r\n",
                g_measuring ? "MEASURE" : "STANDBY",
                (las < 0 ? "-" : ""), nm[lat], (ls < 0 ? "-" : ""), nm[lon], nm[vert],
                gm.peak(), g.dirPeak[0], g.dirPeak[1], g.dirPeak[2], g.dirPeak[3],
                g_fps, (unsigned)(g_drawUs / 1000), (unsigned)(g_waitUs / 1000),
                (unsigned)(g_dmaUs / 1000), mic.ok() ? "ok" : "off");
  Serial.printf("[BAT] %d%%%s | petG=%s | lat=%+.2f lon=%+.2f vert=%+.2f mag=%.2f\r\n", g_battPct, g_battChg ? " charging" : "",
                g_hideG ? "hidden" : "shown", g.lat, g.lon, g.vert, g.mag);
  Serial.printf("[PERF] period=%ums | push=%ums fill=%ums queue=%ums flush=%ums | %s\r\n",
                (unsigned)(g_perUs / 1000),
                (unsigned)(panelPushUs() / 1000), (unsigned)(panelFillUs() / 1000),
                (unsigned)(panelQueueUs() / 1000), (unsigned)(panelFlushUs() / 1000),
                panelCopyFrames() ? "copy" : "direct");
}

static void execCmd(char *cmd) {
  if (!strncmp(cmd, "cal", 3)) {
    if (!g_measuring) { setMeasuring(true); return; }
    gm.requestZero();
    g_notice[0] = 0;              // 进度交给校准浮层显示
    Serial.println("[CMD] zero start");
    return;
  }
  if (!strncmp(cmd, "start", 5)) { setMeasuring(true);  return; }
  if (!strncmp(cmd, "stop", 4))  { setMeasuring(false); return; }
  if (!strncmp(cmd, "reset", 5)) {
    gm.resetPeak();
    setNotice("峰值已清零");
    Serial.println("[CMD] peak cleared");
    return;
  }
  if (!strncmp(cmd, "info", 4)) { printInfo(); return; }
  if (!strncmp(cmd, "bench", 5)) {
    uint32_t t0 = micros();
    canvas.fillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, RGB565(3, 5, 9));
    uint32_t tFill = micros() - t0;
    t0 = micros();
    for (int b = 0; b < PB_N; b++) g_bandHash[b] = bandHash((const uint32_t *)canvas.buffer() + (size_t)b * (LCD_WIDTH * PB_ROWS / 2), LCD_WIDTH * PB_ROWS / 2);
    uint32_t tHash = micros() - t0;
    t0 = micros();
    g_pushAll = true;
    pushDirty(canvas.buffer());
    uint32_t tPush = micros() - t0;
    t0 = micros();
    gfx->draw16bitRGBBitmap(0, 0, canvas.buffer(), LCD_WIDTH, LCD_HEIGHT);
    uint32_t tGfx = micros() - t0;
    Serial.printf("[BENCH] fill=%ums hash=%ums pushAll=%ums gfxFull=%ums\r\n",
                  (unsigned)(tFill / 1000), (unsigned)(tHash / 1000),
                  (unsigned)(tPush / 1000), (unsigned)(tGfx / 1000));
    return;
  }
  if (!strncmp(cmd, "pushfast", 8)) {
    g_fastPush = (strstr(cmd, "off") == nullptr);
    g_pushAll = true;
    Serial.printf("[CMD] pushfast %s\r\n", g_fastPush ? "on" : "off");
    return;
  }
  if (!strncmp(cmd, "lcd", 3)) {
    g_pushAll = true;                    /* 面板可能被复位/重配，下一帧整屏重推 */
    if (strstr(cmd, "reinit")) {
      waitPushIdle();
      panelFlush();
      /* GFX_SKIP_DATABUS_BEGIN：不重开 SPI 总线，只做面板硬件复位（RESET 脚拉低再放）
       * + 完整 CO5300 初始化序列，这是把"面板卡死/丢锁"救回来的唯一可靠办法 */
      bool ok = gfx->begin(GFX_SKIP_DATABUS_BEGIN);
      gfx->setBrightness(BRIGHTNESS_DEF);
      bus->writeC8D8(0x36, LCD_MADCTL);
      Serial.printf("[LCD] hw reinit %s\r\n", ok ? "ok" : "failed");
      setNotice("面板已重新初始化", 2000);
    } else if (strstr(cmd, "softinit")) {
      waitPushIdle();
      panelReinit();
      setNotice("面板软复位完成", 2000);
    } else if (strstr(cmd, "bisect")) {
      /* 逐段二分：每段 2.5 秒，找出"我们的推屏"到底断在哪一步 */
      waitPushIdle();
      panelFlush();
      Serial.println("[LCD] bisect P1 GFX native  = RED");
      gfx->fillScreen(0xF800); delay(2500);

      canvas.fillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0x07E0);   /* 纯绿 */
      Serial.println("[LCD] bisect P2 our sync    = GREEN");
      panelSetWindow(0, 0, LCD_WIDTH, LCD_HEIGHT);
      panelPushSync(canvas.buffer(), (size_t)LCD_WIDTH * LCD_HEIGHT);
      delay(2500);

      canvas.fillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0x001F);   /* 纯蓝 */
      Serial.println("[LCD] bisect P3 our async   = BLUE");
      panelSetWindow(0, 0, LCD_WIDTH, LCD_HEIGHT);
      pushFrameAsync(canvas.buffer());
      delay(2500);
      waitPushIdle();
      panelFlush();

      canvas.fillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0xFFFF);   /* 纯白 */
      Serial.println("[LCD] bisect P4 our sync whit");
      panelSetWindow(0, 0, LCD_WIDTH, LCD_HEIGHT);
      panelPushSync(canvas.buffer(), (size_t)LCD_WIDTH * LCD_HEIGHT);
      delay(2500);
      Serial.println("[LCD] bisect done");
      setNotice("分段测试完成", 2000);
    } else if (strstr(cmd, "sync")) {
      waitPushIdle();
      panelFlush();
      canvas.fillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0xF800);
      panelSetWindow(0, 0, LCD_WIDTH, LCD_HEIGHT);
      panelPushSync(canvas.buffer(), (size_t)LCD_WIDTH * LCD_HEIGHT);
      Serial.printf("[LCD] sync push done, %luus\r\n", (unsigned long)panelPushUs());
      setNotice("同步推屏测试", 1500);
    } else if (strstr(cmd, "gfx")) {
      /* 边界测试：完全绕开我们的 DMA 推屏，走 GFX 官方轮询路径整屏刷色。
       * 这一条亮 → 面板活着，问题在我们的推屏路径；这一条也黑 → 面板/排线/供电 */
      waitPushIdle();
      panelFlush();
      gfx->begin(GFX_SKIP_DATABUS_BEGIN);
      gfx->setBrightness(BRIGHTNESS_DEF);
      const uint16_t cols[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
      for (int i = 0; i < 4; i++) { gfx->fillScreen(cols[i]); delay(900); }
      bus->writeC8D8(0x36, LCD_MADCTL);
      Serial.println("[LCD] gfx path test done");
      setNotice("GFX 直推测试完成", 2000);
    } else if (strstr(cmd, "test")) {
      waitPushIdle();
      g_lcdTestStart = millis();
      g_lcdTestUntil = g_lcdTestStart + 3000;
      Serial.println("[LCD] test pattern: red/green/blue/white/black 3s");
    } else if (strstr(cmd, "on")) {
      waitPushIdle();
      panelCmd(0x29, nullptr, 0);
      Serial.println("[LCD] display on");
    } else if (strstr(cmd, "off")) {
      waitPushIdle();
      panelCmd(0x28, nullptr, 0);
      Serial.println("[LCD] display off");
    } else {
      Serial.println("[CMD] 用法: lcd reinit|softinit|test|bisect|sync|gfx|on|off");
    }
    return;
  }
  if (!strncmp(cmd, "spec", 4)) {
    bool on = !strstr(cmd, "off");
    if (strstr(cmd, "on")) on = true;
    setSpectrumMode(on);
    return;
  }
  if (!strncmp(cmd, "fb", 2)) {
    for (int k = 0; k < 2; k++) {
      uint16_t *b = k ? bufB : bufA;
      if (!b) { Serial.printf("[FB] %c null\r\n", k ? 'B' : 'A'); continue; }
      uint32_t nz = 0, n = (uint32_t)LCD_WIDTH * LCD_HEIGHT;
      for (uint32_t i = 0; i < n; i += 64) if (b[i]) nz++;
      Serial.printf("[FB] %c samples=%lu nonzero=%lu p00=%04X pmid=%04X pbar=%04X\r\n",
                    k ? 'B' : 'A', (unsigned long)(n / 64), (unsigned long)nz,
                    b[0], b[(LCD_HEIGHT / 2) * LCD_WIDTH + LCD_WIDTH / 2], b[(LCD_HEIGHT - 30) * LCD_WIDTH + 60]);
    }
    return;
  }
  if (!strncmp(cmd, "axp", 3)) {
    Serial.printf("[AXP] batt=%dmV %d%% chg=%d vbus=%d batt_in=%d | ALDO1..4=%d%d%d%d BLDO1/2=%d%d\r\n",
                  g_pmuOk ? power.getBattVoltage() : 0, g_battPct, (int)g_battChg,
                  g_pmuOk ? (int)power.isVbusIn() : 0, g_pmuOk ? (int)power.isBatteryConnect() : 0,
                  g_pmuOk ? (int)power.isEnableALDO1() : 0, g_pmuOk ? (int)power.isEnableALDO2() : 0,
                  g_pmuOk ? (int)power.isEnableALDO3() : 0, g_pmuOk ? (int)power.isEnableALDO4() : 0,
                  g_pmuOk ? (int)power.isEnableBLDO1() : 0, g_pmuOk ? (int)power.isEnableBLDO2() : 0);
    return;
  }
  if (!strncmp(cmd, "g", 1) && (cmd[1] == 0 || cmd[1] == '=')) {
    if (!strncmp(cmd + 1, "=hide", 5))      g_hideG = true;
    else if (!strncmp(cmd + 1, "=show", 5)) g_hideG = false;
    else                                    g_hideG = !g_hideG;
    pet.setHideG(g_hideG);
    saveHideG();
    setNotice(g_hideG ? "已隐藏G值" : "已显示G值", 1600);
    Serial.printf("[CMD] pet G values: %s\r\n", g_hideG ? "hidden" : "shown");
    return;
  }
  if (!strncmp(cmd, "bench", 5)) {
    waitPushIdle();
    uint32_t t0 = micros();
    canvas.fillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
    uint32_t t1 = micros();
    panelSetWindow(0, 0, LCD_WIDTH, LCD_HEIGHT);
    panelPush(canvas.buffer(), (size_t)LCD_WIDTH * LCD_HEIGHT);
    uint32_t t2 = micros();
    panelFlush();
    uint32_t t3 = micros();
    Serial.printf("[BENCH] fullfill=%ums push(copy+queue)=%ums drain=%ums | psram write=%.1fMB/s push=%.1fMB/s\r\n",
                  (unsigned)((t1 - t0) / 1000), (unsigned)((t2 - t1) / 1000), (unsigned)((t3 - t2) / 1000),
                  (float)(LCD_WIDTH * LCD_HEIGHT * 2) / (float)(t1 - t0),
                  (float)(LCD_WIDTH * LCD_HEIGHT * 2) / (float)(t2 - t1));
    return;
  }
  if (!strncmp(cmd, "help", 4) || cmd[0] == '?') {
    Serial.println("cal | start | stop | reset | info | spec[on|off] | bench | lcd reinit|softinit|test|bisect|sync|gfx|on|off | axp | g[=hide|show] | mode pet|pro|gg | bright=0..255 | axis lat=-x lon=-z vert=y");
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
      waitPushIdle();                     /* 命令走 GFX 轮询通道，先等 DMA 推完 */
      gfx->setBrightness((uint8_t)v);
      Serial.printf("[CMD] brightness=%d\r\n", v);
    }
    return;
  }
  if (!strncmp(cmd, "axis", 4)) {
    uint8_t lat, lon, vert, i;
    int8_t las, ls;
    int sg;
    gm.getAxis(lat, lon, vert, las, ls);
    bool any = false;
    if (findAxis(cmd, "lat", i, sg))   { lat = i; las = (int8_t)sg; any = true; }
    if (findAxis(cmd, "lon", i, sg))   { lon = i; ls = (int8_t)sg; any = true; }
    if (findAxis(cmd, "vert", i, sg))  { vert = i; any = true; }
    if (any) {
      gm.setAxis(lat, lon, vert, las, ls);
      printInfo();
      setNotice("轴向已保存");
    } else {
      Serial.println("[CMD] 用法: axis lat=-x lon=-z vert=y");
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

/* -------------------- 宠物页 G 值显示开关（存 NVS，重启后保持） -------------------- */
static void saveHideG() {
  Preferences p;
  p.begin("gmeter", false);
  p.putBool("hideg", g_hideG);
  p.end();
}

/* ------------------------- BOOT 键：长按开测 / 短按取零点 ------------------------- */
static void updateBootKey(uint32_t now) {
  static bool down = false, longDone = false;
  static uint32_t downMs = 0;

  bool raw = (digitalRead(PIN_BOOT) == LOW);
  if (raw && !down) {
    down = true;
    downMs = now;
    longDone = false;
  } else if (raw && down && !longDone && (now - downMs) >= BOOT_LONG_MS) {
    /* 按住到点直接触发，不用等松手 */
    longDone = true;
    setMeasuring(!g_measuring);
  } else if (!raw && down) {
    down = false;
    if (!longDone && (now - downMs) >= 120) {      /* 防抖：短促误触不算 */
      if (g_mode == 0) {                            /* 宠物页：隐藏 / 显示 G 值 */
        g_hideG = !g_hideG;
        pet.setHideG(g_hideG);
        saveHideG();
        setNotice(g_hideG ? "已隐藏G值" : "已显示G值", 1600);
        Serial.printf("[KEY] %s\r\n", g_hideG ? "hide G" : "show G");
      } else if (g_measuring) {
        gm.requestZero();
        Serial.println("[BOOT] 水平零点重取");
      } else {
        setNotice("待机中，长按 BOOT 开始测量", 2000);
      }
    }
  }
}

/* ------------------------------ 侧面键 ------------------------------ */
static void updateButtons(uint32_t now) {
  static bool pressed = false;
  static uint32_t downMs = 0, releaseMs = 0;
  static bool longDone = false;
  static uint8_t clicks = 0;

  bool raw = (digitalRead(PIN_KEY3) == LOW);

  if (raw && !pressed) {                       // 按下
    pressed = true;
    downMs = now;
    longDone = false;
  } else if (!raw && pressed) {                // 松开
    pressed = false;
    releaseMs = now;
    if (!longDone) {
      uint32_t held = now - downMs;
      if (held > 40) clicks = 1;                /* 双击清峰值已删除：只认单击切页 */
    }
  } else if (raw && pressed && !longDone) {    // 长按
    if (now - downMs > 1200) {
      longDone = true;
      if (!g_measuring) {
        setMeasuring(true);
      } else {
        gm.requestZero();
        g_notice[0] = 0;                       // 交给校准浮层
      }
    }
  }

  /* 单击判定（松手后 350ms 内不再按下） */
  if (!raw && !pressed && clicks == 1 && (now - releaseMs) > 350) {
    clicks = 0;
    applyMode((uint8_t)(g_mode + 1));
  }
}

/* PWR 键（AXP2101 PWRON）：短按在宠物页切频谱柱 */
static void setSpectrumMode(bool on) {
  g_spec = on;
  mic.setSpectrum(on);
  syncMicActive();                                  /* 频谱要用麦克风，待机时临时开起来 */
  /* 频谱页与三页互相切换时整屏重推 + 让当前页按整屏重画，
   * 否则专业页的局部刷新会把频谱的像素留在屏上（切换残留） */
  if (!on) {
    if (g_mode == 0) pet.reset();
    else if (g_mode == 1) pro.reset();
    else gg.reset();
  }
  requestFullPush();
  setNotice(on ? "频谱柱已开启" : "频谱柱已关闭", 1200);
  Serial.printf("[CMD] spectrum %s\r\n", on ? "on" : "off");
}

static void pollPwrKey() {
  if (!g_pmuOk) return;
  uint64_t irq = power.getIrqStatus();
  if (!irq) return;
  if (irq & (1ULL << 11)) {                 /* XPOWERS_AXP2101_PKEY_SHORT_IRQ */
    setSpectrumMode(!g_spec);               /* 任何页面按 PWR 都能切频谱 */
  }
  power.clearIrqStatus();
}

/* ------------------------------ IMU 任务 ------------------------------ */
static void imuTask(void *arg) {
  (void)arg;
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(IMU_PERIOD_MS);
  while (true) {
    if (g_imuResync) {
      /* 从待机恢复：重置节拍，避免补跑暂停期间的次数 */
      g_imuResync = false;
      last = xTaskGetTickCount();
    }
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
  g_pmuOk = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (g_pmuOk) {
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
    /* PWR（PWRON/PEKEY）短按中断：宠物页用来切频谱柱 */
    power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    power.clearIrqStatus();
    g_battPct = power.getBatteryPercent();
    g_battChg = power.isCharging();
    pro.setBattery(g_battPct, g_battChg);
    Serial.printf("[PMU] AXP2101 ok, battery=%d%% %s\r\n", g_battPct, g_battChg ? "(charging)" : "");
  } else {
    Serial.println("[PMU] AXP2101 未找到（屏幕仍可用）");
  }

  if (!gfx->begin(GVM_LCD_HZ)) {
    Serial.println("[LCD] gfx->begin() failed");
  }
  bus->writeC8D8(0x36, LCD_MADCTL);
  gfx->setBrightness(BRIGHTNESS_DEF);

  if (!canvas.begin()) {
    Serial.println("[MEM] PSRAM 帧缓冲分配失败，请确认板子 PSRAM=OPI 且已启用");
    gfx->fillScreen(RGB565_BLACK);
    gfx->setCursor(30, 200);
    gfx->setTextSize(2);
    gfx->setTextColor(COL_RED, COL_RED);
    gfx->println("PSRAM ALLOC FAILED");
    while (true) delay(200);
  }
  bufA = canvas.buffer();
  /* 单缓冲：GFX 的 draw16bitRGBBitmap 是同步的（送完才返回），推屏期间不会绘制，
   * 不需要第二块。双缓冲反而让"局部刷新"的页面在两块缓冲之间来回跳 → 闪屏/残影。 */
  bufB = nullptr;

#ifndef GVM_PUSH_GFX
  panelAsyncBegin();                       /* 在 GFX 初始化好的 SPI2 上挂异步设备（GFX 推屏模式下不需要） */
#endif
  g_pushQ    = xQueueCreate(2, sizeof(GFrameJob));
  g_pushDone = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(pushTask, "push", 4096, nullptr, 3, nullptr, 1);

  pinMode(PIN_KEY3, INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);

  bootScreen("正在启动传感器…", 0.05f);

  if (!gm.begin()) {
    bootScreen("未找到 QMI8658", 0.2f);
    Serial.println("[IMU] QMI8658 未找到");
  }

  if (!mic.begin()) {
    Serial.println("[MIC] 音频不可用，宠物表情仅由 G 值驱动");
  }

  /* 上电默认待机：IMU 任务创建后立刻挂起，麦克风同理 */
  xTaskCreatePinnedToCore(imuTask, "imu", 3072, nullptr, 2, &g_imuTask, 0);
  gm.setActive(false);
  mic.setActive(false);
  pet.setStandby(true);
  { Preferences p; p.begin("gmeter", true); g_hideG = p.getBool("hideg", false); p.end(); }
  pet.setHideG(g_hideG);

  bootScreen("就绪 · 长按 BOOT 开测", 1.0f);
  delay(300);

  pet.reset();
  pro.reset();
  gg.reset();
  setNotice("侧面键短按切换界面", 2000);
  Serial.println("[BOOT] G-Meter ready (待机). 输入 help 查看串口命令");
  Serial.println("[BOOT] BOOT长按1.5s=开测/停测 BOOT短按=宠物页隐藏/显示G值(其他页=重启水平零点) | 侧面键短按=翻页 双击=清峰值 长按1.2s=取零点");
}

/* ------------------------------ loop ------------------------------ */
void loop() {
  uint32_t now = millis();

  updateBootKey(now);
  static uint32_t lastPwr = 0;
  if (now - lastPwr >= 20) { lastPwr = now; pollPwrKey(); }
  updateButtons(now);
  handleSerial();

  static uint32_t lastFrame = 0, fpsT = 0;
  static uint16_t frameCnt = 0;

  /* 屏体纯色自检：5 色循环，用来判断"面板/背光是否活着" */
  if (g_lcdTestUntil && now < g_lcdTestUntil) {
    static const uint16_t kCol[5] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
    uint32_t el = now - g_lcdTestStart;
    if (bufB) canvas.setBuffer(canvas.buffer() == bufA ? bufB : bufA);
    canvas.fillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, kCol[(el / 600) % 5]);
    waitPushIdle();
    pushFrameAsync(canvas.buffer());
    delay(10);
    return;
  }

  uint32_t frameMs = g_measuring ? UI_FRAME_MS : UI_FRAME_MS_STANDBY;
  if (now - lastFrame >= frameMs) {
    float dt = (now - lastFrame) * 0.001f;
    lastFrame = now;

    GSnapshot g;
    memset(&g, 0, sizeof(g));
    gm.snapshot(g);
    AudioSnapshot a;
    memset(&a, 0, sizeof(a));
    if (g_spec || (g_mode == 0 && g_measuring)) mic.snapshot(a);
    if (!g_measuring) zeroSnapshot(g);

    /* 画到"不在推屏"的那块缓冲上，上一帧的 DMA 还在跑也没关系 */
    if (bufB) canvas.setBuffer(canvas.buffer() == bufA ? bufB : bufA);

    uint32_t tDraw = micros();
    g_perUs = tDraw - g_lastDraw;
    g_lastDraw = tDraw;
    if (g_spec) {
      spectrumDraw(&canvas, a.bands, a.bandPk, now * 0.001f, a.level);
    } else if (g_mode == 0) {
      pet.update(g, a, dt);
      pet.draw(&canvas, g, a);
    } else if (g_mode == 1) {
      pro.update(g, dt);
      pro.draw(&canvas, g);
    } else {
      gg.update(g, dt);
      gg.draw(&canvas, g);
    }

    if (!g_measuring && g_mode != 0 && !g_spec) uiStandbyHint(&canvas, now * 0.001f);
    /* 提示框消失后必须重画它压过的地方：否则局部刷新不会去擦，会一直"留"在屏上 */
    static bool noticeWas = false;
    bool noticeNow = (g_notice[0] && now <= g_noticeUntil);
    if (noticeWas && !noticeNow) {
      if (g_mode == 1 && !g_spec) pro.requestFull();
    }
    noticeWas = noticeNow;
    /* 校准浮层（底部黄色进度框）消失后必须整页重画，否则会留在屏幕上 */
    static bool calPrev = false;
    if (calPrev && !g.calibrating) pro.requestFull();
    calPrev = g.calibrating;
    drawCalOverlay(&canvas, g);
    drawNotice(&canvas, now);
    uint32_t tPush = micros();
    g_drawUs = tPush - tDraw;

    /* 走 GFX 官方推屏路径（面板只认这条；我们自建的 DMA 路径会把面板推成黑屏） */
#ifdef GVM_PUSH_GFX
    uint32_t tQueue = micros();
    g_waitUs = tQueue - tPush;
    pushDirty(canvas.buffer());
    g_dmaUs = micros() - tQueue;
#else
    waitPushIdle();
    uint32_t tQueue = micros();
    g_waitUs = tQueue - tPush;
    pushFrameAsync(canvas.buffer());
    g_dmaUs = panelLastTransferUs();
#endif

    frameCnt++;
    if (now - fpsT >= 1000) {
      g_fps = frameCnt;
      frameCnt = 0;
      fpsT = now;
    }
  }

  /* 每秒读一次电量（I2C 只读两个寄存器，不影响帧率） */
  static uint32_t lastBat = 0;
  if (g_pmuOk && now - lastBat >= 1000) {
    lastBat = now;
    g_battPct = power.getBatteryPercent();
    g_battChg = power.isCharging();
    pro.setBattery(g_battPct, g_battChg);
  }

  /* 每秒打印一次运行状态（串口监视器可直接看 G 值、帧率与耗时） */
  static uint32_t lastLog = 0;
  if (now - lastLog >= 1000) {
    lastLog = now;
    printInfo();
  }

  delay(1);
}
