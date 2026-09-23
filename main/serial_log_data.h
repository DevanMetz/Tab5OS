#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SERIAL_LOG_MAX_BYTES 256

typedef struct {
    uint64_t unix_time;
    bool tx;
    uint16_t length;
    uint8_t bytes[SERIAL_LOG_MAX_BYTES];
} serial_log_row_t;

typedef struct {
    size_t count;
    size_t ignored;
    bool truncated;
} serial_log_data_t;

bool serial_log_detect(const char *header);
bool serial_log_read(FILE *file, serial_log_row_t *rows, size_t capacity,
                     serial_log_data_t *result);
