#pragma once

#include <stdbool.h>
#include "lvgl.h"

void ntp_tool_show(lv_obj_t *parent, bool connected);
void ntp_tool_stop(void);
bool ntp_tool_busy(void);
