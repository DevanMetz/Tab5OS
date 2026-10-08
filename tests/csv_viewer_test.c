/* Complete CSV viewers/parsers and real LVGL; native files, controlled heap/I/O. */
#include "capture_viewer.h"
#include "serial_log_viewer.h"
#include "payload_clipboard.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef enum { NO_FAULT, OPEN_FAIL, SEEK_FAIL, HEADER_FAIL, FIRST_ROW_FAIL, LATER_ROW_FAIL,
               FULL_ROW_FAIL, CLOSE_FAIL, READ_CLOSE_FAIL, CLOSE_NO_ERRNO } fault_t;
static fault_t fault;
static FILE *active;
static bool flagged, owns_file;
static unsigned handles, opens, seeks, lines, closes, allocations, attempts, fail_allocation;
static unsigned cases, failures;
static char path[96];
static uint64_t original;
static lv_obj_t *content;
static uint16_t framebuffer[720 * 1280];
static const uint8_t retained[] = {0, 0xff, 0x41};
static const char *scope_header = "unix_time,elapsed_us,gpio,millivolts,sample_rate_hz,offset_mv,scale_permille\n";
static const char *serial_header = "unix_time,direction,data_hex\n";

void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (++attempts == fail_allocation) return NULL;
    void *memory = malloc(size); assert(memory); allocations++; return memory;
}
void heap_caps_free(void *memory)
{
    if (memory) { assert(allocations); allocations--; free(memory); }
}
FILE *csv_fixture_open(const char *name, const char *mode)
{
    assert(!active && !handles && !strcmp(name, path) && !strcmp(mode, "rb")); opens++;
    if (fault == OPEN_FAIL) { errno = EACCES; return NULL; }
    active = fopen(name, mode); flagged = false;
    if (active) handles++;
    return active;
}
int csv_fixture_seek(FILE *file, long offset, int origin)
{
    assert(file == active && offset == 0 && origin == SEEK_SET); seeks++;
    if (fault == SEEK_FAIL) { errno = EACCES; return -1; }
    return fseek(file, offset, origin);
}
char *csv_fixture_gets(char *text, int size, FILE *file)
{
    assert(file == active); lines++;
    if ((fault == HEADER_FAIL && lines == 2) || (fault == FIRST_ROW_FAIL && lines == 3) ||
        ((fault == LATER_ROW_FAIL || fault == READ_CLOSE_FAIL) && lines == 4)) {
        flagged = true; errno = EIO; return NULL;
    }
    char *value = fgets(text, size, file);
    if (fault == FULL_ROW_FAIL && lines == 3 && value) { flagged = true; errno = EIO; }
    return value;
}
int csv_fixture_error(FILE *file) { assert(file == active); return flagged || ferror(file); }
int csv_fixture_close(FILE *file)
{
    assert(file == active && handles == 1); active = NULL; handles--; closes++;
    int result = fclose(file);
    if (fault == CLOSE_FAIL || fault == READ_CLOSE_FAIL || fault == CLOSE_NO_ERRNO) {
        errno = fault == CLOSE_NO_ERRNO ? 0 : EIO; return EOF;
    }
    return result;
}
static uint64_t fingerprint(void)
{
    FILE *file = fopen(path, "rb"); assert(file);
    uint64_t hash = UINT64_C(14695981039346656037);
    int ch;
    while ((ch = fgetc(file)) != EOF) hash = (hash ^ (unsigned)ch) * UINT64_C(1099511628211);
    assert(!ferror(file) && fclose(file) == 0); return hash;
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }
static void refresh(void)
{ lv_tick_inc(40); lv_timer_handler(); lv_obj_update_layout(content); lv_refr_now(NULL); }
static void stop(void)
{
    capture_viewer_stop(); serial_log_viewer_stop();
    capture_viewer_stop(); serial_log_viewer_stop();
    assert(!handles && !allocations); lv_obj_clean(content); refresh();
}
static unsigned objects(lv_obj_t *parent, const lv_obj_class_t *type)
{
    unsigned count = lv_obj_check_type(parent, type);
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); i++) count += objects(lv_obj_get_child(parent, i), type);
    return count;
}
static lv_obj_t *find(lv_obj_t *parent, const lv_obj_class_t *type)
{
    if (lv_obj_check_type(parent, type)) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); i++) {
        lv_obj_t *found = find(lv_obj_get_child(parent, i), type); if (found) return found;
    }
    return NULL;
}
static bool text_exists(lv_obj_t *parent, const char *value)
{
    if (lv_obj_check_type(parent, &lv_label_class) && strstr(lv_label_get_text(parent), value)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); i++)
        if (text_exists(lv_obj_get_child(parent, i), value)) return true;
    return false;
}
static bool clipboard_retained(void)
{
    const payload_clipboard_t *copy = payload_clipboard_peek();
    return copy && copy->length == sizeof(retained) && !memcmp(copy->bytes, retained, sizeof(retained));
}
static void prepare(bool serial, unsigned kind)
{
    stop(); assert(owns_file);
    FILE *file = fopen(path, "wb"); assert(file);
    if (kind == 3) assert(fputs("wrong header\n1,2,3\n", file) >= 0);
    else if (kind == 4) {
        assert(fputs("unix_time,address,register,value,status,speed_khz\r\n"
                     "100,0x76,0xD0,0x60,ESP_OK,100\r\n"
                     "101,0x76,0xD0,,ESP_ERR_TIMEOUT,100\r\n"
                     "102,0x76,0xD0,0x61,ESP_OK,100\r\n", file) >= 0);
    } else {
        assert(fputs(serial ? serial_header : scope_header, file) >= 0);
        if (kind == 2) {
            unsigned limit = serial ? 512 : 2048;
            for (unsigned i = 0; i <= limit; i++)
                assert(fprintf(file, serial ? "%u,RX,01\n" : "100,%u,16,1200,5000,0,1000\n", i) > 0);
        } else if (kind == 0) {
            assert(fputs(serial ? "100,RX,01 02 FF\nbad,row\n101,TX,41 0D\n" :
                         "100,0,16,1200,5000,0,1000\nbad,row\n100,200,16,1300,5000,0,1000\n", file) >= 0);
        }
    }
    assert(fclose(file) == 0); original = fingerprint();
    fault = NO_FAULT; flagged = false; opens = seeks = lines = closes = attempts = fail_allocation = 0;
    assert(payload_clipboard_store(retained, sizeof(retained)));
}
static bool show(bool serial)
{ bool handled = serial ? serial_log_viewer_show(content, path) : capture_viewer_show(content, path); refresh(); return handled; }
static bool success_view(bool serial, unsigned kind)
{
    if (serial) {
        const char *expected = kind == 2 ? "Rows 1-8 of 512" : "Rows 1-2 of 2";
        return allocations == 2 && objects(content, &lv_chart_class) == 0 &&
            text_exists(content, expected) && text_exists(content, "COPY ROW") &&
            text_exists(content, kind == 2 ? "first 512 loaded" : "1 malformed skipped");
    }
    lv_obj_t *chart = find(content, &lv_chart_class);
    if (!chart || allocations != 1 || lv_chart_get_point_count(chart) != 300) return false;
    lv_chart_series_t *series = lv_chart_get_series_next(chart, NULL);
    int32_t *values = lv_chart_get_series_y_array(chart, series);
    return text_exists(content, kind == 2 ? "2048 rows" : kind == 4 ? "3 rows" : "2 rows") &&
        values[0] == (kind == 4 ? 96 : 1200) && values[299] == (kind == 4 ? 97 : kind == 2 ? 1200 : 1300) &&
        (kind != 4 || values[150] == LV_CHART_POINT_NONE) &&
        (kind != 2 || text_exists(content, "showing first 2048"));
}
static bool error_view(bool serial, const char *message)
{
    return !allocations && !objects(content, &lv_chart_class) && !text_exists(content, "COPY ROW") &&
        text_exists(content, message ? message : serial ? "Could not read log" : "Could not read capture");
}
static void report(bool serial, const char *name, bool pass, bool handled)
{
    bool unchanged = fingerprint() == original;
    pass = pass && unchanged && !handles && clipboard_retained(); cases++; failures += !pass;
    printf("%s %s-%s handled=%d charts=%u copy_controls=%d allocations=%u handles=%u opens=%u seeks=%u lines=%u closes=%u file_unchanged=%d\n",
        pass ? "PASS" : "FAIL", serial ? "serial" : "capture", name, handled,
        objects(content, &lv_chart_class), text_exists(content, "COPY ROW"), allocations, handles, opens, seeks, lines, closes, unchanged);
    stop();
}
static void ordinary_case(bool serial, const char *name, unsigned kind)
{
    prepare(serial, kind); bool handled = show(serial);
    bool pass = kind == 1 ? handled && error_view(serial, serial ? "No readable log rows" : "No readable capture rows") :
                kind == 3 ? !handled && !allocations && lv_obj_get_child_count(content) == 0 : handled && success_view(serial, kind);
    report(serial, name, pass, handled);
}
static void failure_case(bool serial, const char *name, fault_t injected, unsigned allocation)
{
    prepare(serial, 0); fault = injected; fail_allocation = allocation;
    bool handled = show(serial);
    bool pass = injected == OPEN_FAIL ? !handled && !allocations && !handles :
        handled && error_view(serial, allocation ? serial ? "Not enough memory to open log" : "Not enough memory to open capture" : NULL);
    report(serial, name, pass, handled);
}
static void retry_case(bool serial)
{
    prepare(serial, 0); fault = CLOSE_FAIL; bool handled = show(serial);
    bool pass = handled && error_view(serial, NULL); stop(); fault = NO_FAULT;
    handled = show(serial); pass = pass && handled && success_view(serial, 0) && opens == 2 && closes == 2;
    report(serial, "retry-after-close", pass, handled);
}
static void cycle_case(bool serial)
{
    prepare(serial, 0); lv_mem_monitor_t before, after; lv_mem_monitor(&before);
    bool pass = true, handled = false;
    for (unsigned i = 0; i < 25; i++) {
        fault = NO_FAULT; handled = show(serial); pass = pass && handled && success_view(serial, 0); stop();
        fault = CLOSE_FAIL; handled = show(serial); pass = pass && handled && error_view(serial, NULL); stop();
        fault = NO_FAULT; handled = show(serial); pass = pass && handled && success_view(serial, 0); stop();
    }
    lv_mem_monitor(&after);
    pass = pass && before.free_size == after.free_size && before.used_cnt == after.used_cnt && opens == 75 && closes == 75;
    printf("HEAP %s cycles=25 before_free=%zu after_free=%zu before_used=%zu after_used=%zu\n", serial ? "serial" : "capture",
           before.free_size, after.free_size, before.used_cnt, after.used_cnt);
    report(serial, "cycles-25", pass, handled);
}
int main(void)
{
    snprintf(path, sizeof(path), ".csv_viewer_%d.CSV", _getpid());
    struct stat existing; assert(stat(path, &existing) != 0 && errno == ENOENT); owns_file = true;
    lv_init(); lv_display_t *display = lv_display_create(720, 1280);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, framebuffer, NULL, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    content = lv_obj_create(lv_screen_active()); lv_obj_set_size(content, 720, 1180);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_all(content, 28, 0);
    lv_obj_set_style_pad_row(content, 10, 0); refresh();
    for (unsigned kind = 0; kind < 2; kind++) {
        bool serial = kind != 0;
        ordinary_case(serial, "data", 0); ordinary_case(serial, "empty", 1);
        ordinary_case(serial, "limit", 2); ordinary_case(serial, "unsupported-header", 3);
        if (!serial) ordinary_case(false, "i2c-gaps", 4);
        prepare(serial, 0); bool handled = serial ? serial_log_viewer_show(content, "unpublished.TMP") : capture_viewer_show(content, "unpublished.TMP");
        report(serial, "unsupported-extension", !handled && !opens && !allocations, handled);
        failure_case(serial, "open", OPEN_FAIL, 0); failure_case(serial, "allocation-first", NO_FAULT, 1);
        if (serial) failure_case(true, "allocation-second", NO_FAULT, 2);
        failure_case(serial, "seek", SEEK_FAIL, 0); failure_case(serial, "header-read", HEADER_FAIL, 0);
        failure_case(serial, "first-row-read", FIRST_ROW_FAIL, 0); failure_case(serial, "later-row-read", LATER_ROW_FAIL, 0);
        failure_case(serial, "full-row-read", FULL_ROW_FAIL, 0); failure_case(serial, "close", CLOSE_FAIL, 0);
        failure_case(serial, "read-and-close", READ_CLOSE_FAIL, 0); failure_case(serial, "close-no-errno", CLOSE_NO_ERRNO, 0);
        retry_case(serial); cycle_case(serial);
    }
    assert(remove(path) == 0); owns_file = false; lv_deinit();
    printf("%s %u CSV viewer cases failures=%u handles=%u allocations=%u (real LVGL; native files; controlled heap/I/O)\n",
           failures ? "FAIL" : "PASS", cases, failures, handles, allocations);
    return failures ? 1 : 0;
}
