#pragma once

#include <stdbool.h>

#include "lvgl.h"

void spi_tool_show(lv_obj_t *parent);
void spi_tool_stop(void);
bool spi_tool_busy(void);
void spi_tool_self_test(void);
