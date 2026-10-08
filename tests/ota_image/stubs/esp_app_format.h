#pragma once
#include <stddef.h>
#include <stdint.h>
/* The MSVC ABI ignores packed enums. Represent only the device image fields
 * used by the SDK OTA reader, preserving the pinned P4 serialized layout. */
typedef uint16_t esp_chip_id_t;
typedef struct {
    uint8_t prefix[12];
    esp_chip_id_t chip_id;
    uint8_t suffix[10];
} esp_image_header_t;
typedef struct { uint32_t load_addr, data_len; } esp_image_segment_header_t;
_Static_assert(sizeof(esp_chip_id_t) == 2, "P4 chip ID width");
_Static_assert(sizeof(esp_image_header_t) == 24, "P4 serialized image header");
_Static_assert(offsetof(esp_image_header_t, chip_id) == 12, "P4 chip ID offset");
_Static_assert(sizeof(esp_image_segment_header_t) == 8, "P4 serialized segment header");
