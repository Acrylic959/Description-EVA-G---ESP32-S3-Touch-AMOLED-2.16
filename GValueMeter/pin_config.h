#pragma once
/* ===========================================================================
 *  pin_config.h  —  板级引脚 + 全局可调参数
 *  目标板：Waveshare ESP32-S3-Touch-AMOLED-2.16 (ESP32-S3R8 / 8MB PSRAM / 16MB Flash)
 *  所有引脚均取自官方 wiki《GPIO Pin Assignments》
 * =========================================================================== */

/* ---------------- LCD (CO5300, QSPI) ---------------- */
#define LCD_CS        12
#define LCD_SCLK      38
#define LCD_SDIO0      4
#define LCD_SDIO1      5
#define LCD_SDIO2      6
#define LCD_SDIO3      7
#define LCD_RESET     39
#define LCD_WIDTH    480
#define LCD_HEIGHT   480
/* MADCTL：0xA0 = 官方例程方向；若屏幕上下颠倒可改 0x00 / 0x60 / 0xC0 */
#define LCD_MADCTL  0xA0

/* ---------------- I2C（触摸/PMU/IMU/RTC/Codec 共用） ---------------- */
#define IIC_SDA       15
#define IIC_SCL       14

/* ---------------- 按键 / 其它 ---------------- */
#define PIN_KEY3      18     // 侧面用户键，上拉，按下为低
#define PIN_BOOT       0     // BOOT 键（调试用，本固件也作为备用键）

/* ---------------- 音频（ES7210 双麦 + ES8311 共用 I2S 时钟） ---------------- */
#define I2S_MCLK      42
#define I2S_BCLK       9
#define I2S_LRCK      45
#define I2S_MIC_DIN   10     // ES7210 -> ESP32 (ASDOUT)
#define I2S_SPK_DOUT   8     // ES8311 <- ESP32 (DSDIN，本固件未用)
#define PIN_PA_CTRL   46     // 功放使能（本固件未用，保持静音）

#define ES7210_ADDR   0x40
#define MIC_SAMPLE_RATE 16000
#define MIC_GAIN_DB      14  // ES7210 PGA 档位：0=0dB 每档 3dB，14 = 42dB（麦克风灵敏度拉满）

/* ---------------- 运行节奏 ---------------- */
#define IMU_RATE_HZ     200          // 加速度采样频率
#define IMU_PERIOD_MS   (1000 / IMU_RATE_HZ)
#define UI_FPS           30
#define UI_FRAME_MS      (1000 / UI_FPS)
#define UI_FPS_STANDBY    20               /* 待机只播循环动画：降到 20fps 省电 */
#define UI_FRAME_MS_STANDBY (1000 / UI_FPS_STANDBY)
#define BOOT_LONG_MS      1500             /* BOOT 长按这么久 = 开测 / 停测 */
#define CURVE_N         120          // 曲线点数（10Hz × 120 = 最近 12 秒）
#define CURVE_PERIOD_MS 100

/* ---------------- G 值处理 ---------------- */
#define ACC_FILTER_TAU   0.045f      // 显示用低通时间常数(秒)，越小越"灵敏"
#define PEAK_TAU         0.012f      // 峰值/曲线用快滤波
#define ZERO_SAMPLES     240         // 上电/校准采样点数（200Hz ≈ 1.2s）
#define HIGH_G_LIMIT     0.80f       // 报警阈值(g)

/* G-G 圆盘（第三页）方向约定：1 = 刹车朝上（车身前冲方向），0 = 加速朝上（与博主视频一致） */
#define GG_BRAKE_UP      1
#define BAR_FULL_SCALE   1.00f       // 三轴条满量程 ±1g
#define CHART_FULL_SCALE 1.20f       // 曲线纵轴满量程

/* ---------------- 默认轴向映射（屏幕竖直朝驾驶员安装时）
 *  lat  : 侧向（左右转弯），取 QMI8658 的 X
 *  lon  : 纵向（加速/刹车），取 Y（屏幕面内长边）；正负号决定"加速"方向
 *  vert : 竖直（颠簸），取 Z（垂直板面）
 *  运行时可用串口命令 axis 修改，无需重新编译，见 README。
 * ------------------------------------------------------------------------- */
#define DEF_LAT_AXIS   0     // 0=X 1=Y 2=Z
#define DEF_LON_AXIS   1
#define DEF_VERT_AXIS  2
/* 安装方式按"水平仪"理解：屏面朝上平放（或近似平放），
 *   侧向 = X（左右）、纵向 = Y（屏幕面内的前后）、竖直 = Z（垂直板面）。
 * 实测约定：lon>0=加速、lon<0=刹车；lat>0=右转。
 * 每次校准只是重设水平面，不做重力方向跟踪、也不自动换轴。 */
#define DEF_LAT_SIGN   (+1)  // +1 / -1（左右甩动方向）
#define DEF_LON_SIGN   (+1)  // +1 / -1

/* ---------------- 宠物表情阈值(g) ---------------- */
#define PET_ACC_G      0.13f   // 纵向加速触发 ^ ^
#define PET_BRK_G      0.11f   // 纵向减速触发 > <
#define PET_TURN_G     0.13f   // 侧向触发"看向转向侧"
#define PET_BUMP_P2P   0.55f   // 竖直 240ms 峰峰值 > 此值判定颠簸/减速带
#define PET_BUMP_CD_MS 700     // 颠簸事件最小间隔

/* ---------------- 其它 ---------------- */
/* QSPI 时钟：面板只在 GFX 官方轮询路径上稳定，40~80MHz 都实测可跑 */
#define GVM_LCD_HZ     80000000L
#define BRIGHTNESS_DEF  210
#define UI_NOTICE_MS    1200
