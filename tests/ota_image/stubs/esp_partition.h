#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint8_t esp_partition_subtype_t;
typedef struct {
    esp_partition_subtype_t subtype;
    uint32_t address, size;
    char label[17];
} esp_partition_t;
