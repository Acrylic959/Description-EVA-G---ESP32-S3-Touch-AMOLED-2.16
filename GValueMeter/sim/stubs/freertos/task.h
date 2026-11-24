#pragma once
#include "FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
#define tskNO_AFFINITY 0x7FFFFFFF

inline int xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
                                   uint32_t stack, void *arg, int prio,
                                   TaskHandle_t *handle, int core) {
  (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)handle; (void)core;
  return 1;   /* sim 里不创建真实任务，采集在主线里按步调用 */
}
inline void vTaskDelay(uint32_t t) { (void)t; }
inline void vTaskDelayUntil(TickType_t *prev, TickType_t inc) { (void)prev; (void)inc; }
inline TickType_t xTaskGetTickCount() { return 0; }
