#pragma once

#include <stdbool.h>
#include "lvgl.h"

void udp_tool_show(lv_obj_t *parent, bool connected);
void udp_tool_stop(void);
bool udp_tool_busy(void);
