#pragma once

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

// UI objects need byte-addressable memory, not scarce internal DMA memory.
// Keep the existing fixed pool size while leaving SRAM for SDIO receive bursts.
static inline void *tab5_lvgl_pool_alloc(size_t size)
{
    void *pool = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(pool ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI("ui-memory", "LVGL pool: %u bytes in PSRAM", (unsigned)size);
    return pool;
}
