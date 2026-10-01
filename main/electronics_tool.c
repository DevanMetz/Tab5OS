#include "electronics_tool.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "electronics_math.h"

enum { MODE_OHM, MODE_DIVIDER, MODE_LED, MODE_RC, MODE_COUNT };

static unsigned mode;
static electronics_unknown_t unknown = ELECTRONICS_SOLVE_CURRENT;
/* The three Ohm selections have independent input pairs. Retain rejected UTF-8
 * and one extra character so an overlong edit cannot become a valid prefix. */
static char inputs[6][3][(ELECTRONICS_INPUT_MAX + 1) * 4 + 1] = {
    {"20", "150", ""},
    {"5", "1000", ""},
    {"3.3", "10", ""},
    {"5", "10000", "10000"},
    {"5", "2", "20"},
    {"10000", "0.1", ""},
};
static lv_obj_t *mode_buttons[MODE_COUNT];
static lv_obj_t *unknown_row;
static lv_obj_t *unknown_select;
static lv_obj_t *field_rows[3];
static lv_obj_t *field_labels[3];
static lv_obj_t *fields[3];
static lv_obj_t *help_label;
static lv_obj_t *result_label;
static lv_obj_t *note_label;
static lv_obj_t *keyboard;
static lv_obj_t *keys_label;
static bool updating;

static unsigned input_set(void)
{
    return mode == MODE_OHM ? (unsigned)unknown : mode + 2;
}

static unsigned input_count(void)
{
    return mode == MODE_OHM || mode == MODE_RC ? 2 : 3;
}

static void calculate(void)
{
    if (!result_label) return;
    double values[3] = {0};
    unsigned count = input_count();
    char text[320];
    for (unsigned i = 0; i < count; i++) {
        if (!electronics_parse_positive(inputs[input_set()][i], &values[i])) {
            snprintf(text, sizeof(text), "%s: enter a decimal from\n0.000000001 to 1000000000000.\nNo spaces or unit suffixes.",
                     lv_label_get_text(field_labels[i]));
            lv_label_set_text(result_label, text);
            return;
        }
    }

    bool valid;
    if (mode == MODE_OHM) {
        electronics_ohm_result_t result;
        double voltage = unknown == ELECTRONICS_SOLVE_VOLTAGE ? 0 : values[0];
        double current = unknown == ELECTRONICS_SOLVE_VOLTAGE ? values[0] / 1000 :
                         unknown == ELECTRONICS_SOLVE_RESISTANCE ? values[1] / 1000 : 0;
        double resistance = unknown == ELECTRONICS_SOLVE_RESISTANCE ? 0 : values[1];
        valid = electronics_ohm(unknown, voltage, current, resistance, &result);
        if (valid) {
            snprintf(text, sizeof(text), "Voltage: %.6g V\nCurrent: %.6g mA\nResistance: %.6g ohm\nPower: %.6g W",
                     result.voltage_v, result.current_a * 1000, result.resistance_ohm, result.power_w);
        }
    } else if (mode == MODE_DIVIDER) {
        electronics_divider_result_t result;
        valid = electronics_divider(values[0], values[1], values[2], &result);
        if (valid) {
            snprintf(text, sizeof(text), "Output: %.6g V\nCurrent: %.6g mA\nTop resistor: %.6g W\nBottom resistor: %.6g W",
                     result.output_v, result.current_a * 1000, result.top_power_w, result.bottom_power_w);
        }
    } else if (mode == MODE_LED) {
        if (values[0] <= values[1]) {
            lv_label_set_text(result_label, "Supply voltage must be higher\nthan the LED forward voltage.");
            return;
        }
        electronics_led_result_t result;
        valid = electronics_led(values[0], values[1], values[2] / 1000, &result);
        if (valid) {
            snprintf(text, sizeof(text), "Series resistor: %.6g ohm\nResistor dissipation: %.6g W\nLED dissipation: %.6g W\nTarget current: %.6g mA",
                     result.resistance_ohm, result.resistor_power_w, result.led_power_w, values[2]);
        }
    } else {
        electronics_rc_result_t result;
        valid = electronics_rc(values[0], values[1] / 1000000, &result);
        if (valid) {
            snprintf(text, sizeof(text), "Time constant: %.6g ms\nCutoff (-3 dB): %.6g Hz\n"
                     "10-90%% rise: %.6g ms\n1%% settling: %.6g ms",
                     result.time_constant_s * 1000, result.cutoff_hz,
                     result.rise_10_90_s * 1000, result.settling_1_percent_s * 1000);
        }
    }
    lv_label_set_text(result_label, valid ? text : "Result is outside the calculation range.\nCheck the input values and units.");
}

static void keyboard_visible(bool visible)
{
    if (!keyboard) return;
    if (visible) lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(keys_label, visible ? "Hide keys" : "Show keys");
}

static void calculate_clicked(lv_event_t *event)
{
    (void)event;
    calculate();
}

static void keys_clicked(lv_event_t *event)
{
    (void)event;
    keyboard_visible(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
}

static void keyboard_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY) calculate();
    keyboard_visible(false);
}

static void field_event(lv_event_t *event)
{
    if (updating || !keyboard) return;
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *field = lv_event_get_target_obj(event);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_textarea(keyboard, field);
        keyboard_visible(true);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
        snprintf(inputs[input_set()][index], sizeof(inputs[0][0]), "%s", lv_textarea_get_text(field));
        lv_label_set_text(result_label, "Inputs changed. Tap Calculate.");
    }
}

static void update_mode(void)
{
    static const char *const labels[6][3] = {
        {"Current (mA)", "Resistance (ohm)", ""},
        {"Voltage (V)", "Resistance (ohm)", ""},
        {"Voltage (V)", "Current (mA)", ""},
        {"Input (V)", "Top R (ohm)", "Bottom R (ohm)"},
        {"Supply (V)", "LED forward (V)", "LED current (mA)"},
        {"Resistance (ohm)", "Capacitance (uF)", ""},
    };
    static const char *const help[] = {
        "Find one unknown from two values. V = I x R; P = V x I.",
        "Vin -- Top R -- Vout -- Bottom R -- GND\nOutput is measured across the bottom resistor, with no load.",
        "Supply -- series resistor -- one LED -- GND\nUse the LED's forward voltage at your intended current.",
        "Vin -- R -- Vout; C from Vout to GND.\nCapacitance is in microfarads: 0.1 uF = 100 nF.",
    };
    static const char *const notes[] = {
        "Ideal DC resistor calculation. Inputs are retained until restart.\nDecimal inputs: 0.000000001 to 1000000000000; up to 24 characters.",
        "A connected load changes Vout. Check actual component tolerances.\nDecimal inputs: 0.000000001 to 1000000000000; up to 24 characters.",
        "Choose an equal or higher resistance and a power rating above the\n"
        "calculated dissipation. Values assume a fixed LED forward voltage.\n"
        "Decimal inputs: 0.000000001 to 1000000000000; up to 24 characters.",
        "Ideal RC low-pass: zero source impedance and no output load.\n"
        "Settling means within 1% of the final step value. Actual component\n"
        "tolerances and loading change the result. Inputs stay in RAM.",
    };
    updating = true;
    for (unsigned i = 0; i < MODE_COUNT; i++) {
        if (i == mode) lv_obj_add_state(mode_buttons[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(mode_buttons[i], LV_STATE_CHECKED);
    }
    for (unsigned i = 0; i < 3; i++) {
        lv_label_set_text(field_labels[i], labels[input_set()][i]);
        lv_textarea_set_text(fields[i], inputs[input_set()][i]);
    }
    if (mode == MODE_OHM) {
        lv_obj_remove_flag(unknown_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(unknown_row, LV_OBJ_FLAG_HIDDEN);
    }
    if (input_count() == 2) {
        lv_obj_add_flag(field_rows[2], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(field_rows[2], LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(help_label, help[mode]);
    lv_label_set_text(note_label, notes[mode]);
    lv_keyboard_set_textarea(keyboard, fields[0]);
    updating = false;
    calculate();
}

static void mode_clicked(lv_event_t *event)
{
    mode = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    update_mode();
}

static void unknown_changed(lv_event_t *event)
{
    (void)event;
    unknown = (electronics_unknown_t)lv_dropdown_get_selected(unknown_select);
    update_mode();
}

static lv_obj_t *row(lv_obj_t *parent, int height)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, 640, height);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(object, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(object, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return object;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int width,
                        lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_set_size(object, width, 60);
    lv_obj_t *label = lv_label_create(object);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, user_data);
    return object;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, bool small)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_width(object, 640);
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    if (small) lv_obj_set_style_text_font(object, &lv_font_montserrat_14, 0);
    lv_label_set_text(object, text);
    return object;
}

void electronics_tool_stop(void)
{
    if (keyboard) lv_keyboard_set_textarea(keyboard, NULL);
    keyboard = NULL;
    keys_label = NULL;
    unknown_row = NULL;
    unknown_select = NULL;
    help_label = NULL;
    result_label = NULL;
    note_label = NULL;
    for (unsigned i = 0; i < MODE_COUNT; i++) mode_buttons[i] = NULL;
    for (unsigned i = 0; i < 3; i++) {
        field_rows[i] = NULL;
        field_labels[i] = NULL;
        fields[i] = NULL;
    }
    updating = false;
}

void electronics_tool_show(lv_obj_t *parent)
{
    electronics_tool_stop();
    lv_obj_set_style_pad_row(parent, 12, 0);
    lv_obj_t *column = lv_obj_create(parent);
    lv_obj_remove_style_all(column);
    lv_obj_set_size(column, 640, LV_SIZE_CONTENT);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 12, 0);
    lv_obj_set_style_text_font(column, &lv_font_montserrat_28, 0);
    label(column, "Electronics Calculator", false);

    lv_obj_t *modes = NULL;
    static const char *const names[] = {"Ohm's law", "Divider", "LED resistor", "RC filter"};
    for (unsigned i = 0; i < MODE_COUNT; i++) {
        if (i % 2 == 0) modes = row(column, 60);
        mode_buttons[i] = button(modes, names[i], 310, mode_clicked, (void *)(uintptr_t)i);
        lv_obj_set_style_bg_color(mode_buttons[i], lv_color_hex(0x00695C), LV_STATE_CHECKED);
    }
    unknown_row = row(column, 60);
    lv_obj_t *solve = lv_label_create(unknown_row);
    lv_label_set_text(solve, "Solve for");
    unknown_select = lv_dropdown_create(unknown_row);
    lv_obj_set_size(unknown_select, 400, 60);
    lv_dropdown_set_options(unknown_select, "Voltage (V)\nCurrent (mA)\nResistance (ohm)");
    lv_dropdown_set_selected(unknown_select, unknown);
    lv_obj_add_event_cb(unknown_select, unknown_changed, LV_EVENT_VALUE_CHANGED, NULL);
    help_label = label(column, "", true);

    for (unsigned i = 0; i < 3; i++) {
        field_rows[i] = row(column, 64);
        field_labels[i] = lv_label_create(field_rows[i]);
        lv_obj_set_width(field_labels[i], 275);
        fields[i] = lv_textarea_create(field_rows[i]);
        lv_obj_set_size(fields[i], 354, 64);
        /* Preserve pasted newlines; the decimal parser rejects them. */
        lv_textarea_set_max_length(fields[i], ELECTRONICS_INPUT_MAX + 1);
        lv_obj_add_event_cb(fields[i], field_event, LV_EVENT_ALL, (void *)(uintptr_t)i);
    }

    lv_obj_t *actions = row(column, 60);
    button(actions, "Calculate", 310, calculate_clicked, NULL);
    lv_obj_t *keys = button(actions, "Hide keys", 310, keys_clicked, NULL);
    keys_label = lv_obj_get_child(keys, 0);
    result_label = label(column, "", false);
    lv_obj_set_height(result_label, 160);
    note_label = label(column, "", true);
    keyboard = lv_keyboard_create(column);
    lv_obj_set_size(keyboard, 640, 246);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_CANCEL, NULL);
    update_mode();
    keyboard_visible(false);
}
