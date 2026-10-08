#pragma once
#include "../../http_network/stubs/esp_heap_caps.h"
#define MALLOC_CAP_DEFAULT (1U << 12)
void *heap_caps_malloc(size_t size, unsigned capabilities);
