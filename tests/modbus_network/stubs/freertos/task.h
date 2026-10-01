#pragma once
#include "host.h"
#define xTaskCreate host_task_create
#define vTaskDelete(handle) ((void)(handle))
#define vTaskDelay(ticks) Sleep(ticks)
