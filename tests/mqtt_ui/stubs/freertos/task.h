#pragma once
#include "freertos/FreeRTOS.h"
int mqtt_host_task_create(void (*function)(void *), const char *name, unsigned stack,
                          void *argument, unsigned priority, TaskHandle_t *handle, unsigned caps);
uint32_t mqtt_host_task_take(int clear, uint32_t wait);
void mqtt_host_task_notify(TaskHandle_t handle);
#define xTaskCreateWithCaps mqtt_host_task_create
#define ulTaskNotifyTake mqtt_host_task_take
#define xTaskNotifyGive mqtt_host_task_notify
#define vTaskDelay(ticks) Sleep(ticks)
