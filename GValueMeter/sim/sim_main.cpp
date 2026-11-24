/* ===========================================================================
 * sim/sim_main.cpp — 桌面模拟器
 *
 * 作用：在没有板子的情况下，把固件里真正跑在设备上的那部分代码
 *       （gmeter.cpp 的信号链、pet_mode.cpp 的表情状态机、pro_mode.cpp 的
 *        仪表盘、hud.h 的绘图原语、canvas_gfx.h 的帧缓冲）跑起来，
 *       用合成运动数据当作 QMI8658 的输入，把每一帧渲染成 PNG。
 *
 * 它验证的是：能不能编译、逻辑对不对、版式有没有重叠/越界。
 * 它不能验证：QSPI 屏时序、I2C 器件、麦克风、实时性能。
 * =========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zlib.h>

#include "Arduino.h"
#include "Wire.h"
#include "Arduino_GFX_Library.h"
#include "canvas_gfx.h"
#include "hud.h"
#include "gmeter.h"
#include "mic_audio.h"
#include "pet_mode.h"
#include "spectrum.h"
#include "pro_mode.h"
#include "gg_mode.h"
#include "ui_screen.h"

/* ----------------------------- 基础替身实现 ----------------------------- */
static uint32_t g_ms = 0;
uint32_t millis() { return g_ms; }
void simAdvance(uint32_t ms) { g_ms += ms; }
void delay(uint32_t ms) { g_ms += ms; }

SimSerial Serial;
TwoWire Wire;

void SimSerial::printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

static float g_ax = 0.f, g_ay = 1.f, g_az = 0.f;
void simSetAccel(float x, float y, float z) { g_ax = x; g_ay = y; g_az = z; }
void simGetAccel(float &x, float &y, float &z) { x = g_ax; y = g_ay; z = g_az; }

/* qmi8658_min 的替身：真机走 I2C，这里直接喂合成加速度（单位 g） */
bool qmiBegin(uint8_t, uint8_t) { return true; }
bool qmiDataReady() { return true; }
bool qmiReadAccelMG(float &x, float &y, float &z) { x = g_ax * 1000.f; y = g_ay * 1000.f; z = g_az * 1000.f; return true; }
uint8_t qmiAddr() { return 0x6B; }

static uint8_t g_pinMode[64];
void pinMode(uint8_t pin, uint8_t mode) { g_pinMode[pin & 63] = mode; }
int digitalRead(uint8_t pin) { (void)pin; return HIGH; }

static uint32_t g_seed = 12345;
static uint32_t lcg() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }
long random(long max) { return max <= 0 ? 0 : (long)(lcg() % (uint32_t)max); }
long random(long min, long max) { return max <= min ? min : min + (long)(lcg() % (uint32_t)(max - min)); }

/* ----------------------------- PNG 输出 ----------------------------- */
static void pngChunk(FILE *f, const char *type, const unsigned char *data, uint32_t len) {
  unsigned char hdr[4] = {(unsigned char)(len >> 24), (unsigned char)(len >> 16),
                          (unsigned char)(len >> 8), (unsigned char)len};
  fwrite(hdr, 1, 4, f);
  fwrite(type, 1, 4, f);
  if (len) fwrite(data, 1, len, f);
  uint32_t crc = crc32(0L, (const Bytef *)type, 4);
  if (len) crc = crc32(crc, (const Bytef *)data, len);
  unsigned char out[4] = {(unsigned char)(crc >> 24), (unsigned char)(crc >> 16),
                          (unsigned char)(crc >> 8), (unsigned char)crc};
  fwrite(out, 1, 4, f);
}

static bool savePng(const char *path, const uint16_t *fb, int w, int h) {
  unsigned char *raw = (unsigned char *)malloc((size_t)(w * 3 + 1) * h);
  if (!raw) return false;
  for (int y = 0; y < h; y++) {
    unsigned char *row = raw + (size_t)y * (w * 3 + 1);
    row[0] = 0;
    for (int x = 0; x < w; x++) {
      uint16_t p = fb[(size_t)y * w + x];
      uint8_t r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
      uint8_t g = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
      uint8_t b = (uint8_t)((p & 0x1F) * 255 / 31);
      row[1 + x * 3 + 0] = r;
      row[1 + x * 3 + 1] = g;
      row[1 + x * 3 + 2] = b;
    }
  }
  size_t rawLen = (size_t)(w * 3 + 1) * h;
  size_t compLen = rawLen + 65536;
  unsigned char *comp = (unsigned char *)malloc(compLen);
  if (!comp) { free(raw); return false; }
  compress2(comp, (uLongf *)&compLen, raw, (uLong)rawLen, 6);

  FILE *f = fopen(path, "wb");
  if (!f) { free(raw); free(comp); return false; }
  const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  fwrite(sig, 1, 8, f);
  unsigned char ihdr[13] = {(unsigned char)(w >> 24), (unsigned char)(w >> 16),
                            (unsigned char)(w >> 8), (unsigned char)w,
                            (unsigned char)(h >> 24), (unsigned char)(h >> 16),
                            (unsigned char)(h >> 8), (unsigned char)h,
                            8, 2, 0, 0, 0};
  pngChunk(f, "IHDR", ihdr, 13);
  pngChunk(f, "IDAT", comp, (uint32_t)compLen);
  pngChunk(f, "IEND", nullptr, 0);
  fclose(f);
  free(raw);
  free(comp);
  printf("  -> %s\n", path);
  return true;
}

/* ----------------------------- 合成运动数据 ----------------------------- */
/* 轴向约定（与固件默认映射一致）：X = 侧向 lat，Y = 竖直 vert，Z = 纵向 lon */
static double env(double t, double t0, double t1, double r) {
  if (t < t0 || t > t1) return 0.0;
  if (t < t0 + r) return (t - t0) / r;
  if (t > t1 - r) return (t1 - t) / r;
  return 1.0;
}

static void motion(double t, float &ax, float &ay, float &az) {
  double lat = 0.0, lon = 0.0, vert = 0.0;

  lon += 0.38 * env(t, 2.6, 4.1, 0.30);        /* 起步加速 */
  lon -= 0.33 * env(t, 5.0, 6.4, 0.22);        /* 刹车 */
  /* 减速带：3Hz 上下颠两下，振幅 0.55g */
  if (t > 7.0 && t < 7.55) vert += 0.55 * sin(2.0 * M_PI * 3.0 * (t - 7.0));
  lat += 0.30 * env(t, 9.0, 10.3, 0.28);       /* 右弯 */
  lat -= 0.30 * env(t, 11.0, 12.3, 0.28);      /* 左弯 */

  /* 17s 之后进入"专业模式"的综合驾驶段 */
  lon += 0.26 * sin(2.0 * M_PI * 0.35 * (t - 17.0)) * env(t, 17.0, 26.0, 0.5);
  lat += 0.18 * sin(2.0 * M_PI * 0.22 * (t - 17.0)) * env(t, 17.0, 26.0, 0.5);

  /* 高 G 事件：一脚重刹 + 侧向，合成 > 0.8g，触发报警 */
  lon -= 0.72 * env(t, 18.3, 19.5, 0.18);
  lat += 0.48 * env(t, 18.5, 19.6, 0.18);

  /* 15.2~16.4s：宠物页的重刹 + 侧向，脸被推到边缘，检查五官间距压缩 */
  lon -= 0.55 * env(t, 15.2, 16.4, 0.14);
  lat -= 0.42 * env(t, 15.2, 16.4, 0.14);

  /* 26s 之后：G-G 圆盘页专用段——右转 → 重刹 → 加速 → 大 G 对角 */
  lat += 0.55 * env(t, 27.4, 28.4, 0.20);      /* 右转：点向右 */
  lon -= 0.62 * env(t, 28.9, 29.9, 0.16);      /* 重刹：点向上 */
  lat += 0.40 * env(t, 28.6, 29.9, 0.14);      /* 同时带一点右转 → 点走右上对角 */
  lon += 0.58 * env(t, 30.3, 31.2, 0.18);      /* 加速：点向下 */
  lat -= 0.62 * env(t, 31.8, 32.6, 0.16);      /* 左转 */
  lat -= 0.72 * env(t, 32.7, 33.7, 0.10);      /* 左转 + 重刹同时到位，合成 ≈0.91g */
  lon -= 0.55 * env(t, 32.7, 33.7, 0.10);

  /* 传感器噪声 */
  float n1 = ((float)(lcg() % 1000) / 1000.f - 0.5f) * 0.012f;
  float n2 = ((float)(lcg() % 1000) / 1000.f - 0.5f) * 0.012f;
  float n3 = ((float)(lcg() % 1000) / 1000.f - 0.5f) * 0.012f;

  ax = (float)lat + n1;
  ay = 1.0f + (float)vert + n2;      /* 重力在竖直轴，零点校准会把它减掉 */
  az = (float)lon + n3;
}

/* 合成麦克风信号：12.6~16.6s 播放音乐，1.5Hz 呼吸 + 0.42s 一拍 */
static void audioAt(double t, AudioSnapshot &a) {
  a.ok = true;
  a.rms = 0.f;
  if (t > 12.6 && t < 16.6) {
    a.music = true;
    double beat = fmod(t - 12.6, 0.42);
    a.level = (float)(0.15 + 0.55 * fabs(sin(2.0 * M_PI * 1.5 * (t - 12.6))) + 0.25 * exp(-beat * 12.0));
    a.beatAge = (float)beat;
    a.beatSeq = (uint32_t)((t - 12.6) / 0.42);
  } else {
    a.music = false;
    a.level = 0.03f;
    a.beatAge = 9.f;
    a.beatSeq = 0;
  }
}

/* ----------------------------- 主流程 ----------------------------- */
struct Capture { double t; const char *name; int mode; };

static const Capture CAPS[] = {
    {2.30, "01-pet-idle", 0},
    {2.63, "01b-pet-blink", 0},
    {3.45, "02-pet-accel", 0},
    {5.85, "03-pet-brake", 0},
    {7.18, "04-pet-bump", 0},
    {9.70, "05-pet-turn", 0},
    {14.00, "06-pet-sing", 0},
    {15.95, "06b-pet-stress", 0},
    {17.80, "07-pro-normal", 1},
    {19.10, "08-pro-alarm", 1},
    {21.60, "09-pro-history", 1},
    {24.00, "10-pro-peak", 1},
    {27.90, "11-gg-turn", 2},
    {29.30, "12-gg-brake", 2},
    {30.80, "13-gg-accel", 2},
    {33.20, "14-gg-alarm", 2},
};
static const int NCAP = (int)(sizeof(CAPS) / sizeof(CAPS[0]));

int main(int argc, char **argv) {
  const char *outDir = (argc > 1) ? argv[1] : "sim/out";

  /* 显示端 + 画布（与固件里 gfx / canvas 的关系一致） */
  Arduino_GFX gfx(480, 480);
  gfx.begin();
  CanvasGFX canvas(480, 480);
  if (!canvas.begin()) {
    printf("[SIM] canvas alloc failed\n");
    return 1;
  }

  GMeter gm;
  PetMode pet;
  ProMode pro;
  GgMode  gg;
  gm.begin();
  pet.reset();
  pro.reset();
  gg.reset();

  /* ---- 启动屏 / 提示条 / 校准浮层：各出一张，专门检查中文排版 ---- */
  {
    GSnapshot g0;
    AudioSnapshot a0;
    g0.mag = 0.f; g0.peak = 0.f; g0.lat = g0.lon = g0.vert = 0.f;
    g0.curveCount = 0; g0.calibrating = true; g0.calProgress = 0.62f;
    g0.bumpSeq = 0; g0.vertP2P = 0.f;
    a0.music = false; a0.level = 0.f; a0.beatSeq = 0; a0.beatAge = 0.f;

    uiBoot(&canvas, "正在启动传感器…", 0.28f);
    canvasPresentAll(&gfx, &canvas);
    { char p[256]; snprintf(p, sizeof(p), "%s/00-boot.png", outDir); savePng(p, gfx.framebuffer(), 480, 480); }

    uiBoot(&canvas, "零点校准中，请保持水平静止", 0.62f);
    canvasPresentAll(&gfx, &canvas);
    { char p[256]; snprintf(p, sizeof(p), "%s/00-boot-cal.png", outDir); savePng(p, gfx.framebuffer(), 480, 480); }

    pet.reset();
    pet.draw(&canvas, g0, a0);
    uiNotice(&canvas, "宠物表情模式");
    canvasPresentAll(&gfx, &canvas);
    { char p[256]; snprintf(p, sizeof(p), "%s/00-notice.png", outDir); savePng(p, gfx.framebuffer(), 480, 480); }

    pet.draw(&canvas, g0, a0);
    uiNotice(&canvas, "左右滑动切换界面");
    canvasPresentAll(&gfx, &canvas);
    { char p[256]; snprintf(p, sizeof(p), "%s/00-swipe-hint.png", outDir); savePng(p, gfx.framebuffer(), 480, 480); }

    g0.mag = 0.42f; g0.peak = 0.42f;
    pet.draw(&canvas, g0, a0);
    uiCalOverlay(&canvas, 0.62f);
    canvasPresentAll(&gfx, &canvas);
    { char p[256]; snprintf(p, sizeof(p), "%s/00-cal-overlay.png", outDir); savePng(p, gfx.framebuffer(), 480, 480); }
    printf("[CAP] 启动屏 / 提示条 / 校准浮层 4 张\n");
  }

  /* ---- 待机态：宠物只播循环动画 + 专业页 / 圆盘页中央提示 ---- */
  {
    GSnapshot g0; AudioSnapshot a0;
    memset(&g0, 0, sizeof(g0)); memset(&a0, 0, sizeof(a0));
    g0.calProgress = 1.f;
    pet.setStandby(true);
    pet.draw(&canvas, g0, a0);
    canvasPresentAll(&gfx, &canvas);
    { char p[256]; snprintf(p, sizeof(p), "%s/00-standby-pet.png", outDir); savePng(p, gfx.framebuffer(), 480, 480); }
    pro.reset();
    pro.setBattery(82, false);
    pro.draw(&canvas, g0);
    uiStandbyHint(&canvas, 1.3f);
    canvasPresentAll(&gfx, &canvas);
    { char p[256]; snprintf(p, sizeof(p), "%s/00-standby-pro.png", outDir); savePng(p, gfx.framebuffer(), 480, 480); }
    pet.setHideG(true);
    pet.draw(&canvas, g0, a0);
    canvasPresentAll(&gfx, &canvas);
    { char p[256]; snprintf(p, sizeof(p), "%s/00-pet-nogv.png", outDir); savePng(p, gfx.framebuffer(), 480, 480); }
    pet.setHideG(false);
    pet.setStandby(false);
    printf("[CAP] 待机态 3 张\n");
  }

  /* 频谱柱：宠物页 + 合成谱线（低频高、高频削，模拟说话/放歌的形态） */
  {
    float bands[AUDIO_BANDS], pk[AUDIO_BANDS];
    for (int i = 0; i < AUDIO_BANDS; i++) {
      float f = (float)i / (float)(AUDIO_BANDS - 1);
      float env = 0.92f - 0.60f * f;
      float wob = 0.55f + 0.45f * sinf((float)i * 1.7f);
      bands[i] = env * wob;
      pk[i] = bands[i] + 0.10f;
    }
    spectrumDraw(&canvas, bands, pk, 1.0f, 0.55f);
    canvasPresentAll(&gfx, &canvas);
    char p[256]; snprintf(p, sizeof(p), "%s/20-spectrum.png", outDir);
    savePng(p, gfx.framebuffer(), 480, 480);
    printf("[CAP] 20-spectrum\n");
  }

  pro.setBattery(82, false);      /* 模拟器里固定一个电量，方便看排版 */

  int nextCap = 0;
  double nextRender = 0.0;
  double dt = 0.005;                 /* 200Hz，与 IMU_RATE_HZ 一致 */
  int frames = 0;
  int mode = 0;

  for (double t = 0.0; t < 35.0; t += dt) {
    simAdvance((uint32_t)(dt * 1000 + 0.5));

    float ax, ay, az;
    motion(t, ax, ay, az);
    simSetAccel(ax, ay, az);
    gm.taskStep();

    if (t >= nextRender) {
      nextRender += 1.0 / 30.0;

      GSnapshot g;
      gm.snapshot(g);
      AudioSnapshot a;
      audioAt(t, a);

      if (mode == 0) {
        pet.update(g, a, 1.f / 30.f);
        pet.draw(&canvas, g, a);
      } else if (mode == 1) {
        pro.update(g, 1.f / 30.f);
        pro.draw(&canvas, g);
      } else {
        gg.update(g, 1.f / 30.f);
        gg.draw(&canvas, g);
      }
      canvasPresentAll(&gfx, &canvas);
      frames++;

      /* 到点截图 */
      if (nextCap < NCAP && t >= CAPS[nextCap].t) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s.png", outDir, CAPS[nextCap].name);
        printf("[CAP] t=%.2fs %s | mode=%d mag=%.2f peak=%.2f lat=%.2f lon=%.2f vert=%.2f p2p=%.2f bump=%u cv=%d\n",
               t, CAPS[nextCap].name, mode, g.mag, g.peak, g.lat, g.lon, g.vert,
               g.vertP2P, (unsigned)g.bumpSeq, g.curveCount);
        savePng(path, gfx.framebuffer(), 480, 480);
        nextCap++;

        /* 第 7 张截图时切到专业模式 */
        if (nextCap < NCAP && CAPS[nextCap].mode != mode) {
          mode = CAPS[nextCap].mode;
          if (mode == 0) pet.reset();
          else if (mode == 1) pro.reset();
          else gg.reset();
          printf("[MODE] switch to %s\n", mode == 0 ? "PET" : (mode == 1 ? "PRO" : "GG"));
        }
      }
    }
  }

  printf("[SIM] done: %d frames rendered, %d captures\n", frames, nextCap);
  return 0;
}
