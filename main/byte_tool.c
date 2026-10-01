#include "byte_tool.h"

#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "byte_data.h"
#include "payload_clipboard.h"

/* LVGL's limit counts Unicode characters; keep enough RAM for rejected UTF-8
 * input as well so an invalid edit is never truncated into a valid payload. */
static char saved_input[2][(BYTE_DATA_MAX_TEXT + 1) * 4 + 1] = {
    "01 03 00 00 00 0A", "123456789"
};
enum { INPUT_NUMBER = 2 };
static unsigned input_mode = BYTE_DATA_HEX;
static char saved_number[(BYTE_NUMBER_MAX_TEXT + 1) * 4 + 1] = "4660";
static byte_number_type_t number_type = BYTE_NUMBER_UINT16;
static bool little_endian;
static const char *const number_names[] = {
    "Unsigned 8-bit", "Signed 8-bit", "Unsigned 16-bit", "Signed 16-bit",
    "Unsigned 32-bit", "Signed 32-bit", "Float32"
};
static const char *const number_examples[] = {"165", "-42", "4660", "-1234", "305419896", "-123456", "1.5"};
static bool loading_input;
static lv_obj_t *mode_select;
static lv_obj_t *number_controls;
static lv_obj_t *type_select;
static lv_obj_t *order_select;
static lv_obj_t *use_hex_button;
static lv_obj_t *input_area;
static lv_obj_t *help_label;
static lv_obj_t *result_label;
static lv_obj_t *preview_label;
static lv_obj_t *keyboard;
static lv_obj_t *copy_button;
static lv_obj_t *paste_button;
static lv_obj_t *clear_copy_button;
static lv_obj_t *clipboard_label;
static bool render_queued;

static const char *const hex_key_map[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "\n",
    "8", "9", "A", "B", "C", "D", "E", "F", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};
/* Control entries count buttons only; newlines and the terminator have none.
 * Each row has 16 width units, with eight equally sized hex keys per row. */
static const lv_buttonmatrix_ctrl_t hex_key_controls[] = {
    2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2,
    LV_BUTTONMATRIX_CTRL_CHECKED | 2, 6, LV_BUTTONMATRIX_CTRL_CHECKED | 2,
    LV_BUTTONMATRIX_CTRL_CHECKED | 3, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 3
};
_Static_assert(sizeof(hex_key_map) / sizeof(hex_key_map[0]) ==
               sizeof(hex_key_controls) / sizeof(hex_key_controls[0]) + 3,
               "Hex keyboard map and controls must match");

static const char *const number_key_map[] = {
    "1", "2", "3", "4", "5", "\n", "6", "7", "8", "9", "0", "\n",
    "+", "-", ".", "e", "NaN", "\n", "+Inf", "-Inf",
    LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t number_key_controls[] = {
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, LV_BUTTONMATRIX_CTRL_CHECKED | 1, LV_BUTTONMATRIX_CTRL_CHECKED | 1,
    LV_BUTTONMATRIX_CTRL_CHECKED | 2, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2
};
_Static_assert(sizeof(number_key_map) / sizeof(number_key_map[0]) ==
               sizeof(number_key_controls) / sizeof(number_key_controls[0]) + 4,
               "Number keyboard map and controls must match");

static void update_keyboard_mode(void)
{
    if (!keyboard) return;
    lv_keyboard_set_mode(keyboard, input_mode == BYTE_DATA_HEX ?
                          LV_KEYBOARD_MODE_USER_1 : input_mode == INPUT_NUMBER ?
                          LV_KEYBOARD_MODE_USER_2 : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_text_font(keyboard, input_mode != BYTE_DATA_ASCII ?
                               &lv_font_montserrat_28 : &lv_font_montserrat_14, 0);
}

static void format_float32(double value, char text[32])
{
    if (isnan(value)) snprintf(text, 32, "NaN (not a number)");
    else if (isinf(value)) snprintf(text, 32, "%sInfinity", signbit(value) ? "-" : "+");
    else snprintf(text, 32, "%.9g", value);
}

static void render_result(void)
{
    if (!input_area || !result_label || !preview_label) return;
    byte_data_t data;
    byte_data_status_t status = input_mode == INPUT_NUMBER ?
        byte_data_encode(lv_textarea_get_text(input_area), number_type, little_endian, &data) :
        byte_data_parse(lv_textarea_get_text(input_area), (byte_data_mode_t)input_mode, &data);
    if (status == BYTE_DATA_OK) {
        lv_obj_remove_state(use_hex_button, LV_STATE_DISABLED);
        lv_obj_remove_state(copy_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(use_hex_button, LV_STATE_DISABLED);
        lv_obj_add_state(copy_button, LV_STATE_DISABLED);
    }
    if (status != BYTE_DATA_OK) {
        lv_label_set_text(result_label, byte_data_error(status));
        lv_label_set_text(preview_label, "Fix the input to calculate checksums and preview bytes.");
        return;
    }

    char heading[100] = "";
    if (input_mode == INPUT_NUMBER) {
        char value[32];
        if (number_type == BYTE_NUMBER_FLOAT32)
            format_float32(little_endian ? data.float32_little_endian : data.float32_big_endian, value);
        else if ((unsigned)number_type % 2)
            snprintf(value, sizeof(value), "%ld", (long)(little_endian ? data.signed_little_endian : data.signed_big_endian));
        else snprintf(value, sizeof(value), "%lu", (unsigned long)(little_endian ? data.little_endian : data.big_endian));
        snprintf(heading, sizeof(heading), "Encoded %s %s: %s\n", number_names[number_type], little_endian ? "LE" : "BE", value);
    }
    char result[512];
    size_t used = (size_t)snprintf(result, sizeof(result),
        "%s%u bytes | SUM8: %02X | XOR8: %02X\n"
        "CRC-16/MODBUS: %04X\nWire CRC (low byte first): %02X %02X\n"
        "CRC-32/ISO-HDLC: %08lX",
        heading, (unsigned)data.length, (unsigned)data.sum8, (unsigned)data.xor8,
        (unsigned)data.crc16_modbus, (unsigned)(data.crc16_modbus & 0xff),
        (unsigned)(data.crc16_modbus >> 8), (unsigned long)data.crc32);
    if (input_mode != INPUT_NUMBER && (data.length == 1 || data.length == 2 || data.length == 4)) {
        used += (size_t)snprintf(result + used, sizeof(result) - used,
            "\nUnsigned %u-bit\nBE %lu | LE %lu\n"
            "Signed %u-bit (two's complement)\nBE %ld | LE %ld",
            (unsigned)data.length * 8, (unsigned long)data.big_endian,
            (unsigned long)data.little_endian, (unsigned)data.length * 8,
            (long)data.signed_big_endian, (long)data.signed_little_endian);
        if (data.length == 4) {
            char big[32], little[32];
            format_float32(data.float32_big_endian, big);
            format_float32(data.float32_little_endian, little);
            snprintf(result + used, sizeof(result) - used,
                     "\nFloat32 BE: %s\nFloat32 LE: %s", big, little);
        }
    }
    lv_label_set_text(result_label, result);

    char preview[BYTE_DATA_MAX_BYTES * 4 + 100];
    used = (size_t)snprintf(preview, sizeof(preview), "HEX\n");
    for (size_t i = 0; i < data.length; i++) {
        used += (size_t)snprintf(preview + used, sizeof(preview) - used,
                                "%02X%s", (unsigned)data.bytes[i],
                                i + 1 == data.length ? "" : " ");
    }
    used += (size_t)snprintf(preview + used, sizeof(preview) - used,
                            "%s\n\nASCII (non-printable = .)\n", data.length ? "" : "(empty)");
    if (!data.length) {
        snprintf(preview + used, sizeof(preview) - used, "(empty)");
    } else {
        for (size_t i = 0; i < data.length; i++) {
            uint8_t ch = data.bytes[i];
            preview[used++] = ch >= 0x20 && ch <= 0x7e ? (char)ch : '.';
        }
        preview[used] = '\0';
    }
    lv_label_set_text(preview_label, preview);
}

static void update_help(void)
{
    if (input_mode == INPUT_NUMBER) {
        static const char *const limits[] = {
            "0 to 255", "-128 to 127", "0 to 65535", "-32768 to 32767",
            "0 to 4294967295", "-2147483648 to 2147483647", "decimal, exponent, NaN or +/-Inf"
        };
        char text[224];
        snprintf(text, sizeof(text), "%s: %s. Up to 32 characters; no spaces or units.\n%s",
                 number_names[number_type], limits[number_type], number_type == BYTE_NUMBER_FLOAT32 ?
                 "Result shows the rounded Float32 value. Nonzero underflow to zero is rejected." :
                 "Decimal integers only. Signed types use two's complement.");
        lv_label_set_text(help_label, text);
        return;
    }
    lv_label_set_text(help_label, input_mode == BYTE_DATA_HEX ?
        "Hex byte pairs: 01 03 or 0103. Whitespace between bytes is OK. No 0x prefix. Maximum 128 bytes." :
        "Literal 7-bit ASCII, maximum 128 bytes. Newlines count; backslash escapes are not expanded. Use hex for binary data.");
}

static void render_later(void *context)
{
    (void)context;
    render_queued = false;
    render_result();
}

static void input_changed(lv_event_t *event)
{
    (void)event;
    if (loading_input) return;
    if (input_mode == INPUT_NUMBER)
        snprintf(saved_number, sizeof(saved_number), "%s", lv_textarea_get_text(input_area));
    else snprintf(saved_input[input_mode], sizeof(saved_input[input_mode]), "%s", lv_textarea_get_text(input_area));
    /* A bounded textarea emits one event per inserted character. Coalesce a
     * bulk paste into one render; alternating valid/invalid hex nibbles must
     * not queue hundreds of button state animations in LVGL's fixed heap. */
    if (!render_queued) {
        render_queued = lv_async_call(render_later, NULL) == LV_RESULT_OK;
        if (!render_queued) {
            lv_obj_add_state(copy_button, LV_STATE_DISABLED);
            lv_obj_add_state(use_hex_button, LV_STATE_DISABLED);
            lv_label_set_text(result_label, "Display update unavailable. Return Home and reopen.");
            lv_label_set_text(preview_label, "");
        }
    }
}

static void set_input_text(const char *text)
{
    /* A bounded LVGL textarea inserts one character at a time and emits change
     * events during set_text. Do not overwrite our saved source mid-restore. */
    loading_input = true;
    lv_textarea_set_text(input_area, text);
    loading_input = false;
    input_changed(NULL);
}

static void apply_mode(void)
{
    if (input_mode == INPUT_NUMBER) {
        lv_obj_remove_flag(number_controls, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(use_hex_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(number_controls, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(use_hex_button, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_height(input_area, input_mode == INPUT_NUMBER ? 64 : 120);
    lv_obj_set_style_text_font(preview_label, input_mode == INPUT_NUMBER ?
                               &lv_font_montserrat_28 : &lv_font_montserrat_14, 0);
    lv_textarea_set_max_length(input_area, input_mode == INPUT_NUMBER ? BYTE_NUMBER_MAX_TEXT + 1 :
                              input_mode == BYTE_DATA_ASCII ? BYTE_DATA_MAX_BYTES + 1 : BYTE_DATA_MAX_TEXT + 1);
    update_keyboard_mode();
    update_help();
    set_input_text(input_mode == INPUT_NUMBER ? saved_number : saved_input[input_mode]);
}

static void mode_changed(lv_event_t *event)
{
    (void)event;
    input_mode = lv_dropdown_get_selected(mode_select);
    apply_mode();
}

static void number_changed(lv_event_t *event)
{
    (void)event;
    number_type = (byte_number_type_t)lv_dropdown_get_selected(type_select);
    little_endian = lv_dropdown_get_selected(order_select) != 0;
    update_help();
    render_result();
}

static void use_hex_clicked(lv_event_t *event)
{
    (void)event;
    byte_data_t data;
    if (input_mode != INPUT_NUMBER || byte_data_encode(saved_number, number_type, little_endian, &data) != BYTE_DATA_OK) return;
    /* Explicitly replace the Hex draft; keep the Number and ASCII drafts. */
    char hex[12];
    size_t used = 0;
    for (size_t i = 0; i < data.length; i++)
        used += (size_t)snprintf(hex + used, sizeof(hex) - used, "%02X%s", (unsigned)data.bytes[i], i + 1 == data.length ? "" : " ");
    snprintf(saved_input[BYTE_DATA_HEX], sizeof(saved_input[BYTE_DATA_HEX]), "%s", hex);
    input_mode = BYTE_DATA_HEX;
    lv_dropdown_set_selected(mode_select, input_mode);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    apply_mode();
}

static void update_clipboard(void)
{
    const payload_clipboard_t *copy = payload_clipboard_peek();
    if (copy) {
        lv_obj_remove_state(paste_button, LV_STATE_DISABLED);
        lv_obj_remove_state(clear_copy_button, LV_STATE_DISABLED);
        lv_label_set_text_fmt(clipboard_label, "Byte clipboard: %u bytes in RAM. Paste replaces the Hex draft.\n"
                              "Shared across bench tools. Copy and paste never send.", (unsigned)copy->length);
    } else {
        lv_obj_add_state(paste_button, LV_STATE_DISABLED);
        lv_obj_add_state(clear_copy_button, LV_STATE_DISABLED);
        lv_label_set_text(clipboard_label, "Byte clipboard is empty. Copy bytes to share across bench tools.\n"
                                           "Paste replaces the Hex draft. Clipboard clears on restart.");
    }
}

static void copy_clicked(lv_event_t *event)
{
    (void)event;
    byte_data_t data;
    byte_data_status_t status = input_mode == INPUT_NUMBER ?
        byte_data_encode(lv_textarea_get_text(input_area), number_type, little_endian, &data) :
        byte_data_parse(lv_textarea_get_text(input_area), (byte_data_mode_t)input_mode, &data);
    if (status == BYTE_DATA_OK) payload_clipboard_store(data.bytes, data.length);
    update_clipboard();
}

static void paste_clicked(lv_event_t *event)
{
    (void)event;
    if (!payload_clipboard_peek()) return;
    if (!payload_clipboard_hex(saved_input[BYTE_DATA_HEX], sizeof(saved_input[BYTE_DATA_HEX]))) return;
    input_mode = BYTE_DATA_HEX;
    lv_dropdown_set_selected(mode_select, input_mode);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    apply_mode();
}

static void clear_copy_clicked(lv_event_t *event)
{
    (void)event;
    payload_clipboard_clear();
    update_clipboard();
}

static void clear_clicked(lv_event_t *event)
{
    (void)event;
    set_input_text("");
}

static void example_clicked(lv_event_t *event)
{
    (void)event;
    set_input_text(input_mode == INPUT_NUMBER ? number_examples[number_type] :
                   input_mode == BYTE_DATA_HEX ? "01 03 00 00 00 0A" : "123456789");
}

static void input_focused(lv_event_t *event)
{
    (void)event;
    if (keyboard) lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void keyboard_done(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(input_area, LV_STATE_FOCUSED);
    }
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_size(control, 170, 64);
    lv_obj_set_style_text_font(control, &lv_font_montserrat_28, 0);
    lv_obj_add_event_cb(control, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(control);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return control;
}

static void readable_options(lv_obj_t *dropdown)
{
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_28, LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(list, 20, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 400, LV_PART_MAIN);
}

void byte_tool_stop(void)
{
    if (render_queued) lv_async_call_cancel(render_later, NULL);
    render_queued = false;
    if (keyboard) lv_keyboard_set_textarea(keyboard, NULL);
    loading_input = false;
    mode_select = number_controls = type_select = order_select = use_hex_button = NULL;
    input_area = NULL;
    help_label = NULL;
    result_label = NULL;
    preview_label = NULL;
    keyboard = NULL;
    copy_button = paste_button = clear_copy_button = clipboard_label = NULL;
}

void byte_tool_show(lv_obj_t *parent)
{
    lv_obj_set_style_pad_row(parent, 14, 0);
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Byte Lab");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *controls = lv_obj_create(parent);
    lv_obj_remove_style_all(controls);
    lv_obj_set_size(controls, 640, 72);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    mode_select = lv_dropdown_create(controls);
    readable_options(mode_select);
    lv_obj_set_size(mode_select, 280, 64);
    lv_obj_set_style_text_font(mode_select, &lv_font_montserrat_28, 0);
    lv_dropdown_set_options(mode_select, "Hex input\nASCII input\nNumber input");
    lv_dropdown_set_selected(mode_select, input_mode);
    lv_obj_add_event_cb(mode_select, mode_changed, LV_EVENT_VALUE_CHANGED, NULL);
    button(controls, "EXAMPLE", example_clicked);
    button(controls, "CLEAR", clear_clicked);

    help_label = lv_label_create(parent);
    lv_obj_set_width(help_label, 640);
    update_help();

    number_controls = lv_obj_create(parent);
    lv_obj_remove_style_all(number_controls);
    lv_obj_set_size(number_controls, 640, 64);
    lv_obj_remove_flag(number_controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(number_controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(number_controls, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    type_select = lv_dropdown_create(number_controls);
    readable_options(type_select);
    lv_obj_set_size(type_select, 340, 64);
    lv_obj_set_style_text_font(type_select, &lv_font_montserrat_28, 0);
    lv_dropdown_set_options(type_select, "Unsigned 8-bit\nSigned 8-bit\nUnsigned 16-bit\nSigned 16-bit\nUnsigned 32-bit\nSigned 32-bit\nFloat32");
    lv_dropdown_set_selected(type_select, number_type);
    lv_obj_add_event_cb(type_select, number_changed, LV_EVENT_VALUE_CHANGED, NULL);
    order_select = lv_dropdown_create(number_controls);
    readable_options(order_select);
    lv_obj_set_size(order_select, 280, 64);
    lv_obj_set_style_text_font(order_select, &lv_font_montserrat_28, 0);
    lv_dropdown_set_options(order_select, "Big endian\nLittle endian");
    lv_dropdown_set_selected(order_select, little_endian ? 1 : 0);
    lv_obj_add_event_cb(order_select, number_changed, LV_EVENT_VALUE_CHANGED, NULL);

    input_area = lv_textarea_create(parent);
    lv_obj_set_size(input_area, 640, 120);
    lv_obj_set_style_text_font(input_area, &lv_font_montserrat_28, 0);
    lv_obj_add_event_cb(input_area, input_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(input_area, input_focused, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(input_area, input_focused, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clipboard_controls = lv_obj_create(parent);
    lv_obj_remove_style_all(clipboard_controls);
    lv_obj_set_size(clipboard_controls, 640, 64);
    lv_obj_remove_flag(clipboard_controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(clipboard_controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clipboard_controls, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    copy_button = button(clipboard_controls, "Copy bytes", copy_clicked);
    paste_button = button(clipboard_controls, "Paste hex", paste_clicked);
    clear_copy_button = button(clipboard_controls, "Clear copy", clear_copy_clicked);
    lv_obj_set_width(copy_button, 206);
    lv_obj_set_width(paste_button, 206);
    lv_obj_set_width(clear_copy_button, 206);
    clipboard_label = lv_label_create(parent);
    lv_obj_set_width(clipboard_label, 640);
    lv_obj_set_style_text_font(clipboard_label, &lv_font_montserrat_14, 0);
    update_clipboard();

    keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(keyboard, 640, 300);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1, hex_key_map, hex_key_controls);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_2, number_key_map, number_key_controls);
    update_keyboard_mode();
    lv_keyboard_set_textarea(keyboard, input_area);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    result_label = lv_label_create(parent);
    lv_obj_set_width(result_label, 640);
    lv_obj_set_style_text_font(result_label, &lv_font_montserrat_28, 0);
    lv_label_set_long_mode(result_label, LV_LABEL_LONG_WRAP);

    lv_obj_t *preview = lv_obj_create(parent);
    lv_obj_set_size(preview, 640, 225);
    preview_label = lv_label_create(preview);
    lv_obj_set_width(preview_label, LV_PCT(100));
    lv_label_set_long_mode(preview_label, LV_LABEL_LONG_WRAP);

    use_hex_button = button(parent, "USE AS HEX (replaces Hex draft)", use_hex_clicked);
    lv_obj_set_width(use_hex_button, 640);

    lv_obj_t *footnote = lv_label_create(parent);
    lv_obj_set_width(footnote, 640);
    lv_label_set_text(footnote, "Offline calculation only. Inputs stay in RAM until restart.\n"
                                "Tap the payload for keys; Done hides them. BE = big endian, LE = little endian.\n"
                                "Integer views need 1, 2 or 4 bytes; IEEE 754 Float32 needs exactly 4.\n"
                                "SUM8 wraps modulo 256; checksums cover every decoded byte.\n"
                                "MODBUS: init FFFF, reflected A001, xor 0000.\n"
                                "CRC-32: init/xor FFFFFFFF, reflected EDB88320.");
    apply_mode();
}
