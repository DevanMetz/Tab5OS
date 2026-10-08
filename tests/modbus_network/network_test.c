/* Uses actual Modbus UI/worker code, native threads, and real loopback TCP. */
#include "host.h"
#include "lvgl.h"
#include "modbus_tool.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint16_t framebuffer[720 * 1280];
static lv_obj_t *content;
static const char *output_directory;
static unsigned queued_failures;
static uint32_t ticks(void) { return (uint32_t)GetTickCount64(); }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}
static void pump(void)
{
    lv_timer_handler();
    Sleep(2);
}
static void pump_for(unsigned ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < until)
        pump();
}
static lv_obj_t *find_type(lv_obj_t *o, const lv_obj_class_t *type, unsigned *index)
{
    if (lv_obj_check_type(o, type) && (*index)-- == 0)
        return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *found = find_type(lv_obj_get_child(o, i), type, index);
        if (found)
            return found;
    }
    return NULL;
}
static lv_obj_t *nth(const lv_obj_class_t *type, unsigned index)
{
    lv_obj_t *o = find_type(content, type, &index);
    assert(o);
    return o;
}
static lv_obj_t *find_text(lv_obj_t *o, const char *text, bool exact)
{
    if (lv_obj_check_type(o, &lv_label_class) &&
        (exact ? !strcmp(lv_label_get_text(o), text) : strstr(lv_label_get_text(o), text) != NULL))
        return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *found = find_text(lv_obj_get_child(o, i), text, exact);
        if (found)
            return found;
    }
    return NULL;
}
static void expect(const char *text)
{
    if (!find_text(content, text, false)) {
        fprintf(stderr, "Missing UI text: %s\n", text);
        lv_obj_t *failure = find_text(content, "failed", false);
        if (failure) fprintf(stderr, "UI failure: %s\n", lv_label_get_text(failure));
        assert(0);
    }
}
static void click(const char *text)
{
    lv_obj_t *label = find_text(content, text, true);
    assert(label);
    lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL);
}
static void input(unsigned index, const char *text)
{
    lv_textarea_set_text(nth(&lv_textarea_class, index), text);
}
static void select_option(unsigned index, unsigned value)
{
    lv_obj_t *o = nth(&lv_dropdown_class, index);
    assert(!lv_obj_has_state(o, LV_STATE_DISABLED));
    lv_dropdown_set_selected(o, value);
    lv_obj_send_event(o, LV_EVENT_VALUE_CHANGED, NULL);
}
static void function(unsigned fc) { select_option(0, fc - 1); }
static void start_read(void)
{
    LONG before = host_sockets_opened;
    click("READ");
    expect("CONFIRM");
    assert(host_sockets_opened == before);
    click("CONFIRM");
}
static void finish(void)
{
    int64_t until = esp_timer_get_time() + 7000000;
    while (modbus_tool_busy() || InterlockedCompareExchange(&host_active_tasks, 0, 0)) {
        assert(esp_timer_get_time() < until);
        pump();
    }
    pump_for(120);
    assert(host_open_sockets == 0);
    assert(host_active_tasks == 0);
}
static void open_app(void)
{
    modbus_tool_show(content, true);
    pump_for(120);
}
static void clean_app(void)
{
    modbus_tool_stop();
    lv_obj_clean(content);
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
}
static void queued_check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL queued worker: %s\n", message);
        queued_failures++;
    }
}
static unsigned displayed_duration(void)
{
    lv_obj_t *label = find_text(content, "Connection closed", false);
    assert(label);
    const char *text = lv_label_get_text(label);
    const char *value = strrchr(text, '|');
    if (!value) value = strrchr(text, ',');
    unsigned duration = 0;
    assert(value && sscanf(value + 1, " %u ms", &duration) == 1);
    return duration;
}
static void hold_read(bool probe)
{
    InterlockedExchange(&host_hold_task_start, 1);
    LONG sockets = host_sockets_opened;
    if (probe) click("TEST TCP");
    else start_read();
    DWORD until = GetTickCount() + 2000;
    while (!InterlockedCompareExchange(&host_task_start_waiters, 0, 0)) {
        assert((LONG)(until - GetTickCount()) > 0);
        pump();
    }
    assert(modbus_tool_busy() && host_active_tasks == 1 && host_sockets_opened == sockets);
    /* A stale event cannot admit a second owner while the worker is held. */
    LONG tasks = host_tasks_started;
    click("TEST TCP");
    assert(modbus_tool_busy() && host_tasks_started == tasks);
}
static void release_read(int64_t elapsed_us)
{
    InterlockedExchangeAdd64(&host_monotonic_offset_us, elapsed_us);
    InterlockedExchange(&host_hold_task_start, 0);
    finish();
    assert(host_task_start_waiters == 0 && host_active_tasks == 0 && !modbus_tool_busy());
}
static void queued_case(const char *scenario)
{
    if (!strcmp(scenario, "queued-retry")) {
        /* Warm the socket provider and compare identical, closed UI states. */
        start_read(); finish(); expect("Read completed: 16 values.");
        clean_app(); pump_for(25);
        DWORD before, after;
        GetProcessHandleCount(GetCurrentProcess(), &before);
        lv_mem_monitor_t heap_before, heap_after;
        lv_mem_monitor(&heap_before);
        for (unsigned i = 0; i < 25; i++) {
            open_app();
            LONG sockets = host_sockets_opened;
            hold_read(false);
            release_read(6000000);
            queued_check(host_sockets_opened == sockets, "expired retry opened a socket");
            queued_check(find_text(content, "5-second exchange timed out", false) != NULL,
                         "expired retry was not reported as timed out");
            queued_check(displayed_duration() >= 6000, "retry omitted scheduling time");
            start_read(); finish(); expect("Read completed: 16 values.");
            queued_check(displayed_duration() < 1500, "fresh retry inherited the previous deadline");
            clean_app(); pump_for(25);
        }
        GetProcessHandleCount(GetCurrentProcess(), &after);
        lv_mem_monitor(&heap_after);
        assert(before == after && heap_before.free_size == heap_after.free_size &&
               heap_before.used_cnt == heap_after.used_cnt);
        printf("queued_pairs=25 heap_free=%u->%u heap_used=%u->%u handles=%lu->%lu ",
               (unsigned)heap_before.free_size, (unsigned)heap_after.free_size,
               (unsigned)heap_before.used_cnt, (unsigned)heap_after.used_cnt,
               (unsigned long)before, (unsigned long)after);
        return;
    }
    bool probe = !strcmp(scenario, "queued-probe-expired");
    bool ready = !strcmp(scenario, "queued-read-ready");
    bool remaining = !strcmp(scenario, "queued-read-remaining");
    bool stop = !strcmp(scenario, "queued-read-stop");
    bool home = !strcmp(scenario, "queued-read-home");
    LONG sockets = host_sockets_opened;
    hold_read(probe);
    if (stop) click("STOP");
    if (home) {
        clean_app(); open_app();
        assert(modbus_tool_busy());
        LONG tasks = host_tasks_started;
        click("TEST TCP");
        assert(host_tasks_started == tasks && host_sockets_opened == sockets);
    }
    ULONGLONG released_at = GetTickCount64();
    release_read(ready || remaining ? 4000000 : 6000000);
    ULONGLONG real_wait_ms = GetTickCount64() - released_at;
    unsigned duration = displayed_duration();
    queued_check(duration >= (ready || remaining ? 4000U : 6000U), "duration omitted scheduling time");
    if (ready) {
        expect("Read completed: 16 values.");
        queued_check(duration < 5000 && host_sockets_opened == sockets + 1,
                     "request with remaining time did not complete once");
    } else {
        queued_check(find_text(content, stop || home ? "Cancelled; connection closed." :
                               "5-second exchange timed out", false) != NULL,
                     "queued request did not retain its cancellation/deadline result");
        queued_check(find_text(content, "No values returned.", false) != NULL,
                     "queued failure exposed response values");
        queued_check(host_sockets_opened == sockets + (remaining ? 1 : 0),
                     "cancelled or expired request opened a socket");
        if (remaining) queued_check(real_wait_ms >= 600 && real_wait_ms < 2200 && duration < 6200,
                                    "worker received a new five-second budget");
        else queued_check(real_wait_ms < 1500, "cancelled or expired worker did not promptly finish");
    }
    printf("queued_duration_ms=%u remaining_wait_ms=%llu ", duration, (unsigned long long)real_wait_ms);
}
static void check_bounds(lv_obj_t *o)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(o, &lv_textarea_class) || lv_obj_check_type(o, &lv_keyboard_class) ||
        lv_obj_check_type(o, &lv_dropdown_class) || lv_obj_check_type(o, &lv_button_class) || lv_obj_get_parent(o) == content) {
        lv_area_t area; lv_obj_get_coords(o, &area);
        if (area.x1 < 28 || area.x2 > 691 || area.y1 < 128 || area.y2 > 1251) {
            fprintf(stderr, "Object overflow: (%ld,%ld)-(%ld,%ld)\n", (long)area.x1, (long)area.y1, (long)area.x2, (long)area.y2);
            assert(0);
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) check_bounds(lv_obj_get_child(o, i));
}
static void shot(const char *name)
{
    pump_for(100); lv_obj_update_layout(content); lv_refr_now(NULL);
    check_bounds(lv_obj_get_child(content, 0));
    char path[1024];
    assert(snprintf(path, sizeof(path), "%s/%s.ppm", output_directory, name) < (int)sizeof(path));
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n720 1280\n255\n");
    for (size_t i = 0; i < 720U * 1280; i++) {
        uint16_t p = framebuffer[i];
        unsigned char rgb[] = {(unsigned char)(((p >> 11) & 31) * 255 / 31),
                               (unsigned char)(((p >> 5) & 63) * 255 / 63),
                               (unsigned char)((p & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}
int main(int argc, char **argv)
{
    assert(argc == 4);
    output_directory = argv[3];
    const char *scenario = argv[2];
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    setvbuf(stdout, NULL, _IONBF, 0);
    WSADATA data;
    assert(!WSAStartup(MAKEWORD(2, 2), &data));
    lv_init(); lv_tick_set_cb(ticks);
    lv_display_t *display = lv_display_create(720, 1280);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, framebuffer, NULL, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x10141f), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, 720, 100);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x20283a), 0);
    lv_obj_set_style_text_color(header, lv_color_white(), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_28, 0);
    lv_obj_t *brand = lv_label_create(header); lv_label_set_text(brand, "Tab5 OS");
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *home = lv_button_create(header); lv_obj_set_size(home, 120, 64);
    lv_obj_align(home, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_t *label = lv_label_create(home); lv_label_set_text(label, "Home"); lv_obj_center(label);
    content = lv_obj_create(screen); lv_obj_set_size(content, 720, 1180);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x10141f), 0);
    lv_obj_set_style_text_color(content, lv_color_white(), 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, 28, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    open_app();
    input(0, "127.0.0.1");
    input(1, argv[1]);
    input(2, "1");
    input(3, "10");
    input(4, "16");
    bool warmed_handles = false;
    DWORD handles_before = 0, handles_after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles_before);
    int64_t began = esp_timer_get_time();
    if (!strncmp(scenario, "queued-", 7)) {
        queued_case(scenario);
    } else if (!strcmp(scenario, "ui")) {
        assert(lv_obj_has_state(nth(&lv_dropdown_class, 1), LV_STATE_DISABLED));
        assert(lv_obj_has_state(nth(&lv_dropdown_class, 2), LV_STATE_DISABLED));
        shot("modbus-default");
        lv_obj_t *keyboard = nth(&lv_keyboard_class, 0);
        assert(lv_obj_get_style_text_font(keyboard, LV_PART_ITEMS) == &lv_font_montserrat_28);
        lv_obj_send_event(nth(&lv_textarea_class, 0), LV_EVENT_CLICKED, NULL);
        shot("modbus-keyboard");
        lv_obj_send_event(keyboard, LV_EVENT_READY, NULL);
        assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        lv_dropdown_open(nth(&lv_dropdown_class, 0)); shot("modbus-function-menu");
        clean_app(); pump_for(25);
        lv_mem_monitor_t before, after; lv_mem_monitor(&before);
        for (unsigned i = 0; i < 100; i++) {
            open_app();
            lv_dropdown_open(nth(&lv_dropdown_class, 0));
            clean_app(); pump();
        }
        pump_for(25); lv_mem_monitor(&after);
        assert(before.free_size == after.free_size && before.used_cnt == after.used_cnt);
        assert(host_sockets_opened == 0 && host_tasks_started == 0);
        printf("100 lifecycles heap_free=%u->%u ", (unsigned)before.free_size, (unsigned)after.free_size);
    } else if (!strcmp(scenario, "views")) {
        start_read();
        assert(lv_obj_has_state(nth(&lv_dropdown_class, 1), LV_STATE_DISABLED));
        finish();
        expect("10 : 16256 | 0x3F80 | 16256");
        shot("modbus-words");
        select_option(1, 3); expect("Float32 | ABCD");
        expect("10-11 : 1\n"); expect("12-13 : -0\n");
        expect("14-15 : +Infinity\n"); expect("16-17 : -Infinity\n");
        expect("18-19 : NaN\n"); expect("20-21 : 1.40129846e-45\n");
        expect("22-23 : 3.40282347e+38\n"); expect("24-25 : NaN\n");
        shot("modbus-float32");
        select_option(1, 2); expect("24-25 : -1\n"); expect("12-13 : -2147483648\n");
        select_option(1, 1); expect("24-25 : 4294967295\n"); expect("12-13 : 2147483648\n");
        const char *values[] = {"10-11 : 1065353216\n", "10-11 : 16256\n", "10-11 : 2151612416\n", "10-11 : 32831\n"};
        for (unsigned order = 0; order < 4; order++) { select_option(2, order); expect(values[order]); }
        expect("Raw 3F80 0000 | bits 0x0000803F");
        assert(host_tasks_started == 1 && host_sockets_opened == 1);
        /* Editing the next request cannot relabel, re-pair or refetch the reply. */
        input(3, "100"); input(4, "1"); function(1);
        select_option(1, 3); select_option(2, 0);
        expect("unit 1 | FC 03"); expect("Zero-based address 10, count 16"); expect("10-11 : 1\n");
        select_option(2, 1);
        lv_dropdown_open(nth(&lv_dropdown_class, 2)); shot("modbus-order-menu");
        clean_app(); open_app();
        assert(lv_dropdown_get_selected(nth(&lv_dropdown_class, 1)) == 3);
        assert(lv_dropdown_get_selected(nth(&lv_dropdown_class, 2)) == 1);
        expect("Float32 | CDAB"); expect("unit 1 | FC 03");
        assert(host_tasks_started == 1 && host_sockets_opened == 1);
        /* A final unmatched register is displayed explicitly, never padded. */
        function(4); input(3, "65533"); input(4, "3");
        select_option(2, 0); start_read(); finish();
        expect("65533-65534 : 123456\n"); expect("65535 : unpaired register 0xABCD");
        shot("modbus-odd-pair");
        input(3, "65535"); input(4, "1"); start_read(); finish();
        expect("65535 : unpaired register 0xABCD");
        assert(!find_text(content, "65536", false));
        function(1); input(3, "10"); input(4, "16"); start_read(); finish();
        expect("10 : 0"); expect("25 : 1");
        assert(lv_obj_has_state(nth(&lv_dropdown_class, 1), LV_STATE_DISABLED));
        assert(lv_obj_has_state(nth(&lv_dropdown_class, 2), LV_STATE_DISABLED));
        assert(host_tasks_started == 4 && host_sockets_opened == 4);
    } else if (!strcmp(scenario, "inputs")) {
        const char *bad_hosts[] = {"127.1", "0177.0.0.1", "0x7f.0.0.1", "2130706433",
            "127.0.0.01", "127.0.0.1 ", "+127.0.0.1", "127.0.0.-1", "127.0.0.1/24",
            "127.\n0.0.1", "127.0.0.1\t", "255.255.255.2551", "127.0.0.\xc2\xb9"};
        for (size_t i = 0; i < sizeof(bad_hosts) / sizeof(bad_hosts[0]); i++) {
            input(0, bad_hosts[i]);
            assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), bad_hosts[i]));
            click("READ"); expect("full decimal IPv4");
            click("TEST TCP"); expect("full decimal IPv4");
            assert(host_sockets_opened == 0 && host_tasks_started == 0 && !modbus_tool_busy());
        }
        const char *defaults[] = {"127.0.0.1", argv[1], "1", "10", "16"};
        input(0, defaults[0]);
        const char *bad_numbers[] = {"-1", "+1", "1e2", "1\n0", "12x", " 1", "1 ", "000001", "999999"};
        const char *errors[] = {"", "TCP port must", "Unit ID must", "Address must", "Address must"};
        for (unsigned field = 1; field < 5; field++) {
            for (size_t i = 0; i < sizeof(bad_numbers) / sizeof(bad_numbers[0]); i++) {
                input(field, bad_numbers[i]);
                assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, field)), bad_numbers[i]));
                click("READ"); expect(errors[field]);
                if (field == 1) { click("TEST TCP"); expect(errors[field]); }
                assert(host_sockets_opened == 0 && host_tasks_started == 0);
            }
            clean_app(); open_app();
            assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, field)), "999999"));
            click("READ"); expect(errors[field]);
            input(field, defaults[field]);
        }
        char unicode[65];
        for (unsigned i = 0; i < 16; i++) memcpy(unicode + i * 4, "\xf0\x9f\x94\xa5", 4);
        unicode[64] = 0;
        input(0, unicode); clean_app(); open_app();
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), unicode));
        click("READ"); expect("full decimal IPv4");
        input(0, "127.0.0.1");
        click("READ"); expect("CONFIRM");
        input(3, "-1"); click("READ"); expect("Address must");
        assert(host_sockets_opened == 0 && host_tasks_started == 0 && !modbus_tool_busy());
    } else if (!strcmp(scenario, "functions") || !strcmp(scenario, "fragmented")) {
        for (unsigned fc = 1; fc <= 4; fc++) {
            function(fc);
            start_read();
            finish();
            expect("Read completed: 16 values.");
            if (fc == 1) {
                GetProcessHandleCount(GetCurrentProcess(), &handles_before);
                warmed_handles = true;
            }
            if (fc <= 2) {
                expect("10 : 0");
                expect("25 : 1");
            } else {
                expect("0x000A");
                expect("0x0019");
            }
        }
    } else if (!strcmp(scenario, "gates")) {
        click("READ");
        expect("CONFIRM");
        assert(host_sockets_opened == 0);
        input(3, "11");
        expect("Inputs updated");
        assert(find_text(content, "READ", true));
        assert(host_sockets_opened == 0);
        click("READ");
        expect("CONFIRM");
        input(3, "10");
        assert(host_sockets_opened == 0);
        click("READ");
        expect("CONFIRM");
        pump_for(5200);
        expect("Read confirmation expired");
        assert(host_sockets_opened == 0);
        start_read();
        finish();
        expect("Read completed: 16 values.");
    } else if (!strcmp(scenario, "probe")) {
        click("TEST TCP");
        finish();
        expect("TCP connection succeeded; no application bytes sent.");
    } else if (!strcmp(scenario, "repeated")) {
        for (unsigned i = 0; i < 100; i++) {
            start_read();
            finish();
            expect("Read completed: 16 values.");
            if (i == 0) {
                GetProcessHandleCount(GetCurrentProcess(), &handles_before);
                warmed_handles = true;
            }
        }
    } else if (!strcmp(scenario, "stop") || !strcmp(scenario, "home")) {
        start_read();
        pump_for(250);
        assert(modbus_tool_busy());
        int64_t cancel_at = esp_timer_get_time();
        if (!strcmp(scenario, "stop"))
            click("STOP");
        else
            clean_app();
        finish();
        int64_t cancelled_ms = (esp_timer_get_time() - cancel_at) / 1000;
        assert(cancelled_ms < 1200);
        if (!strcmp(scenario, "home"))
            open_app();
        expect("Cancelled; connection closed.");
        expect("No values returned.");
        printf("cancel_ms=%lld ", (long long)cancelled_ms);
        start_read();
        finish();
        expect("Read completed: 16 values.");
    } else {
        start_read();
        finish();
        if (!strcmp(scenario, "exception")) {
            expect("Modbus exception 0x02");
            expect("No values returned.");
        } else if (!strcmp(scenario, "mismatch") || !strcmp(scenario, "oversized")) {
            expect("Rejected response");
            expect("No values returned.");
        } else if (!strcmp(scenario, "short")) {
            expect("Receive failed");
            expect("No values returned.");
        } else if (!strcmp(scenario, "closed")) {
            expect("Connect failed");
            expect("No values returned.");
        } else if (!strcmp(scenario, "slow") || !strcmp(scenario, "timeout")) {
            expect("5-second exchange timed out");
            expect("No values returned.");
            int64_t elapsed = esp_timer_get_time() - began;
            assert(elapsed >= 4800000 && elapsed < 6500000);
        } else
            assert(0);
    }
    GetProcessHandleCount(GetCurrentProcess(), &handles_after);
    if (warmed_handles)
        assert(handles_after == handles_before);
    assert(host_open_sockets == 0 && host_active_tasks == 0);
    printf("%s %s elapsed_ms=%lld tasks=%ld sockets=%ld handles=%lu->%lu failures=%u\n", scenario,
           queued_failures ? "FAIL" : "PASS",
           (long long)((esp_timer_get_time() - began) / 1000), host_tasks_started,
           host_sockets_opened, (unsigned long)handles_before, (unsigned long)handles_after, queued_failures);
    clean_app();
    lv_deinit();
    WSACleanup();
    return queued_failures ? 1 : 0;
}
