#pragma once

#include "lvgl.h"

void electronics_tool_show(lv_obj_t *parent);
/* Call before deleting the parent. Mode and input text remain in RAM. */
void electronics_tool_stop(void);
