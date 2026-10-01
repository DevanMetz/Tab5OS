#include "electronics_math.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

static bool positive(double value)
{
    return isfinite(value) && value > 0;
}

bool electronics_parse_positive(const char *text, double *value)
{
    if (!text || !value) return false;
    bool dot = false;
    bool digit = false;
    size_t length = 0;
    while (text[length]) {
        if (length == ELECTRONICS_INPUT_MAX) return false;
        char byte = text[length++];
        if (byte >= '0' && byte <= '9') digit = true;
        else if (byte == '.' && !dot) dot = true;
        else return false;
    }
    if (!digit) return false;
    errno = 0;
    char *end;
    double parsed = strtod(text, &end);
    if (errno == ERANGE || *end || !isfinite(parsed) ||
        parsed < ELECTRONICS_INPUT_MIN || parsed > ELECTRONICS_INPUT_LIMIT) return false;
    *value = parsed;
    return true;
}

bool electronics_ohm(electronics_unknown_t unknown, double voltage_v,
                     double current_a, double resistance_ohm,
                     electronics_ohm_result_t *result)
{
    if (!result) return false;
    switch (unknown) {
    case ELECTRONICS_SOLVE_VOLTAGE:
        if (!positive(current_a) || !positive(resistance_ohm)) return false;
        voltage_v = current_a * resistance_ohm;
        break;
    case ELECTRONICS_SOLVE_CURRENT:
        if (!positive(voltage_v) || !positive(resistance_ohm)) return false;
        current_a = voltage_v / resistance_ohm;
        break;
    case ELECTRONICS_SOLVE_RESISTANCE:
        if (!positive(voltage_v) || !positive(current_a)) return false;
        resistance_ohm = voltage_v / current_a;
        break;
    default:
        return false;
    }
    double power_w = voltage_v * current_a;
    if (!positive(voltage_v) || !positive(current_a) ||
        !positive(resistance_ohm) || !positive(power_w)) return false;
    *result = (electronics_ohm_result_t){voltage_v, current_a, resistance_ohm, power_w};
    return true;
}

bool electronics_divider(double input_v, double top_ohm, double bottom_ohm,
                         electronics_divider_result_t *result)
{
    if (!result || !positive(input_v) || !positive(top_ohm) || !positive(bottom_ohm)) return false;
    double total_ohm = top_ohm + bottom_ohm;
    if (!positive(total_ohm)) return false;
    double current_a = input_v / total_ohm;
    double output_v = current_a * bottom_ohm;
    double top_power_w = (current_a * top_ohm) * current_a;
    double bottom_power_w = output_v * current_a;
    if (!positive(current_a) || !positive(output_v) ||
        !positive(top_power_w) || !positive(bottom_power_w)) return false;
    *result = (electronics_divider_result_t){output_v, current_a, top_power_w, bottom_power_w};
    return true;
}

bool electronics_led(double supply_v, double forward_v, double current_a,
                     electronics_led_result_t *result)
{
    if (!result || !positive(supply_v) || !positive(forward_v) ||
        !positive(current_a) || supply_v <= forward_v) return false;
    double drop_v = supply_v - forward_v;
    double resistance_ohm = drop_v / current_a;
    double resistor_power_w = drop_v * current_a;
    double led_power_w = forward_v * current_a;
    if (!positive(resistance_ohm) || !positive(resistor_power_w) || !positive(led_power_w)) return false;
    *result = (electronics_led_result_t){resistance_ohm, resistor_power_w, led_power_w};
    return true;
}

bool electronics_rc(double resistance_ohm, double capacitance_f,
                    electronics_rc_result_t *result)
{
    if (!result || !positive(resistance_ohm) || !positive(capacitance_f)) return false;
    double time_constant_s = resistance_ohm * capacitance_f;
    if (!positive(time_constant_s)) return false;
    const double pi = 3.14159265358979323846;
    double cutoff_hz = (0.5 / pi) / time_constant_s;
    double rise_10_90_s = log(9.0) * time_constant_s;
    double settling_1_percent_s = log(100.0) * time_constant_s;
    if (!positive(cutoff_hz) || !positive(rise_10_90_s) ||
        !positive(settling_1_percent_s)) return false;
    *result = (electronics_rc_result_t){time_constant_s, cutoff_hz,
                                       rise_10_90_s, settling_1_percent_s};
    return true;
}

void electronics_math_self_test(void)
{
#ifndef NDEBUG
    double value;
    electronics_ohm_result_t ohm;
    electronics_divider_result_t divider;
    electronics_led_result_t led;
    electronics_rc_result_t rc;
    assert(electronics_parse_positive(".02", &value) && value == 0.02);
    assert(!electronics_parse_positive("nan", &value));
    assert(!electronics_parse_positive("1e3", &value));
    assert(!electronics_parse_positive("0", &value));
    assert(electronics_ohm(ELECTRONICS_SOLVE_CURRENT, 5, 0, 1000, &ohm));
    assert(fabs(ohm.current_a - 0.005) < 1e-12 && fabs(ohm.power_w - 0.025) < 1e-12);
    assert(electronics_divider(5, 10000, 10000, &divider));
    assert(divider.output_v == 2.5);
    assert(electronics_led(5, 2, 0.02, &led) && led.resistance_ohm == 150);
    assert(!electronics_led(2, 2, 0.02, &led));
    assert(electronics_rc(10000, 0.0000001, &rc));
    assert(fabs(rc.time_constant_s - 0.001) < 1e-12);
    assert(fabs(rc.cutoff_hz - 159.154943091895) < 1e-9);
#endif
}
