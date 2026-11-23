#include "audio.h"
#include "pin_config.h"
#include "config.h"
#include <Wire.h>
#include <driver/i2s.h>
#include <esp_idf_version.h>
#include "es7210.h"

// ============================================================
// ES7210 麦克风采集 + RMS 音量检测
// 基于微雪官方 06_ES7210 示例 (传统 I2S API, 适配 Arduino core 2.x)
// ============================================================

#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_FRAME_LEN     320                 // 20ms @16K
#define I2S_CH              I2S_NUM_1

static int16_t *s_buf = nullptr;
static float   s_level = 0.0f;      // 平滑音量
static float   s_peak  = 0.0f;      // 峰值(用于动态归一)
static bool    s_beat  = false;
static unsigned long s_beatHoldMs = 0;

bool audioBegin() {
  s_buf = (int16_t *)malloc(AUDIO_FRAME_LEN * sizeof(int16_t));
  if (!s_buf) {
    Serial.println("[AUDIO] malloc failed!");
    return false;
  }

  audio_hal_codec_config_t cfg = {
    .adc_input   = AUDIO_HAL_ADC_INPUT_ALL,
    .codec_mode  = AUDIO_HAL_CODEC_MODE_ENCODE,
    .i2s_iface = {
      .mode    = AUDIO_HAL_MODE_SLAVE,
      .fmt     = AUDIO_HAL_I2S_NORMAL,
      .samples = AUDIO_HAL_16K_SAMPLES,
      .bits    = AUDIO_HAL_BIT_LENGTH_16BITS,
    },
  };

  esp_err_t ret = ESP_OK;
  ret |= es7210_adc_init(&Wire, &cfg);
  ret |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
  // MIC1/2 低增益, MIC3/4 高增益 (与官方示例一致)
  ret |= es7210_adc_set_gain(
    (es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2),
    (es7210_gain_value_t)GAIN_0DB);
  ret |= es7210_adc_set_gain(
    (es7210_input_mics_t)(ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4),
    (es7210_gain_value_t)GAIN_37_5DB);
  ret |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);
  if (ret != ESP_OK) {
    Serial.printf("[AUDIO] ES7210 init failed: 0x%x\n", ret);
    return false;
  }

  i2s_config_t i2s_cfg = {};
  i2s_cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  i2s_cfg.sample_rate          = AUDIO_SAMPLE_RATE;
  i2s_cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  i2s_cfg.channel_format       = I2S_CHANNEL_FMT_ALL_LEFT;
  i2s_cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  i2s_cfg.dma_buf_count        = 8;
  i2s_cfg.dma_buf_len          = 64;
  i2s_cfg.use_apll             = false;
  i2s_cfg.tx_desc_auto_clear   = true;
  i2s_cfg.fixed_mclk           = 0;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  // ESP-IDF 5.x (Arduino-ESP32 3.x) 才有 these 字段
  i2s_cfg.mclk_multiple        = I2S_MCLK_MULTIPLE_256;
  i2s_cfg.bits_per_chan        = I2S_BITS_PER_CHAN_16BIT;
  i2s_cfg.chan_mask            = (i2s_channel_t)(I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1);
#endif
  i2s_driver_install(I2S_CH, &i2s_cfg, 0, NULL);

  i2s_pin_config_t pin_cfg = { 0 };
  pin_cfg.bck_io_num  = PIN_ES7210_BCLK;
  pin_cfg.ws_io_num   = PIN_ES7210_LRCK;
  pin_cfg.data_in_num = PIN_ES7210_DIN;
  pin_cfg.mck_io_num  = PIN_ES7210_MCLK;
  i2s_set_pin(I2S_CH, &pin_cfg);
  i2s_zero_dma_buffer(I2S_CH);

  Serial.println("[AUDIO] ES7210 + I2S OK");
  return true;
}

float audioSample() {
  size_t bytesRead = 0;
  i2s_read(I2S_CH, (char *)s_buf, AUDIO_FRAME_LEN * sizeof(int16_t),
           &bytesRead, portMAX_DELAY);
  int n = bytesRead / sizeof(int16_t);
  if (n <= 0) return s_level;

  // RMS 音量
  double sum = 0;
  for (int i = 0; i < n; i++) {
    double v = (double)s_buf[i] / 32768.0;
    sum += v * v;
  }
  float rms = sqrtf(sum / n);

  // 慢速跟踪峰值作为动态归一化基准 (避免环境差异)
  s_peak = s_peak * 0.995f + rms * 0.005f;
  if (s_peak < 0.001f) s_peak = 0.001f;
  float norm = constrain(rms / (s_peak * 2.0f), 0.0f, 1.0f);

  // 节拍检测: 瞬时音量明显高于平滑值
  float rise = norm - s_level;
  if (rise > 0.18f && norm > 0.25f && millis() > s_beatHoldMs) {
    s_beat = true;
    s_beatHoldMs = millis() + 180;   // 节拍最小间隔
  }

  // 平滑音量
  s_level += 0.25f * (norm - s_level);
  return s_level;
}

float audioLevel() { return s_level; }

bool audioBeat() {
  bool b = s_beat;
  s_beat = false;
  return b;
}
