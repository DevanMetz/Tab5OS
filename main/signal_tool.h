#pragma once

#include <stdbool.h>

#include "lvgl.h"

void signal_tool_show(lv_obj_t *parent);
void signal_tool_stop(void);
bool signal_tool_busy(void);
void signal_tool_self_test(void);
