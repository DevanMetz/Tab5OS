#pragma once

#include <stdbool.h>

#include "lvgl.h"

typedef void (*http_tool_storage_error_cb_t)(int error);

void http_tool_show(lv_obj_t *parent, bool connected, bool sd_available,
                    http_tool_storage_error_cb_t storage_error_cb);
void http_tool_stop(void);
bool http_tool_busy(void);
void http_tool_self_test(void);
