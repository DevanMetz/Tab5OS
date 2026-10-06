#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_EVENT_DECLARE_BASE(name) extern const char *name
#define ESP_EVENT_DEFINE_BASE(name) const char *name = #name
static inline esp_err_t esp_event_post(const char *base, int32_t id, const void *data, size_t size, unsigned ticks)
{
    (void)base; (void)id; (void)data; (void)size; (void)ticks;
    return ESP_OK;
}
