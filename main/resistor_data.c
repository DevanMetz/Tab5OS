#include "resistor_data.h"

#include <assert.h>
#include <string.h>

/* Manufacturer references and supported marking conventions: docs/resistor-lab.md. */
static const unsigned short e96[] = {
    100, 102, 105, 107, 110, 113, 115, 118, 121, 124, 127, 130,
    133, 137, 140, 143, 147, 150, 154, 158, 162, 165, 169, 174,
    178, 182, 187, 191, 196, 200, 205, 210, 215, 221, 226, 232,
    237, 243, 249, 255, 261, 267, 274, 280, 287, 294, 301, 309,
    316, 324, 332, 340, 348, 357, 365, 374, 383, 392, 402, 412,
    422, 432, 442, 453, 464, 475, 487, 499, 511, 523, 536, 549,
    562, 576, 590, 604, 619, 634, 649, 665, 681, 698, 715, 732,
    750, 768, 787, 806, 825, 845, 866, 887, 909, 931, 953, 976
};
_Static_assert(sizeof(e96) / sizeof(e96[0]) == 96, "E96 table must contain 96 values");

static double scale(unsigned digits, int exponent)
{
    double value = digits;
    while (exponent > 0) { value *= 10; exponent--; }
    while (exponent < 0) { value /= 10; exponent++; }
    return value;
}

resistor_status_t resistor_decode_bands(const resistor_color_t *colors, size_t count,
                                        resistor_data_t *result)
{
    if (!result) return RESISTOR_BAD_ARGUMENT;
    memset(result, 0, sizeof(*result));
    if (!colors) return RESISTOR_BAD_ARGUMENT;
    if (count != 4 && count != 5) return RESISTOR_BAD_BANDS;
    unsigned digits = 0;
    for (size_t i = 0; i < count - 2; i++) {
        if ((unsigned)colors[i] > RESISTOR_WHITE || (i == 0 && colors[i] == RESISTOR_BLACK))
            return RESISTOR_BAD_DIGIT;
        digits = digits * 10 + (unsigned)colors[i];
    }
    resistor_color_t multiplier = colors[count - 2];
    if ((unsigned)multiplier >= RESISTOR_COLOR_COUNT) return RESISTOR_BAD_MULTIPLIER;
    int exponent = multiplier <= RESISTOR_WHITE ? (int)multiplier :
        multiplier == RESISTOR_GOLD ? -1 : -2;
    double tolerance;
    switch (colors[count - 1]) {
    case RESISTOR_BROWN: tolerance = 1; break;
    case RESISTOR_RED: tolerance = 2; break;
    case RESISTOR_GREEN: tolerance = 0.5; break;
    case RESISTOR_BLUE: tolerance = 0.25; break;
    case RESISTOR_VIOLET: tolerance = 0.1; break;
    case RESISTOR_GRAY: tolerance = 0.05; break;
    case RESISTOR_GOLD: tolerance = 5; break;
    case RESISTOR_SILVER: tolerance = 10; break;
    default: return RESISTOR_BAD_TOLERANCE;
    }
    double nominal = scale(digits, exponent);
    *result = (resistor_data_t){
        .ohms = nominal, .tolerance_percent = tolerance,
        .minimum_ohms = nominal * (1 - tolerance / 100),
        .maximum_ohms = nominal * (1 + tolerance / 100),
        .significand = digits, .exponent10 = exponent,
        .tolerance_known = true, .kind = RESISTOR_BANDS
    };
    return RESISTOR_OK;
}

resistor_status_t resistor_decode_smd(const char *code, resistor_data_t *result)
{
    if (!result) return RESISTOR_BAD_ARGUMENT;
    memset(result, 0, sizeof(*result));
    if (!code) return RESISTOR_BAD_ARGUMENT;
    size_t length = 0;
    while (code[length]) {
        if (length == RESISTOR_CODE_MAX) return RESISTOR_CODE_TOO_LONG;
        length++;
    }
    if (!length) return RESISTOR_BAD_CODE;
    unsigned digits = 0, r_count = 0, after_r = 0, digit_count = 0;
    bool numeric = true, zero = true;
    for (size_t i = 0; i < length; i++) {
        if (code[i] < '0' || code[i] > '9') { numeric = false; zero = false; }
        else {
            digits = digits * 10 + (unsigned)(code[i] - '0');
            digit_count++;
            if (r_count) after_r++;
            if (code[i] != '0') zero = false;
        }
        if (code[i] == 'R') r_count++;
    }
    resistor_kind_t kind;
    int exponent = 0;
    if (zero) {
        kind = RESISTOR_JUMPER;
    } else if (numeric && (length == 3 || length == 4)) {
        exponent = (int)(digits % 10);
        digits /= 10;
        if (!digits) return RESISTOR_BAD_CODE;
        kind = RESISTOR_NUMERIC;
    } else if (r_count == 1 && digit_count + 1 == length && digit_count > 0) {
        exponent = -(int)after_r;
        kind = digits ? RESISTOR_R_DECIMAL : RESISTOR_JUMPER;
    } else if (length == 3 && code[0] >= '0' && code[0] <= '9' &&
               code[1] >= '0' && code[1] <= '9') {
        unsigned index = (unsigned)(code[0] - '0') * 10 + (unsigned)(code[1] - '0');
        if (index == 0 || index > 96) return RESISTOR_BAD_CODE;
        if (code[2] >= 'A' && code[2] <= 'H') exponent = code[2] - 'A';
        else if (code[2] == 'X') exponent = -1;
        else if (code[2] == 'Y') exponent = -2;
        else if (code[2] == 'Z') exponent = -3;
        else return RESISTOR_BAD_CODE;
        digits = e96[index - 1];
        kind = RESISTOR_EIA96;
    } else return RESISTOR_BAD_CODE;
    result->ohms = scale(digits, exponent);
    result->significand = digits;
    result->exponent10 = exponent;
    result->kind = kind;
    return RESISTOR_OK;
}

const char *resistor_error(resistor_status_t status)
{
    switch (status) {
    case RESISTOR_OK: return "OK";
    case RESISTOR_BAD_BANDS: return "Choose four or five color bands.";
    case RESISTOR_BAD_DIGIT: return "Digits use black through white; the first digit cannot be black.";
    case RESISTOR_BAD_MULTIPLIER: return "Choose a multiplier from silver (x0.01) through white (x1G).";
    case RESISTOR_BAD_TOLERANCE: return "Choose a supported tolerance color.";
    case RESISTOR_BAD_CODE: return "Use 3/4 digits, an R decimal, or EIA-96 (01-96 + A-H/X/Y/Z). Uppercase, no spaces.";
    case RESISTOR_CODE_TOO_LONG: return "Marking is too long: enter at most four ASCII characters.";
    default: return "Invalid resistor input.";
    }
}

void resistor_data_self_test(void)
{
#ifndef NDEBUG
    resistor_data_t data;
    const resistor_color_t colors[] = {RESISTOR_YELLOW, RESISTOR_VIOLET, RESISTOR_RED, RESISTOR_GOLD};
    assert(resistor_decode_bands(colors, 4, &data) == RESISTOR_OK && data.ohms == 4700 && data.tolerance_percent == 5);
    assert(resistor_decode_smd("103", &data) == RESISTOR_OK && data.ohms == 10000);
    assert(resistor_decode_smd("4R7", &data) == RESISTOR_OK && data.ohms == 4.7);
    assert(resistor_decode_smd("10C", &data) == RESISTOR_OK && data.ohms == 12400);
    assert(resistor_decode_smd("97A", &data) == RESISTOR_BAD_CODE && data.ohms == 0);
#endif
}
