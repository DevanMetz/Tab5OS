#pragma once

#include <stdbool.h>
#include <stddef.h>

#define RESISTOR_CODE_MAX 4

typedef enum {
    RESISTOR_BLACK, RESISTOR_BROWN, RESISTOR_RED, RESISTOR_ORANGE,
    RESISTOR_YELLOW, RESISTOR_GREEN, RESISTOR_BLUE, RESISTOR_VIOLET,
    RESISTOR_GRAY, RESISTOR_WHITE, RESISTOR_GOLD, RESISTOR_SILVER,
    RESISTOR_COLOR_COUNT
} resistor_color_t;

typedef enum {
    RESISTOR_OK, RESISTOR_BAD_ARGUMENT, RESISTOR_BAD_BANDS,
    RESISTOR_BAD_DIGIT, RESISTOR_BAD_MULTIPLIER, RESISTOR_BAD_TOLERANCE,
    RESISTOR_BAD_CODE, RESISTOR_CODE_TOO_LONG
} resistor_status_t;

typedef enum {
    RESISTOR_BANDS, RESISTOR_NUMERIC, RESISTOR_R_DECIMAL,
    RESISTOR_EIA96, RESISTOR_JUMPER
} resistor_kind_t;

typedef struct {
    double ohms;
    double tolerance_percent;
    double minimum_ohms;
    double maximum_ohms;
    unsigned significand;
    int exponent10;
    bool tolerance_known;
    resistor_kind_t kind;
} resistor_data_t;

/* Four bands: two digits, multiplier, tolerance. Five bands: three digits,
 * multiplier, tolerance. First digit cannot be black. Multiplier: silver (0.01)
 * through white (1e9). Tolerances follow the linked Vishay table in the guide.
 * These functions clear the result on error and never access hardware. */
resistor_status_t resistor_decode_bands(const resistor_color_t *colors, size_t count,
                                        resistor_data_t *result);

/* Strict 1-4 ASCII characters, with no whitespace, signs, or case conversion.
 * Numeric: three/four digits, last digit is a power of ten; all-zero markings
 * identify a jumper. R-decimal: one R and 1-3 digits. EIA-96: 01..96 followed by
 * A..H, X, Y, or Z. SMD marking alone does not establish tolerance or ratings. */
resistor_status_t resistor_decode_smd(const char *code, resistor_data_t *result);
const char *resistor_error(resistor_status_t status);
void resistor_data_self_test(void);
