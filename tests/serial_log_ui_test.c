/* Uses actual fixture files under build/, the real reader/viewer and LVGL.
 * No serial driver, worker, network service or physical SD adapter is needed. */
#include "serial_log_viewer.h"
#include "payload_clipboard.h"
#include "modbus_rtu_tool.h"
#include "byte_tool.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check(expression) do { if (!(expression)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); exit(1); \
} } while (0)

static unsigned allocations, allocation_attempts, fail_at;
void *heap_caps_malloc(size_t size, unsigned capabilities)
{
    check(capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (++allocation_attempts == fail_at) { fail_at = 0; return NULL; }
    void *memory = malloc(size);
    if (memory) allocations++;
    return memory;
}
void heap_caps_free(void *memory)
{
    if (memory) { check(allocations); allocations--; free(memory); }
}

static uint16_t framebuffer[720 * 1280];
static lv_obj_t *content;
static const char *directory;
static bool snapshots;
static char fixture[1024];
static const uint8_t request[] = {1, 3, 0, 0, 0, 2, 0xc4, 0x0b};
static const uint8_t reply[] = {1, 3, 4, 0x3f, 0xc0, 0, 0, 0xf6, 0x1b};
static const uint8_t controls[] = {0, 0xff, 0x0d, 0x0a, 0x09, 0x41};

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
static lv_obj_t *button(const char *text)
{
    lv_obj_t *caption = text_find(content, text, true); check(caption); return lv_obj_get_parent(caption);
}
static void click(const char *text)
{
    lv_obj_t *object = button(text); check(!lv_obj_has_state(object, LV_STATE_DISABLED));
    lv_obj_send_event(object, LV_EVENT_CLICKED, NULL); refresh();
}
static void select_row(unsigned index)
{
    lv_obj_t *object = nth(&lv_dropdown_class, 0);
    lv_dropdown_set_selected(object, index); lv_obj_send_event(object, LV_EVENT_VALUE_CHANGED, NULL); refresh();
}
static void expect_copy(const uint8_t *bytes, size_t length)
{
    const payload_clipboard_t *copy = payload_clipboard_peek();
    check(copy && copy->length == length && !memcmp(copy->bytes, bytes, length));
}
static void reject_copy(const uint8_t *previous, size_t length)
{
    check(lv_obj_has_state(button("COPY ROW"), LV_STATE_DISABLED));
    lv_obj_send_event(button("COPY ROW"), LV_EVENT_CLICKED, NULL);
    expect_copy(previous, length);
}
static void clean_content(void)
{
    lv_obj_clean(content); lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF); refresh();
}
static void close_viewer(void)
{
    serial_log_viewer_stop(); serial_log_viewer_stop(); check(!allocations); clean_content();
}
static void open_viewer(const char *path)
{
    /* Files puts this control above every viewer. Include it in layout checks. */
    lv_obj_t *back = lv_button_create(content); lv_obj_set_size(back, 640, 68);
    lv_obj_t *label = lv_label_create(back); lv_label_set_text(label, "Back to files");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0); lv_obj_center(label);
    check(serial_log_viewer_show(content, path)); refresh();
}
static void shot(const char *name)
{
    refresh();
    /* Body text may be longer than its scroll viewport. Check each top-level
     * control/viewport against the same fixed shell used by the tablet. */
    for (uint32_t i = 0; i < lv_obj_get_child_count(content); i++) {
        lv_obj_t *object = lv_obj_get_child(content, i);
        if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t a; lv_obj_get_coords(object, &a);
        if (a.x1 < 28 || a.x2 > 691 || a.y1 < 128 || a.y2 > 1251)
            fprintf(stderr, "Outside content: (%ld,%ld)-(%ld,%ld)\n", (long)a.x1, (long)a.y1, (long)a.x2, (long)a.y2);
        check(a.x1 >= 28 && a.x2 <= 691 && a.y1 >= 128 && a.y2 <= 1251);
    }
    if (!snapshots) return;
    char path[1200]; snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
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
    lv_obj_t *screen = lv_screen_active(); lv_obj_set_style_bg_color(screen, lv_color_hex(0x10141f), 0);
    lv_obj_t *header = lv_obj_create(screen); lv_obj_set_size(header, 720, 100);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x20283a), 0); lv_obj_set_style_text_color(header, lv_color_white(), 0);
    lv_obj_t *home = lv_button_create(header); lv_obj_set_size(home, 120, 64); lv_obj_align(home, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *label = lv_label_create(home); lv_label_set_text(label, LV_SYMBOL_HOME); lv_obj_center(label);
    label = lv_label_create(header); lv_label_set_text(label, "Tab5 OS");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0); lv_obj_center(label);
    content = lv_obj_create(screen); lv_obj_set_size(content, 720, 1180); lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x10141f), 0); lv_obj_set_style_text_color(content, lv_color_white(), 0);
    lv_obj_set_style_border_width(content, 0, 0); lv_obj_set_style_pad_all(content, 28, 0);
    lv_obj_set_style_pad_row(content, 10, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}
static void write_row(FILE *file, unsigned stamp, bool tx, const uint8_t *bytes, size_t length)
{
    check(fprintf(file, "%u,%s,", stamp, tx ? "TX" : "RX") > 0);
    for (size_t i = 0; i < length; i++) check(fprintf(file, "%s%02X", i ? " " : "", bytes[i]) > 0);
    check(fputc('\n', file) != EOF);
}
static FILE *new_fixture(const char *name)
{
    snprintf(fixture, sizeof(fixture), "%s/%s", directory, name);
    FILE *file = fopen(fixture, "wb"); check(file);
    check(fputs("unix_time,direction,data_hex\n", file) >= 0); return file;
}
static uint64_t fingerprint(const char *path)
{
    FILE *file = fopen(path, "rb"); check(file);
    uint64_t hash = UINT64_C(14695981039346656037);
    int c;
    while ((c = fgetc(file)) != EOF) hash = (hash ^ (unsigned)c) * UINT64_C(1099511628211);
    check(!ferror(file)); fclose(file); return hash;
}

int main(int argc, char **argv)
{
    check(argc >= 2); directory = argv[1]; snapshots = argc > 2 && !strcmp(argv[2], "--snapshots");
    setup();
    uint8_t bytes[256]; for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)i;
    FILE *file = new_fixture("serial.CSV");
    write_row(file, 100, true, request, sizeof(request)); write_row(file, 101, false, reply, sizeof(reply));
    check(fputs("not a valid row\n", file) >= 0);
    write_row(file, 102, false, controls, sizeof(controls));
    write_row(file, 103, true, bytes, 128); write_row(file, 104, false, bytes, 129); write_row(file, 105, true, bytes, 256);
    write_row(file, 106, false, bytes + 7, 1); write_row(file, 107, false, bytes + 8, 2);
    write_row(file, 108, true, bytes + 9, 3); write_row(file, 109, false, bytes + 10, 3);
    fclose(file); uint64_t original = fingerprint(fixture);
    open_viewer(fixture); check(allocations == 2); expect("Rows 1-8 of 10"); expect("1 malformed skipped");
    expect("Selected record 1 | TX"); check(!payload_clipboard_peek()); click("COPY ROW"); expect_copy(request, sizeof(request));
    select_row(1); expect("Selected record 2 | RX"); click("COPY ROW"); expect_copy(reply, sizeof(reply));
    shot("serial-log-copy");
    select_row(2); click("View: Hex"); expect("..\\r\\n\\tA"); check(lv_dropdown_get_selected(nth(&lv_dropdown_class, 0)) == 2);
    click("COPY ROW"); expect_copy(controls, sizeof(controls));
    select_row(3); click("COPY ROW"); expect_copy(bytes, 128);
    select_row(4); reject_copy(bytes, 128); expect("exceeds the 128-byte clipboard");
    select_row(5); reject_copy(bytes, 128); shot("serial-log-oversized");
    click("Next"); expect("Rows 9-10 of 10"); expect("Selected record 9 | TX"); click("COPY ROW"); expect_copy(bytes + 9, 3);
    select_row(1); click("COPY ROW"); expect_copy(bytes + 10, 3);
    click("Filter: All"); expect("Rows 1-6 of 6"); expect("Selected record 2 | RX"); click("COPY ROW"); expect_copy(reply, sizeof(reply));
    select_row(2); expect("Selected record 5 | RX"); reject_copy(reply, sizeof(reply));
    click("Filter: RX"); expect("Rows 1-4 of 4"); expect("Selected record 1 | TX");
    select_row(3); expect("Selected record 9 | TX"); click("COPY ROW"); expect_copy(bytes + 9, 3);
    click("Filter: TX"); expect("Rows 1-8 of 10"); select_row(1); click("COPY ROW"); close_viewer();
    check(fingerprint(fixture) == original); expect_copy(reply, sizeof(reply));

    /* A saved binary record returns through both actual inspector screens. */
    modbus_rtu_tool_show(content); refresh(); click("PASTE REPLY"); click("DECODE"); expect("CRC, unit, function and count match");
    lv_obj_t *view = nth(&lv_dropdown_class, 1); lv_dropdown_set_selected(view, 3);
    lv_obj_send_event(view, LV_EVENT_VALUE_CHANGED, NULL); refresh(); expect("0-1 : 1.5");
    modbus_rtu_tool_stop(); clean_content();
    byte_tool_show(content); refresh(); click("Paste hex"); expect("01 03 04 3F C0 00 00 F6 1B"); expect("CRC-16/MODBUS: 0000");
    byte_tool_stop(); clean_content();
    /* Each allocation failure releases the other buffer immediately. */
    for (unsigned allocation = 1; allocation <= 2; allocation++) {
        fail_at = allocation_attempts + allocation; open_viewer(fixture); expect("Not enough memory to open log");
        check(!allocations); close_viewer(); expect_copy(reply, sizeof(reply));
    }
    lv_mem_monitor_t before, after; lv_mem_monitor(&before);
    for (unsigned i = 0; i < 8; i++) {
        open_viewer(fixture); select_row(i); lv_dropdown_open(nth(&lv_dropdown_class, 0)); close_viewer();
    }
    lv_mem_monitor(&after); check(after.free_size == before.free_size && after.used_cnt == before.used_cnt);
    printf("Serial-log copy, inspector handoff and 8 lifecycle cycles passed; free heap %zu -> %zu.\n", before.free_size, after.free_size);

    file = new_fixture("only-tx.csv"); write_row(file, 200, true, controls, sizeof(controls)); fclose(file);
    open_viewer(fixture); click("Filter: All"); expect("No rows match this filter");
    check(lv_obj_has_state(nth(&lv_dropdown_class, 0), LV_STATE_DISABLED)); reject_copy(reply, sizeof(reply));
    click("Filter: RX"); click("COPY ROW"); expect_copy(controls, sizeof(controls)); close_viewer();
    file = new_fixture("empty.CSV"); fclose(file); open_viewer(fixture); expect("No readable log rows"); check(!allocations); close_viewer();
    file = new_fixture("large.CSV");
    for (unsigned i = 0; i < 8; i++) write_row(file, 200 + i, false, bytes, sizeof(bytes));
    fclose(file); open_viewer(fixture); select_row(7); expect("Selected record 8 | RX"); reject_copy(controls, sizeof(controls)); close_viewer();
    file = new_fixture("limited.CSV");
    for (unsigned i = 0; i < 513; i++) write_row(file, i, false, bytes + i % 256, 1);
    fclose(file); open_viewer(fixture); expect("first 512 loaded");
    for (unsigned i = 0; i < 63; i++) click("Next");
    expect("Rows 505-512 of 512"); select_row(7); click("COPY ROW"); expect_copy(bytes + 255, 1);
    check(lv_obj_has_state(button("Next"), LV_STATE_DISABLED)); close_viewer();
    check(!serial_log_viewer_show(content, NULL)); check(!serial_log_viewer_show(content, "missing.CSV"));
    check(!serial_log_viewer_show(content, "unpublished.TMP")); check(!allocations);
    file = new_fixture("wrong-header.CSV"); fclose(file);
    file = fopen(fixture, "wb"); check(file); fputs("wrong header\n1,RX,01\n", file); fclose(file);
    check(!serial_log_viewer_show(content, fixture)); check(!allocations);
    puts("Empty/filter/paging limits, file immutability and allocation failures passed.");
    lv_deinit(); return 0;
}
