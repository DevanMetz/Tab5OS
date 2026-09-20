#pragma once

#include <stdbool.h>

#include "lvgl.h"

typedef void (*mqtt_tool_storage_error_cb_t)(int error);

void mqtt_tool_show(lv_obj_t *parent, bool wifi_connected, bool sd_available,
                    mqtt_tool_storage_error_cb_t storage_error_cb);
void mqtt_tool_stop(void);
bool mqtt_tool_busy(void);
void mqtt_tool_self_test(void);
