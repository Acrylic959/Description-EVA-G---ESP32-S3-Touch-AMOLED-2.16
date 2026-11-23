#pragma once

// ============================================================
// 微雪 ESP32-S3-Touch-AMOLED-2.16 引脚定义
// 来源: https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.16
// ============================================================

// ---- 显示屏 (2.16" AMOLED 480x480, CO5300, QSPI) ----
#define LCD_WIDTH  480
#define LCD_HEIGHT 480
#define LCD_SCLK   38   // QSPI 时钟
#define LCD_SDIO0  4    // QSPI 数据0
#define LCD_SDIO1  5    // QSPI 数据1
#define LCD_SDIO2  6    // QSPI 数据2
#define LCD_SDIO3  7    // QSPI 数据3
#define LCD_CS     12   // QSPI 片选
#define LCD_RESET  39   // 屏幕复位

// ---- 板载共享 I2C (触摸 / IMU / PMU / RTC / 音频 Codec 共用) ----
#define IIC_SDA    15
#define IIC_SCL    14

// ---- 触摸 (CST9220) ----
#define TP_INT     11   // 触摸中断
#define TP_RST     40   // 触摸复位

// ---- IMU (QMI8658) ----
#define QMI_INT1   17
#define QMI_INT2   21

// ---- 麦克风 ADC (ES7210, I2S) ----
#define PIN_ES7210_MCLK 42
#define PIN_ES7210_BCLK 9
#define PIN_ES7210_LRCK 45
#define PIN_ES7210_DIN  10  // ES7210 ASDOUT -> ESP32 数据输入

// ---- 用户按键 (按下接地, 上拉) ----
#define KEY3_PIN   18
