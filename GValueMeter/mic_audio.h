#pragma once
/* ===========================================================================
 *  mic_audio.h — ES7210 双麦采集 → 音量包络 + 节拍
 *
 *  采集在独立任务中跑（Core 0），UI 只取快照。
 *  信号链：两路麦克风取均值 → 40Hz 高通去直流/低频轰鸣
 *          → 250~3500Hz 带通（人声/哼唱/音乐主能量）
 *          → 快/慢包络 → 音量 + 起音(节拍)检测
 * =========================================================================== */
#include <stdint.h>

struct AudioSnapshot {
  float level;       // 0..1，直接驱动嘴型张合
  float rms;         // 带通 RMS（调试用）
  bool  music;       // 是否判定为"在听歌/哼歌"
  bool  ok;          // 麦克风是否可用
  uint32_t beatSeq;  // 节拍计数（递增）
  float beatAge;     // 距上次节拍的时间(秒)，用于音符动画
};

class MicAudio {
 public:
  bool begin();
  void runTask();                     // 采集任务主体（由 begin() 创建的任务调用）
  void snapshot(AudioSnapshot &o);
  bool ok() const { return _ok; }
  uint32_t beatCount() const { return _beatSeq; }

 private:
  bool _ok = false;
  volatile float _level = 0.f;
  volatile float _rms = 0.f;
  volatile bool  _music = false;
  volatile uint32_t _beatSeq = 0;
  volatile uint32_t _beatMs = 0;
};
