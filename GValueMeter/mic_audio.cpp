#include "mic_audio.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
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
static bool i2sInstall() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = MIC_SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;     // L=MIC1, R=MIC2
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 128;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
  cfg.chan_mask = (i2s_channel_t)(I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1);

  if (i2s_driver_install(I2S_NUM_1, &cfg, 0, nullptr) != ESP_OK) return false;

  i2s_pin_config_t pc = {};
  pc.mck_io_num = I2S_MCLK;
  pc.bck_io_num = I2S_BCLK;
  pc.ws_io_num = I2S_LRCK;
  pc.data_out_num = I2S_PIN_NO_CHANGE;
  pc.data_in_num = I2S_MIC_DIN;
  if (i2s_set_pin(I2S_NUM_1, &pc) != ESP_OK) return false;

  i2s_zero_dma_buffer(I2S_NUM_1);
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
  xTaskCreatePinnedToCore(micTaskEntry, "mic", 4096, this, 3, nullptr, 0);
  return true;
}

/* ------------------------ 采集 + 特征提取 ------------------------------ */
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
    if (i2s_read(I2S_NUM_1, buf, sizeof(buf), &got, pdMS_TO_TICKS(100)) != ESP_OK) continue;
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
    }
    float rms = sqrtf(sumSq / (float)n);

    /* 快/慢包络：快包络跟嘴型，慢包络当噪声/底噪参考 */
    if (rms > envFast) envFast += (rms - envFast) * 0.55f;
    else               envFast += (rms - envFast) * 0.06f;
    envSlow += (rms - envSlow) * 0.015f;

    uint32_t now = millis();

    /* 起音检测：快包络明显高于慢包络 → 一个节拍 */
    if (envFast > envSlow * 1.9f + 0.0015f && envFast > 0.004f &&
        (now - lastBeat) > 130) {
      lastBeat = now;
      _beatSeq = _beatSeq + 1;
      _beatMs = now;
    }

    /* 是否"在听歌/哼歌"：带通能量持续高于阈值（带迟滞） */
    if (envSlow > 0.0045f) { musicOnMs += 8; musicOffMs = 0; }
    else                  { musicOffMs += 8; musicOnMs = 0; }
    if (musicOnMs > 300) _music = true;
    if (musicOffMs > 700) _music = false;

    float lv = envFast * 7.0f;
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
}
