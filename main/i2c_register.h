#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define I2C_REGISTER_MAX_BYTES 32

/* Synchronous UI-task operations on the external G53/G54 bus. Reads send one
 * register-pointer byte, then receive 1-32 bytes in a single transaction. The
 * target defines any auto-increment behavior. Only 100/400 kHz and 0x08-0x77
 * addresses are accepted. Invalid requests perform no bus operations.
 * For valid requests, a read failure (including cleanup) zeroes the output. */
esp_err_t i2c_register_read(uint8_t address, uint8_t reg, uint32_t speed_hz,
                            uint8_t *bytes, size_t length);
esp_err_t i2c_register_write_byte(uint8_t address, uint8_t reg, uint32_t speed_hz,
                                  uint8_t value);
/* Failed device/bus removal retains ownership for a later retry. Call before
 * scanning, on Home, or to retry cleanup after a failed operation. */
esp_err_t i2c_register_stop(void);
bool i2c_register_busy(void);
