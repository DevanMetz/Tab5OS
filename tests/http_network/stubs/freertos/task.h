#pragma once
#include "host.h"
#define xTaskCreateWithCaps(function, name, stack, argument, priority, handle, capabilities) \
    host_task_create(function, name, stack, argument, priority, handle)
#define vTaskDeleteWithCaps(handle) ((void)(handle))
#define vTaskDelay(ticks) Sleep(ticks)
