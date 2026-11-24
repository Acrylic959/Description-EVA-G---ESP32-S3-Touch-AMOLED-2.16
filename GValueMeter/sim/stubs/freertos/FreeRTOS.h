#pragma once
/* sim 用 FreeRTOS 替身：单线程下运行，临界区为空实现 */
#include <stdint.h>

typedef uint32_t TickType_t;
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

typedef struct { int dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }

#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux)  ((void)(mux))
