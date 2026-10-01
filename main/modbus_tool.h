#pragma once

#include <stdbool.h>

#include "lvgl.h"

void modbus_tool_show(lv_obj_t *parent, bool connected);
void modbus_tool_stop(void);
bool modbus_tool_busy(void);
