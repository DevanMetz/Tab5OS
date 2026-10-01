#include "resistor_data.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void near(double actual, double expected)
{
    assert(fabs(actual - expected) <= fmax(fabs(expected) * 1e-12, 1e-15));
}

static void cleared(const resistor_data_t *result)
{
    resistor_data_t zero;
    memset(&zero, 0, sizeof(zero));
    assert(memcmp(result, &zero, sizeof(zero)) == 0);
}

int main(void)
{
    resistor_data_self_test();
    resistor_data_t result;
    const struct { const char *code; double ohms; resistor_kind_t kind; } examples[] = {
        {"103", 10000, RESISTOR_NUMERIC}, {"4422", 44200, RESISTOR_NUMERIC},
        {"100", 10, RESISTOR_NUMERIC}, {"1000", 100, RESISTOR_NUMERIC},
        {"9999", 999e9, RESISTOR_NUMERIC}, {"010", 1, RESISTOR_NUMERIC},
        {"4R7", 4.7, RESISTOR_R_DECIMAL}, {"7R5", 7.5, RESISTOR_R_DECIMAL},
        {"8R25", 8.25, RESISTOR_R_DECIMAL}, {"R005", 0.005, RESISTOR_R_DECIMAL},
        {"R1", 0.1, RESISTOR_R_DECIMAL}, {"1R", 1, RESISTOR_R_DECIMAL},
        {"0", 0, RESISTOR_JUMPER}, {"00", 0, RESISTOR_JUMPER},
        {"000", 0, RESISTOR_JUMPER}, {"0000", 0, RESISTOR_JUMPER},
        {"0R0", 0, RESISTOR_JUMPER}, {"01A", 100, RESISTOR_EIA96},
        {"10C", 12400, RESISTOR_EIA96}, {"96F", 97600000, RESISTOR_EIA96},
        {"01Z", 0.1, RESISTOR_EIA96}, {"96H", 9760000000, RESISTOR_EIA96}
    };
    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
        memset(&result, 0xff, sizeof(result));
        assert(resistor_decode_smd(examples[i].code, &result) == RESISTOR_OK);
        near(result.ohms, examples[i].ohms);
        assert(result.kind == examples[i].kind && !result.tolerance_known);
        assert(result.tolerance_percent == 0 && result.minimum_ohms == 0 && result.maximum_ohms == 0);
    }
    /* E96 values independently generated from the geometric series rounded to
     * three significant figures; checks every stored table entry and multiplier. */
    const char letters[] = "ABCDEFGHXYZ";
    unsigned reference_count = 0;
    for (unsigned i = 1; i <= 96; i++) {
        double significant = round(100 * pow(10, (double)(i - 1) / 96));
        for (size_t j = 0; j < sizeof(letters) - 1; j++) {
            char code[5]; snprintf(code, sizeof(code), "%02u%c", i, letters[j]);
            assert(resistor_decode_smd(code, &result) == RESISTOR_OK);
            int power = j < 8 ? (int)j : -(int)(j - 7);
            near(result.ohms, significant * pow(10, power));
            assert(result.significand == (unsigned)significant && result.exponent10 == power);
            reference_count++;
        }
    }
    const char *const bad[] = {
        "", "1", "47", "R", "RR", "1RR", "4r7", " 103", "103 ", "1 3", "10\t",
        "4.7", "-103", "+103", "1e3", "4K7", "1M0", "00A", "97A", "99H",
        "01I", "01J", "01S", "01a", "A01", "1A0", "009", "0009", "0x10",
        "10300", "123456789", "\xc2\xb9", "10\n", "10/", "01\x7f"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(&result, 0xff, sizeof(result));
        assert(resistor_decode_smd(bad[i], &result) != RESISTOR_OK);
        cleared(&result);
    }
    assert(resistor_decode_smd(NULL, &result) == RESISTOR_BAD_ARGUMENT); cleared(&result);
    assert(resistor_decode_smd("103", NULL) == RESISTOR_BAD_ARGUMENT);
    assert(resistor_decode_bands(NULL, 4, &result) == RESISTOR_BAD_ARGUMENT); cleared(&result);
    const resistor_color_t four[] = {RESISTOR_YELLOW, RESISTOR_VIOLET, RESISTOR_RED, RESISTOR_GOLD};
    assert(resistor_decode_bands(four, 4, &result) == RESISTOR_OK);
    near(result.ohms, 4700); near(result.minimum_ohms, 4465); near(result.maximum_ohms, 4935);
    assert(result.tolerance_known && result.tolerance_percent == 5);
    const resistor_color_t five[] = {RESISTOR_RED, RESISTOR_ORANGE, RESISTOR_VIOLET, RESISTOR_BLACK, RESISTOR_BROWN};
    assert(resistor_decode_bands(five, 5, &result) == RESISTOR_OK);
    near(result.ohms, 237); near(result.minimum_ohms, 234.63); near(result.maximum_ohms, 239.37);
    assert(result.tolerance_percent == 1 && result.significand == 237 && result.exponent10 == 0);
    const struct { resistor_color_t color; double percent; } tolerance_cases[] = {
        {RESISTOR_BROWN, 1}, {RESISTOR_RED, 2}, {RESISTOR_GREEN, .5}, {RESISTOR_BLUE, .25},
        {RESISTOR_VIOLET, .1}, {RESISTOR_GRAY, .05}, {RESISTOR_GOLD, 5}, {RESISTOR_SILVER, 10}
    };
    for (unsigned multiplier = 0; multiplier < RESISTOR_COLOR_COUNT; multiplier++) {
        for (size_t i = 0; i < sizeof(tolerance_cases) / sizeof(tolerance_cases[0]); i++) {
            resistor_color_t bands[] = {RESISTOR_BROWN, RESISTOR_BLACK, (resistor_color_t)multiplier, tolerance_cases[i].color};
            assert(resistor_decode_bands(bands, 4, &result) == RESISTOR_OK);
            double expected = multiplier < 10 ? pow(10, (int)multiplier + 1) : multiplier == RESISTOR_GOLD ? 1 : .1;
            near(result.ohms, expected);
            near(result.tolerance_percent, tolerance_cases[i].percent);
            near(result.minimum_ohms + result.maximum_ohms, 2 * expected);
            near(result.maximum_ohms - expected, expected * tolerance_cases[i].percent / 100);
        }
    }
    resistor_color_t maximum[] = {RESISTOR_WHITE, RESISTOR_WHITE, RESISTOR_WHITE, RESISTOR_WHITE, RESISTOR_SILVER};
    assert(resistor_decode_bands(maximum, 5, &result) == RESISTOR_OK);
    near(result.ohms, 999e9); near(result.maximum_ohms, 1098.9e9);
    for (unsigned count = 0; count < 8; count++) {
        if (count == 4 || count == 5) continue;
        assert(resistor_decode_bands(four, count, &result) == RESISTOR_BAD_BANDS); cleared(&result);
    }
    for (unsigned index = 0; index < 4; index++) {
        resistor_color_t bands[4]; memcpy(bands, four, sizeof(bands));
        bands[index] = (resistor_color_t)-1;
        assert(resistor_decode_bands(bands, 4, &result) != RESISTOR_OK); cleared(&result);
        bands[index] = RESISTOR_COLOR_COUNT;
        assert(resistor_decode_bands(bands, 4, &result) != RESISTOR_OK); cleared(&result);
    }
    resistor_color_t invalid[] = {RESISTOR_BLACK, RESISTOR_RED, RESISTOR_BLACK, RESISTOR_GOLD};
    assert(resistor_decode_bands(invalid, 4, &result) == RESISTOR_BAD_DIGIT); cleared(&result);
    invalid[0] = RESISTOR_BROWN; invalid[3] = RESISTOR_BLACK;
    assert(resistor_decode_bands(invalid, 4, &result) == RESISTOR_BAD_TOLERANCE); cleared(&result);
    invalid[1] = RESISTOR_GOLD;
    assert(resistor_decode_bands(invalid, 4, &result) == RESISTOR_BAD_DIGIT); cleared(&result);
    assert(resistor_decode_bands(four, 4, NULL) == RESISTOR_BAD_ARGUMENT);
    printf("Resistor known values, band roles/tolerances, malformed markings, and %u EIA-96 references passed\n", reference_count);
    return 0;
}
