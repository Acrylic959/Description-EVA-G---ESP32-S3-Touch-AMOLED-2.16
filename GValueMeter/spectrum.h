#pragma once
/* ===========================================================================
 *  spectrum.h — 音乐频谱柱动画（宠物页，PWR 键切换）
 *
 *  参考 web 上的 audio-visualizer：竖条 + 底部青绿→顶部琥珀红的渐变、
 *  峰值帽慢下落、底部倒影。数据来自 mic_audio 的 20 段 FFT。
 * =========================================================================== */
#include <stdint.h>

#include "canvas_gfx.h"
#include "mic_audio.h"

void spectrumDraw(CanvasGFX *c, const float *bands, const float *pk,
                  float t, float level);
