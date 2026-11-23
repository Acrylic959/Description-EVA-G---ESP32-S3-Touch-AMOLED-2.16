#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "config.h"

// ============================================================
// 显示模块 - 2.16" AMOLED 480x480, CO5300 QSPI, Canvas 双缓冲
// ------------------------------------------------------------
// UI 先绘制到 PSRAM 中的 Canvas, 再一次性 flush 到屏幕,
// 避免 G 曲线与宠物动画刷新时的撕裂/闪烁。
// ============================================================

extern Arduino_CO5300 *gDisplay;   // 物理屏
extern Arduino_Canvas  *gCanvas;   // 内存帧缓冲 (480x480x2 ≈ 460KB, PSRAM)

// 初始化显示 + Canvas, 返回是否成功
bool displayBegin();

// 将 Canvas 内容刷到屏幕
void displayFlush();

// 设置亮度 (0~255)
void displaySetBrightness(uint8_t level);
