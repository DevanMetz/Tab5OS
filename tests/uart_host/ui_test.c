#include "host.h"
#include "uart_tool.h"
#include "modbus_rtu_tool.h"
#include "byte_tool.h"
#include "payload_clipboard.h"
#include "lvgl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The debug CRT can suppress assertion text in headless sessions. */
#undef assert
#define assert(expression) do { if (!(expression)) { \
    fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expression); exit(1); \
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
        lv_obj_t *found = find(lv_obj_get_child(object, i), type, index);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *nth(const lv_obj_class_t *type, unsigned index)
{
    lv_obj_t *object = find(content, type, &index); assert(object); return object;
}
static lv_obj_t *label_find(lv_obj_t *object, const char *text, bool exact)
{
    if (lv_obj_check_type(object, &lv_label_class) &&
        (exact ? !strcmp(lv_label_get_text(object), text) : strstr(lv_label_get_text(object), text) != NULL)) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = label_find(lv_obj_get_child(object, i), text, exact);
        if (found) return found;
    }
    return NULL;
}
static void expect(const char *text)
{
    if (!label_find(content, text, false)) fprintf(stderr, "Missing text: %s\n", text);
    assert(label_find(content, text, false));
}
static lv_obj_t *button(const char *text)
{
    lv_obj_t *label = label_find(content, text, true); assert(label); return lv_obj_get_parent(label);
}
static void click(const char *text)
{
    lv_obj_t *object = button(text); assert(!lv_obj_has_state(object, LV_STATE_DISABLED));
    lv_obj_send_event(object, LV_EVENT_CLICKED, NULL); refresh();
}
static const char *draft(void) { return lv_textarea_get_text(nth(&lv_textarea_class, 1)); }
static void input(const char *text) { lv_textarea_set_text(nth(&lv_textarea_class, 1), text); refresh(); }
static void select_option(unsigned index, unsigned selected)
{
    lv_obj_t *object = nth(&lv_dropdown_class, index);
    lv_dropdown_set_selected(object, selected); lv_obj_send_event(object, LV_EVENT_VALUE_CHANGED, NULL); refresh();
}
static void expect_tx(const uint8_t *bytes, size_t length)
{
    assert(uart_host_last_length == length && !memcmp(uart_host_last_tx, bytes, length));
}
static void reject_send(void)
{
    unsigned before = uart_host_writes;
    assert(lv_obj_has_state(button("SEND"), LV_STATE_DISABLED));
    lv_obj_send_event(button("SEND"), LV_EVENT_CLICKED, NULL); refresh();
    assert(uart_host_writes == before);
}
static void close_app(void)
{
    uart_tool_stop(); uart_tool_stop();
    assert(!uart_tool_busy() && !uart_host_running && !uart_host_allocations);
    lv_obj_clean(content); lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF); refresh();
}
static void open_app(void) { uart_tool_show(content, false, NULL); refresh(); }
static void check_bounds(lv_obj_t *object)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(object, &a);
    if (a.x1 < 28 || a.x2 > 691 || a.y1 < 128 || a.y2 > 1251) {
        fprintf(stderr, "Visible object overflow (%ld,%ld)-(%ld,%ld)\n", (long)a.x1, (long)a.y1, (long)a.x2, (long)a.y2);
        assert(0);
    }
    if (lv_obj_check_type(object, &lv_textarea_class) || lv_obj_check_type(object, &lv_dropdown_class)) return;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) check_bounds(lv_obj_get_child(object, i));
}
static void shot(const char *name)
{
    refresh();
    for (uint32_t i = 0; i < lv_obj_get_child_count(content); i++) check_bounds(lv_obj_get_child(content, i));
    if (!output_directory) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.ppm", output_directory, name);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n720 1280\n255\n");
    for (size_t i = 0; i < 720U * 1280; i++) {
        uint16_t p = framebuffer[i];
        const unsigned char rgb[] = {(unsigned char)(((p >> 11) & 31) * 255 / 31),
            (unsigned char)(((p >> 5) & 63) * 255 / 63), (unsigned char)((p & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}
static void setup_screen(void)
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
    assert(copy && copy->length == length && !memcmp(copy->bytes, bytes, length));
}

static void reject_rx_copy(const uint8_t *previous, size_t length)
{
    assert(lv_obj_has_state(button("COPY RX"), LV_STATE_DISABLED));
    lv_obj_send_event(button("COPY RX"), LV_EVENT_CLICKED, NULL);
    expect_copy(previous, length);
}

static void rx_capture_checks(void)
{
    const uint8_t prior[] = {0x3f, 0xc0, 0, 0};
    const uint8_t reply[] = {0, 0xff, 0x41, 0x0d, 0x0a, 0x80, 0x7f};
    unsigned writes = uart_host_writes;
    open_app(); assert(payload_clipboard_store(prior, sizeof(prior)));
    click("CAPTURE\nRX"); expect("Start Serial before starting");
    reject_rx_copy(prior, sizeof(prior)); assert(!uart_tool_busy());
    click("START");
    /* Data queued before the capture boundary goes only to transcript/log. */
    uart_host_receive(prior, sizeof(prior)); click("CAPTURE\nRX"); expect("0/128 bytes");
    reject_rx_copy(prior, sizeof(prior));
    uart_host_receive(reply, 2); refresh(); expect("2/128 bytes");
    uart_host_receive(reply + 2, 3); refresh(); expect("5/128 bytes");
    /* Freeze must drain the tail even before the next receive timer fires. */
    uart_host_receive(reply + 5, 2); click("FREEZE\nRX"); expect("Frozen RX"); expect("7/128 bytes");
    click("COPY RX"); expect_copy(reply, sizeof(reply));
    uart_host_receive(prior, sizeof(prior)); refresh(); click("COPY RX"); expect_copy(reply, sizeof(reply));
    click("CLEAR"); click("COPY RX"); expect_copy(reply, sizeof(reply)); /* Transcript clear is independent. */
    shot("serial-rx-frozen");
    click("CAPTURE\nRX"); click("FREEZE\nRX"); expect("0/128 bytes");
    reject_rx_copy(reply, sizeof(reply));
    uint8_t bytes[256]; for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)i;
    click("CAPTURE\nRX"); uart_host_receive(bytes, 128); refresh(); expect("128/128 bytes");
    reject_rx_copy(reply, sizeof(reply)); click("FREEZE\nRX"); click("COPY RX"); expect_copy(bytes, 128);
    click("CAPTURE\nRX"); uart_host_receive(bytes, 128); refresh();
    uart_host_receive(bytes, 1); refresh(); expect("Window exceeded 128 bytes"); reject_rx_copy(bytes, 128);
    click("CAPTURE\nRX"); uart_host_receive(bytes, 256); refresh(); expect("Window exceeded 128 bytes");
    reject_rx_copy(bytes, 128); shot("serial-rx-overflow");
    click("CLEAR RX"); expect("RX capture is empty"); expect_copy(bytes, 128); assert(uart_tool_busy());
    /* A capture cleared while running never resumes when the next bytes arrive. */
    click("CAPTURE\nRX"); uart_host_receive(reply, 2); refresh(); click("CLEAR RX");
    uart_host_receive(reply + 2, 5); refresh(); reject_rx_copy(bytes, 128);
    click("CAPTURE\nRX"); uart_host_receive(reply, 2); refresh();
    uart_host_fail_pending = true; refresh(); expect("Read failed RX"); reject_rx_copy(bytes, 128);
    click("CAPTURE\nRX"); uart_host_receive(reply, sizeof(reply)); uart_host_fail_read = true;
    refresh(); expect("Read failed RX"); reject_rx_copy(bytes, 128);
    /* Leave data pending: starting a new capture first drains that stale reply. */
    click("CAPTURE\nRX"); expect("0/128 bytes");
    uart_host_receive(reply, sizeof(reply)); click("STOP"); expect("Frozen RX");
    click("COPY RX"); expect_copy(reply, sizeof(reply));
    /* Source settings belong to the capture even after interface changes. */
    expect("UART 9600 8E2"); click("Link\nUART"); expect("UART 9600 8E2");
    click("COPY RX"); expect_copy(reply, sizeof(reply));
    click("START"); click("CAPTURE\nRX"); expect("RS-485 9600 8E2");
    uart_host_receive(reply, 3); refresh(); uart_host_receive(reply + 3, 4);
    close_app(); open_app(); expect("Frozen RX"); expect("RS-485 9600 8E2");
    click("COPY RX"); expect_copy(reply, sizeof(reply));
    click("Link\nRS-485"); click("START");
    /* Failed boundary establishment preserves the previous frozen window. */
    uart_host_fail_pending = true; click("CAPTURE\nRX"); expect("No new capture started");
    click("COPY RX"); expect_copy(reply, sizeof(reply));
    uint8_t backlog[1100]; memset(backlog, 0x55, sizeof(backlog));
    uart_host_receive(backlog, sizeof(backlog)); click("CAPTURE\nRX"); expect("No new capture started");
    click("COPY RX"); expect_copy(reply, sizeof(reply));
    click("CAPTURE\nRX"); uart_host_receive(reply, sizeof(reply)); uart_host_read_limit = 1;
    click("FREEZE\nRX"); expect("Read failed RX"); reject_rx_copy(reply, sizeof(reply));
    uart_host_read_limit = 0; refresh();
    assert(uart_host_writes == writes); /* Capture/copy controls never transmit. */
    click("CLEAR RX"); close_app();
    puts("RX capture boundaries, exact bytes, overflow, faults, source identity and Home retention passed");
}

static void rtu_serial_roundtrip(void)
{
    modbus_rtu_tool_show(content); refresh(); click("EXAMPLE"); click("COPY REQUEST");
    const uint8_t request[] = {1, 3, 0, 0, 0, 2, 0xc4, 0x0b};
    const uint8_t reply[] = {1, 3, 4, 0x3f, 0xc0, 0, 0, 0xf6, 0x1b};
    expect_copy(request, sizeof(request));
    modbus_rtu_tool_stop(); lv_obj_clean(content); refresh();
    open_app(); unsigned writes = uart_host_writes;
    click("PASTE"); assert(!uart_tool_busy() && uart_host_writes == writes);
    click("START"); click("CAPTURE\nRX"); assert(uart_host_writes == writes);
    click("SEND"); expect_tx(request, sizeof(request)); assert(uart_host_writes == writes + 1);
    uart_host_receive(reply, 4); refresh(); uart_host_receive(reply + 4, 5);
    click("FREEZE\nRX"); click("COPY RX"); expect_copy(reply, sizeof(reply)); close_app();
    modbus_rtu_tool_show(content); refresh(); click("CLEAR"); click("PASTE REPLY"); click("DECODE");
    expect("0-1 : 1.5"); expect("CRC, unit, function and count match"); shot("serial-rtu-roundtrip");
    modbus_rtu_tool_stop(); lv_obj_clean(content); refresh();
    byte_tool_show(content); refresh(); click("Paste hex");
    expect("CRC-16/MODBUS: 0000"); expect("01 03 04 3F C0 00 00 F6 1B");
    byte_tool_stop(); lv_obj_clean(content); refresh();
    assert(uart_host_writes == writes + 1 && !uart_host_running && !uart_tool_busy());
    puts("RTU request -> Serial TX / split RX -> RTU decode -> Byte Lab clipboard round-trip passed");
}

int main(int argc, char **argv)
{
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned cycles = 100;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--quick")) cycles = 8;
        else output_directory = argv[i];
    }
    setup_screen(); uart_tool_self_test(); open_app(); shot("serial-default");
    assert(!uart_tool_busy() && !uart_host_starts && !uart_host_routes && !uart_host_writes);
    input("keep ASCII"); click("PASTE"); assert(!strcmp(draft(), "keep ASCII"));
    click("PREV"); expect("Send history is empty");
    const uint8_t copied[] = {0x00, 0x00, 0xc0, 0x3f};
    assert(payload_clipboard_store(copied, sizeof(copied))); click("PASTE");
    assert(!strcmp(draft(), "00 00 C0 3F"));
    assert(lv_dropdown_get_selected(nth(&lv_dropdown_class, 0)) == 1);
    assert(lv_obj_has_state(nth(&lv_dropdown_class, 1), LV_STATE_DISABLED));
    reject_send(); assert(!uart_host_starts && !uart_host_routes);
    assert(payload_clipboard_store(NULL, 0)); click("PASTE"); assert(!strcmp(draft(), "00 00 C0 3F"));
    select_option(0, 0); assert(!strcmp(draft(), "keep ASCII"));
    input("AT"); select_option(1, 3); expect("Ready: 4 TX bytes");
    click("VIEW\nASCII"); assert(!strcmp(draft(), "AT"));
    assert(lv_dropdown_get_selected(nth(&lv_dropdown_class, 0)) == 0);
    click("START"); assert(uart_tool_busy()); click("SEND");
    expect_tx((const uint8_t *)"AT\r\n", 4); assert(!*draft());
    click("PREV"); assert(!strcmp(draft(), "AT")); click("SEND"); expect_tx((const uint8_t *)"AT\r\n", 4);
    click("PREV"); click("PREV"); click("PREV"); assert(!strcmp(draft(), "AT")); /* Wrap populated entries only. */
    shot("serial-ascii-crlf");
    puts("Serial short history and clipboard passed");
    input("Hi\n\\n"); click("SEND"); expect_tx((const uint8_t *)"Hi\n\\n\r\n", 7);
    input(""); select_option(1, 1); click("SEND"); expect_tx((const uint8_t *)"\n", 1);
    select_option(1, 2); click("SEND"); expect_tx((const uint8_t *)"\r", 1);
    select_option(1, 0); reject_send();
    input("caf\xc3\xa9"); reject_send(); expect("7-bit characters");
    puts("Serial suffix and ASCII rejection passed");
    char maximum[258]; memset(maximum, 'A', 257); maximum[257] = 0;
    input(maximum); assert(strlen(draft()) == 257); reject_send();
    puts("Serial overlong ASCII rejection passed");
    close_app(); open_app(); assert(strlen(draft()) == 257); click("START"); reject_send();
    puts("Serial overlong ASCII re-entry passed");
    maximum[256] = 0; input(maximum); click("SEND"); expect_tx((const uint8_t *)maximum, 256);
    puts("Serial maximum ASCII send passed");
    char unicode[1029];
    for (unsigned i = 0; i < 257; i++) memcpy(unicode + i * 4, "\xf0\x9f\x94\xa5", 4);
    unicode[1028] = 0;
    input(unicode); assert(!strcmp(draft(), unicode)); reject_send();
    close_app(); open_app(); assert(!strcmp(draft(), unicode)); click("START"); reject_send();
    click("PREV"); assert(strlen(draft()) == 256); select_option(1, 1); reject_send();
    select_option(1, 3); maximum[254] = 0; input(maximum); click("SEND");
    assert(uart_host_last_length == 256 && uart_host_last_tx[254] == '\r' && uart_host_last_tx[255] == '\n');
    puts("Serial maximum suffixed ASCII passed");
    select_option(0, 1); assert(!strcmp(draft(), "00 00 C0 3F"));
    click("SEND"); expect_tx(copied, sizeof(copied));
    puts("Serial retained Hex draft passed");
    const char *invalid[] = {"0", "0\n0", "0011", "0xFF", "AA;BB", ("AA\xc2\xa0" "BB")};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        input(invalid[i]); assert(!strcmp(draft(), invalid[i])); reject_send();
    }
    puts("Serial malformed Hex rejection passed");
    char hex[772]; uint8_t bytes[256];
    for (unsigned i = 0; i < 256; i++) {
        bytes[i] = (uint8_t)i;
        if (i) hex[i * 3 - 1] = ' ';
        snprintf(hex + i * 3, 3, "%02X", i);
    }
    input(hex); assert(strlen(draft()) == 767); click("SEND"); expect_tx(bytes, 256);
    puts("Serial maximum Hex send passed");
    click("PREV"); assert(strlen(draft()) == 767);
    strcat(hex, " 00"); input(hex); assert(strlen(draft()) == 768); reject_send();
    close_app(); open_app(); assert(strlen(draft()) == 768); click("START"); reject_send();
    puts("Serial overlong Hex re-entry passed");
    input("A5 5A");
    unsigned before_unrendered = uart_host_writes;
    lv_textarea_set_text(nth(&lv_textarea_class, 1), "A5 GG");
    lv_obj_send_event(button("SEND"), LV_EVENT_CLICKED, NULL);
    assert(uart_host_writes == before_unrendered); /* Validation cannot wait for the display tick. */
    input("A5 5A"); uart_host_write_result = 1; click("SEND");
    expect_tx((const uint8_t *)"\xa5", 1); expect("Only 1 of 2 bytes accepted"); assert(!strcmp(draft(), "A5 5A"));
    uart_host_write_result = -1; click("SEND"); expect("Serial send failed"); assert(!strcmp(draft(), "A5 5A"));
    click("PREV"); assert(strlen(draft()) == 767); /* Failed/partial writes never replace history. */
    for (unsigned i = 0; i < 10; i++) {
        select_option(0, i % 2); select_option(1, 0);
        char value[4]; snprintf(value, sizeof(value), i % 2 ? "%02X" : "%u", i);
        input(value); click("SEND");
    }
    for (unsigned i = 0; i < 16; i++) {
        unsigned value = 9 - i % 8;
        click("PREV");
        char expected[4]; snprintf(expected, sizeof(expected), value % 2 ? "%02X" : "%u", value);
        assert(!strcmp(draft(), expected));
        assert(lv_dropdown_get_selected(nth(&lv_dropdown_class, 0)) == value % 2);
    }
    assert(payload_clipboard_store(bytes, 128)); click("PASTE");
    assert(strlen(draft()) == 383); click("SEND"); expect_tx(bytes, 128);
    input("");
    lv_obj_t *keys = nth(&lv_keyboard_class, 0);
    lv_obj_send_event(nth(&lv_textarea_class, 1), LV_EVENT_CLICKED, NULL); refresh();
    assert(lv_keyboard_get_mode(keys) == LV_KEYBOARD_MODE_USER_1);
    assert(lv_obj_has_flag(nth(&lv_textarea_class, 0), LV_OBJ_FLAG_HIDDEN));
    unsigned writes = uart_host_writes;
    const unsigned keystrokes[] = {10, 5, 17, 5, 10, 20}; /* A5 5A Done */
    for (size_t i = 0; i < sizeof(keystrokes) / sizeof(keystrokes[0]); i++) {
        lv_buttonmatrix_set_selected_button(keys, keystrokes[i]); lv_obj_send_event(keys, LV_EVENT_VALUE_CHANGED, NULL); refresh();
    }
    assert(!strcmp(draft(), "A5 5A") && uart_host_writes == writes);
    assert(lv_obj_has_flag(keys, LV_OBJ_FLAG_HIDDEN));
    uart_host_receive((const uint8_t *)"OK\r\n", 4); refresh(); expect("RX 4F 4B 0D 0A");
    lv_obj_send_event(nth(&lv_textarea_class, 1), LV_EVENT_CLICKED, NULL); shot("serial-hex-keyboard");
    lv_obj_send_event(keys, LV_EVENT_CANCEL, NULL); refresh();
    click("STOP"); click("Link\nUART");
    click("LINE\nTEST"); expect("RS-485 needs an external");
    click("START"); assert(uart_host_rs485); click("SEND"); expect_tx((const uint8_t *)"\xa5\x5a", 2);
    click("START\nLOG"); expect("SD card unavailable");
    shot("serial-rs485"); close_app(); open_app();
    click("Link\nRS-485"); unsigned routes = uart_host_routes;
    click("LINE\nTEST"); expect("Internal loopback passed"); assert(uart_host_routes == routes && !uart_tool_busy());
    click("Baud\n115200"); click("Baud\n230400"); click("Baud\n921600");
    click("Parity\nNone"); click("Stop\n1"); /* 9600 baud, 8E2: 12 bits per byte. */
    click("START"); click("PASTE"); click("SEND"); click("STOP");
    assert(uart_host_last_drain_ms >= 320 && uart_host_last_drain_ms <= 350);
    click("START"); uart_host_fail_drain = true; click("STOP");
    expect("TX drain failed"); assert(!uart_tool_busy() && !uart_host_running);
    close_app(); uart_host_fail_allocation = true; open_app();
    unsigned starts = uart_host_starts; click("START"); expect("Serial display resources unavailable");
    assert(uart_host_starts == starts); close_app();
    rx_capture_checks();
    rtu_serial_roundtrip();
    lv_mem_monitor_t before, after; lv_mem_monitor(&before);
    for (unsigned i = 0; i < cycles; i++) {
        open_app(); assert(!uart_tool_busy());
        click(i % 2 ? "Link\nRS-485" : "Link\nUART");
        click("START"); click("PASTE");
        if (i % 2) lv_dropdown_open(nth(&lv_dropdown_class, 0));
        else lv_obj_send_event(nth(&lv_textarea_class, 1), LV_EVENT_CLICKED, NULL);
        close_app();
    }
    lv_mem_monitor(&after);
    assert(after.free_size >= before.free_size && after.used_cnt == before.used_cnt);
    assert(lv_mem_test() == LV_RESULT_OK);
    printf("UART/RS-485 exact TX, independent view, all endings, limits/history/clipboard, partial failures, RX, allocation guard PASS\n");
    printf("%u serial lifecycle cycles: free=%zu->%zu allocations=%zu->%zu, no UART or output buffer retained\n",
           cycles, before.free_size, after.free_size, before.used_cnt, after.used_cnt);
    lv_deinit(); return 0;
}
