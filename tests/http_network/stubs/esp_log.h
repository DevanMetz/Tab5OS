#pragma once
#include <stdio.h>
typedef int esp_log_level_t;
#define ESP_LOG_WARN 1
#define ESP_LOG_DEBUG 2
#define ESP_LOGE(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW ESP_LOGE
#define ESP_LOGD ESP_LOGE
#define ESP_LOGV ESP_LOGE
#define ESP_LOG_LEVEL(level, tag, ...) do { (void)(level); ESP_LOGE(tag, __VA_ARGS__); } while (0)
