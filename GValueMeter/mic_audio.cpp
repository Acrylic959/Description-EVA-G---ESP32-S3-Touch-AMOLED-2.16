#include "mic_audio.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

#include "pin_config.h"

/* ----------------------------- ES7210 寄存器 ----------------------------- */
/* 精简版初始化：只做本项目需要的（从模式 / 16k / 16bit / I2S normal / MIC1+2）
 * 寄存器含义见 ES7210 datasheet；时序取自 Espressif es7210 驱动。 */
static bool esw(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static uint8_t esr(uint8_t reg) {
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  Wire.requestFrom((uint8_t)ES7210_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}

static bool es7210Init() {
  bool ok = true;
  ok &= esw(0x00, 0xFF);            // 软复位
  delay(5);
  ok &= esw(0x00, 0x41);
  ok &= esw(0x01, 0x1F);            // 全部 ADC 时钟先关
  ok &= esw(0x09, 0x30);            // chip state cycle
  ok &= esw(0x0A, 0x30);            // power on cycle
  ok &= esw(0x40, 0xC3);            // analog: vdda 3.3V
  ok &= esw(0x41, 0x70);            // MIC bias 2.87V
  ok &= esw(0x42, 0x70);
  ok &= esw(0x07, 0x20);            // OSR
  ok &= esw(0x02, 0xC1);            // ADC div / dll / doubler
  ok &= esw(0x04, 0x01);            // LRCK div H  (MCLK 4.096MHz -> 16kHz)
  ok &= esw(0x05, 0x00);            // LRCK div L
  ok &= esw(0x11, 0x60);            // 16bit / I2S normal
  ok &= esw(0x12, 0x00);            // ADC1/2 -> SDOUT1
  ok &= esw(0x4B, 0xFF);            // MIC1/2 bias/ADC 先关
  ok &= esw(0x4C, 0xFF);
  ok &= esw(0x43, (uint8_t)(0x10 | (MIC_GAIN_DB & 0x0F)));   // PGA 上电 + 增益
  ok &= esw(0x44, (uint8_t)(0x10 | (MIC_GAIN_DB & 0x0F)));
  ok &= esw(0x45, 0x00);
  ok &= esw(0x46, 0x00);
  ok &= esw(0x01, 0x14);            // MIC1/2 时钟开，MIC3/4 保持关
  ok &= esw(0x06, 0x00);            // 退出 power down
  ok &= esw(0x47, 0x00);            // MIC1/2 上电
  ok &= esw(0x48, 0x00);
  ok &= esw(0x4B, 0x00);            // MIC1/2 bias + ADC + PGA 上电
  delay(10);

  Serial.printf("[MIC] ES7210 REG02=0x%02X REG11=0x%02X REG01=0x%02X\r\n",
                esr(0x02), esr(0x11), esr(0x01));
  return ok;
}

/* ------------------------------- I2S ---------------------------------- */
/* ESP-IDF 5.x 新版 I2S 驱动（driver/i2s_std.h）。
 * ES7210 从模式：MCLK 4.096MHz = 16k × 256，I2S Philips 标准、16bit 立体声。 */
static i2s_chan_handle_t s_rx = nullptr;

static bool i2sInstall() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num  = 8;
  chan_cfg.dma_frame_num = 128;
  chan_cfg.auto_clear    = true;
  if (i2s_new_channel(&chan_cfg, nullptr, &s_rx) != ESP_OK) return false;

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  std_cfg.gpio_cfg.mclk = (gpio_num_t)I2S_MCLK;
  std_cfg.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK;
  std_cfg.gpio_cfg.ws   = (gpio_num_t)I2S_LRCK;
  std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.din  = (gpio_num_t)I2S_MIC_DIN;

  if (i2s_channel_init_std_mode(s_rx, &std_cfg) != ESP_OK) return false;
  if (i2s_channel_enable(s_rx) != ESP_OK) return false;
  return true;
}

/* --------------------------- 任务入口 ---------------------------------- */
static void micTaskEntry(void *arg) {
  static_cast<MicAudio *>(arg)->runTask();
}

bool MicAudio::begin() {
  if (!es7210Init()) {
    Serial.println("[MIC] ES7210 初始化失败（I2C 无应答），音频功能关闭");
    _ok = false;
    return false;
  }
  if (!i2sInstall()) {
    Serial.println("[MIC] I2S 安装失败，音频功能关闭");
    _ok = false;
    return false;
  }
  _ok = true;
  xTaskCreatePinnedToCore(micTaskEntry, "mic", 4096, this, 3, &_task, 0);
  return true;
}

void MicAudio::setActive(bool on) {
  if (!_task) return;
  if (on) {
    _level = 0.f; _rms = 0.f; _music = false;   /* 恢复时不让旧包络残留 */
    vTaskResume(_task);
  } else {
    vTaskSuspend(_task);
  }
}

/* ------------------------ 采集 + 特征提取 ------------------------------ */
/* ============================ 频谱：512 点 FFT ============================ */
#define SP_N     512
#define SP_FS    16000.f

static float s_re[SP_N], s_im[SP_N], s_smp[SP_N];
static int   s_fill = 0;

static volatile float s_bands[AUDIO_BANDS];
static volatile float s_pk[AUDIO_BANDS];
static volatile bool  s_specOn = false;   /* 只在频谱页打开时才算，省 CPU */

/* 频段边界查表：只在第一次算频谱时跑一遍 powf，省掉每帧 40 次 powf */
static int  s_k0[AUDIO_BANDS], s_k1[AUDIO_BANDS];
static bool s_tab = false;
static float s_agc = 0.f;                 /* 自动增益跟踪（当前最响那一段） */
static volatile float s_noise = 0.008f;   /* 环境底噪（时域），用来做音量门 */

static void buildBands() {
  const float binHz = SP_FS / (float)SP_N;
  for (int b = 0; b < AUDIO_BANDS; b++) {
    float f0 = 80.f * powf(5000.f / 80.f, (float)b / (float)AUDIO_BANDS);
    float f1 = 80.f * powf(5000.f / 80.f, (float)(b + 1) / (float)AUDIO_BANDS);
    int k0 = (int)(f0 / binHz), k1 = (int)(f1 / binHz);
    if (k0 < 1) k0 = 1;
    if (k1 <= k0) k1 = k0 + 1;
    if (k1 > SP_N / 2 - 1) k1 = SP_N / 2 - 1;
    s_k0[b] = k0;
    s_k1[b] = k1;
  }
  s_tab = true;
}

static void fftRadix2(float *re, float *im) {
  for (int i = 1, j = 0; i < SP_N; i++) {           /* 位反转 */
    int bit = SP_N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float tr = re[i], ti = im[i];
      re[i] = re[j]; im[i] = im[j];
      re[j] = tr;    im[j] = ti;
    }
  }
  for (int len = 2; len <= SP_N; len <<= 1) {
    float ang = -6.2831853f / (float)len;
    int half = len >> 1;
    for (int i = 0; i < SP_N; i += len) {
      for (int k = 0; k < half; k++) {
        float w = ang * (float)k;
        float wr = cosf(w), wi = sinf(w);
        int a = i + k, b = i + k + half;
        float xr = re[b] * wr - im[b] * wi;
        float xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr;  im[b] = im[a] - xi;
        re[a] += xr;         im[a] += xi;
      }
    }
  }
}

/* 一次频谱：Hann 窗 → FFT → 20 段对数能量 → dB 压缩 + 自动增益 → 平滑 */
static void computeSpectrum() {
  if (!s_tab) buildBands();
  for (int i = 0; i < SP_N; i++) {
    float w = 0.5f - 0.5f * cosf(6.2831853f * (float)i / (float)(SP_N - 1));
    s_re[i] = s_smp[i] * w;
    s_im[i] = 0.f;
  }
  fftRadix2(s_re, s_im);

  float raw[AUDIO_BANDS];
  float vmax = 0.f, dbmax = -999.f;
  for (int b = 0; b < AUDIO_BANDS; b++) {
    int k0 = s_k0[b], k1 = s_k1[b];
    float sum = 0.f;
    for (int k = k0; k <= k1; k++) sum += s_re[k] * s_re[k] + s_im[k] * s_im[k];
    float amp = sqrtf(sum / (float)(k1 - k0 + 1)) / (float)SP_N;
    /* 用 dB 而不是线性幅值来映射：小声也能看出柱形，不再"只有大声才动" */
    float db = 20.f * log10f(amp + 1e-7f);        /* 约 -140..0 dB */
    if (db > dbmax) dbmax = db;
    float v  = (db + 90.f) * (1.f / 45.f);        /* -90dB→0，-45dB→1（实测底噪约 -84dB） */
    if (v < 0.f) v = 0.f;
    raw[b] = v;
    if (v > vmax) vmax = v;
  }

  /* 自动增益：把当前最响那一段往满格拉，持续小声也能撑起柱形；
   * 只放大不衰减，静音（vmax 很小）时最多放大 4 倍，避免底噪全顶到天上 */
  if (vmax > s_agc) s_agc += (vmax - s_agc) * 0.50f;
  else              s_agc += (vmax - s_agc) * 0.05f;
  float gain = 1.f;
  if (s_agc > 0.22f) gain = 0.95f / s_agc;   /* 有真实声音才抬增益（底噪不放大） */
  if (gain < 1.f) gain = 1.f;
  if (gain > 4.5f) gain = 4.5f;

  /* 音量门（噪声门）：低于门限的全部按 0 处理，只有一定音量以上的声音才进柱子，
   * 这样底噪不会把柱子顶起来，柱形也更平滑 */
  float th = 0.22f;
  if (s_agc > 0.35f) th = s_agc * 0.38f;
  if (th < 0.20f) th = 0.20f;
  if (th > 0.52f) th = 0.52f;

  for (int b = 0; b < AUDIO_BANDS; b++) {
    float v = raw[b] * gain;
    v = (v - th) * (1.f / (1.f - th));      /* 门限以下 → 0；门限以上线性抬到满格 */
    if (v < 0.f) v = 0.f;
    if (v > 1.f) v = 1.f;
    float old = s_bands[b];
    /* 起音快、回落更慢：柱形平滑，不会一抖一抖 */
    s_bands[b] = (v > old) ? (old + (v - old) * 0.55f) : (old + (v - old) * 0.085f);
    float pk = s_pk[b];
    if (v > pk) s_pk[b] = v;
    else { pk -= 0.012f; s_pk[b] = pk > 0.f ? pk : 0.f; }
  }

  /* 调试：每 30 帧（约 1 秒）打一行，方便对着串口调灵敏度 */
  static int dbg = 0;
  if (++dbg >= 30) {
    dbg = 0;
    Serial.printf("[SPEC] dbmax=%.1f vmax=%.2f gain=%.2f agc=%.2f\r\n",
                  dbmax, vmax, gain, s_agc);
  }
}

void MicAudio::setSpectrum(bool on) {
  s_specOn = on;
  s_fill = 0;
  if (on) {                                  /* 重新开时清掉上一轮的柱形/峰值/增益 */
    for (int i = 0; i < AUDIO_BANDS; i++) { s_bands[i] = 0.f; s_pk[i] = 0.f; }
    s_agc = 0.f;
  }
}
bool MicAudio::spectrumOn() const { return s_specOn; }


void MicAudio::runTask() {
  const int FRAMES = 128;                 // 8ms @16k
  static int16_t buf[FRAMES * 2];

  const float fs = (float)MIC_SAMPLE_RATE;
  const float TAU = 6.2831853f;
  const float aHp  = 1.f - expf(-TAU * 40.f / fs);      // 40Hz 高通（去直流/低频轰鸣）
  const float aLow = 1.f - expf(-TAU * 250.f / fs);     // 250Hz 低截
  const float aHi  = 1.f - expf(-TAU * 3500.f / fs);    // 3.5kHz 高截

  float lp = 0.f, lpLow = 0.f, lpHi = 0.f;
  float envFast = 0.f, envSlow = 0.f;
  uint32_t lastBeat = 0;
  int musicOnMs = 0, musicOffMs = 0;

  while (true) {
    size_t got = 0;
    if (i2s_channel_read(s_rx, buf, sizeof(buf), &got, pdMS_TO_TICKS(100)) != ESP_OK) continue;
    int n = (int)(got / 4);               // 每帧 2×int16
    if (n <= 0) continue;

    float sumSq = 0.f;
    for (int i = 0; i < n; i++) {
      float s = (float)(buf[2 * i] + buf[2 * i + 1]) * 0.5f / 32768.0f;
      lp += (s - lp) * aHp;
      float hp = s - lp;
      lpLow += (hp - lpLow) * aLow;
      lpHi  += (hp - lpHi)  * aHi;
      float mid = lpHi - lpLow;           // 250..3500Hz 带通
      sumSq += mid * mid;
      if (s_specOn) {                     /* 频谱页才累积，避免白烧 CPU */
        s_smp[s_fill++] = hp;               /* 频谱用高通信号：保留 40Hz 起的低频段 */
        if (s_fill >= SP_N) { s_fill = 0; computeSpectrum(); }
      }
    }
    float rms = sqrtf(sumSq / (float)n);

    /* 快/慢包络：快包络跟嘴型，慢包络当噪声/底噪参考 */
    if (rms > envFast) envFast += (rms - envFast) * 0.55f;
    else               envFast += (rms - envFast) * 0.06f;
    envSlow += (rms - envSlow) * 0.015f;

    /* 底噪跟踪：往下立刻跟，往上极慢，得到一个"环境静音电平" */
    if (envSlow < s_noise) s_noise = envSlow;
    else                   s_noise += (envSlow - s_noise) * 0.0015f;

    uint32_t now = millis();

    /* 起音检测：快包络明显高于慢包络 → 一个节拍 */
    if (envFast > envSlow * 1.9f + 0.0015f && envFast > 0.004f &&
        (now - lastBeat) > 130) {
      lastBeat = now;
      _beatSeq = _beatSeq + 1;
      _beatMs = now;
    }

    /* 是否"在听歌/哼歌"：和频谱同一套音量门——必须明显高于环境底噪 */
    float gate = s_noise * 2.6f + 0.0025f;
    if (envSlow > gate) { musicOnMs += 8; musicOffMs = 0; }
    else                  { musicOffMs += 8; musicOnMs = 0; }
    if (musicOnMs > 300) _music = true;
    if (musicOffMs > 700) _music = false;

    float lv = (envFast - s_noise * 1.4f) * 9.0f;   /* 减掉底噪：安静时嘴不动 */
    if (lv < 0.f) lv = 0.f;
    if (lv > 1.f) lv = 1.f;
    _level = powf(lv, 0.62f);             // 更跟手的听感
    _rms = rms;
  }
}

/* ---------------------------------------------------------------------- */
void MicAudio::snapshot(AudioSnapshot &o) {
  o.level = _level;
  o.rms = _rms;
  o.music = _music;
  o.ok = _ok;
  o.beatSeq = _beatSeq;
  o.beatAge = (_beatMs == 0) ? 9.f : (millis() - _beatMs) * 0.001f;
  for (int i = 0; i < AUDIO_BANDS; i++) {
    o.bands[i]  = s_bands[i];
    o.bandPk[i] = s_pk[i];
  }
}
