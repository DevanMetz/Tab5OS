#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

/* UI-task-only saved-read panel. The existing inspector owns all bus activity.
 * before_action cancels a pending write before either panel button acts. */
void i2c_result_show(lv_obj_t *parent, void (*before_action)(void));
/* Store a complete 1-32-byte read; invalid lengths, null data and failed reads
 * clear the snapshot. The bytes are copied, so the caller retains no buffer. */
void i2c_result_record(bool success, uint8_t address, uint8_t reg,
                       uint32_t speed_hz, const uint8_t *bytes, size_t length);
/* Clear before an actual write; failed reads also clear the saved bytes. */
void i2c_result_clear(void);
/* Detach UI pointers before the parent is deleted; retain the saved bytes. */
void i2c_result_stop(void);
