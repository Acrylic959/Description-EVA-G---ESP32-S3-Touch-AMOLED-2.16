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
#define SP_N     1024    /* 64ms 窗：512 点时频带能量抖得厉害，底噪也会把柱子顶起来 */
#define SP_FS    16000.f

static float s_re[SP_N], s_im[SP_N], s_smp[SP_N];
static float s_bPow[AUDIO_BANDS];   /* 频带功率的帧间平滑，压掉随机的单帧跳动 */
static int   s_fill = 0;

static volatile float s_bands[AUDIO_BANDS];
static volatile float s_pk[AUDIO_BANDS];
static volatile bool  s_specOn = false;   /* 只在频谱页打开时才算，省 CPU */

/* 频段边界查表：只在第一次算频谱时跑一遍 powf，省掉每帧 40 次 powf */
static int  s_k0[AUDIO_BANDS], s_k1[AUDIO_BANDS];
static bool s_tab = false;
static float s_agc = 0.f;                 /* 自动增益跟踪（当前最响那一段） */
static volatile float s_noise = 0.008f;   /* 环境底噪（时域） */
/* 按 dB 域做工况判断：家用车实测车内噪音 怠速 35~45dB、60km/h 55~61、
 * 80km/h 58~64、120km/h 63~71。音乐要比这个底噪高 10~15dB 才听得出，
 * 所以频谱/听歌都用"高出底噪多少 dB"（SNR）来判决，而不是绝对幅值。 */
static float s_nFloorDb    = -85.f;   /* 全带频谱底噪（dB） */
static float s_midFloorDb  = -85.f;   /* 250~3500Hz（音乐主要能量）底噪（dB） */
/* 底噪用"滚动窗口最小值"估计：窗口内最安静的那一帧就是环境底噪。
 * 比"慢慢往上跟"稳得多——放歌/说话不会把底噪抬高，只有真的变吵才会跟着变。 */
#define NF_SPEC_WIN 64     /* 频谱 ~15 帧/s → 约 4 秒 */
#define NF_MID_WIN  160    /* 时域 ~15 次/s → 约 10 秒 */
static float s_nRing[NF_SPEC_WIN];  static int s_nIdx = 0, s_nFill = 0;
/* 每个频段单独一条底噪：安静时各段都贴着 0，不会因为"某一段偏高"就整排闪 */
static float s_bFloor[AUDIO_BANDS]; static int s_bFill = 0, s_bIdx = 0;
static float s_bRing[AUDIO_BANDS][NF_SPEC_WIN];
static float s_midRing[NF_MID_WIN]; static int s_midIdx = 0, s_midFill = 0, s_midTick = 0;


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

  float v0[AUDIO_BANDS];       /* 门限之后的 0..1（还没乘增益） */
  float vmax = 0.f, dbmax = -999.f, dbSum = 0.f;
  for (int b = 0; b < AUDIO_BANDS; b++) {
    int k0 = s_k0[b], k1 = s_k1[b];
    float sum = 0.f;
    for (int k = k0; k <= k1; k++) sum += s_re[k] * s_re[k] + s_im[k] * s_im[k];
    /* 先在功率域做帧间平滑（跨 2~3 帧），再转 dB：
     * 短窗 FFT 的单帧波动能有十几 dB，不平滑的话安静时也会乱闪 */
    float pw = sum / (float)(k1 - k0 + 1) / (float)(SP_N * SP_N);
    s_bPow[b] = s_bPow[b] * 0.62f + pw * 0.38f;
    float db = 10.f * log10f(s_bPow[b] + 1e-14f);   /* 约 -140..0 dB */
    if (db > dbmax) dbmax = db;
    dbSum += db;
    v0[b] = db;
    (void)vmax;
  }

  /* 底噪 = 最近 4 秒里最安静的一帧；忽然变低就立刻跟下去 */
  float avgDb = dbSum / (float)AUDIO_BANDS;
  s_nRing[s_nIdx] = avgDb;
  s_nIdx = (s_nIdx + 1) % NF_SPEC_WIN;
  if (s_nFill < NF_SPEC_WIN) s_nFill++;
  float nfloor = s_nRing[0];
  for (int i = 1; i < s_nFill; i++) if (s_nRing[i] < nfloor) nfloor = s_nRing[i];
  s_nFloorDb = (nfloor < s_nFloorDb) ? nfloor : s_nFloorDb + (nfloor - s_nFloorDb) * 0.30f;

  /* 逐频段底噪：每段记最近 4 秒最安静的一帧，段与段互不影响 */
  if (s_bFill == 0) {
    for (int b = 0; b < AUDIO_BANDS; b++) {
      s_bFloor[b] = v0[b];
      for (int i = 0; i < NF_SPEC_WIN; i++) s_bRing[b][i] = v0[b];
    }
    s_bFill = 1;
  } else {
    for (int b = 0; b < AUDIO_BANDS; b++) {
      s_bRing[b][s_bIdx] = v0[b];
      float bf = s_bRing[b][0];
      for (int i = 1; i < NF_SPEC_WIN; i++) if (s_bRing[b][i] < bf) bf = s_bRing[b][i];
      s_bFloor[b] = (bf < s_bFloor[b]) ? bf : s_bFloor[b] + (bf - s_bFloor[b]) * 0.30f;
    }
    s_bIdx = (s_bIdx + 1) % NF_SPEC_WIN;
  }

  /* 逐段 SNR（相对本段底噪），并找最响的一段 */
  float snrB[AUDIO_BANDS];
  float maxSnr = -999.f;
  for (int b = 0; b < AUDIO_BANDS; b++) {
    snrB[b] = v0[b] - s_bFloor[b];
    if (snrB[b] > maxSnr) maxSnr = snrB[b];
  }

  /* "满格"对应的 SNR 自适应：跟住最近的音乐响度（抬得快、落得慢）。
   * 关键点：满格按 1.18 倍留余量，最响的那段也只到 ~0.85，
   * 一般车内音量（70~85dB 播放）不会一路顶到上限。 */
  static float s_ceil = 22.f;
  if (maxSnr > s_ceil) s_ceil += (maxSnr - s_ceil) * 0.06f;
  else                 s_ceil += (maxSnr - s_ceil) * 0.004f;
  if (s_ceil < 18.f) s_ceil = 18.f;
  if (s_ceil > 46.f) s_ceil = 46.f;

  const float startSnr = 8.f;                    /* 高出本段底噪 8dB 才起步 */
  float denom = (s_ceil - startSnr) * 1.18f;
  if (denom < 8.f) denom = 8.f;

  for (int b = 0; b < AUDIO_BANDS; b++) {
    float v = (snrB[b] - startSnr) * (1.f / denom);
    if (v < 0.08f) v = 0.f;                      /* 死区：底噪抖动不出柱 */
    if (v > 1.f) v = 1.f;
    float old = s_bands[b];
    /* 起音快、回落更慢：柱形平滑，不会一抖一抖 */
    s_bands[b] = (v > old) ? (old + (v - old) * 0.45f) : (old + (v - old) * 0.075f);
    float pk = s_pk[b];
    if (v > pk) s_pk[b] = v;
    else { pk -= 0.012f; s_pk[b] = pk > 0.f ? pk : 0.f; }
  }

  /* 调试：每 30 帧（约 1 秒）打一行，方便对着串口调灵敏度 */
  static int dbg = 0;
  if (++dbg >= 30) {
    dbg = 0;
    float barSum = 0.f; for (int b = 0; b < AUDIO_BANDS; b++) barSum += s_bands[b];
    Serial.printf("[SPEC] dbmax=%.1f b0floor=%.1f mid=%.1f ceil=%.1f bar=%.2f\r\n",
                  dbmax, s_bFloor[0], s_midFloorDb, s_ceil, barSum);
  }
}

void MicAudio::setSpectrum(bool on) {
  s_specOn = on;
  s_fill = 0;
  for (int b = 0; b < AUDIO_BANDS; b++) s_bPow[b] = 0.f;
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

    /* 是否"在听歌"：音乐频段（250~3500Hz）要比底噪高 12dB 以上——
     * 车里胎噪/风噪 60~70dB 集中在低频，这个门限能把发动机/路噪挡掉 */
    float midDb = 20.f * log10f(rms + 1e-7f);
    /* 每 8 次采样记一帧（约 15 帧/s），底噪取最近 10 秒的最小值：
     * 放歌/说话时最小值仍然是环境底噪，不会被抬上去 */
    if (++s_midTick >= 8) {
      s_midTick = 0;
      s_midRing[s_midIdx] = midDb;
      s_midIdx = (s_midIdx + 1) % NF_MID_WIN;
      if (s_midFill < NF_MID_WIN) s_midFill++;
      float mfloor = s_midRing[0];
      for (int i = 1; i < s_midFill; i++) if (s_midRing[i] < mfloor) mfloor = s_midRing[i];
      s_midFloorDb = (mfloor < s_midFloorDb) ? mfloor : s_midFloorDb + (mfloor - s_midFloorDb) * 0.30f;
    }
    float snr = midDb - s_midFloorDb;              /* 高出底噪多少 dB */

    if (snr > 12.f) { musicOnMs += 8; musicOffMs = 0; }
    else            { musicOffMs += 8; musicOnMs = 0; }
    if (musicOnMs > 400 && !_music) {
      _music = true;
      Serial.printf("[MUSIC] on  snr=%.1f mid=%.1f floor=%.1f\r\n", snr, midDb, s_midFloorDb);
    }
    if (musicOffMs > 900 && _music) {
      _music = false;
      Serial.printf("[MUSIC] off snr=%.1f mid=%.1f floor=%.1f\r\n", snr, midDb, s_midFloorDb);
    }

    /* 嘴型：把 SNR 映射到 0..1（6dB 以下不动，22dB 以上全开），再减掉底噪抖动 */
    static uint32_t dbgT = 0;
    if (now - dbgT > 1000) {
      dbgT = now;
      Serial.printf("[MIC] mid=%.1f floor=%.1f snr=%.1f\r\n", midDb, s_midFloorDb, snr);
    }

    float lv = (snr - 6.f) * (1.f / 16.f);
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
