#pragma once

#include <stdbool.h>

#include "lvgl.h"

void network_tool_show(lv_obj_t *parent, bool connected);
void network_tool_stop(void);
bool network_tool_busy(void);
void network_tool_self_test(void);
