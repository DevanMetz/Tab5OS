#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define OTA_MANIFEST_VERSION_MAX 31

typedef struct {
    char version[OTA_MANIFEST_VERSION_MAX + 1];
    char url[256];
    char minimum_predecessor[OTA_MANIFEST_VERSION_MAX + 1];
    size_t size;
    uint8_t sha256[32];
} ota_manifest_t;

esp_err_t ota_manifest_fetch(const char *manifest_url, ota_manifest_t *manifest,
                             char *message, size_t message_size);
esp_err_t ota_manifest_check(const ota_manifest_t *manifest, const char *current_version,
                             char *message, size_t message_size);
esp_err_t ota_manifest_install(const ota_manifest_t *manifest,
                               char *message, size_t message_size);
void ota_manifest_self_test(void);
