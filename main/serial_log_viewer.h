#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* Returns false when the file is not a published serial CSV. */
bool serial_log_viewer_show(lv_obj_t *parent, const char *path);
void serial_log_viewer_stop(void);
