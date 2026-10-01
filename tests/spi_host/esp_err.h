/* Minimal SDK service adapter for real SPI UI tests; no physical hardware. */
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_STATE 0x103
static inline const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "ESP_OK" : error == ESP_FAIL ? "ESP_FAIL" : "ESP_ERR_INVALID_STATE";
}
