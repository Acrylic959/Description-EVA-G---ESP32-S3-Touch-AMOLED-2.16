# EVA 车载 G 值表固件

> 为 **微雪 ESP32-S3-Touch-AMOLED-2.16** 开发的车载 G 值表固件，复刻小红书博主「飞翔的AMX」的 DIY 项目功能，采用 EVA / NERV 主题界面。
>

---

## 功能

### 🐾 宠物模式（滑动切换）
- **待机表情**：圆脸呼吸动画 + 眨眼高光
- **加速表情**：^_^ 兴奋眼、张嘴、速度线
- **减速表情**：> < 震惊眼、O 型嘴、感叹号
- **减速带 / 颠簸**：X 眼抖动 + 旋转星星
- **左右转弯**：眼睛看向转向侧 + 侧倾动感线
- **听歌哼歌同步**：板载 ES7210 双麦采集音量/节拍，驱动嘴巴开合 + 音符跳动
- 顶部显示**当前 G 值**与**历史最大 G 值**

### 📊 专业 G 值模式
- 大号**当前合成 G 值**
- **历史最大 G 值**
- 三轴分量条：**X 侧向 / Y 纵向 / Z 竖直**（中心零位，±1g 满量程）
- **实时 G 值曲线**（最近 120 点，带 0.8G 报警参考线）
- **G > 0.8 红框报警**（闪烁红框 + HIGH G 提示）

### 操作

| 操作 | 功能 |
|------|------|
| 左滑 | 进入专业 G 值模式 |
| 右滑 | 返回宠物模式 |
| 单击屏幕 | 重置历史最大 G 值 |
| 上电 | 自动校准 IMU 零偏（保持静止约 1 秒） |

---

## 硬件

| 项目 | 规格 |
|------|------|
| 主控 | ESP32-S3R8（8MB OPI PSRAM + 16MB Flash，240MHz） |
| 屏幕 | 2.16" AMOLED 480×480，CO5300 驱动，QSPI |
| 触摸 | CST9220 电容触摸 |
| IMU | QMI8658 六轴加速度计 + 陀螺仪 |
| 麦克风 | ES7210 双麦克风 ADC |
| 供电 | Type-C / 3.7V 锂电池（MX1.25） |

> 无需额外硬件，把开发板固定车内即可（建议屏幕朝向驾驶员、粘贴牢固）。

---

## 编译与烧录

### PlatformIO（推荐）

1. 安装 [VS Code](https://code.visualstudio.com/) + PlatformIO IDE 插件，或命令行：
   ```bash
   pip install platformio
   ```
2. 用 VS Code 打开本项目目录。
3. 编译 / 烧录：
   ```bash
   pio run              # 编译
   pio run -t upload    # 烧录（连接 USB 后）
   pio device monitor   # 串口日志 115200
   ```

首次编译会自动下载 ESP32 工具链与依赖库，需联网。

### 依赖库（platformio.ini 已配置）
- `GFX Library for Arduino` 1.5.0（CO5300 显示 + Canvas；1.6.x 依赖 core 3.x 的 `esp32-hal-periman.h`，与本项目 legacy I2S 不兼容）
- `SensorLib` 0.3.3（QMI8658 + CST9220）

> 已用 Arduino-ESP32 core 2.0.16 编译验证通过：`firmware.bin` ≈ 376KB，RAM 6.4%、Flash 5.9%。

### Arduino IDE（可选）
参考微雪官方文档搭建：<https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.16/Arduino/>
安装上述两个库，开发板选 ESP32S3 Dev Module，开启 **OPI PSRAM**、USB CDC，Flash 16MB。将 `src/` 下的 `.cpp` 与 `.h` 放到同一 sketch 目录即可。

---

## 目录结构

```
gmeter-eva/
├── platformio.ini
├── README.md
├── include/
│   ├── pin_config.h      # 引脚定义（微雪官方）
│   ├── config.h          # G值阈值 / 主题配色 / 枚举
│   └── display.h         # 显示模块接口
└── src/
    ├── main.cpp          # 主程序：初始化、主循环、表情判定、UI 绘制
    ├── display.cpp       # CO5300 QSPI + Canvas 双缓冲（接口见 include/display.h）
    ├── imu.cpp/.h        # QMI8658 + G值计算 + 零偏校准 + 减速带检测
    ├── touch.cpp/.h      # CST9220 + 滑动/点击手势
    ├── audio.cpp/.h      # ES7210 音量/节拍检测
    ├── es7210.cpp/.h     # ES7210 驱动（微雪官方示例，Espressif MIT）
    └── audio_hal.h       # 音频 HAL 定义
```

> 说明：`display.h` 位于 `include/`，`display.cpp` 位于 `src/`，PlatformIO 会自动处理头文件路径。

---

## 可调参数（`include/config.h`）

| 参数 | 含义 | 默认 |
|------|------|------|
| `BUMP_THRESHOLD` | 减速带触发阈值(g) | 0.35 |
| `ACCEL_THRESHOLD` / `BRAKE_THRESHOLD` | 加/减速表情阈值(g) | ±0.12 |
| `TURN_THRESHOLD` | 转弯表情阈值(g) | 0.10 |
| `ALARM_G` | 红框报警阈值(g) | 0.8 |
| `G_FILTER_ALPHA` | G值低通滤波系数 | 0.35 |
| `G_HISTORY_LEN` | 曲线点数 | 120 |

> 若滑动方向与预期相反，在 `src/main.cpp` 的 `onSwipe()` 中交换 `dir > 0` 对应的模式即可。

---

## 坐标系

`src/imu.cpp` 约定（屏幕朝向驾驶员安装）：
- **X 轴**：侧向 G，正值 = 右转离心力
- **Y 轴**：纵向 G，正值 = 加速
- **Z 轴**：竖直，去除 1g 后用于减速带检测

上电自动校准零偏，**校准时保持车辆静止**。安装方向不同可在 `imuUpdate()` 中调整轴映射/符号。

---

## 致谢
- ES7210 驱动来自微雪官方示例（Espressif MIT License）
- 传感器/显示驱动：[SensorLib](https://github.com/lewisxhe/SensorLib)、[GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX)
- 功能创意来自小红书 @飞翔的AMX 的 DIY 分享
