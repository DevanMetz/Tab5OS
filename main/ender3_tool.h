#pragma once

#include <stdbool.h>

#include "lvgl.h"

void ender3_tool_show(lv_obj_t *parent);
void ender3_tool_stop(void);
bool ender3_tool_busy(void);
void ender3_tool_self_test(void);
