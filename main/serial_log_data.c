#include "serial_log_data.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define SERIAL_LOG_LINE_BYTES 1024
#define SERIAL_LOG_HEADER "unix_time,direction,data_hex"

bool serial_log_detect(const char *header)
{
    if (!header) return false;
    if ((unsigned char)header[0] == 0xef && (unsigned char)header[1] == 0xbb &&
        (unsigned char)header[2] == 0xbf) header += 3;
    return strcmp(header, SERIAL_LOG_HEADER) == 0;
}

static void strip_line_end(char *line)
{
    size_t size = strlen(line);
    while (size && (line[size - 1] == '\n' || line[size - 1] == '\r')) line[--size] = '\0';
}

static int hex_digit(char digit)
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

static bool parse_row(char *line, serial_log_row_t *row)
{
    char *direction = strchr(line, ',');
    if (!direction) return false;
    *direction++ = '\0';
    char *hex = strchr(direction, ',');
    if (!hex) return false;
    *hex++ = '\0';
    if (strchr(hex, ',')) return false;

    if (!line[0] || line[0] == '-') return false;
    errno = 0;
    char *end;
    unsigned long long stamp = strtoull(line, &end, 10);
    if (errno || *end) return false;
    row->unix_time = stamp;
    if (!strcmp(direction, "TX")) row->tx = true;
    else if (!strcmp(direction, "RX")) row->tx = false;
    else return false;

    row->length = 0;
    while (*hex) {
        if (!hex[1] || row->length == SERIAL_LOG_MAX_BYTES) return false;
        int high = hex_digit(hex[0]);
        int low = hex_digit(hex[1]);
        if (high < 0 || low < 0) return false;
        row->bytes[row->length++] = (uint8_t)((high << 4) | low);
        hex += 2;
        if (!*hex) break;
        if (*hex++ != ' ' || !*hex) return false;
    }
    return row->length > 0;
}

bool serial_log_read(FILE *file, serial_log_row_t *rows, size_t capacity,
                     serial_log_data_t *result)
{
    if (!file || !rows || !capacity || !result || fseek(file, 0, SEEK_SET) != 0) return false;
    memset(result, 0, sizeof(*result));
    char line[SERIAL_LOG_LINE_BYTES];
    if (!fgets(line, sizeof(line), file)) return false;
    strip_line_end(line);
    if (!serial_log_detect(line)) return false;

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
        serial_log_row_t row;
        if (!parse_row(line, &row)) {
            result->ignored++;
            continue;
        }
        if (result->count == capacity) {
            result->truncated = true;
            break;
        }
        rows[result->count++] = row;
    }
    return !ferror(file);
}
