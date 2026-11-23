#pragma once

#include <Arduino.h>
#include "config.h"

// ============================================================
// IMU / G值计算模块  (QMI8658 六轴传感器)
// ------------------------------------------------------------
// 坐标系约定 (设备安装时屏幕朝向自己):
//   X轴 -> 侧向 (左右转弯)   正值 = 右转离心力
//   Y轴 -> 纵向 (加速/刹车)  正值 = 加速
//   Z轴 -> 竖直 (重力/颠簸)
// 静止时 Z≈1g(或-1g), X/Y≈0, 启动时自动校准零偏。
// ============================================================

struct GData {
  float gx;        // 侧向 G (滤波后, 已去重力)
  float gy;        // 纵向 G (滤波后, 已去重力)
  float gz;        // 竖直 G (滤波后, 已去除1g重力)
  float gTotal;    // 合成 G = sqrt(gx^2 + gy^2)
  float gMax;      // 历史最大合成 G
};

// 初始化 QMI8658 (返回 false 表示未找到传感器)
bool imuBegin();

// 校准零偏 (设备需静止, 采样 N 次求平均)
void imuCalibrate();

// 读取并更新 G 数据, 返回是否有新数据
bool imuUpdate(GData &data);

// 重置历史最大值
void imuResetMax();

// 检测减速带事件 (竖直G突变), 返回本次是否触发
bool imuCheckBump();

// 获取最近一次原始竖直加速度突变强度(用于动画幅度)
float imuBumpStrength();
