#pragma once
#include "host.h"
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef struct mqtt_net_task *TaskHandle_t;
typedef struct mqtt_net_mutex *SemaphoreHandle_t;
typedef struct mqtt_net_group *EventGroupHandle_t;
typedef uint32_t EventBits_t;
typedef SRWLOCK portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED SRWLOCK_INIT
#define portENTER_CRITICAL(lock) AcquireSRWLockExclusive(lock)
#define portEXIT_CRITICAL(lock) ReleaseSRWLockExclusive(lock)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) (ms)
