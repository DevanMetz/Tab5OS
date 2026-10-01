#pragma once

#include <stdbool.h>
#include "lvgl.h"

void wol_tool_show(lv_obj_t *parent, bool connected);
void wol_tool_stop(void);
bool wol_tool_busy(void);
