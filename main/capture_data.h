#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    CAPTURE_FORMAT_NONE,
    CAPTURE_FORMAT_SCOPE,
    CAPTURE_FORMAT_I2C,
} capture_format_t;

typedef struct {
    uint64_t time;
    int32_t value;
    bool valid;
} capture_point_t;

typedef struct {
    capture_format_t format;
    size_t count;
    size_t ignored;
    bool truncated;
} capture_data_t;

capture_format_t capture_data_detect(const char *header);
bool capture_data_read(FILE *file, capture_point_t *points, size_t capacity, capture_data_t *result);
