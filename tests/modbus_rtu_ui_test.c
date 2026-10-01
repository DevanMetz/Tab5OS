/* Actual LVGL/app code; no UART, socket, task or driver adapters. */
#include "modbus_rtu_tool.h"
#include "payload_clipboard.h"
#include "modbus_data.h"
#include "byte_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check(expression) do { if (!(expression)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); exit(1); \
} } while (0)

static uint16_t framebuffer[720 * 1280];
static lv_obj_t *content;
static const char *output_directory;
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels; lv_display_flush_ready(display);
}
static void refresh(void)
{
    lv_tick_inc(40); lv_timer_handler(); lv_obj_update_layout(content); lv_refr_now(NULL);
}
static lv_obj_t *find(lv_obj_t *object, const lv_obj_class_t *type, unsigned *index)
{
    if (lv_obj_check_type(object, type) && (*index)-- == 0) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *match = find(lv_obj_get_child(object, i), type, index);
        if (match) return match;
    }
    return NULL;
}
static lv_obj_t *nth(const lv_obj_class_t *type, unsigned index)
{
    lv_obj_t *object = find(content, type, &index); check(object); return object;
}
static lv_obj_t *text_find(lv_obj_t *object, const char *text, bool exact)
{
    if (lv_obj_check_type(object, &lv_label_class) &&
        (exact ? !strcmp(lv_label_get_text(object), text) : strstr(lv_label_get_text(object), text) != NULL)) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *match = text_find(lv_obj_get_child(object, i), text, exact);
        if (match) return match;
    }
    return NULL;
}
static void expect(const char *text)
{
    if (!text_find(content, text, false)) fprintf(stderr, "Missing text: %s\n", text);
    check(text_find(content, text, false));
}
static void click(const char *text)
{
    lv_obj_t *caption = text_find(content, text, true); check(caption);
    lv_obj_send_event(lv_obj_get_parent(caption), LV_EVENT_CLICKED, NULL); refresh();
}
static void input(unsigned index, const char *text)
{
    lv_textarea_set_text(nth(&lv_textarea_class, index), text); refresh();
}
static const char *draft(unsigned index) { return lv_textarea_get_text(nth(&lv_textarea_class, index)); }
static void select_option(unsigned index, unsigned selected)
{
    lv_obj_t *object = nth(&lv_dropdown_class, index);
    lv_dropdown_set_selected(object, selected); lv_obj_send_event(object, LV_EVENT_VALUE_CHANGED, NULL); refresh();
}
static void close_app(void)
{
    modbus_rtu_tool_stop(); modbus_rtu_tool_stop();
    lv_obj_clean(content); lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF); refresh();
}
static void open_app(void) { modbus_rtu_tool_show(content); refresh(); }
static void bounds(lv_obj_t *object)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(object, &a);
    if (a.x1 < 28 || a.x2 > 691 || a.y1 < 128 || a.y2 > 1251) {
        fprintf(stderr, "Outside content: (%ld,%ld)-(%ld,%ld)\n", (long)a.x1, (long)a.y1, (long)a.x2, (long)a.y2);
        check(false);
    }
    if (lv_obj_check_type(object, &lv_textarea_class) || lv_obj_check_type(object, &lv_dropdown_class)) return;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) bounds(lv_obj_get_child(object, i));
}
static void shot(const char *name)
{
    refresh();
    for (uint32_t i = 0; i < lv_obj_get_child_count(content); i++) bounds(lv_obj_get_child(content, i));
    if (!output_directory) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.ppm", output_directory, name);
    FILE *file = fopen(path, "wb"); check(file);
    fprintf(file, "P6\n720 1280\n255\n");
    for (size_t i = 0; i < 720U * 1280; i++) {
        uint16_t p = framebuffer[i];
        const unsigned char rgb[] = {(unsigned char)(((p >> 11) & 31) * 255 / 31),
            (unsigned char)(((p >> 5) & 63) * 255 / 63), (unsigned char)((p & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}
static void setup(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(720, 1280);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, framebuffer, NULL, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x10141f), 0);
    lv_obj_t *header = lv_obj_create(screen); lv_obj_set_size(header, 720, 100);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x20283a), 0);
    lv_obj_set_style_text_color(header, lv_color_white(), 0);
    lv_obj_t *home = lv_button_create(header); lv_obj_set_size(home, 120, 64);
    lv_obj_align(home, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *label = lv_label_create(home); lv_label_set_text(label, LV_SYMBOL_HOME); lv_obj_center(label);
    label = lv_label_create(header); lv_label_set_text(label, "Tab5 OS");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0); lv_obj_center(label);
    content = lv_obj_create(screen); lv_obj_set_size(content, 720, 1180);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x10141f), 0);
    lv_obj_set_style_text_color(content, lv_color_white(), 0);
    lv_obj_set_style_border_width(content, 0, 0); lv_obj_set_style_pad_all(content, 28, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}
static void expect_copy(const uint8_t *bytes, size_t length)
{
    const payload_clipboard_t *copy = payload_clipboard_peek();
    check(copy && copy->length == length && !memcmp(copy->bytes, bytes, length));
}

int main(int argc, char **argv)
{
    output_directory = argc > 1 ? argv[1] : NULL;
    setup(); open_app(); shot("rtu-default");
    check(!payload_clipboard_peek());
    input(2, "10"); click("COPY REQUEST");
    const uint8_t request[] = {1, 3, 0, 0, 0, 10, 0xc5, 0xcd};
    expect_copy(request, sizeof(request)); expect("01 03 00 00 00 0A C5 CD");
    const char *invalid_units[] = {"0", "248", "256", "1\n", "+1", "000001"};
    for (size_t i = 0; i < sizeof(invalid_units) / sizeof(invalid_units[0]); i++) {
        input(0, invalid_units[i]); click("COPY REQUEST");
        expect("Unit must be 1 to 247"); expect_copy(request, sizeof(request));
    }
    input(0, "1"); input(1, "65535"); input(2, "2"); click("COPY REQUEST");
    expect("without crossing address 65535"); expect_copy(request, sizeof(request));
    input(1, "1e2"); click("COPY REQUEST"); expect_copy(request, sizeof(request));
    click("EXAMPLE"); expect("0-1 : 1.5"); expect_copy(request, sizeof(request));
    shot("rtu-example");
    select_option(1, MODBUS_VIEW_WORDS); expect("0 : 16320 | 0x3FC0 | 16320");
    select_option(1, MODBUS_VIEW_UINT32); expect("0-1 : 1069547520");
    select_option(2, MODBUS_ORDER_CDAB); expect("0-1 : 16320");
    select_option(2, MODBUS_ORDER_ABCD);
    input(1, "100"); check(!text_find(content, "0-1 :", false));
    click("DECODE"); expect("100-101 : 1069547520"); expect("Address is taken from your request fields");
    char reply[128]; snprintf(reply, sizeof(reply), "%s", draft(3));
    byte_data_t parsed; check(byte_data_parse(reply, BYTE_DATA_HEX, &parsed) == BYTE_DATA_OK);
    check(payload_clipboard_store(parsed.bytes, parsed.length));
    click("CLEAR"); check(!draft(3)[0]); click("PASTE REPLY"); check(!strcmp(draft(3), reply));
    check(!text_find(content, "100-101 :", false)); click("DECODE"); expect("100-101 :");
    payload_clipboard_clear(); click("PASTE REPLY"); check(!strcmp(draft(3), reply));
    check(payload_clipboard_store(NULL, 0)); click("PASTE REPLY"); check(!strcmp(draft(3), reply));
    uint8_t too_big[38] = {0}; check(payload_clipboard_store(too_big, sizeof(too_big)));
    click("PASTE REPLY"); check(!strcmp(draft(3), reply));
    input(3, "01 83 02 C0 F1"); click("DECODE"); expect("exception 0x02");
    check(!text_find(content, "100-101 :", false));
    input(3, "01 83 02 C0 F0"); click("DECODE"); expect("CRC mismatch");
    shot("rtu-crc-error");
    input(3, "01 83 0"); click("DECODE"); expect("hex");
    click("EXAMPLE"); input(2, "1"); click("DECODE"); expect("did not match the request");
    uint8_t maximum[MODBUS_RTU_MAX_RESPONSE_BYTES] = {1, 4, 32};
    memset(maximum + 3, 0xff, 32);
    uint16_t crc = byte_data_crc16_modbus(maximum, sizeof(maximum) - 2);
    maximum[35] = (uint8_t)crc; maximum[36] = (uint8_t)(crc >> 8);
    input(1, "65520"); input(2, "16"); select_option(0, 3); select_option(1, MODBUS_VIEW_WORDS);
    check(payload_clipboard_store(maximum, sizeof(maximum))); click("PASTE REPLY"); click("DECODE");
    expect("65535 : 65535 | 0xFFFF | -1");
    uint8_t coils[] = {1, 1, 2, 0x55, 0x01, 0, 0};
    crc = byte_data_crc16_modbus(coils, sizeof(coils) - 2);
    coils[5] = (uint8_t)crc; coils[6] = (uint8_t)(crc >> 8);
    input(1, "100"); input(2, "9"); select_option(0, 0);
    check(payload_clipboard_store(coils, sizeof(coils))); click("PASTE REPLY"); click("DECODE");
    expect("100 : 1"); expect("101 : 0"); expect("108 : 1");
    select_option(1, MODBUS_VIEW_FLOAT32); expect("Zero-based address : bit value");
    coils[4] = 0x81; crc = byte_data_crc16_modbus(coils, sizeof(coils) - 2);
    coils[5] = (uint8_t)crc; coils[6] = (uint8_t)(crc >> 8);
    check(payload_clipboard_store(coils, sizeof(coils))); click("PASTE REPLY"); click("DECODE");
    expect("did not match the request"); check(!text_find(content, "100 : 1", false));
    click("EXAMPLE"); input(3, "");
    lv_obj_t *keyboard = nth(&lv_keyboard_class, 0);
    lv_obj_send_event(nth(&lv_textarea_class, 3), LV_EVENT_CLICKED, NULL); refresh();
    check(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_USER_1);
    const unsigned keys[] = {0, 1, 17, 8, 3, 17, 0, 2, 17, 12, 0, 17, 15, 1};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        lv_buttonmatrix_set_selected_button(keyboard, keys[i]); lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, NULL);
    }
    check(!strcmp(draft(3), "01 83 02 C0 F1"));
    shot("rtu-hex-keyboard");
    lv_buttonmatrix_set_selected_button(keyboard, 20); lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, NULL);
    check(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    check(!text_find(content, "exception 0x02", false)); click("DECODE"); expect("exception 0x02");
    lv_obj_send_event(nth(&lv_textarea_class, 0), LV_EVENT_CLICKED, NULL); refresh();
    check(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_NUMBER);
    lv_obj_send_event(keyboard, LV_EVENT_CANCEL, NULL); check(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));

    char long_text[514]; memset(long_text, '0', sizeof(long_text) - 1); long_text[513] = 0;
    input(3, long_text); click("DECODE"); check(!strcmp(draft(3), long_text));
    check(!text_find(content, "0-1 : 1.5", false)); close_app(); open_app();
    check(!strcmp(draft(3), long_text));
    char unicode[513 * 4 + 1];
    for (size_t i = 0; i < 513; i++) memcpy(unicode + i * 4, "\xf0\x9f\x98\x80", 4);
    unicode[sizeof(unicode) - 1] = 0;
    input(3, unicode); close_app(); open_app(); check(!strcmp(draft(3), unicode));
    click("COPY REQUEST"); /* Malformed reply does not prevent preparing a read. */
    click("EXAMPLE"); close_app();
    lv_mem_monitor_t before, after; lv_mem_monitor(&before);
    for (unsigned i = 0; i < 8; i++) {
        open_app(); expect("0-1 : 1.5");
        if (i % 2) lv_dropdown_open(nth(&lv_dropdown_class, 0));
        else lv_obj_send_event(nth(&lv_textarea_class, 3), LV_EVENT_CLICKED, NULL);
        close_app();
    }
    lv_mem_monitor(&after);
    check(after.free_size == before.free_size && after.used_cnt == before.used_cnt);
    printf("RTU UI checks and 8 lifecycle cycles passed; free heap %zu -> %zu.\n", before.free_size, after.free_size);
    lv_deinit(); return 0;
}
