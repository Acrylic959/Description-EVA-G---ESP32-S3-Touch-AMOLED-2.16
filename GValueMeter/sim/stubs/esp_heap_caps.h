#pragma once
/* sim 用 esp_heap_caps 替身：直接 malloc，模拟 PSRAM 分配成功 */
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 0x0001
#define MALLOC_CAP_8BIT   0x0002

inline void *heap_caps_malloc(size_t size, uint32_t caps) {
  (void)caps;
  return malloc(size);
}
