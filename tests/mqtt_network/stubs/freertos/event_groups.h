#pragma once
#include "freertos/FreeRTOS.h"
EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupGetBits(EventGroupHandle_t group);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                              BaseType_t clear, BaseType_t all, TickType_t timeout);
void vEventGroupDelete(EventGroupHandle_t group);
