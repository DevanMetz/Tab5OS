#pragma once
#include "host.h"
typedef SRWLOCK portMUX_TYPE;
typedef struct mqtt_host_task *TaskHandle_t;
#define portMUX_INITIALIZER_UNLOCKED SRWLOCK_INIT
#define portENTER_CRITICAL(lock) AcquireSRWLockExclusive(lock)
#define portEXIT_CRITICAL(lock) ReleaseSRWLockExclusive(lock)
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
