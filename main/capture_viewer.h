#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* Returns false when the file is not a supported capture CSV. */
bool capture_viewer_show(lv_obj_t *parent, const char *path);
void capture_viewer_stop(void);
