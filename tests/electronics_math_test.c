#include "electronics_math.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>

static void near(double actual, double expected)
{
    if (fabs(actual - expected) > fabs(expected) * 1e-12 + 1e-15)
        fprintf(stderr, "Expected %.17g, got %.17g\n", expected, actual);
    assert(fabs(actual - expected) <= fabs(expected) * 1e-12 + 1e-15);
}

int main(void)
{
    electronics_math_self_test();
    double value = 42;
    const char *invalid[] = {
        "", ".", "0", "-1", "+1", "1e3", "NaN", "inf", "0x10", "1..2", "2 ohm",
        " 2", "2 ", "1\n", "1000000000001", "0.0000000001", "0000000000000000000000001",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(!electronics_parse_positive(invalid[i], &value));
        assert(value == 42);
    }
    assert(!electronics_parse_positive(NULL, &value));
    assert(!electronics_parse_positive("1", NULL));
    assert(electronics_parse_positive("000000000000000000000001", &value) && value == 1);
    assert(electronics_parse_positive("0.000000001", &value) && value == ELECTRONICS_INPUT_MIN);
    assert(electronics_parse_positive("1000000000000", &value) && value == ELECTRONICS_INPUT_LIMIT);
    assert(electronics_parse_positive(".5", &value) && value == 0.5);
    assert(electronics_parse_positive("25.", &value) && value == 25);

    electronics_ohm_result_t ohm;
    assert(electronics_ohm(ELECTRONICS_SOLVE_VOLTAGE, NAN, 0.02, 150, &ohm));
    near(ohm.voltage_v, 3);
    near(ohm.power_w, 0.06);
    assert(electronics_ohm(ELECTRONICS_SOLVE_CURRENT, 12, NAN, 1200, &ohm));
    near(ohm.current_a, 0.01);
    near(ohm.power_w, 0.12);
    assert(electronics_ohm(ELECTRONICS_SOLVE_RESISTANCE, 3.3, 0.01, NAN, &ohm));
    near(ohm.resistance_ohm, 330);
    double saved = ohm.resistance_ohm;
    assert(!electronics_ohm(ELECTRONICS_SOLVE_CURRENT, 5, 0, 0, &ohm));
    assert(!electronics_ohm(ELECTRONICS_SOLVE_CURRENT, -5, 0, 10, &ohm));
    assert(!electronics_ohm(ELECTRONICS_SOLVE_CURRENT, INFINITY, 0, 10, &ohm));
    assert(!electronics_ohm(ELECTRONICS_SOLVE_VOLTAGE, 0, DBL_MAX, 2, &ohm));
    assert(!electronics_ohm(ELECTRONICS_SOLVE_CURRENT, DBL_MAX, 0, DBL_MIN, &ohm));
    assert(!electronics_ohm(ELECTRONICS_SOLVE_CURRENT, DBL_MIN, 0, DBL_MAX, &ohm));
    assert(!electronics_ohm((electronics_unknown_t)3, 5, 1, 5, &ohm));
    assert(!electronics_ohm(ELECTRONICS_SOLVE_CURRENT, 5, 0, 10, NULL));
    assert(ohm.resistance_ohm == saved);

    electronics_divider_result_t divider;
    assert(electronics_divider(12, 1000, 2000, &divider));
    near(divider.output_v, 8);
    near(divider.current_a, 0.004);
    near(divider.top_power_w, 0.016);
    near(divider.bottom_power_w, 0.032);
    near(divider.top_power_w + divider.bottom_power_w, 12 * divider.current_a);
    assert(!electronics_divider(12, 0, 2000, &divider));
    assert(!electronics_divider(NAN, 1000, 2000, &divider));
    assert(!electronics_divider(12, DBL_MAX, DBL_MAX, &divider));
    assert(!electronics_divider(DBL_MAX, 1, 1, &divider));
    assert(!electronics_divider(DBL_MIN, DBL_MAX, 1, &divider));
    assert(!electronics_divider(12, 1000, 2000, NULL));
    assert(divider.output_v == 8);

    electronics_led_result_t led;
    assert(electronics_led(5, 2, 0.02, &led));
    near(led.resistance_ohm, 150);
    near(led.resistor_power_w, 0.06);
    near(led.led_power_w, 0.04);
    assert(!electronics_led(2, 2, 0.02, &led));
    assert(!electronics_led(1, 2, 0.02, &led));
    assert(!electronics_led(5, 2, 0, &led));
    assert(!electronics_led(5, -2, 0.02, &led));
    assert(!electronics_led(INFINITY, 2, 0.02, &led));
    assert(!electronics_led(5, 2, NAN, &led));
    assert(!electronics_led(DBL_MAX, 2, DBL_MIN, &led));
    assert(!electronics_led(DBL_MAX, 2, 2, &led));
    assert(!electronics_led(5, 2, 0.02, NULL));
    assert(led.resistance_ohm == 150);
    electronics_rc_result_t rc;
    assert(electronics_rc(10000, 0.0000001, &rc));
    near(rc.time_constant_s, 0.001);
    near(rc.cutoff_hz, 159.15494309189535);
    near(rc.rise_10_90_s, 0.0021972245773362196);
    near(rc.settling_1_percent_s, 0.004605170185988091);
    /* Check the settling result against the independent step-response form. */
    near(exp(-rc.settling_1_percent_s / rc.time_constant_s), 0.01);
    assert(electronics_rc(4700, 0.00001, &rc));
    near(rc.time_constant_s, 0.047);
    near(rc.cutoff_hz, 3.386275384933943);
    near(rc.rise_10_90_s, 0.10326955513480232);
    near(rc.settling_1_percent_s, 0.2164429987414403);
    electronics_rc_result_t saved_rc = rc;
    const double invalid_rc[] = {0, -1, NAN, INFINITY, -INFINITY};
    for (size_t i = 0; i < sizeof(invalid_rc) / sizeof(invalid_rc[0]); i++) {
        assert(!electronics_rc(invalid_rc[i], 0.00001, &rc));
        assert(!electronics_rc(4700, invalid_rc[i], &rc));
    }
    assert(!electronics_rc(DBL_MAX, 2, &rc));
    assert(!electronics_rc(DBL_MAX, 1, &rc)); /* Settling overflows. */
    assert(!electronics_rc(DBL_MIN, DBL_MIN, &rc)); /* Time constant underflows. */
    assert(!electronics_rc(DBL_MIN, 0.00001, &rc)); /* Cutoff overflows. */
    assert(!electronics_rc(4700, 0.00001, NULL));
    assert(rc.time_constant_s == saved_rc.time_constant_s && rc.cutoff_hz == saved_rc.cutoff_hz);
    assert(rc.rise_10_90_s == saved_rc.rise_10_90_s &&
           rc.settling_1_percent_s == saved_rc.settling_1_percent_s);
    puts("electronics math tests passed");
    return 0;
}
