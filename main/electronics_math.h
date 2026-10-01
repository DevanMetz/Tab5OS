#pragma once

#include <stdbool.h>

#define ELECTRONICS_INPUT_MAX 24
#define ELECTRONICS_INPUT_MIN 0.000000001
#define ELECTRONICS_INPUT_LIMIT 1000000000000.0

typedef enum {
    ELECTRONICS_SOLVE_VOLTAGE,
    ELECTRONICS_SOLVE_CURRENT,
    ELECTRONICS_SOLVE_RESISTANCE,
} electronics_unknown_t;

typedef struct {
    double voltage_v;
    double current_a;
    double resistance_ohm;
    double power_w;
} electronics_ohm_result_t;

typedef struct {
    double output_v;
    double current_a;
    double top_power_w;
    double bottom_power_w;
} electronics_divider_result_t;

typedef struct {
    double resistance_ohm;
    double resistor_power_w;
    double led_power_w;
} electronics_led_result_t;

typedef struct {
    double time_constant_s;
    double cutoff_hz;
    double rise_10_90_s;
    double settling_1_percent_s;
} electronics_rc_result_t;

/* Decimal digits and one optional dot, 1e-9..1e12, at most 24 characters.
 * No whitespace, signs, exponent, or unit suffixes. Output changes on success only. */
bool electronics_parse_positive(const char *text, double *value);

/* Calculations use SI units and require positive finite inputs and outputs.
 * The selected unknown input is ignored. Results change on success only. */
bool electronics_ohm(electronics_unknown_t unknown, double voltage_v,
                     double current_a, double resistance_ohm,
                     electronics_ohm_result_t *result);
bool electronics_divider(double input_v, double top_ohm, double bottom_ohm,
                         electronics_divider_result_t *result);
bool electronics_led(double supply_v, double forward_v, double current_a,
                     electronics_led_result_t *result);
/* Ideal unloaded one-pole RC low-pass, with zero source impedance. Capacitance
 * is in farads. Settling is to within 1% of the final step value. */
bool electronics_rc(double resistance_ohm, double capacitance_f,
                    electronics_rc_result_t *result);
void electronics_math_self_test(void);
