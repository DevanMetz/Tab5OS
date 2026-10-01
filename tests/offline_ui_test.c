/* Headless integration checks using real LVGL. Run tools/test_offline_ui.ps1.
 * An optional output-directory argument saves 720x1280 PPM screen captures. */
#include "byte_tool.h"
#include "electronics_tool.h"
#include "subnet_tool.h"
#include "resistor_tool.h"
#include "spi_tool.h"
#include "spi_adapter.h"
#include "i2c_result.h"
#include "payload_clipboard.h"
#include "lvgl.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t framebuffer[720 * 1280];
static lv_obj_t *content;
static const char *output_directory;
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}
static void refresh(void)
{
    lv_obj_update_layout(lv_screen_active());
    lv_tick_inc(40);
    lv_timer_handler();
    lv_refr_now(NULL);
}
static void reset_content(void)
{
    lv_obj_clean(content);
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}
static void shot(const char *name)
{
    refresh();
    lv_tick_inc(300);
    lv_timer_handler();
    lv_refr_now(NULL);
    if (!output_directory)
        return;
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s.ppm", output_directory, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n720 1280\n255\n");
    for (size_t i = 0; i < 720U * 1280; i++) {
        uint16_t p = framebuffer[i];
        unsigned char b[3] = {(unsigned char)(((p >> 11) & 31) * 255 / 31),
                              (unsigned char)(((p >> 5) & 63) * 255 / 63),
                              (unsigned char)((p & 31) * 255 / 31)};
        fwrite(b, 1, 3, f);
    }
    fclose(f);
}
static lv_obj_t *find(lv_obj_t *obj, const lv_obj_class_t *klass, unsigned *index)
{
    if (lv_obj_check_type(obj, klass) && (*index)-- == 0)
        return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *match = find(lv_obj_get_child(obj, i), klass, index);
        if (match)
            return match;
    }
    return NULL;
}
static lv_obj_t *nth(const lv_obj_class_t *klass, unsigned index)
{
    return find(content, klass, &index);
}
static lv_obj_t *label_find(lv_obj_t *obj, const char *text)
{
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text))
        return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *match = label_find(lv_obj_get_child(obj, i), text);
        if (match)
            return match;
    }
    return NULL;
}
static int has_text(lv_obj_t *obj, const char *text)
{
    if (lv_obj_check_type(obj, &lv_label_class) && strstr(lv_label_get_text(obj), text))
        return 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        if (has_text(lv_obj_get_child(obj, i), text))
            return 1;
    return 0;
}
static int click(const char *text)
{
    lv_obj_t *label = label_find(content, text);
    if (!label)
        return 0;
    lv_obj_t *button = lv_obj_get_parent(label);
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    refresh();
    return 1;
}
static void select_dropdown(unsigned index, uint32_t value)
{
    lv_obj_t *d = nth(&lv_dropdown_class, index);
    assert(d);
    lv_dropdown_set_selected(d, value);
    lv_obj_send_event(d, LV_EVENT_VALUE_CHANGED, NULL);
    refresh();
}
static void set_input(unsigned index, const char *value)
{
    lv_obj_t *a = nth(&lv_textarea_class, index);
    assert(a);
    lv_textarea_set_text(a, value);
    refresh();
}
static void hex_key(unsigned index)
{
    lv_obj_t *keyboard = nth(&lv_keyboard_class, 0);
    assert(keyboard && lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_USER_1);
    lv_buttonmatrix_set_selected_button(keyboard, index);
    lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, NULL);
    refresh();
}

static void resistor_bounds(lv_obj_t *object)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    assert(area.x1 >= 0 && area.x2 < 720 && area.y1 >= 100 && area.y2 < 1280);
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++)
        resistor_bounds(lv_obj_get_child(object, i));
}

static void byte_number_checks(void)
{
    lv_obj_send_event(nth(&lv_keyboard_class, 0), LV_EVENT_READY, NULL);
    select_dropdown(0, 2);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "4660"));
    assert(has_text(content, "Encoded Unsigned 16-bit BE: 4660"));
    assert(has_text(content, "HEX\n12 34\n"));
    select_dropdown(2, 1);
    assert(has_text(content, "HEX\n34 12\n"));
    select_dropdown(1, 3); set_input(0, "-1234");
    assert(has_text(content, "Encoded Signed 16-bit LE: -1234"));
    assert(has_text(content, "HEX\n2E FB\n"));
    select_dropdown(1, 0);
    assert(has_text(content, "Invalid number")); assert(!has_text(content, "CRC-32/"));
    assert(click("EXAMPLE")); assert(has_text(content, "HEX\nA5\n"));
    select_dropdown(1, 1); assert(click("EXAMPLE")); assert(has_text(content, "HEX\nD6\n"));
    select_dropdown(1, 4); set_input(0, "4294967295");
    assert(has_text(content, "HEX\nFF FF FF FF\n"));
    select_dropdown(1, 5); set_input(0, "-2147483648");
    assert(has_text(content, "HEX\n00 00 00 80\n"));
    select_dropdown(1, 6); assert(click("EXAMPLE"));
    assert(has_text(content, "Encoded Float32 LE: 1.5"));
    assert(has_text(content, "HEX\n00 00 C0 3F\n"));
    shot("bytes-encode-float32");
    lv_obj_t *keyboard = nth(&lv_keyboard_class, 0);
    lv_obj_send_event(nth(&lv_textarea_class, 0), LV_EVENT_CLICKED, NULL);
    assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_USER_2);
    shot("bytes-encode-keyboard");
    assert(click("CLEAR"));
    const unsigned keys[] = {0, 12, 4, 20}; /* 1 . 5 Done */
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        lv_buttonmatrix_set_selected_button(keyboard, keys[i]);
        lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, NULL); refresh();
    }
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "1.5"));
    assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    set_input(0, "0.1"); assert(has_text(content, "Encoded Float32 LE: 0.100000001"));
    assert(click("USE AS HEX (replaces Hex draft)"));
    assert(lv_dropdown_get_selected(nth(&lv_dropdown_class, 0)) == 0);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "CD CC CC 3D"));
    assert(has_text(content, "Float32 LE: 0.100000001"));
    select_dropdown(0, 1);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "123456789"));
    select_dropdown(0, 2);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "0.1"));
    assert(lv_dropdown_get_selected(nth(&lv_dropdown_class, 1)) == 6);
    assert(lv_dropdown_get_selected(nth(&lv_dropdown_class, 2)) == 1);
    byte_tool_stop(); reset_content(); byte_tool_show(content); refresh();
    assert(has_text(content, "Encoded Float32 LE: 0.100000001"));
    set_input(0, "-0"); assert(has_text(content, "Encoded Float32 LE: -0"));
    set_input(0, "NaN"); assert(has_text(content, "HEX\n00 00 C0 7F\n"));
    set_input(0, "+Inf"); assert(has_text(content, "Encoded Float32 LE: +Infinity"));
    set_input(0, "1e-45"); assert(has_text(content, "HEX\n01 00 00 00\n"));
    const char *bad[] = {"1\n2", "1e", "0x1p0", "1e39", "1e-46", "000000000000000000000000000000000"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        set_input(0, bad[i]);
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), bad[i]));
        assert(!has_text(content, "Encoded ") && !has_text(content, "CRC-32/"));
        lv_obj_t *use = lv_obj_get_parent(label_find(content, "USE AS HEX (replaces Hex draft)"));
        assert(lv_obj_has_state(use, LV_STATE_DISABLED));
        lv_obj_send_event(use, LV_EVENT_CLICKED, NULL);
        assert(lv_dropdown_get_selected(nth(&lv_dropdown_class, 0)) == 2);
    }
    byte_tool_stop(); reset_content(); byte_tool_show(content); refresh();
    assert(has_text(content, "Number exceeds the 32-character limit"));
    char unicode[133];
    for (unsigned i = 0; i < 33; i++) memcpy(unicode + i * 4, "\xf0\x9f\x94\xa5", 4);
    unicode[132] = 0;
    set_input(0, unicode);
    byte_tool_stop(); reset_content(); byte_tool_show(content); refresh();
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), unicode));
    assert(has_text(content, "Invalid number"));
    set_input(0, "1e39"); shot("bytes-encode-invalid");
    assert(click("EXAMPLE"));
    lv_dropdown_open(nth(&lv_dropdown_class, 1)); shot("bytes-number-menu");
    byte_tool_stop(); reset_content(); byte_tool_show(content); refresh();
    /* Retain the first excessive payload character instead of silently making
     * a 513-character draft valid by truncating it to the parser's 512 limit. */
    select_dropdown(0, 0);
    char long_hex[514]; memset(long_hex, ' ', 513); long_hex[0] = '0'; long_hex[1] = '1'; long_hex[513] = 0;
    set_input(0, long_hex); assert(has_text(content, "Input exceeds the 512-character limit"));
    byte_tool_stop(); reset_content(); byte_tool_show(content); refresh();
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), long_hex));
    assert(!has_text(content, "CRC-32/"));
    assert(click("EXAMPLE"));
    printf("BYTE NUMBER all integer widths, Float32/order/rounding, Hex handoff, draft/error retention and keyboard PASS\n");
}

static void byte_clipboard_checks(void)
{
    payload_clipboard_clear();
    assert(click("Clear copy"));
    set_input(0, "12 34");
    assert(click("Copy bytes"));
    const payload_clipboard_t *copy = payload_clipboard_peek();
    assert(copy && copy->length == 2 && copy->bytes[0] == 0x12 && copy->bytes[1] == 0x34);
    set_input(0, "12 GG");
    assert(lv_obj_has_state(lv_obj_get_parent(label_find(content, "Copy bytes")), LV_STATE_DISABLED));
    assert(click("Copy bytes")); /* Even a direct callback cannot copy invalid input. */
    assert(copy->length == 2 && copy->bytes[1] == 0x34);
    assert(click("Paste hex"));
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "12 34"));
    select_dropdown(0, 1); set_input(0, "keep ASCII");
    select_dropdown(0, 2); /* Number draft retained from the encoder checks. */
    assert(has_text(content, "Encoded Float32 LE: 1.5"));
    assert(click("Copy bytes"));
    assert(copy->length == 4 && !memcmp(copy->bytes, "\x00\x00\xc0\x3f", 4));
    assert(click("Paste hex"));
    assert(lv_dropdown_get_selected(nth(&lv_dropdown_class, 0)) == 0);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "00 00 C0 3F"));
    shot("bytes-clipboard");
    select_dropdown(0, 1); assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "keep ASCII"));
    select_dropdown(0, 2); assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "1.5"));
    byte_tool_stop(); reset_content(); byte_tool_show(content); refresh();
    assert(has_text(content, "Byte clipboard: 4 bytes in RAM"));
    assert(click("Paste hex"));
    assert(click("CLEAR")); assert(click("Copy bytes"));
    assert(copy->length == 0 && payload_clipboard_peek());
    set_input(0, "FF"); assert(click("Paste hex"));
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), ""));
    select_dropdown(0, 1);
    char maximum[129]; memset(maximum, 'Z', 128); maximum[128] = 0;
    set_input(0, maximum); assert(click("Copy bytes"));
    assert(copy->length == 128);
    assert(click("Paste hex")); assert(has_text(content, "128 bytes |"));
    assert(strlen(lv_textarea_get_text(nth(&lv_textarea_class, 0))) == 383);
    char bulk_hex[384];
    for (unsigned i = 0; i < 128; i++) {
        if (i) bulk_hex[i * 3 - 1] = ' ';
        snprintf(bulk_hex + i * 3, 3, "%02X", i);
    }
    set_input(0, bulk_hex);
    assert(has_text(content, "128 bytes |"));
    assert(click("Copy bytes"));
    assert(copy->length == 128 && copy->bytes[0] == 0 && copy->bytes[127] == 127);
    lv_textarea_set_text(nth(&lv_textarea_class, 0), "GG");
    lv_obj_send_event(lv_obj_get_parent(label_find(content, "Copy bytes")), LV_EVENT_CLICKED, NULL);
    assert(copy->length == 128 && copy->bytes[127] == 127); /* Guard before the deferred render. */
    lv_textarea_set_text(nth(&lv_textarea_class, 0), bulk_hex);
    byte_tool_stop(); reset_content(); refresh(); /* Cancel a render queued immediately before Home. */
    byte_tool_show(content); refresh();
    assert(has_text(content, "128 bytes |"));
    assert(click("Clear copy")); assert(!payload_clipboard_peek());
    assert(click("Paste hex")); /* Absent clipboard must preserve the draft. */
    assert(strlen(lv_textarea_get_text(nth(&lv_textarea_class, 0))) == 383);
    assert(click("EXAMPLE"));
    printf("BYTE CLIPBOARD Number/ASCII/Hex, empty/max, invalid copy, clear and retained drafts PASS\n");
}

static void spi_expect_copy(const uint8_t *bytes, size_t length)
{
    const payload_clipboard_t *copy = payload_clipboard_peek();
    assert(copy && copy->length == length && !memcmp(copy->bytes, bytes, length));
}

static void spi_copy_enabled(bool enabled)
{
    lv_obj_t *label = label_find(content, "COPY RX");
    assert(label);
    assert(lv_obj_has_state(lv_obj_get_parent(label), LV_STATE_DISABLED) != enabled);
}

static void spi_checks(void)
{
    spi_tool_self_test();
    spi_tool_show(content); refresh();
    assert(!spi_tool_busy() && !spi_host_gpio_writes);
    spi_copy_enabled(false);
    assert(click("COPY RX")); /* Direct events must also validate disabled controls. */
    assert(!payload_clipboard_peek());
    assert(click("PASTE\nBYTES")); assert(has_text(content, "Byte clipboard is empty"));
    assert(payload_clipboard_store((const uint8_t *)"\x00\x00\xc0\x3f", 4));
    assert(click("PASTE\nBYTES"));
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "00 00 C0 3F"));
    assert(click("COPY RX")); spi_expect_copy((const uint8_t *)"\x00\x00\xc0\x3f", 4);
    assert(!spi_host_starts && !spi_host_transfers && !spi_host_gpio_writes);
    assert(click("TRANSFER")); assert(has_text(content, "Press START"));
    uint8_t bytes[128]; for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)i;
    const size_t rejected[] = {0, 33, 128};
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        assert(payload_clipboard_store(bytes, rejected[i]));
        assert(click("PASTE\nBYTES")); assert(has_text(content, "draft unchanged"));
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "00 00 C0 3F"));
    }
    assert(!spi_host_starts && !spi_host_transfers && !spi_host_gpio_writes);
    assert(click("START")); assert(spi_tool_busy() && spi_host_starts == 1);
    const char *invalid[] = {"", "0", "00GG", "00;FF", "0\n0", "0011", ("00\xc2\xa0" "11")};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        set_input(0, invalid[i]);
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), invalid[i]));
        assert(click("TRANSFER")); assert(has_text(content, "Enter 1-32"));
        assert(!spi_host_transfers);
    }
    char excessive[99];
    for (unsigned i = 0; i < 33; i++) snprintf(excessive + 3 * i, 3, "%02X", i);
    for (unsigned i = 0; i < 32; i++) excessive[3 * i + 2] = ' ';
    set_input(0, excessive); /* UI retains the 96th character: parser must reject. */
    assert(strlen(lv_textarea_get_text(nth(&lv_textarea_class, 0))) == 96);
    assert(click("TRANSFER")); assert(!spi_host_transfers);
    assert(payload_clipboard_store(bytes, 32)); assert(click("PASTE\nBYTES"));
    assert(!spi_host_transfers && spi_host_starts == 1);
    for (size_t i = 0; i < 32; i++) spi_host_reply[i] = (uint8_t)(255 - i);
    spi_host_reply_length = 32;
    assert(click("TRANSFER")); assert(spi_host_transfers == 1);
    assert(spi_host_last_length == 32 && !memcmp(spi_host_last_tx, bytes, 32));
    assert(strstr(lv_textarea_get_text(nth(&lv_textarea_class, 1)), "RX (32): FF FE FD FC"));
    assert(has_text(content, "Saved: mode 0 | 1000000 Hz | 32 bytes"));
    assert(has_text(content, "Transferred 32 bytes"));
    spi_expect_copy(bytes, 32); /* A transfer alone must not change the clipboard. */
    spi_copy_enabled(true);
    assert(click("COPY RX")); spi_expect_copy(spi_host_reply, 32);
    assert(spi_host_starts == 1 && spi_host_transfers == 1 && spi_host_gpio_writes == 2);
    assert(lv_obj_get_scroll_bottom(content) <= 0); /* Negative means spare space. */
    shot("spi-clipboard");
    set_input(0, "G1"); assert(click("TRANSFER"));
    assert(spi_host_transfers == 1 && has_text(content, "Saved: mode 0 | 1000000 Hz | 32 bytes"));
    assert(click("COPY RX")); spi_expect_copy(spi_host_reply, 32);
    spi_host_reply_length = 0;
    set_input(0, "A5\n5A"); assert(click("TRANSFER"));
    assert(spi_host_transfers == 2); /* Whitespace between complete pairs is valid. */
    assert(spi_host_last_length == 2 && spi_host_last_tx[0] == 0xa5 && spi_host_last_tx[1] == 0x5a);
    assert(click("COPY RX")); spi_expect_copy((const uint8_t *)"\xa5\x5a", 2);
    assert(click("STOP")); assert(!spi_tool_busy());
    assert(!spi_host_bus_owned && !spi_host_device_owned);
    assert(click("Mode\n0")); assert(click("Clock\n1 MHz"));
    assert(has_text(content, "Saved: mode 0 | 1000000 Hz | 2 bytes"));
    set_input(0, "DE AD");
    assert(click("COPY RX")); spi_expect_copy((const uint8_t *)"\xa5\x5a", 2);
    assert(spi_host_starts == 1 && spi_host_transfers == 2);
    assert(click("START"));
    assert(spi_host_mode == 1 && spi_host_clock_hz == 5000000);
    spi_host_fail_next_transfer = true;
    assert(click("TRANSFER")); assert(spi_host_transfers == 3);
    assert(has_text(content, "SPI transfer failed: ESP_FAIL. Saved result cleared"));
    assert(!strlen(lv_textarea_get_text(nth(&lv_textarea_class, 1))));
    spi_copy_enabled(false);
    spi_expect_copy((const uint8_t *)"\xa5\x5a", 2);
    shot("spi-failed-transfer");
    assert(click("COPY RX")); spi_expect_copy((const uint8_t *)"\xa5\x5a", 2);
    spi_tool_stop(); reset_content(); spi_tool_show(content); refresh();
    assert(!spi_host_bus_owned && !spi_host_device_owned);
    spi_copy_enabled(false);
    assert(!strlen(lv_textarea_get_text(nth(&lv_textarea_class, 1))));
    assert(click("START"));
    set_input(0, "7E"); assert(click("TRANSFER"));
    assert(spi_host_transfers == 4);
    assert(click("COPY RX")); spi_expect_copy((const uint8_t *)"\x7e", 1);
    assert(click("CLEAR RESULT"));
    spi_copy_enabled(false);
    assert(!strlen(lv_textarea_get_text(nth(&lv_textarea_class, 1))));
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "7E"));
    spi_expect_copy((const uint8_t *)"\x7e", 1);
    assert(click("COPY RX")); spi_expect_copy((const uint8_t *)"\x7e", 1);
    assert(click("CLEAR RESULT")); /* Repeated clear is harmless. */

    memcpy(spi_host_reply, "\x00\x00\xc0\x3f", 4);
    spi_host_reply_length = 4;
    set_input(0, "FF FF FF FF"); assert(click("TRANSFER"));
    assert(spi_host_starts == 3 && spi_host_transfers == 5);
    assert(has_text(content, "Saved: mode 1 | 5000000 Hz | 4 bytes"));
    assert(strstr(lv_textarea_get_text(nth(&lv_textarea_class, 1)), "TX (4): FF FF FF FF\nRX (4): 00 00 C0 3F"));
    assert(click("COPY RX")); spi_expect_copy(spi_host_reply, 4);
    set_input(0, "00"); set_input(1, "Displayed text is not the copy source");
    assert(click("COPY RX")); spi_expect_copy(spi_host_reply, 4);
    payload_clipboard_clear();
    assert(click("COPY RX")); spi_expect_copy(spi_host_reply, 4);
    spi_tool_stop(); reset_content(); spi_tool_show(content); refresh();
    assert(!spi_tool_busy() && !spi_host_bus_owned && !spi_host_device_owned);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "00"));
    assert(has_text(content, "Saved: mode 1 | 5000000 Hz | 4 bytes"));
    assert(strstr(lv_textarea_get_text(nth(&lv_textarea_class, 1)), "RX (4): 00 00 C0 3F"));
    assert(click("TRANSFER")); assert(has_text(content, "Press START"));
    spi_copy_enabled(true);
    assert(click("COPY RX")); spi_expect_copy(spi_host_reply, 4);
    assert(spi_host_starts == 3 && spi_host_transfers == 5 && spi_host_gpio_writes == 6);
    shot("spi-retained-reply");
    spi_host_reply_length = 0;
    spi_tool_stop(); spi_tool_stop(); reset_content(); refresh();
    printf("SPI CLIPBOARD raw RX, 1/32-byte bounds, immutable source settings, no-traffic copy/paste, "
           "driver failure, clear, Home retention and pin cleanup PASS\n");
}

static void spi_keyboard_show(lv_event_code_t event)
{
    lv_obj_t *input = nth(&lv_textarea_class, 0);
    lv_obj_send_event(input, event, NULL); refresh();
    lv_obj_t *keys = nth(&lv_keyboard_class, 0);
    assert(keys && !lv_obj_has_flag(keys, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keys) == input);
    assert(lv_keyboard_get_mode(keys) == LV_KEYBOARD_MODE_USER_1);
    assert(lv_obj_get_scroll_bottom(content) <= 0);
}

static void spi_keyboard_hidden(void)
{
    lv_obj_t *keys = nth(&lv_keyboard_class, 0);
    assert(keys && lv_obj_has_flag(keys, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_keyboard_get_textarea(keys));
}

static void spi_editor_checks(void)
{
    unsigned starts = spi_host_starts, transfers = spi_host_transfers, writes = spi_host_gpio_writes;
    spi_tool_show(content); refresh(); spi_keyboard_hidden();
    set_input(0, "");
    spi_keyboard_show(LV_EVENT_FOCUSED);
    const unsigned keys[] = {9, 15, 17, 0, 0, 17, 10, 5}; /* 9F 00 A5 */
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) hex_key(keys[i]);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "9F 00 A5"));
    hex_key(16); hex_key(19); /* Move left, then remove A. */
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "9F 00 5"));
    hex_key(10); hex_key(18); hex_key(19); hex_key(5);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "9F 00 A5"));
    shot("spi-hex-editor");
    hex_key(20); spi_keyboard_hidden(); /* Done cannot start or send. */
    assert(spi_host_starts == starts && spi_host_transfers == transfers && spi_host_gpio_writes == writes);
    spi_keyboard_show(LV_EVENT_CLICKED);
    lv_obj_send_event(nth(&lv_keyboard_class, 0), LV_EVENT_CANCEL, NULL); refresh();
    spi_keyboard_hidden();
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "9F 00 A5"));
    spi_keyboard_show(LV_EVENT_CLICKED);
    spi_tool_stop(); spi_tool_stop(); reset_content(); spi_tool_show(content); refresh();
    spi_keyboard_hidden();
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "9F 00 A5"));
    assert(has_text(content, "Saved: mode 1 | 5000000 Hz | 4 bytes"));
    spi_keyboard_show(LV_EVENT_CLICKED);
    assert(click("CLEAR TX")); spi_keyboard_hidden();
    assert(!strlen(lv_textarea_get_text(nth(&lv_textarea_class, 0))));
    assert(has_text(content, "Saved: mode 1 | 5000000 Hz | 4 bytes"));
    spi_expect_copy((const uint8_t *)"\x00\x00\xc0\x3f", 4);
    assert(click("CLEAR TX"));
    spi_tool_stop(); reset_content(); spi_tool_show(content); refresh();
    assert(!strlen(lv_textarea_get_text(nth(&lv_textarea_class, 0))));
    assert(spi_host_starts == starts && spi_host_transfers == transfers && spi_host_gpio_writes == writes);

    char maximum[96], excessive[99], excess_prefix[97], excess_unicode[100], unicode[385];
    for (unsigned i = 0; i < 33; i++) snprintf(excessive + 3 * i, 3, "%02X", i);
    for (unsigned i = 0; i < 32; i++) excessive[3 * i + 2] = ' ';
    memcpy(maximum, excessive, 95); maximum[95] = '\0';
    memcpy(excess_prefix, excessive, 96); excess_prefix[96] = '\0';
    memcpy(excess_unicode, maximum, 95); memcpy(excess_unicode + 95, "\xf0\x9f\x94\xa5", 5);
    for (unsigned i = 0; i < 96; i++) memcpy(unicode + 4 * i, "\xf0\x9f\x94\xa5", 4);
    unicode[384] = '\0';
    const struct { const char *input, *retained; bool rejected; } drafts[] = {
        {maximum, maximum, false},
        {"0\n0", "0\n0", true},
        {"00;FF", "00;FF", true},
        {"00\xc2\xa0" "11", "00\xc2\xa0" "11", true},
        {excessive, excess_prefix, true},
        {excess_unicode, excess_unicode, true},
        {unicode, unicode, true}
    };
    for (size_t i = 0; i < sizeof(drafts) / sizeof(drafts[0]); i++) {
        /* Leave immediately after the final event, without a deferred render. */
        lv_textarea_set_text(nth(&lv_textarea_class, 0), drafts[i].input);
        spi_tool_stop(); reset_content(); spi_tool_show(content); refresh();
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), drafts[i].retained));
        spi_keyboard_hidden();
        assert(!spi_tool_busy() && !spi_host_bus_owned && !spi_host_device_owned);
        if (drafts[i].rejected) {
            assert(click("START"));
            assert(click("TRANSFER")); assert(has_text(content, "Enter 1-32"));
            assert(spi_host_transfers == transfers);
            assert(click("STOP"));
        }
        assert(has_text(content, "Saved: mode 1 | 5000000 Hz | 4 bytes"));
        spi_expect_copy((const uint8_t *)"\x00\x00\xc0\x3f", 4);
    }
    set_input(0, "9F 00 A5");
    spi_tool_stop(); reset_content(); refresh();
    printf("SPI EDITOR hex keys, cursor/backspace, no-send Done/Cancel, independent clear, "
           "maximum/invalid/UTF-8 draft retention and rejection after Home PASS\n");
}

static void spi_only_checks(void)
{
    payload_clipboard_clear();
    spi_checks();
    spi_editor_checks();
    byte_tool_show(content); refresh();
    assert(click("Paste hex"));
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "00 00 C0 3F"));
    assert(has_text(content, "Float32 LE: 1.5"));
    shot("spi-byte-lab");
    byte_tool_stop(); reset_content(); refresh();
    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    const unsigned starts = spi_host_starts, transfers = spi_host_transfers, writes = spi_host_gpio_writes;
    for (unsigned i = 0; i < 8; i++) {
        spi_tool_show(content); refresh();
        assert(has_text(content, "Saved: mode 1 | 5000000 Hz | 4 bytes"));
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "9F 00 A5"));
        spi_keyboard_hidden();
        assert(click("COPY RX")); spi_expect_copy((const uint8_t *)"\x00\x00\xc0\x3f", 4);
        assert(!spi_tool_busy() && !spi_host_bus_owned && !spi_host_device_owned);
        assert(spi_host_starts == starts && spi_host_transfers == transfers && spi_host_gpio_writes == writes);
        if (i % 2) spi_keyboard_show(LV_EVENT_CLICKED); /* Leave with keys attached. */
        spi_tool_stop(); spi_tool_stop(); reset_content(); refresh();
    }
    lv_mem_monitor(&after);
    printf("SPI TO BYTE LAB Float32 1.5 PASS | 8 REOPEN CYCLES "
           "free=%zu -> %zu allocations=%zu -> %zu\n",
           before.free_size, after.free_size, before.used_cnt, after.used_cnt);
    assert(after.free_size >= before.free_size && after.used_cnt == before.used_cnt);
    assert(lv_mem_test() == LV_RESULT_OK);
}

static unsigned i2c_result_actions;
static bool i2c_pending_write;
static void cancel_i2c_write(void)
{
    i2c_result_actions++;
    i2c_pending_write = false;
}

static void record_i2c_byte(bool success, uint8_t address, uint8_t reg,
                            uint32_t speed_hz, uint8_t value)
{
    i2c_result_record(success, address, reg, speed_hz, &value, 1);
}

static void i2c_result_checks(void)
{
    const uint8_t previous[] = {0x5a, 0};
    assert(payload_clipboard_store(previous, sizeof(previous)));
    i2c_result_show(content, cancel_i2c_write); refresh();
    assert(has_text(content, "Saved read: none"));
    lv_obj_t *copy = lv_obj_get_parent(label_find(content, "COPY READ"));
    assert(lv_obj_has_state(copy, LV_STATE_DISABLED));
    i2c_pending_write = true;
    assert(click("COPY READ")); /* The callback also guards a direct disabled event. */
    assert(!i2c_pending_write && i2c_result_actions == 1);
    const payload_clipboard_t *payload = payload_clipboard_peek();
    assert(payload && payload->length == 2 && !memcmp(payload->bytes, previous, 2));

    record_i2c_byte(true, 0x08, 0, 100000, 0); refresh();
    assert(has_text(content, "addr 0x08 | reg 0x00 | 100 kHz"));
    assert(has_text(content, "Byte: 0x00 (0)"));
    assert(payload_clipboard_peek()->length == 2); /* Reads alone never copy. */
    assert(!lv_obj_has_state(copy, LV_STATE_DISABLED));
    assert(click("COPY READ"));
    assert(payload_clipboard_peek()->length == 1 && payload_clipboard_peek()->bytes[0] == 0);
    record_i2c_byte(true, 0x77, 0xff, 400000, 0xff); refresh();
    assert(has_text(content, "addr 0x77 | reg 0xFF | 400 kHz"));
    assert(has_text(content, "Byte: 0xFF (255)"));
    assert(click("COPY READ"));
    assert(payload_clipboard_peek()->bytes[0] == 0xff);

    record_i2c_byte(false, 0x3c, 0x10, 100000, 0xee); refresh();
    assert(has_text(content, "Saved read: none"));
    assert(lv_obj_has_state(copy, LV_STATE_DISABLED));
    assert(click("COPY READ"));
    assert(payload_clipboard_peek()->length == 1 && payload_clipboard_peek()->bytes[0] == 0xff);
    shot("i2c-result-invalid");
    i2c_result_stop(); i2c_result_stop(); reset_content();
    i2c_result_show(content, cancel_i2c_write); refresh();
    assert(has_text(content, "Saved read: none"));
    record_i2c_byte(true, 0x3c, 0x10, 400000, 0xa5); refresh();
    i2c_result_stop(); reset_content(); i2c_result_show(content, cancel_i2c_write); refresh();
    assert(has_text(content, "addr 0x3C | reg 0x10 | 400 kHz"));
    assert(has_text(content, "Byte: 0xA5 (165)"));
    /* Corrupting display text must not change the raw-byte copy source. */
    lv_label_set_text(nth(&lv_label_class, 2), "Displayed text is not the saved byte");
    i2c_pending_write = true;
    unsigned actions = i2c_result_actions;
    assert(click("COPY READ"));
    assert(!i2c_pending_write && i2c_result_actions == actions + 1);
    assert(payload_clipboard_peek()->length == 1 && payload_clipboard_peek()->bytes[0] == 0xa5);
    assert(has_text(content, "addr 0x3C | reg 0x10 | 400 kHz"));
    shot("i2c-result-copy");
    i2c_pending_write = true; actions = i2c_result_actions;
    assert(click("CLEAR READ"));
    assert(!i2c_pending_write && i2c_result_actions == actions + 1);
    assert(has_text(content, "Saved read: none"));
    assert(payload_clipboard_peek()->bytes[0] == 0xa5);
    assert(click("COPY READ")); assert(payload_clipboard_peek()->bytes[0] == 0xa5);
    i2c_result_stop(); reset_content(); i2c_result_show(content, NULL); refresh();
    assert(has_text(content, "Saved read: none"));
    record_i2c_byte(true, 0x3c, 0x10, 400000, 0xa5);
    i2c_result_clear(); /* A confirmed write invalidates a prior read before bus I/O. */
    assert(has_text(content, "Saved read: none"));
    assert(payload_clipboard_peek()->bytes[0] == 0xa5);
    i2c_result_stop(); reset_content();
    i2c_result_clear(); /* No UI pointers remain. */
    record_i2c_byte(true, 0x3c, 0x10, 400000, 0xa5);
    i2c_result_show(content, cancel_i2c_write); refresh();
    payload_clipboard_clear(); assert(click("COPY READ"));
    assert(payload_clipboard_peek()->length == 1 && payload_clipboard_peek()->bytes[0] == 0xa5);
    i2c_result_stop(); reset_content(); refresh();

    i2c_result_show(content, cancel_i2c_write); refresh();
    uint8_t bytes[33], expected[32];
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)(i * 7);
    memcpy(expected, bytes, sizeof(expected));
    for (size_t length = 1; length <= 32; length++) {
        i2c_result_record(true, 0x68, 0x3b, 100000, bytes, length); refresh();
        assert(has_text(content, "addr 0x68 | reg 0x3B | 100 kHz"));
        assert(click("COPY READ"));
        payload = payload_clipboard_peek();
        assert(payload && payload->length == length && !memcmp(payload->bytes, expected, length));
    }
    memset(bytes, 0xee, sizeof(bytes)); /* The panel must own a copy of RX. */
    assert(click("COPY READ"));
    assert(payload_clipboard_peek()->length == 32 && !memcmp(payload_clipboard_peek()->bytes, expected, 32));
    assert(has_text(content, "Bytes (32): 00 07 0E 15"));
    lv_area_t bounds; lv_obj_get_coords(nth(&lv_label_class, 2), &bounds);
    assert(bounds.x1 >= 28 && bounds.x2 <= 691 && bounds.y2 <= 1251);
    shot("i2c-result-block");
    const size_t rejected_lengths[] = {0, 33, SIZE_MAX};
    for (size_t i = 0; i < sizeof(rejected_lengths) / sizeof(rejected_lengths[0]); i++) {
        i2c_result_record(true, 0x68, 0x3b, 100000, bytes, rejected_lengths[i]); refresh();
        assert(has_text(content, "Saved read: none"));
        assert(lv_obj_has_state(lv_obj_get_parent(label_find(content, "COPY READ")), LV_STATE_DISABLED));
        assert(click("COPY READ"));
        assert(payload_clipboard_peek()->length == 32 && !memcmp(payload_clipboard_peek()->bytes, expected, 32));
    }
    i2c_result_record(true, 0x68, 0x3b, 100000, NULL, 4); refresh();
    assert(has_text(content, "Saved read: none"));
    i2c_result_record(false, 0x68, 0x3b, 100000, bytes, 32); refresh();
    assert(has_text(content, "Saved read: none"));
    assert(click("COPY READ"));
    assert(payload_clipboard_peek()->length == 32 && !memcmp(payload_clipboard_peek()->bytes, expected, 32));
    record_i2c_byte(true, 0x3c, 0x10, 400000, 0xa5); assert(click("COPY READ"));
    i2c_result_stop(); reset_content(); refresh();

    lv_mem_monitor_t before, after; lv_mem_monitor(&before);
    for (unsigned i = 0; i < 8; i++) {
        i2c_result_show(content, cancel_i2c_write); refresh();
        assert(has_text(content, "addr 0x3C | reg 0x10 | 400 kHz"));
        i2c_pending_write = true; actions = i2c_result_actions;
        assert(click("COPY READ"));
        assert(!i2c_pending_write && i2c_result_actions == actions + 1);
        assert(payload_clipboard_peek()->length == 1 && payload_clipboard_peek()->bytes[0] == 0xa5);
        i2c_result_stop(); i2c_result_stop(); reset_content(); refresh();
    }
    lv_mem_monitor(&after);
    assert(after.free_size == before.free_size && after.used_cnt == before.used_cnt);
    assert(lv_mem_test() == LV_RESULT_OK);
    printf("I2C RESULT 1-32 bytes, raw 00/FF/A5, owned buffer, invalid lengths, source identity, failed-read/clear invalidation, "
           "action callback and 8 reopen cycles PASS | free=%zu -> %zu\n", before.free_size, after.free_size);
}

static void resistor_checks(void)
{
    resistor_tool_show(content);
    refresh();
    assert(has_text(content, "Nominal: 4.7 kohm"));
    assert(has_text(content, "Tolerance: +/- 5%"));
    assert(has_text(content, "Range: 4.465 kohm to 4.935 kohm"));
    assert(has_text(content, "47 x 10^(2) = 4700 ohm"));
    assert(lv_dropdown_get_option_count(nth(&lv_dropdown_class, 1)) == 9);
    assert(lv_dropdown_get_option_count(nth(&lv_dropdown_class, 2)) == 10);
    assert(lv_dropdown_get_option_count(nth(&lv_dropdown_class, 3)) == 12);
    assert(lv_dropdown_get_option_count(nth(&lv_dropdown_class, 4)) == 8);
    resistor_bounds(content);
    shot("resistor-four-bands");
    select_dropdown(1, 0); /* Brown first digit, excluding black. */
    select_dropdown(2, 0);
    select_dropdown(3, 11); /* Silver multiplier. */
    select_dropdown(4, 7);  /* Silver tolerance. */
    assert(has_text(content, "Nominal: 0.1 ohm"));
    assert(has_text(content, "Range: 0.09 ohm to 0.11 ohm"));
    select_dropdown(0, 1);
    assert(has_text(content, "Nominal: 10 kohm"));
    assert(has_text(content, "Tolerance: +/- 1%"));
    assert(lv_dropdown_get_option_count(nth(&lv_dropdown_class, 3)) == 10);
    assert(lv_dropdown_get_option_count(nth(&lv_dropdown_class, 4)) == 12);
    assert(lv_dropdown_get_option_count(nth(&lv_dropdown_class, 5)) == 8);
    resistor_bounds(content);
    shot("resistor-five-bands");
    select_dropdown(1, 8);
    select_dropdown(2, 9);
    select_dropdown(3, 9);
    select_dropdown(4, 9);
    select_dropdown(5, 7);
    assert(has_text(content, "Nominal: 999 Gohm"));
    assert(has_text(content, "Range: 899.1 Gohm to 1098.9 Gohm"));
    resistor_bounds(content);
    shot("resistor-maximum");
    select_dropdown(1, 1); /* Red */
    select_dropdown(2, 3); /* Orange */
    select_dropdown(3, 7); /* Violet */
    select_dropdown(4, 0); /* x1 */
    select_dropdown(5, 0); /* 1% */
    assert(has_text(content, "Nominal: 237 ohm"));
    assert(has_text(content, "Range: 234.63 ohm to 239.37 ohm"));
    select_dropdown(0, 0);
    assert(has_text(content, "Nominal: 0.1 ohm"));
    assert(click("Example"));
    assert(has_text(content, "Nominal: 4.7 kohm"));
    select_dropdown(0, 1);
    assert(has_text(content, "Nominal: 237 ohm"));
    /* An open dropdown must leave no detached popup after Home. */
    lv_dropdown_open(nth(&lv_dropdown_class, 5));
    refresh();
    assert(lv_dropdown_is_open(nth(&lv_dropdown_class, 5)));
    shot("resistor-color-menu");
    resistor_tool_stop(); reset_content(); resistor_tool_show(content); refresh();
    assert(has_text(content, "Nominal: 237 ohm"));
    assert(!lv_dropdown_is_open(nth(&lv_dropdown_class, 5)));
    select_dropdown(0, 2);
    assert(has_text(content, "Nominal: 10 kohm"));
    assert(has_text(content, "Tolerance: not established"));
    assert(!has_text(content, "Range:"));
    set_input(0, "8R25");
    assert(has_text(content, "Nominal: 8.25 ohm"));
    set_input(0, "10C");
    assert(has_text(content, "Nominal: 12.4 kohm"));
    assert(has_text(content, "EIA-96: indexed significant value"));
    resistor_bounds(content);
    shot("resistor-smd-eia96");
    set_input(0, "000");
    assert(has_text(content, "Nominal: 0 ohm"));
    assert(has_text(content, "Zero-ohm jumper marking"));
    set_input(0, "97A");
    assert(has_text(content, "Marking not decoded"));
    assert(!has_text(content, "Nominal:"));
    assert(!has_text(content, "Zero-ohm jumper marking"));
    set_input(0, "10\n3");
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "10\n3"));
    assert(has_text(content, "Marking not decoded"));
    assert(!has_text(content, "Nominal:"));
    set_input(0, "-103");
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "-103"));
    assert(has_text(content, "Marking not decoded"));
    set_input(0, "10300");
    assert(has_text(content, "Marking is too long"));
    resistor_tool_stop(); reset_content(); resistor_tool_show(content); refresh();
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "10300"));
    assert(has_text(content, "Marking not decoded"));
    assert(!has_text(content, "Nominal:"));
    resistor_bounds(content);
    shot("resistor-invalid");
    set_input(0, "\xf0\x9f\x94\xa5\xf0\x9f\x94\xa5\xf0\x9f\x94\xa5\xf0\x9f\x94\xa5\xf0\x9f\x94\xa5");
    assert(strlen(lv_textarea_get_text(nth(&lv_textarea_class, 0))) == 20);
    resistor_tool_stop(); reset_content(); resistor_tool_show(content); refresh();
    assert(strlen(lv_textarea_get_text(nth(&lv_textarea_class, 0))) == 20);
    assert(has_text(content, "Marking not decoded"));
    assert(click("Example"));
    assert(has_text(content, "Nominal: 10 kohm"));
    set_input(0, "");
    lv_obj_send_event(nth(&lv_textarea_class, 0), LV_EVENT_CLICKED, NULL);
    lv_obj_t *kb = nth(&lv_keyboard_class, 0);
    assert(!lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    hex_key(3); hex_key(10); hex_key(6); /* 4R7 */
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "4R7"));
    assert(has_text(content, "Nominal: 4.7 ohm"));
    resistor_bounds(content);
    shot("resistor-smd-keyboard");
    hex_key(25);
    assert(lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(kb) == NULL);
    lv_obj_send_event(nth(&lv_textarea_class, 0), LV_EVENT_CLICKED, NULL);
    lv_obj_send_event(kb, LV_EVENT_CANCEL, NULL);
    assert(lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    set_input(0, "");
    lv_obj_send_event(nth(&lv_textarea_class, 0), LV_EVENT_CLICKED, NULL);
    hex_key(0); hex_key(9); hex_key(13); hex_key(24); hex_key(13); hex_key(25); /* 10C */
    assert(has_text(content, "Nominal: 12.4 kohm"));
    select_dropdown(0, 0);
    assert(has_text(content, "Nominal: 4.7 kohm"));
    select_dropdown(0, 2);
    assert(has_text(content, "Nominal: 12.4 kohm"));
    resistor_tool_stop(); reset_content(); resistor_tool_show(content); refresh();
    assert(has_text(content, "Nominal: 12.4 kohm"));
    assert(lv_dropdown_get_selected(nth(&lv_dropdown_class, 0)) == 2);
    assert(lv_obj_has_flag(nth(&lv_keyboard_class, 0), LV_OBJ_FLAG_HIDDEN));
    resistor_tool_stop(); resistor_tool_stop(); reset_content(); refresh();
    printf("RESISTOR band roles/colors, SMD, invalid/stale input, keyboard, retained drafts, and bounds PASS\n");
}

int main(int argc, char **argv)
{
    bool spi_only = false;
    bool i2c_only = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--spi-only")) spi_only = true;
        else if (!strcmp(argv[i], "--i2c-only")) i2c_only = true;
        else {
            assert(!output_directory);
            output_directory = argv[i];
        }
    }
#ifdef _WIN32
    /* Failed assertions must exit the runner instead of opening a CRT dialog. */
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    lv_init();
    lv_display_t *display = lv_display_create(720, 1280);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, framebuffer, NULL, sizeof(framebuffer),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x10141f), 0);
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, 720, 100);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x20283a), 0);
    lv_obj_set_style_text_color(header, lv_color_white(), 0);
    lv_obj_t *home = lv_button_create(header);
    lv_obj_set_size(home, 120, 64);
    lv_obj_align(home, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *hl = lv_label_create(home);
    lv_label_set_text(hl, LV_SYMBOL_HOME);
    lv_obj_center(hl);
    lv_obj_t *brand = lv_label_create(header);
    lv_label_set_text(brand, "Tab5 OS");
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_28, 0);
    lv_obj_center(brand);
    content = lv_obj_create(screen);
    lv_obj_set_size(content, 720, 1180);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x10141f), 0);
    lv_obj_set_style_text_color(content, lv_color_white(), 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 28, 0);
    lv_obj_set_style_pad_row(content, 12, 0);
    reset_content();
    if (i2c_only) {
        i2c_result_checks();
        byte_tool_show(content); refresh();
        assert(click("Paste hex"));
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "A5"));
        assert(has_text(content, "Unsigned 8-bit\nBE 165 | LE 165"));
        assert(has_text(content, "Signed 8-bit (two's complement)\nBE -91 | LE -91"));
        shot("i2c-byte-lab");
        byte_tool_stop(); reset_content(); refresh();
        i2c_result_show(content, cancel_i2c_write); refresh();
        const uint8_t float_bytes[] = {0, 0, 0xc0, 0x3f};
        i2c_result_record(true, 0x68, 0x20, 400000, float_bytes, sizeof(float_bytes));
        assert(click("COPY READ"));
        i2c_result_stop(); reset_content(); byte_tool_show(content); refresh();
        assert(click("Paste hex"));
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "00 00 C0 3F"));
        assert(has_text(content, "Float32 LE: 1.5"));
        shot("i2c-block-byte-lab");
        byte_tool_stop(); reset_content(); refresh();
        printf("I2C SAVED READ -> BYTE LAB unsigned/signed/Float32 decoding PASS\n");
        lv_deinit();
        return 0;
    }
    if (spi_only) {
        spi_only_checks();
        lv_deinit();
        return 0;
    }
    electronics_tool_show(content);
    shot("electronics-default");
    assert(has_text(content, "Current: 5 mA"));
    select_dropdown(0, 0);
    assert(has_text(content, "Voltage: 3 V"));
    select_dropdown(0, 2);
    assert(has_text(content, "Resistance: 330 ohm"));
    assert(click("Divider"));
    assert(has_text(content, "Output: 2.5 V"));
    shot("electronics-divider");
    assert(click("LED resistor"));
    assert(has_text(content, "Series resistor: 150 ohm"));
    shot("electronics-led");
    set_input(0, "2");
    assert(click("Calculate"));
    assert(has_text(content, "Supply voltage must be higher"));
    shot("electronics-led-invalid");
    set_input(0, "5");
    assert(click("Calculate"));
    set_input(2, "0");
    assert(click("Calculate"));
    assert(has_text(content, "enter a decimal"));
    set_input(2, "20");
    assert(click("Calculate"));
    lv_obj_t *kb = nth(&lv_keyboard_class, 0);
    assert(kb);
    lv_obj_send_event(kb, LV_EVENT_READY, NULL);
    assert(lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(nth(&lv_textarea_class, 0), LV_EVENT_CLICKED, NULL);
    assert(!lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    assert(click("RC filter"));
    assert(has_text(content, "Time constant: 1 ms"));
    assert(has_text(content, "Cutoff (-3 dB): 159.155 Hz"));
    assert(has_text(content, "10-90% rise: 2.19722 ms"));
    assert(has_text(content, "1% settling: 4.60517 ms"));
    assert(lv_obj_has_flag(lv_obj_get_parent(nth(&lv_textarea_class, 2)), LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(kb, LV_EVENT_READY, NULL);
    shot("electronics-rc");
    set_input(0, "-10000");
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "-10000"));
    assert(click("Calculate"));
    assert(has_text(content, "enter a decimal"));
    assert(!has_text(content, "Time constant:"));
    set_input(0, "1e3");
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "1e3"));
    assert(click("Calculate"));
    assert(has_text(content, "enter a decimal"));
    set_input(0, "1\n2");
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "1\n2"));
    assert(click("Calculate"));
    assert(has_text(content, "enter a decimal"));
    assert(!has_text(content, "Time constant:"));
    set_input(0, "0000000000000000000000012"); /* 25 characters must be rejected. */
    assert(click("Calculate"));
    assert(has_text(content, "enter a decimal"));
    electronics_tool_stop();
    reset_content();
    electronics_tool_show(content);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "0000000000000000000000012"));
    assert(has_text(content, "enter a decimal"));
    set_input(0, "1000");
    set_input(1, "100nF");
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 1)), "100nF"));
    assert(click("Calculate"));
    assert(has_text(content, "Capacitance (uF): enter a decimal"));
    shot("electronics-rc-invalid");
    set_input(1, "0");
    assert(click("Calculate"));
    assert(has_text(content, "enter a decimal"));
    set_input(1, "2.2");
    assert(click("Calculate"));
    assert(has_text(content, "Time constant: 2.2 ms"));
    assert(click("LED resistor"));
    assert(has_text(content, "Series resistor: 150 ohm"));
    assert(click("RC filter"));
    assert(has_text(content, "Time constant: 2.2 ms"));
    electronics_tool_stop();
    reset_content();
    electronics_tool_show(content);
    assert(has_text(content, "Time constant: 2.2 ms"));
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 1)), "2.2"));
    assert(click("Ohm's law"));
    select_dropdown(0, 1);
    set_input(0, "12");
    assert(click("Calculate"));
    assert(has_text(content, "Current: 12 mA"));
    assert(click("Divider"));
    assert(click("Ohm's law"));
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "12"));
    printf("ELECTRONICS Ohm/divider/LED/RC calculations, invalid input, keyboard reopen, mode retention "
           "PASS\n");
    electronics_tool_stop();
    reset_content();
    byte_tool_show(content);
    shot("bytes-default");
    assert(has_text(content, "CRC-16/MODBUS: CDC5"));
    assert(lv_obj_has_flag(nth(&lv_keyboard_class, 0), LV_OBJ_FLAG_HIDDEN));
    select_dropdown(0, 1);
    assert(has_text(content, "CRC-16/MODBUS: 4B37"));
    assert(has_text(content, "CRC-32/ISO-HDLC: CBF43926"));
    shot("bytes-ascii");
    set_input(0, "ABCD");
    assert(has_text(content, "BE 1094861636 | LE 1145258561"));
    select_dropdown(0, 0);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "01 03 00 00 00 0A"));
    set_input(0, "12 3");
    assert(has_text(content, "Each hex byte needs two digits"));
    shot("bytes-invalid");
    assert(click("CLEAR"));
    assert(has_text(content, "0 bytes | SUM8: 00 | XOR8: 00"));
    assert(has_text(content, "CRC-16/MODBUS: FFFF"));
    lv_obj_send_event(nth(&lv_textarea_class, 0), LV_EVENT_CLICKED, NULL);
    assert(!lv_obj_has_flag(nth(&lv_keyboard_class, 0), LV_OBJ_FLAG_HIDDEN));
    hex_key(10); /* A */
    assert(has_text(content, "Each hex byte needs two digits"));
    hex_key(11); /* B */
    hex_key(17); /* Space */
    hex_key(0);
    hex_key(15); /* F */
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "AB 0F"));
    assert(has_text(content, "BE 43791 | LE 4011"));
    hex_key(19); /* Backspace */
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "AB 0"));
    hex_key(20); /* Ready */
    assert(lv_obj_has_flag(nth(&lv_keyboard_class, 0), LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(nth(&lv_textarea_class, 0), LV_EVENT_CLICKED, NULL);
    assert(!lv_obj_has_flag(nth(&lv_keyboard_class, 0), LV_OBJ_FLAG_HIDDEN));
    set_input(0, "00 01 02 03");
    assert(has_text(content, "BE 66051 | LE 50462976"));
    lv_obj_send_event(nth(&lv_keyboard_class, 0), LV_EVENT_READY, NULL);
    set_input(0, "80");
    assert(has_text(content, "Signed 8-bit (two's complement)\nBE -128 | LE -128"));
    assert(!has_text(content, "Float32 BE:"));
    set_input(0, "FF 7F");
    assert(has_text(content, "Signed 16-bit (two's complement)\nBE -129 | LE 32767"));
    set_input(0, "80 00 00 00");
    assert(has_text(content, "Signed 32-bit (two's complement)\nBE -2147483648 | LE 128"));
    assert(has_text(content, "Float32 BE: -0\n"));
    shot("bytes-signed-negative-zero");
    set_input(0, "3F 80 00 00");
    assert(has_text(content, "Float32 BE: 1\n"));
    set_input(0, "00 00 80 3F");
    assert(has_text(content, "Float32 LE: 1"));
    set_input(0, "00 00 00 01");
    assert(has_text(content, "Float32 BE: 1.40129846e-45"));
    set_input(0, "7F 7F FF FF");
    assert(has_text(content, "Float32 BE: 3.40282347e+38"));
    shot("bytes-float32-maximum");
    set_input(0, "7F 80 00 00");
    assert(has_text(content, "Float32 BE: +Infinity"));
    set_input(0, "FF 80 00 00");
    assert(has_text(content, "Float32 BE: -Infinity"));
    set_input(0, "7F 80 00 01");
    assert(has_text(content, "Float32 BE: NaN (not a number)"));
    set_input(0, "01 02 03");
    assert(!has_text(content, "Signed "));
    assert(!has_text(content, "Float32 BE:"));
    set_input(0, "12 3");
    assert(!has_text(content, "Float32 BE:"));
    assert(has_text(content, "Each hex byte needs two digits"));
    select_dropdown(0, 1);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "ABCD"));
    char many[130];
    memset(many, 'A', 129);
    many[129] = 0;
    set_input(0, many);
    assert(has_text(content, "Payload exceeds the 128-byte limit"));
    many[128] = 0;
    set_input(0, many);
    assert(has_text(content, "128 bytes"));
    shot("bytes-max-ascii");
    kb = nth(&lv_keyboard_class, 0);
    assert(kb);
    lv_obj_send_event(kb, LV_EVENT_CANCEL, NULL);
    assert(lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(nth(&lv_textarea_class, 0), LV_EVENT_CLICKED, NULL);
    assert(!lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    assert(click("EXAMPLE"));
    select_dropdown(0, 0);
    assert(click("EXAMPLE"));
    printf("BYTES known vectors, signed/unsigned/Float32 endian values, invalid/empty/oversize/max input, keyboard reopen, "
           "mode retention PASS\n");
    byte_number_checks();
    byte_clipboard_checks();
    byte_tool_stop();
    reset_content();
    subnet_tool_show(content);
    shot("subnet-default");
    assert(has_text(content, "Network: 192.168.1.0/24"));
    assert(has_text(content, "Broadcast: 192.168.1.255"));
    assert(has_text(content, "Host slots: 254"));
    assert(has_text(content, "inside; host slot"));
    kb = nth(&lv_keyboard_class, 0);
    assert(kb && lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    const char *split_subnet_inputs[] = {"192.168.\n1.42", "2\n4", "192.168.1.\n100"};
    for (unsigned field = 0; field < 3; field++) {
        set_input(field, split_subnet_inputs[field]);
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, field)), split_subnet_inputs[field]));
        assert(click("Calculate"));
        assert(!has_text(content, "Network:"));
        subnet_tool_stop(); reset_content(); subnet_tool_show(content); refresh();
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, field)), split_subnet_inputs[field]));
        assert(!has_text(content, "Network:"));
        assert(click("Example"));
    }
    kb = nth(&lv_keyboard_class, 0);
    set_input(1, "255.255.240.0");
    assert(has_text(content, "Inputs changed"));
    assert(!has_text(content, "Broadcast:"));
    assert(!has_text(content, "inside; host slot"));
    set_input(0, "10.20.30.40");
    set_input(2, "10.20.32.1");
    assert(click("Calculate"));
    assert(has_text(content, "Network: 10.20.16.0/20"));
    assert(has_text(content, "Broadcast: 10.20.31.255"));
    assert(has_text(content, "outside this subnet"));
    shot("subnet-dotted-mask");
    set_input(2, "10.20.16.0");
    assert(click("Calculate"));
    assert(has_text(content, "inside; network address"));
    set_input(2, "10.20.31.255");
    assert(click("Calculate"));
    assert(has_text(content, "inside; broadcast address"));
    set_input(2, "");
    assert(click("Calculate"));
    assert(has_text(content, "Enter a peer to check"));
    set_input(1, "255.0.255.0");
    assert(click("Calculate"));
    assert(has_text(content, "Netmask bits must be contiguous"));
    assert(!has_text(content, "Network:"));
    shot("subnet-invalid-mask");
    set_input(0, "192.0.2.1");
    set_input(1, "/31");
    set_input(2, "192.0.2.0");
    assert(click("Calculate"));
    assert(has_text(content, "Host slots: 2"));
    assert(has_text(content, "Broadcast: none"));
    assert(has_text(content, "two endpoints on a point-to-point link"));
    assert(has_text(content, "inside; host slot"));
    shot("subnet-point-to-point");
    set_input(1, "32");
    assert(click("Calculate"));
    assert(has_text(content, "Host slots: 1"));
    assert(has_text(content, "outside this subnet"));
    assert(has_text(content, "single address/host route"));
    set_input(1, "/0");
    assert(click("Calculate"));
    assert(has_text(content, "Total addresses: 4294967296"));
    assert(has_text(content, "Host slots: 4294967294"));
    shot("subnet-all-ipv4");
    set_input(0, "192.168.001.1");
    assert(click("Calculate"));
    assert(has_text(content, "IPv4 address: use four decimal octets"));
    assert(!has_text(content, "Network:"));
    assert(click("Example"));
    set_input(2, "1.2.3.4/24");
    assert(click("Calculate"));
    assert(has_text(content, "Peer: enter a dotted IPv4 address"));
    assert(click("Example"));
    set_input(1, "");
    lv_obj_send_event(nth(&lv_textarea_class, 1), LV_EVENT_CLICKED, NULL);
    assert(lv_keyboard_get_textarea(kb) == nth(&lv_textarea_class, 1));
    assert(!lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    hex_key(11); /* / */
    hex_key(1);  /* 2 */
    hex_key(3);  /* 4 */
    hex_key(14); /* Backspace */
    hex_key(3);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 1)), "/24"));
    shot("subnet-keyboard");
    hex_key(15); /* Done calculates and dismisses the keyboard. */
    assert(lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    assert(has_text(content, "Network: 192.168.1.0/24"));
    lv_obj_send_event(nth(&lv_textarea_class, 2), LV_EVENT_CLICKED, NULL);
    assert(lv_keyboard_get_textarea(kb) == nth(&lv_textarea_class, 2));
    set_input(2, "1");
    hex_key(10); /* . */
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 2)), "1."));
    lv_obj_send_event(kb, LV_EVENT_CANCEL, NULL);
    assert(lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    assert(has_text(content, "Inputs changed"));
    assert(click("Show keys"));
    assert(!lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    assert(click("Hide keys"));
    assert(lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN));
    /* Unicode-invalid input must survive Home intact, rather than becoming a
     * truncated, potentially valid address while the saved fields are restored. */
    char invalid_utf8[65];
    for (unsigned i = 0; i < 32; i++) {
        invalid_utf8[2 * i] = (char)0xc2;
        invalid_utf8[2 * i + 1] = (char)0xb9;
    }
    invalid_utf8[64] = 0;
    set_input(0, invalid_utf8);
    subnet_tool_stop();
    reset_content();
    subnet_tool_show(content);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), invalid_utf8));
    assert(has_text(content, "IPv4 address: use four decimal octets"));
    assert(click("Example"));
    set_input(0, "10.20.30.40");
    set_input(1, "/20");
    set_input(2, "10.20.31.0");
    assert(click("Calculate"));
    printf("SUBNET CIDR/netmask, peer roles, /0 /31 /32, invalid/stale input, keyboard, UTF-8 retention PASS\n");
    subnet_tool_stop();
    reset_content();
    refresh();
    resistor_checks();
    spi_checks();
    spi_editor_checks();
    i2c_result_checks();
    const unsigned spi_starts = spi_host_starts, spi_transfers = spi_host_transfers;
    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    for (unsigned i = 0; i < 100; i++) {
        electronics_tool_show(content);
        refresh();
        assert(has_text(content, "Current: 12 mA"));
        electronics_tool_stop();
        electronics_tool_stop();
        reset_content();
        byte_tool_show(content);
        refresh();
        select_dropdown(0, 0);
        assert(has_text(content, "CRC-16/MODBUS: CDC5"));
        select_dropdown(0, 2);
        assert(has_text(content, "Encoded Float32 LE: 1.5"));
        if (i % 2) lv_dropdown_open(nth(&lv_dropdown_class, 1));
        byte_tool_stop();
        byte_tool_stop();
        reset_content();
        subnet_tool_show(content);
        refresh();
        assert(has_text(content, "Network: 10.20.16.0/20"));
        assert(has_text(content, "Peer 10.20.31.0:"));
        subnet_tool_stop();
        subnet_tool_stop();
        reset_content();
        resistor_tool_show(content);
        refresh();
        assert(has_text(content, "Nominal: 12.4 kohm"));
        resistor_tool_stop();
        resistor_tool_stop();
        reset_content();
        spi_tool_show(content);
        refresh();
        assert(click("PASTE\nBYTES"));
        assert(!spi_tool_busy() && spi_host_starts == spi_starts && spi_host_transfers == spi_transfers);
        spi_tool_stop();
        spi_tool_stop();
        reset_content();
        refresh();
    }
    lv_mem_monitor(&after);
    printf("ALLOCATIONS before=%zu after=%zu | FREE BLOCKS before=%zu after=%zu\n",
           before.used_cnt, after.used_cnt, before.free_cnt, after.free_cnt);
    printf("CYCLES 100 EACH OF 4 OFFLINE APPS + SPI HOST ADAPTER\nHEAP before_free=%zu after_free=%zu delta=%zd max_used=%zu "
           "frag=%u%%\n",
           before.free_size, after.free_size,
           (ptrdiff_t)after.free_size - (ptrdiff_t)before.free_size, after.max_used,
           after.frag_pct);
    /* Allocation sizes can settle during reuse. Require no lost bytes and no
     * additional live allocations rather than rejecting increased free space. */
    assert(after.free_size >= before.free_size);
    assert(after.used_cnt == before.used_cnt);
    assert(lv_mem_test() == LV_RESULT_OK);
    lv_deinit();
    return 0;
}
