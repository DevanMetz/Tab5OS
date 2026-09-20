#pragma once

#include <stdbool.h>

#include "lvgl.h"

typedef void (*uart_tool_storage_error_cb_t)(int error);

void uart_tool_show(lv_obj_t *parent, bool sd_available,
                    uart_tool_storage_error_cb_t storage_error_cb);
void uart_tool_stop(void);
bool uart_tool_busy(void);
void uart_tool_self_test(void);
