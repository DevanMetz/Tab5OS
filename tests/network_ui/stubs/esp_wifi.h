#pragma once
#include "host.h"
#include "esp_err.h"
#include <string.h>

typedef struct { char ssid[33]; unsigned primary; int rssi; } wifi_ap_record_t;
static inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record)
{
    if (!host_online) return ESP_ERR_INVALID_STATE;
    strcpy(record->ssid, "fixture LAN"); record->primary = 6; record->rssi = -42; return ESP_OK;
}
