#pragma once
#include "freertos/FreeRTOS.h"
BaseType_t xTaskCreate(void (*function)(void *), const char *name, unsigned stack,
                       void *argument, unsigned priority, TaskHandle_t *handle);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelete(TaskHandle_t task);
BaseType_t xTaskCreateWithCaps(void (*function)(void *), const char *name, unsigned stack,
                              void *argument, unsigned priority, TaskHandle_t *handle, unsigned caps);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void xTaskNotifyGive(TaskHandle_t task);
#define vTaskDelay(ticks) Sleep(ticks)
