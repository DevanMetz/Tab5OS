#include "capture_data.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define CAPTURE_LINE_BYTES 256

capture_format_t capture_data_detect(const char *header)
{
    if (!header) return CAPTURE_FORMAT_NONE;
    if ((unsigned char)header[0] == 0xef && (unsigned char)header[1] == 0xbb &&
        (unsigned char)header[2] == 0xbf) header += 3;
    if (!strcmp(header, "unix_time,elapsed_us,gpio,millivolts,sample_rate_hz,offset_mv,scale_permille"))
        return CAPTURE_FORMAT_SCOPE;
    if (!strcmp(header, "unix_time,address,register,value,status,speed_khz"))
        return CAPTURE_FORMAT_I2C;
    return CAPTURE_FORMAT_NONE;
}

static void strip_line_end(char *line)
{
    size_t size = strlen(line);
    while (size && (line[size - 1] == '\n' || line[size - 1] == '\r')) line[--size] = '\0';
}

static bool split_fields(char *line, char **fields, size_t expected)
{
    size_t count = 1;
    fields[0] = line;
    for (char *p = line; *p; p++) {
        if (*p != ',') continue;
        if (count >= expected) return false;
        *p = '\0';
        fields[count++] = p + 1;
    }
    return count == expected;
}

static bool parse_u64(const char *text, int base, uint64_t *value)
{
    if (!text[0] || text[0] == '-') return false;
    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(text, &end, base);
    if (errno || *end) return false;
    *value = parsed;
    return true;
}

static bool parse_i32(const char *text, int32_t *value)
{
    if (!text[0]) return false;
    errno = 0;
    char *end;
    long parsed = strtol(text, &end, 10);
    if (errno || *end || parsed < INT32_MIN || parsed > INT32_MAX) return false;
    *value = (int32_t)parsed;
    return true;
}

static bool parse_point(char *line, capture_format_t format, capture_point_t *point)
{
    char *fields[7];
    if (format == CAPTURE_FORMAT_SCOPE) {
        if (!split_fields(line, fields, 7) || !parse_u64(fields[1], 10, &point->time) ||
            !parse_i32(fields[3], &point->value)) return false;
        if (point->value < 0 || point->value > 3300) return false;
        point->valid = true;
        return true;
    }
    if (!split_fields(line, fields, 6) || !parse_u64(fields[0], 10, &point->time)) return false;
    point->valid = !strcmp(fields[4], "ESP_OK") && fields[3][0];
    point->value = 0;
    if (point->valid) {
        uint64_t value;
        if (!parse_u64(fields[3], 0, &value) || value > 255) return false;
        point->value = (int32_t)value;
    }
    return true;
}

bool capture_data_read(FILE *file, capture_point_t *points, size_t capacity, capture_data_t *result)
{
    if (!file || !points || !capacity || !result || fseek(file, 0, SEEK_SET) != 0) return false;
    memset(result, 0, sizeof(*result));
    char line[CAPTURE_LINE_BYTES];
    if (!fgets(line, sizeof(line), file)) return false;
    strip_line_end(line);
    result->format = capture_data_detect(line);
    if (result->format == CAPTURE_FORMAT_NONE) return false;

    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n')) {
            int ch = fgetc(file);
            if (ch != EOF) {
                while (ch != '\n' && ch != EOF) ch = fgetc(file);
                result->ignored++;
                continue;
            }
        }
        strip_line_end(line);
        if (!line[0]) continue;
        capture_point_t point;
        if (!parse_point(line, result->format, &point)) {
            result->ignored++;
            continue;
        }
        if (result->count == capacity) {
            result->truncated = true;
            break;
        }
        points[result->count++] = point;
    }
    return !ferror(file);
}
