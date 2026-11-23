#pragma once

// ============================================================
// EVA G值表 - 全局配置与主题
// ============================================================

// ---------------- G值计算参数 ----------------
// 加速度计量程与采样率
#define IMU_RANGE_4G        // 4G量程, 0.122mg/LSB 精度足够车载
#define IMU_ODR_HZ 250      // 输出速率 250Hz (SensorLib 支持的最接近档位)

// 竖直方向判定为"减速带/颠簸"的阈值(g)
#define BUMP_THRESHOLD     0.35f
// 明确减速/加速表情阈值(g)
#define ACCEL_THRESHOLD    0.12f
#define BRAKE_THRESHOLD   -0.12f
// 转弯(侧向)判定阈值(g)
#define TURN_THRESHOLD     0.10f
// 报警阈值(g) - 笔记中: G值超过0.8红框报警
#define ALARM_G            0.8f

// 滑动手势判定(像素)
#define SWIPE_MIN_DIST     60
#define SWIPE_MAX_MS       600

// 音量(听歌哼歌同步)判定阈值
#define SING_VOL_THRESH    220     // 麦克风采样幅度

// 低通滤波系数 (越大越灵敏, 0~1)
#define G_FILTER_ALPHA     0.35f
// 历史最大G曲线采样点数
#define G_HISTORY_LEN      120

// ---------------- 主题配色 (RGB565) ----------------
// EVA 风格: 黑底 + 紫/绿高亮 + 红报警
#define C_BG          0x0000  // 黑
#define C_WHITE       0xFFFF
#define C_BLACK       0x0000
#define C_GRAY        0x7BEF  // 暗灰
#define C_GRAY_LIGHT  0xBDF7

// 主题紫 (EVA-01 主色)
#define C_PURPLE      0x781F  // 深紫
#define C_PURPLE_L    0xA27F  // 亮紫
#define C_VIOLET      0x8116  // 紫罗兰

// 主题绿 (NERV 界面)
#define C_GREEN       0x07E0  // 纯绿
#define C_GREEN_D     0x0440  // 暗绿
#define C_MINT        0x1FF7  // 薄荷绿

// 报警红 / 橙
#define C_RED         0xF800
#define C_RED_D       0x7800
#define C_ORANGE      0xFD20
#define C_YELLOW      0xFFE0

// 青色(分量值)
#define C_CYAN        0x07FF

// ---------------- 宠物表情枚举 ----------------
enum PetMood {
  MOOD_IDLE = 0,     // 待机
  MOOD_ACCEL,        // 加速
  MOOD_BRAKE,        // 减速/刹车
  MOOD_BUMP,         // 减速带/颠簸
  MOOD_TURN_L,       // 左转
  MOOD_TURN_R,       // 右转
  MOOD_SING,         // 听歌哼歌
  MOOD_COUNT
};

// ---------------- 页面模式 ----------------
enum AppMode {
  MODE_PET = 0,      // 宠物模式
  MODE_PRO = 1,      // 专业G值模式
  MODE_COUNT
};
