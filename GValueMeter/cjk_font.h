#pragma once
/* 自动生成，勿手改：中文字形点阵表（16px / 24px / 32px，1-bit，行优先）
   数据在 cjk_font.cpp 里，这里只声明，避免每个翻译单元都存一份 */
#include <stdint.h>

typedef struct {
  uint32_t cp;
  const uint8_t *p16;
  const uint8_t *p24;
  const uint8_t *p32;
} CjkGlyph;

extern const CjkGlyph CJK_TABLE[];
extern const int CJK_COUNT;
