#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "host/ble_hs.h"
#include "lvgl.h"

typedef void (*ble_tool_scan_refresh_cb_t)(void);
typedef bool (*ble_tool_connect_allowed_cb_t)(void);
typedef void (*ble_tool_storage_error_cb_t)(int error);

void ble_tool_show(lv_obj_t *parent, bool available, bool sd_available,
                   ble_tool_scan_refresh_cb_t scan_refresh,
                   ble_tool_connect_allowed_cb_t connect_allowed,
                   ble_tool_storage_error_cb_t storage_error_cb);
void ble_tool_stop(void);
bool ble_tool_busy(void);
bool ble_tool_scan_requested(void);
void ble_tool_observe_advertisement(const ble_addr_t *address, int8_t rssi,
                                    const uint8_t *name, uint8_t name_length);
void ble_tool_self_test(void);
