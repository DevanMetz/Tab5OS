#include "modbus_rtu_tool.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "byte_data.h"
#include "modbus_data.h"
#include "payload_clipboard.h"

enum { UNIT, ADDRESS, COUNT, REPLY, FIELD_COUNT };
/* Preserve the first excess character and rejected UTF-8 across Home/re-entry. */
static char saved_fields[3][6 * 4 + 1] = {"1", "0", "2"};
static char saved_reply[(BYTE_DATA_MAX_TEXT + 1) * 4 + 1];
static unsigned function_index = 2;
static modbus_value_view_t value_view;
static modbus_byte_order_t byte_order;
static bool loading, dirty, decoded;
static modbus_request_t decoded_request;
static modbus_result_t decoded_result;
static lv_obj_t *fields[FIELD_COUNT];
static lv_obj_t *function_select, *view_select, *order_select;
static lv_obj_t *frame_label, *status_label, *result_label, *result_box, *keyboard;

static const char *const hex_keys[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "\n",
    "8", "9", "A", "B", "C", "D", "E", "F", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t hex_controls[] = {
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    LV_BUTTONMATRIX_CTRL_CHECKED | 2, 6, LV_BUTTONMATRIX_CTRL_CHECKED | 2,
    LV_BUTTONMATRIX_CTRL_CHECKED | 3, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 3
};
_Static_assert(sizeof(hex_keys) / sizeof(hex_keys[0]) ==
               sizeof(hex_controls) / sizeof(hex_controls[0]) + 3,
               "Hex keyboard map and controls must match");

static void keyboard_visible(bool visible)
{
    if (visible) {
        lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(result_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_keyboard_set_textarea(keyboard, NULL);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(result_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void inputs_changed(void)
{
    decoded = false;
    /* LVGL emits one event per pasted character. Invalidate once, with no
     * per-character result formatting or button-state animations. */
    if (dirty || !status_label) return;
    dirty = true;
    lv_label_set_text(frame_label, "Inputs changed. COPY REQUEST builds an 8-byte read frame.");
    lv_label_set_text(status_label, "Inputs changed. DECODE checks the reply against the current request fields.");
    lv_label_set_text(result_label, "No decoded reply for these inputs.");
}

static bool build_request(modbus_request_t *request, uint8_t bytes[MODBUS_RTU_REQUEST_BYTES])
{
    *request = (modbus_request_t){.function = (uint8_t)(function_index + 1)};
    uint16_t unit;
    if (!modbus_parse_decimal(lv_textarea_get_text(fields[UNIT]), 247, &unit) || !unit) {
        lv_label_set_text(status_label, "Unit must be 1 to 247. Broadcast 0 and reserved units are not supported.");
        return false;
    }
    request->unit_id = (uint8_t)unit;
    if (!modbus_parse_decimal(lv_textarea_get_text(fields[ADDRESS]), 65535, &request->address) ||
        !modbus_parse_decimal(lv_textarea_get_text(fields[COUNT]), MODBUS_MAX_VALUES, &request->quantity) ||
        !modbus_rtu_build_request(request, bytes)) {
        lv_label_set_text(status_label, "Use decimal address 0-65535 and count 1-16, without crossing address 65535.");
        return false;
    }
    char text[144];
    snprintf(text, sizeof(text), "Request: unit %u | FC %02u | address %u | count %u\n"
             "%02X %02X %02X %02X %02X %02X %02X %02X  (last 2 bytes: CRC)",
             (unsigned)request->unit_id, (unsigned)request->function,
             (unsigned)request->address, (unsigned)request->quantity,
             (unsigned)bytes[0], (unsigned)bytes[1], (unsigned)bytes[2], (unsigned)bytes[3],
             (unsigned)bytes[4], (unsigned)bytes[5], (unsigned)bytes[6], (unsigned)bytes[7]);
    lv_label_set_text(frame_label, text);
    dirty = false;
    return true;
}

static void render_values(void)
{
    if (!decoded) return;
    char text[MODBUS_VALUES_TEXT_SIZE];
    if (modbus_format_values(&decoded_request, &decoded_result, value_view, byte_order, text, sizeof(text)))
        lv_label_set_text(result_label, text);
    else lv_label_set_text(result_label, "Could not format this reply.");
    lv_obj_scroll_to_y(result_box, 0, LV_ANIM_OFF);
}

static void decode_clicked(lv_event_t *event)
{
    (void)event;
    decoded = false;
    lv_label_set_text(result_label, "No decoded reply for these inputs.");
    uint8_t request_bytes[MODBUS_RTU_REQUEST_BYTES];
    modbus_request_t request;
    if (!build_request(&request, request_bytes)) return;
    byte_data_t bytes;
    byte_data_status_t input_status = byte_data_parse(lv_textarea_get_text(fields[REPLY]), BYTE_DATA_HEX, &bytes);
    if (input_status != BYTE_DATA_OK) {
        lv_label_set_text(status_label, byte_data_error(input_status));
        return;
    }
    if (!bytes.length) {
        lv_label_set_text(status_label, "Enter or paste one complete reply, including unit, function and CRC.");
        return;
    }
    modbus_result_t result;
    modbus_status_t status = modbus_rtu_parse_response(&request, bytes.bytes, bytes.length, &result);
    keyboard_visible(false);
    if (status == MODBUS_EXCEPTION) {
        char text[96];
        snprintf(text, sizeof(text), "CRC valid. Unit %u returned exception 0x%02X for function %02u.",
                 (unsigned)request.unit_id, (unsigned)result.exception_code, (unsigned)request.function);
        lv_label_set_text(status_label, text);
        lv_label_set_text(result_label, "Exception reply: no register or coil values.");
    } else if (status != MODBUS_OK) {
        lv_label_set_text(status_label, modbus_data_error(status));
    } else {
        decoded_request = request;
        decoded_result = result;
        decoded = true;
        lv_label_set_text(status_label, "CRC, unit, function and count match. Address is taken from your request fields.");
        render_values();
    }
}

static void copy_clicked(lv_event_t *event)
{
    (void)event;
    uint8_t bytes[MODBUS_RTU_REQUEST_BYTES];
    modbus_request_t request;
    if (!build_request(&request, bytes)) return;
    if (!payload_clipboard_store(bytes, sizeof(bytes))) {
        lv_label_set_text(status_label, "Could not copy the request; the previous clipboard is unchanged.");
        return;
    }
    keyboard_visible(false);
    lv_label_set_text(status_label, "8-byte request copied. In Serial, PASTE selects Hex. Check the link, then START and SEND.");
}

static void paste_clicked(lv_event_t *event)
{
    (void)event;
    const payload_clipboard_t *copy = payload_clipboard_peek();
    if (!copy || !copy->length || copy->length > MODBUS_RTU_MAX_RESPONSE_BYTES) {
        lv_label_set_text(status_label, "Paste needs 1-37 copied bytes. The current reply draft is unchanged.");
        return;
    }
    char text[PAYLOAD_CLIPBOARD_HEX_SIZE];
    if (!payload_clipboard_hex(text, sizeof(text))) return;
    lv_textarea_set_text(fields[REPLY], text);
    keyboard_visible(false);
    lv_label_set_text(status_label, "Reply pasted. DECODE checks it against the current request fields.");
}

static void clear_clicked(lv_event_t *event)
{
    (void)event;
    lv_textarea_set_text(fields[REPLY], "");
    inputs_changed();
    lv_label_set_text(status_label, "Reply cleared. Request fields and shared clipboard are unchanged.");
}

static void example_clicked(lv_event_t *event)
{
    (void)event;
    function_index = 2;
    lv_dropdown_set_selected(function_select, function_index);
    lv_textarea_set_text(fields[UNIT], "1");
    lv_textarea_set_text(fields[ADDRESS], "0");
    lv_textarea_set_text(fields[COUNT], "2");
    /* Float32 1.5 in ABCD order; calculate the example CRC with the same helper
     * used for requests, then pass it through the public decoder like a paste. */
    uint8_t reply[] = {1, 3, 4, 0x3f, 0xc0, 0, 0, 0, 0};
    uint16_t crc = byte_data_crc16_modbus(reply, sizeof(reply) - 2);
    reply[7] = (uint8_t)crc; reply[8] = (uint8_t)(crc >> 8);
    char text[sizeof(reply) * 3];
    for (size_t i = 0; i < sizeof(reply); i++)
        snprintf(text + i * 3, sizeof(text) - i * 3, "%02X%s", (unsigned)reply[i], i + 1 < sizeof(reply) ? " " : "");
    lv_textarea_set_text(fields[REPLY], text);
    value_view = MODBUS_VIEW_FLOAT32; byte_order = MODBUS_ORDER_ABCD;
    lv_dropdown_set_selected(view_select, value_view);
    lv_dropdown_set_selected(order_select, byte_order);
    decode_clicked(NULL);
    lv_label_set_text(status_label, "Example reply: Float32 1.5 in ABCD order. No device was contacted.");
}

static void input_event(lv_event_t *event)
{
    if (loading || !keyboard) return;
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    lv_obj_t *object = lv_event_get_target_obj(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_mode(keyboard, index == REPLY ? LV_KEYBOARD_MODE_USER_1 : LV_KEYBOARD_MODE_NUMBER);
        lv_keyboard_set_textarea(keyboard, object);
        keyboard_visible(true);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        char *draft = index == REPLY ? saved_reply : saved_fields[index];
        size_t capacity = index == REPLY ? sizeof(saved_reply) : sizeof(saved_fields[index]);
        snprintf(draft, capacity, "%s", lv_textarea_get_text(object));
        inputs_changed();
    }
}

static void function_changed(lv_event_t *event)
{
    (void)event;
    function_index = lv_dropdown_get_selected(function_select);
    inputs_changed();
}

static void view_changed(lv_event_t *event)
{
    (void)event;
    value_view = (modbus_value_view_t)lv_dropdown_get_selected(view_select);
    byte_order = (modbus_byte_order_t)lv_dropdown_get_selected(order_select);
    render_values();
}

static void keyboard_done(lv_event_t *event)
{
    (void)event;
    keyboard_visible(false);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_width(object, 640);
    lv_obj_set_style_text_font(object, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    lv_label_set_text(object, text);
    return object;
}

static lv_obj_t *row(lv_obj_t *parent, int height)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, 640, height);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(object, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(object, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return object;
}

static lv_obj_t *field_block(lv_obj_t *parent, const char *name, int width)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, width, 80);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(object, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(object, 4, 0);
    lv_obj_set_width(label(object, name), width);
    return object;
}

static void input(lv_obj_t *parent, const char *name, unsigned index, int width)
{
    lv_obj_t *block = field_block(parent, name, width);
    fields[index] = lv_textarea_create(block);
    lv_obj_set_size(fields[index], width, 60);
    lv_textarea_set_max_length(fields[index], 6);
    lv_textarea_set_text(fields[index], saved_fields[index]);
    lv_obj_add_event_cb(fields[index], input_event, LV_EVENT_ALL, (void *)(uintptr_t)index);
}

static lv_obj_t *select_options(lv_obj_t *parent, const char *name, int width,
                                 const char *options, unsigned selected, lv_event_cb_t callback)
{
    lv_obj_t *object = lv_dropdown_create(field_block(parent, name, width));
    lv_obj_set_size(object, width, 60);
    lv_dropdown_set_options(object, options);
    lv_dropdown_set_selected(object, selected);
    lv_obj_t *list = lv_dropdown_get_list(object);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_28, LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(list, 20, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 260, LV_PART_MAIN);
    lv_obj_add_event_cb(object, callback, LV_EVENT_VALUE_CHANGED, NULL);
    return object;
}

static void button(lv_obj_t *parent, const char *text, int width, lv_event_cb_t callback)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_set_size(object, width, 60);
    lv_obj_t *caption = lv_label_create(object);
    lv_label_set_text(caption, text);
    lv_obj_center(caption);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, NULL);
}

void modbus_rtu_tool_show(lv_obj_t *parent)
{
    loading = true; dirty = false; decoded = false;
    lv_obj_t *column = lv_obj_create(parent);
    lv_obj_remove_style_all(column);
    lv_obj_set_size(column, 640, LV_SIZE_CONTENT);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 8, 0);
    lv_obj_set_style_text_font(column, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_font(label(column, "Modbus RTU Frames"), &lv_font_montserrat_28, 0);
    label(column, "Build read frames and inspect captured replies offline.\nUse Serial to send bytes; this app does not open a port.");
    lv_obj_t *operation = row(column, 80);
    function_select = select_options(operation, "Read function", 430,
        "01 Coils\n02 Discrete inputs\n03 Holding registers\n04 Input registers", function_index, function_changed);
    input(operation, "Unit (1-247)", UNIT, 198);
    lv_obj_t *range = row(column, 80);
    input(range, "Zero-based address (0-65535)", ADDRESS, 430);
    input(range, "Count (1-16)", COUNT, 198);
    frame_label = label(column, "COPY REQUEST builds the read frame with its CRC.");
    lv_obj_set_height(frame_label, 40);
    lv_obj_t *copy_actions = row(column, 60);
    button(copy_actions, "COPY REQUEST", 310, copy_clicked);
    button(copy_actions, "PASTE REPLY", 310, paste_clicked);
    label(column, "Complete reply in Hex, including CRC (5-37 bytes)");
    fields[REPLY] = lv_textarea_create(column);
    lv_obj_set_size(fields[REPLY], 640, 84);
    lv_textarea_set_max_length(fields[REPLY], BYTE_DATA_MAX_TEXT + 1);
    lv_textarea_set_text(fields[REPLY], saved_reply);
    lv_obj_add_event_cb(fields[REPLY], input_event, LV_EVENT_ALL, (void *)(uintptr_t)REPLY);
    lv_obj_t *actions = row(column, 60);
    button(actions, "DECODE", 206, decode_clicked);
    button(actions, "EXAMPLE", 206, example_clicked);
    button(actions, "CLEAR", 206, clear_clicked);
    status_label = label(column, "Ready. Copy a request or enter a complete reply to decode.");
    lv_obj_set_height(status_label, 40);
    lv_obj_t *views = row(column, 80);
    view_select = select_options(views, "Decoded values", 310,
        "16-bit registers\nUnsigned 32-bit\nSigned 32-bit\nFloat32", value_view, view_changed);
    order_select = select_options(views, "32-bit byte order", 310,
        "ABCD\nCDAB\nBADC\nDCBA", byte_order, view_changed);
    label(column, "A B = first register; C D = next. Pairs start at the requested address.\nA reply does not echo its address. Verify the request and device mapping.");
    result_box = lv_obj_create(column);
    lv_obj_set_size(result_box, 640, 200);
    lv_obj_set_style_pad_all(result_box, 16, 0);
    lv_obj_set_scroll_dir(result_box, LV_DIR_VER);
    result_label = label(result_box, "No decoded reply for these inputs.");
    lv_obj_set_width(result_label, 596);
    keyboard = lv_keyboard_create(column);
    lv_obj_set_size(keyboard, 640, 220);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1, hex_keys, hex_controls);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_CANCEL, NULL);
    loading = false;
    if (saved_reply[0]) decode_clicked(NULL);
    else {
        modbus_request_t request;
        uint8_t bytes[MODBUS_RTU_REQUEST_BYTES];
        build_request(&request, bytes);
    }
}

void modbus_rtu_tool_stop(void)
{
    if (keyboard) lv_keyboard_set_textarea(keyboard, NULL);
    for (unsigned i = 0; i < FIELD_COUNT; i++) fields[i] = NULL;
    function_select = view_select = order_select = NULL;
    frame_label = status_label = result_label = result_box = keyboard = NULL;
    decoded = dirty = loading = false;
}
