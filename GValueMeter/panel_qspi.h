#pragma once
/* ===========================================================================
 *  panel_qspi.h — CO5300 QSPI 异步 DMA 推屏
 *
 *  Arduino_GFX 的 Arduino_ESP32QSPI::writePixels 是「轮询式」事务：
 *  每块 16KB 发送期间 CPU 原地自旋等待，实测 480×480 一帧要 77~88ms，
 *  且 CPU 在这段时间里什么都干不了。
 *
 *  这里在同一个 SPI2 主机上再挂一个 spi_master 设备（Arduino_GFX 仍负责
 *  上电初始化），用 spi_device_queue_trans 把像素块交给 DMA 后立刻返回，
 *  CPU 接着画下一帧（双 PSRAM 缓冲交替），推屏与绘制完全重叠。
 *  这是 GitHub 上 esp_lcd_co5300 / waveshare-watch-rs 等项目用的同一路数。
 * =========================================================================== */
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

/* Arduino_GFX 已经把 SPI2 主机初始化好之后调用；返回 false 表示挂设备失败 */
bool panelAsyncBegin();

/* 开窗：CS 拉低 → CASET / PASET / RAMWR，之后由 panelPush() 连续灌像素 */
void panelSetWindow(int16_t x, int16_t y, int16_t w, int16_t h);

/* 异步推送 n 个 RGB565 像素（内部按 8192 像素分块走 DMA），立即返回 */
void panelPush(const uint16_t *px, size_t n);
void panelPushSync(const uint16_t *px, size_t n);

/* 等待所有排队事务真正发完，再抬高 CS。下一帧开窗前必须调用。 */
void panelFlush();

/* 上一帧 DMA 实际占用的时间（微秒），仅供串口日志分析 */
uint32_t panelLastTransferUs();

/* 用零拷贝（PSRAM 直接 DMA）还是拷贝模式的帧计数，供日志判断当前通路 */
uint32_t panelDirectFrames();
uint32_t panelCopyFrames();

/* 细分耗时（微秒）：push 内部分成"字节交换拷贝 / 入队等待"，flush 是等 DMA 排空 */
uint32_t panelPushUs();
uint32_t panelFlushUs();
uint32_t panelFillUs();
uint32_t panelQueueUs();

/* 诊断 / 救场：裸命令、写寄存器、整块面板重初始化（不清显存） */
void panelCmd(uint8_t cmd, const uint8_t *data, int n);
void panelWriteReg(uint8_t reg, const uint8_t *data, int n);
void panelReinit();
