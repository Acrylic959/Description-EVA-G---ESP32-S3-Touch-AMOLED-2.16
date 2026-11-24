/* ===========================================================================
 *  qmi8658_min.h — QMI8658A 最小原生 I2C 驱动（不依赖 SensorLib）
 *
 *  SensorLib 0.5.2 的 SensorQMI8658 旧类内部用 std::unique_ptr + 虚函数分发，
 *  在本板实测 begin() 直接跳飞到 PC=0x02（InstrFetchProhibited），故自写驱动：
 *  只做本固件需要的 加速度计 4g / 1000Hz / 轮询读。
 * =========================================================================== */
#pragma once

#include <Arduino.h>

/* 探测 0x6A / 0x6B，成功返回 true 并保存地址 */
bool qmiBegin(uint8_t sda, uint8_t scl);

/* 加速度计数据就绪（STATUS0 bit0） */
bool qmiDataReady();

/* 读三轴加速度，单位 mg（±4g 量程，8192 LSB/g） */
bool qmiReadAccelMG(float &x, float &y, float &z);

/* 实际使用的 I2C 地址（0 表示未探测到） */
uint8_t qmiAddr();
