#include "resistor_tool.h"

#include <stdio.h>
#include <string.h>

#include "resistor_data.h"

/* Drafts are RAM-only. Each band format retains its own selection. */
static unsigned mode;
static resistor_color_t choices[2][5] = {
    {RESISTOR_YELLOW, RESISTOR_VIOLET, RESISTOR_RED, RESISTOR_GOLD, RESISTOR_BROWN},
    {RESISTOR_BROWN, RESISTOR_BLACK, RESISTOR_BLACK, RESISTOR_RED, RESISTOR_BROWN}
};
static char saved_code[(RESISTOR_CODE_MAX + 1) * 4 + 1] = "103";
static lv_obj_t *mode_select;
static lv_obj_t *band_panel;
static lv_obj_t *band_rows[3];
static lv_obj_t *band_blocks[5];
static lv_obj_t *band_captions[5];
static lv_obj_t *band_select[5];
static lv_obj_t *stripes[5];
static lv_obj_t *stripe_numbers[5];
static lv_obj_t *smd_panel;
static lv_obj_t *code_field;
static lv_obj_t *keyboard;
static lv_obj_t *result_label;
static lv_obj_t *range_label;
static lv_obj_t *detail_label;
static lv_obj_t *note_label;

static const resistor_color_t tolerances[] = {
    RESISTOR_BROWN, RESISTOR_RED, RESISTOR_GREEN, RESISTOR_BLUE,
    RESISTOR_VIOLET, RESISTOR_GRAY, RESISTOR_GOLD, RESISTOR_SILVER
};
static const char *const digit_options =
    "Black  0\nBrown  1\nRed  2\nOrange  3\nYellow  4\nGreen  5\nBlue  6\nViolet  7\nGray  8\nWhite  9";
static const char *const multiplier_options =
    "Black  x1\nBrown  x10\nRed  x100\nOrange  x1k\nYellow  x10k\nGreen  x100k\nBlue  x1M\nViolet  x10M\nGray  x100M\nWhite  x1G\nGold  x0.1\nSilver  x0.01";
static const char *const tolerance_options =
    "Brown  1%\nRed  2%\nGreen  0.5%\nBlue  0.25%\nViolet  0.1%\nGray  0.05%\nGold  5%\nSilver  10%";
static const uint32_t colors[] = {
    0x191919, 0x854D27, 0xDD302F, 0xFA871B, 0xF9D83B, 0x269C52,
    0x2E72D2, 0x8B4BB3, 0x838A91, 0xF6F6F3, 0xC7A23D, 0xBAC4CB
};

static const char *const key_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "R", "A", "B", "C", "D", "E", "F", "G", "H", "\n",
    "X", "Y", "Z", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t key_controls[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, LV_BUTTONMATRIX_CTRL_CHECKED | 1, LV_BUTTONMATRIX_CTRL_CHECKED | 1,
    LV_BUTTONMATRIX_CTRL_CHECKED | 2, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2
};
_Static_assert(sizeof(key_map) / sizeof(key_map[0]) ==
               sizeof(key_controls) / sizeof(key_controls[0]) + 3, "Resistor keyboard layout");

static void visible(lv_obj_t *object, bool show)
{
    if (show) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void format_ohms(double ohms, char text[48])
{
    double divisor = ohms >= 1e9 ? 1e9 : ohms >= 1e6 ? 1e6 : ohms >= 1e3 ? 1e3 : 1;
    const char *unit = divisor == 1e9 ? "Gohm" : divisor == 1e6 ? "Mohm" : divisor == 1e3 ? "kohm" : "ohm";
    /* Use libc formatting: firmware LVGL's formatter has floats disabled. */
    snprintf(text, 48, "%.9g %s", ohms / divisor, unit);
}

static void calculate(void)
{
    resistor_data_t data;
    resistor_status_t status = mode < 2 ? resistor_decode_bands(choices[mode], mode + 4, &data) :
        resistor_decode_smd(saved_code, &data);
    lv_label_set_text(range_label, "");
    lv_label_set_text(detail_label, "");
    if (status != RESISTOR_OK) {
        lv_label_set_text(result_label, "Marking not decoded");
        lv_label_set_text(range_label, resistor_error(status));
        return;
    }
    char value[48], low[48], high[48], text[256];
    format_ohms(data.ohms, value);
    snprintf(text, sizeof(text), "Nominal: %s", value);
    lv_label_set_text(result_label, text);
    if (data.tolerance_known) {
        format_ohms(data.minimum_ohms, low);
        format_ohms(data.maximum_ohms, high);
        snprintf(text, sizeof(text), "Tolerance: +/- %.4g%%\nRange: %s to %s", data.tolerance_percent, low, high);
    } else snprintf(text, sizeof(text), data.kind == RESISTOR_JUMPER ?
                    "Zero-ohm jumper marking.\nActual resistance and current rating depend on the part." :
                    "Tolerance: not established by this marking.\nCheck the part's datasheet for limits.");
    lv_label_set_text(range_label, text);
    const char *kind = data.kind == RESISTOR_BANDS ? (mode == 0 ? "Two significant digits" : "Three significant digits") :
        data.kind == RESISTOR_NUMERIC ? "Numeric code: last digit is the exponent" :
        data.kind == RESISTOR_R_DECIMAL ? "R marks the decimal point in ohms" :
        data.kind == RESISTOR_EIA96 ? "EIA-96: indexed significant value + multiplier" : "Zero marking";
    snprintf(text, sizeof(text), "%s\n%u x 10^(%d) = %.12g ohm", kind,
             data.significand, data.exponent10, data.ohms);
    lv_label_set_text(detail_label, text);
}

static void update_bands(void)
{
    unsigned count = mode + 4;
    static const int x4[] = {110, 185, 260, 475};
    static const int x5[] = {100, 165, 230, 305, 475};
    for (unsigned i = 0; i < 5; i++) {
        visible(band_blocks[i], i < count);
        visible(stripes[i], i < count);
        visible(stripe_numbers[i], i < count);
        if (i >= count) continue;
        char caption[48];
        snprintf(caption, sizeof(caption), "Band %u / %s", i + 1,
                 i < count - 2 ? "digit" : i == count - 2 ? "multiplier" : "tolerance");
        lv_label_set_text(band_captions[i], caption);
        unsigned selection = (unsigned)choices[mode][i];
        if (i == count - 1) {
            lv_dropdown_set_options(band_select[i], tolerance_options);
            for (unsigned j = 0; j < sizeof(tolerances) / sizeof(tolerances[0]); j++)
                if (tolerances[j] == choices[mode][i]) selection = j;
        } else if (i == count - 2) lv_dropdown_set_options(band_select[i], multiplier_options);
        else {
            /* The first digit offers 1-9; later significant digits include 0. */
            lv_dropdown_set_options(band_select[i], i == 0 ? strchr(digit_options, '\n') + 1 : digit_options);
            if (i == 0) selection--;
        }
        lv_dropdown_set_selected(band_select[i], selection);
        lv_obj_set_style_bg_color(stripes[i], lv_color_hex(colors[choices[mode][i]]), 0);
        int x = mode == 0 ? x4[i] : x5[i];
        lv_obj_set_x(stripes[i], x);
        lv_obj_set_x(stripe_numbers[i], x);
    }
    visible(band_rows[2], mode == 1);
}

static void band_changed(lv_event_t *event)
{
    if (mode > 1) return;
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    unsigned selected = lv_dropdown_get_selected(band_select[index]);
    choices[mode][index] = index == mode + 3 ? tolerances[selected] :
        (resistor_color_t)(selected + (index == 0 ? 1 : 0));
    lv_obj_set_style_bg_color(stripes[index], lv_color_hex(colors[choices[mode][index]]), 0);
    calculate();
}

static void update_mode(void)
{
    visible(band_panel, mode < 2);
    visible(smd_panel, mode == 2);
    visible(keyboard, false);
    lv_keyboard_set_textarea(keyboard, NULL);
    if (mode < 2) update_bands();
    lv_label_set_text(note_label, mode < 2 ?
        "Read from the first digit toward the separated tolerance band.\n"
        "Four/five-band convention only; extra bands and unusual markings need a datasheet." :
        "Examples: 103 = 10 kohm, 4R7 = 4.7 ohm, 10C = 12.4 kohm.\n"
        "EIA-96 letters: A-H, X, Y, Z. Manufacturer-specific codes may differ.");
    calculate();
}

static void mode_changed(lv_event_t *event)
{
    (void)event;
    mode = lv_dropdown_get_selected(mode_select);
    update_mode();
}

static void code_event(lv_event_t *event)
{
    if (!keyboard) return;
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        snprintf(saved_code, sizeof(saved_code), "%s", lv_textarea_get_text(code_field));
        calculate();
    } else if (lv_event_get_code(event) == LV_EVENT_CLICKED || lv_event_get_code(event) == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(keyboard, code_field);
        visible(keyboard, true);
    }
}

static void keyboard_done(lv_event_t *event)
{
    (void)event;
    visible(keyboard, false);
    lv_keyboard_set_textarea(keyboard, NULL);
    lv_obj_remove_state(code_field, LV_STATE_FOCUSED);
}

static void example_clicked(lv_event_t *event)
{
    (void)event;
    if (mode == 0) {
        resistor_color_t example[] = {RESISTOR_YELLOW, RESISTOR_VIOLET, RESISTOR_RED, RESISTOR_GOLD};
        memcpy(choices[0], example, sizeof(example));
    } else if (mode == 1) {
        resistor_color_t example[] = {RESISTOR_BROWN, RESISTOR_BLACK, RESISTOR_BLACK, RESISTOR_RED, RESISTOR_BROWN};
        memcpy(choices[1], example, sizeof(example));
    } else {
        /* LVGL emits changes while inserting each character. The callback
         * updates saved_code, so the source must not alias that buffer. */
        lv_textarea_set_text(code_field, "103");
    }
    update_mode();
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, bool small)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_width(object, 640);
    if (small) lv_obj_set_style_text_font(object, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    lv_label_set_text(object, text);
    return object;
}

static lv_obj_t *plain(lv_obj_t *parent, int width, int height)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, width, height);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static lv_obj_t *rectangle(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color)
{
    lv_obj_t *object = plain(parent, width, height);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    return object;
}

static void readable_options(lv_obj_t *dropdown)
{
    /* The popup is attached to the screen, so it does not inherit our column
     * font. Give its entries the same legible type and a 48-pixel row pitch. */
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_28, LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(list, 20, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 420, LV_PART_MAIN);
}

void resistor_tool_stop(void)
{
    if (keyboard) lv_keyboard_set_textarea(keyboard, NULL);
    keyboard = NULL;
    mode_select = band_panel = smd_panel = code_field = NULL;
    result_label = range_label = detail_label = note_label = NULL;
    for (unsigned i = 0; i < 5; i++) {
        band_blocks[i] = band_captions[i] = band_select[i] = NULL;
        stripes[i] = stripe_numbers[i] = NULL;
    }
    for (unsigned i = 0; i < 3; i++) band_rows[i] = NULL;
}

void resistor_tool_show(lv_obj_t *parent)
{
    lv_obj_t *column = plain(parent, 640, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 16, 0);
    lv_obj_set_style_text_font(column, &lv_font_montserrat_28, 0);
    label(column, "Resistor Lab", false);
    label(column, "Decode color bands or a printed SMD marking.\nOffline reference; selections update the result immediately.", true);
    mode_select = lv_dropdown_create(column);
    readable_options(mode_select);
    lv_obj_set_size(mode_select, 640, 62);
    lv_dropdown_set_options(mode_select, "4 color bands\n5 color bands\nSMD marking");
    lv_dropdown_set_selected(mode_select, mode);
    lv_obj_add_event_cb(mode_select, mode_changed, LV_EVENT_VALUE_CHANGED, NULL);
    band_panel = plain(column, 640, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(band_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(band_panel, 14, 0);
    lv_obj_t *drawing = plain(band_panel, 640, 134);
    rectangle(drawing, 0, 53, 640, 6, 0xBAC4CB);
    lv_obj_t *body = rectangle(drawing, 60, 18, 520, 80, 0xD3B892);
    lv_obj_set_style_radius(body, 26, 0);
    for (unsigned i = 0; i < 5; i++) {
        stripes[i] = rectangle(drawing, 0, 20, 28, 76, 0);
        stripe_numbers[i] = label(drawing, "", true);
        lv_obj_set_width(stripe_numbers[i], 28);
        lv_obj_set_style_text_align(stripe_numbers[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_y(stripe_numbers[i], 110);
        char text[4]; snprintf(text, sizeof(text), "%u", i + 1);
        lv_label_set_text(stripe_numbers[i], text);
    }
    for (unsigned i = 0; i < 3; i++) {
        band_rows[i] = plain(band_panel, 640, 88);
        lv_obj_set_flex_flow(band_rows[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(band_rows[i], 20, 0);
    }
    for (unsigned i = 0; i < 5; i++) {
        band_blocks[i] = plain(band_rows[i / 2], 310, 88);
        lv_obj_set_flex_flow(band_blocks[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(band_blocks[i], 6, 0);
        band_captions[i] = label(band_blocks[i], "", true);
        lv_obj_set_width(band_captions[i], 310);
        band_select[i] = lv_dropdown_create(band_blocks[i]);
        readable_options(band_select[i]);
        lv_obj_set_size(band_select[i], 310, 62);
        lv_obj_add_event_cb(band_select[i], band_changed, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)i);
    }
    smd_panel = plain(column, 640, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(smd_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(smd_panel, 12, 0);
    label(smd_panel, "Printed code (up to four characters)", true);
    code_field = lv_textarea_create(smd_panel);
    lv_obj_set_size(code_field, 640, 62);
    /* Preserve pasted newlines; they are invalid marking characters. */
    /* Preserve an extra character and rejected Unicode so truncation cannot
     * silently turn an invalid longer code into a valid shorter code. */
    lv_textarea_set_max_length(code_field, RESISTOR_CODE_MAX + 1);
    lv_textarea_set_text(code_field, saved_code);
    lv_obj_add_event_cb(code_field, code_event, LV_EVENT_ALL, NULL);
    keyboard = lv_keyboard_create(smd_panel);
    lv_obj_set_size(keyboard, 640, 240);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_28, 0);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1, key_map, key_controls);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_CANCEL, NULL);
    result_label = label(column, "", false);
    range_label = label(column, "", false);
    detail_label = label(column, "", true);
    note_label = label(column, "", true);
    lv_obj_t *example = lv_button_create(column);
    lv_obj_set_size(example, 640, 62);
    lv_obj_t *caption = lv_label_create(example);
    lv_label_set_text(caption, "Example"); lv_obj_center(caption);
    lv_obj_add_event_cb(example, example_clicked, LV_EVENT_CLICKED, NULL);
    label(column, "Markings do not establish power/voltage ratings or prove a part is healthy.\n"
                  "Confirm ambiguous parts with a meter and datasheet. Inputs stay in RAM until restart.", true);
    update_mode();
}
