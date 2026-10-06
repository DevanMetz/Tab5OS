/* Actual app, pinned SDK HTTP client/parser and LVGL with real loopback TCP. */
#include "adapter.h"
#include "dns_adapter.h"
#include "http_tool.h"
#include "http_transport.h"
#include "storage.h"
#include "lvgl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t framebuffer[720 * 1280];
static lv_obj_t *content;
static const char *output_directory;
static bool sd_available;
static unsigned storage_errors;
static int last_storage_error;
static DWORD ui_thread;
static void storage_error(int error)
{
    assert(GetCurrentThreadId() == ui_thread && error);
    storage_errors++; last_storage_error = error;
}
static uint32_t ticks(void) { return (uint32_t)GetTickCount64(); }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels; lv_display_flush_ready(display);
}
static void pump(void) { lv_timer_handler(); Sleep(2); }
static void pump_for(unsigned ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < until) pump();
}
static lv_obj_t *find_type(lv_obj_t *object, const lv_obj_class_t *type, unsigned *index)
{
    if (lv_obj_check_type(object, type) && (*index)-- == 0) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find_type(lv_obj_get_child(object, i), type, index);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *nth(const lv_obj_class_t *type, unsigned index)
{
    lv_obj_t *object = find_type(content, type, &index); assert(object); return object;
}
static lv_obj_t *find_text(lv_obj_t *object, const char *text, bool exact)
{
    if (lv_obj_check_type(object, &lv_label_class) &&
        (exact ? !strcmp(lv_label_get_text(object), text) : strstr(lv_label_get_text(object), text) != NULL)) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find_text(lv_obj_get_child(object, i), text, exact);
        if (found) return found;
    }
    return NULL;
}
static void dump_text(lv_obj_t *object)
{
    if (lv_obj_check_type(object, &lv_label_class)) printf("[%s]\n", lv_label_get_text(object));
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) dump_text(lv_obj_get_child(object, i));
}
static void expect(const char *text)
{
    if (!find_text(content, text, false)) { fprintf(stderr, "Missing: %s\n", text); dump_text(content); assert(0); }
}
static void click(const char *text)
{
    lv_obj_t *label = find_text(content, text, true); assert(label);
    lv_obj_t *button = lv_obj_get_parent(label); assert(!lv_obj_has_state(button, LV_STATE_DISABLED));
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
}
static void input(unsigned index, const char *text) { lv_textarea_set_text(nth(&lv_textarea_class, index), text); }
static void expect_request(const char *method, const char *endpoint)
{
    char expected[320];
    snprintf(expected, sizeof(expected), "Request: %s %s\nQuery/fragment omitted", method, endpoint);
    if (!find_text(content, expected, true)) { dump_text(content); assert(0); }
}
static void wait_without_ui(void)
{
    int64_t until = esp_timer_get_time() + 3000000;
    while (http_tool_busy() || host_active_tasks) { assert(esp_timer_get_time() < until); Sleep(2); }
    assert(http_host_allocations == 1 && !host_open_sockets && !http_host_open_files);
}
static void finish(void)
{
    int64_t until = esp_timer_get_time() + 18000000;
    while (http_tool_busy() || host_active_tasks) { assert(esp_timer_get_time() < until); pump(); }
    pump_for(220);
    if (host_open_sockets || http_host_allocations || http_host_transports || http_host_sdk_allocations)
        fprintf(stderr, "Outstanding sockets=%ld jobs=%ld transports=%ld SDK allocations=%ld\n",
                host_open_sockets, http_host_allocations, http_host_transports, http_host_sdk_allocations);
    assert(!host_open_sockets && !http_host_allocations && !http_host_transports && !http_host_sdk_allocations);
    assert(!http_host_open_files);
    assert(lv_obj_has_state(lv_obj_get_parent(find_text(content, "CANCEL", true)), LV_STATE_DISABLED));
}
static void start(void)
{
    LONG before = host_tasks_started;
    click("SEND"); expect("Tap SEND again"); assert(host_tasks_started == before);
    click("SEND");
}
static void wait_dns(volatile LONG *counter, LONG target)
{
    int64_t until = esp_timer_get_time() + 1000000;
    while (*counter < target) { assert(esp_timer_get_time() < until); pump(); }
    assert(http_tool_busy() && !host_open_sockets);
}
static bool not_cancelled(void *context) { (void)context; return false; }
static void dns_case(const char *mode, const char *port)
{
    if (!strcmp(mode, "dns-budget")) {
        int64_t began = esp_timer_get_time();
        esp_transport_handle_t transport = http_transport_init(false, began + 120000, not_cancelled, NULL);
        assert(transport);
        assert(esp_transport_connect(transport, "hold.fixture.test", atoi(port), 10000) < 0);
        assert(http_transport_stop_reason(transport) == HTTP_TRANSPORT_DEADLINE);
        assert(http_transport_get_errno(transport) == ETIMEDOUT);
        assert(esp_timer_get_time() - began >= 120000 && esp_timer_get_time() - began < 500000);
        esp_transport_destroy(transport);
        assert(!http_host_allocations && !http_host_sdk_allocations && !http_host_transports);
        assert(http_host_dns_requests == 1);
        http_dns_test_release_oldest(false); pump_for(50);
        assert(http_host_dns_callbacks == 1 && !host_sockets_opened);
        return;
    }
    const char *host = !strcmp(mode, "dns-delayed") || !strcmp(mode, "dns-post-failure") ? "slow.fixture.test" :
                       !strcmp(mode, "dns-failure") ? "fail.fixture.test" :
                       !strcmp(mode, "dns-memory") ? "memory.fixture.test" :
                       !strcmp(mode, "dns-ipv6") ? "v6.fixture.test" :
                       !strcmp(mode, "ipv6-literal") ? "[::1]" :
                       !strcmp(mode, "dns-cancel") || !strcmp(mode, "dns-queued-cancel") || !strcmp(mode, "dns-queued-next") ||
                       !strcmp(mode, "dns-late") || !strcmp(mode, "dns-timeout") ? "hold.fixture.test" :
                       "cache.fixture.test";
    char url[256]; snprintf(url, sizeof(url), "%s://%s:%s/dns?token=DNS_QUERY_SECRET#fragment",
                           !strcmp(mode, "dns-tls") ? "https" : "http", host, port);
    input(0, url);
    if (!strcmp(mode, "dns-tls")) {
        click("SEND"); finish(); expect("Request failed");
        assert(!strcmp(http_host_tls_name, "cache.fixture.test") && !strcmp(http_host_last_peer, "127.0.0.1"));
        assert(http_host_dns_requests == 1 && !host_sockets_opened && http_host_certificates >= 2);
        return;
    }
    int64_t began = esp_timer_get_time(); start();
    if (!strcmp(mode, "dns-cancel") || !strcmp(mode, "dns-queued-cancel") || !strcmp(mode, "dns-queued-next") || !strcmp(mode, "dns-late")) {
        bool queued = !strcmp(mode, "dns-queued-cancel") || !strcmp(mode, "dns-queued-next");
        wait_dns(queued ? &http_host_dns_posts : &http_host_dns_requests, 1);
        int64_t cancelled = esp_timer_get_time(); click("CANCEL"); finish(); expect("Cancelled after");
        assert(esp_timer_get_time() - cancelled < 800000 && !host_sockets_opened);
        if (!strcmp(mode, "dns-queued-next")) {
            snprintf(url, sizeof(url), "http://next.fixture.test:%s/dns?token=DNS_QUERY_SECRET#fragment", port);
            input(0, url); start(); wait_dns(&http_host_dns_posts, 2);
            assert(!http_host_dns_requests);
            http_dns_test_release_posts(); wait_dns(&http_host_dns_requests, 1); pump_for(50);
            assert(http_host_dns_requests == 1 && http_tool_busy() && !host_sockets_opened);
            http_dns_test_release_oldest(true); finish(); expect("HTTP 200");
            assert(http_host_dns_callbacks == 1); return;
        } else if (!strcmp(mode, "dns-late")) {
            snprintf(url, sizeof(url), "http://next.fixture.test:%s/dns?token=DNS_QUERY_SECRET#fragment", port);
            input(0, url); start(); wait_dns(&http_host_dns_requests, 2);
            http_dns_test_release_oldest(false); pump_for(50);
            assert(http_host_dns_callbacks == 1 && http_tool_busy() && !host_sockets_opened);
            http_dns_test_release_oldest(true); finish(); expect("HTTP 200");
            assert(http_host_dns_callbacks == 2); return;
        }
        if (queued) http_dns_test_release_posts(); else http_dns_test_release_oldest(false);
        pump_for(50); expect("Cancelled after");
        assert(http_host_dns_requests == (queued ? 0 : 1) && http_host_dns_callbacks == (queued ? 0 : 1));
        snprintf(url, sizeof(url), "http://127.0.0.1:%s/after", port); input(0, url); start(); finish(); expect("HTTP 200");
        return;
    }
    finish();
    if (!strcmp(mode, "dns-cached") || !strcmp(mode, "dns-delayed")) {
        expect("HTTP 200"); assert(http_host_dns_requests == 1);
        assert(http_host_dns_callbacks == (!strcmp(mode, "dns-delayed") ? 1 : 0));
    } else {
        expect("Request failed"); assert(!host_sockets_opened);
        if (!strcmp(mode, "dns-timeout")) {
            assert(esp_timer_get_time() - began >= 10000000 && esp_timer_get_time() - began < 11000000);
            http_dns_test_release_oldest(false); pump_for(50); expect("Request failed");
        } else if (!strcmp(mode, "dns-ipv6") || !strcmp(mode, "ipv6-literal")) {
            assert(!strcmp(http_host_last_peer, "::1"));
            assert(http_host_dns_requests == (!strcmp(mode, "dns-ipv6") ? 1 : 0));
        } else assert(!strcmp(mode, "dns-failure") || !strcmp(mode, "dns-memory") || !strcmp(mode, "dns-post-failure"));
    }
}
static void clean_app(void)
{
    http_tool_stop(); lv_obj_clean(content); lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
}
static void open_app(void) { http_tool_show(content, true, sd_available, storage_error); pump_for(30); }
static void shot(const char *name)
{
    lv_obj_update_layout(content); lv_refr_now(NULL);
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.ppm", output_directory, name);
    FILE *file = fopen(path, "wb"); assert(file); fprintf(file, "P6\n720 1280\n255\n");
    for (size_t i = 0; i < 720U * 1280; i++) {
        uint16_t value = framebuffer[i];
        uint8_t rgb[] = {(uint8_t)(((value >> 11) & 31) * 255 / 31),
                         (uint8_t)(((value >> 5) & 63) * 255 / 63), (uint8_t)((value & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}
static void setup(void)
{
    lv_init(); lv_tick_set_cb(ticks);
    lv_display_t *display = lv_display_create(720, 1280);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, framebuffer, NULL, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x10141f), 0);
    lv_obj_set_style_text_color(lv_screen_active(), lv_color_white(), 0);
    content = lv_obj_create(lv_screen_active()); lv_obj_set_size(content, 720, 1180);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x10141f), 0);
    lv_obj_set_style_text_color(content, lv_color_white(), 0);
    lv_obj_set_style_border_width(content, 0, 0); lv_obj_set_style_pad_all(content, 28, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    open_app();
}

static void read_log(char *text, size_t capacity)
{
    FILE *file = fopen(http_storage_path(), "rb"); assert(file);
    size_t length = fread(text, 1, capacity - 1, file);
    assert(feof(file) && !ferror(file) && fclose(file) == 0); text[length] = 0;
}
static void expect_log_error(unsigned count)
{
    expect("SD log failed"); expect("metadata was not confirmed saved"); expect("SD LOG\nOFF");
    assert(!find_text(content, "Metadata appended", false));
    assert(storage_errors == count && http_host_storage_failures == (LONG)count);
    shot("http-log-error");
    click("SD LOG\nOFF"); expect("Insert a writable SD card");
}
static void logging_case(const char *mode, const char *port)
{
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%s/evidence,\"quoted\"?token=LOG_QUERY_SECRET#LOG_FRAGMENT_SECRET", port);
    input(0, url);
    input(1, "Authorization: Bearer LOG_AUTH_SECRET\nX-Private: LOG_HEADER_SECRET");
    input(2, "LOG_BODY_SECRET");
    if (!strcmp(mode, "log-optin")) {
        start(); click("SD LOG\nOFF"); finish(); expect("HTTP 200");
        expect("SD LOG was OFF for this request"); assert(!http_host_storage_calls);
        start(); finish(); expect("Metadata appended");
        LONG calls = http_host_storage_calls;
        clean_app(); open_app(); expect("SD LOG\nOFF"); input(0, url);
        start(); finish(); expect("HTTP 200"); expect("SD LOG was OFF for this request");
        assert(http_host_storage_calls == calls);
        assert(!storage_errors); return;
    }
    click("SD LOG\nOFF");
    if (!strcmp(mode, "log-success")) {
        const char *methods[] = {"METHOD\nGET", "METHOD\nPOST", "METHOD\nPUT", "METHOD\nDELETE"};
        for (unsigned i = 0; i < 4; i++) {
            start(); finish(); expect("HTTP 200"); expect("Metadata appended"); click(methods[i]);
        }
    } else if (!strcmp(mode, "log-repair")) {
        http_storage_seed("unix_time,meth");
        start(); finish(); expect("Metadata appended");
        char previous[4096], current[4096]; read_log(previous, sizeof(previous));
        FILE *file = fopen(http_storage_path(), "ab"); assert(file);
        assert(fputs("PARTIAL_SECRET,unfinished", file) >= 0 && fclose(file) == 0);
        start(); finish(); expect("Metadata appended"); read_log(current, sizeof(current));
        assert(!strncmp(previous, current, strlen(previous)) && !strstr(current, "PARTIAL_SECRET"));
    } else if (!strcmp(mode, "log-write")) {
        start(); finish(); expect("Metadata appended");
        http_storage_write_limit(10); start(); finish(); expect("HTTP 200"); expect_log_error(1);
        assert(last_storage_error == ENOSPC);
        clean_app(); open_app(); input(0, url); click("SD LOG\nOFF");
        start(); finish(); expect("Metadata appended"); assert(storage_errors == 1);
    } else if (!strcmp(mode, "log-busy")) {
        InterlockedExchange(&http_host_sync_blocked, 1); start();
        int64_t until = esp_timer_get_time() + 1000000;
        while (!http_host_sync_entered) { assert(esp_timer_get_time() < until); pump(); }
        assert(http_tool_busy() && http_host_open_files == 1 && !host_open_sockets && !http_host_sdk_allocations);
        click("CLEAR"); expect("request is still in progress");
        assert(http_tool_busy() && http_host_open_files == 1);
        input(0, "http://changed.invalid/?CHANGED_QUERY_SECRET");
        input(1, "X-Changed: CHANGED_HEADER_SECRET"); input(2, "CHANGED_BODY_SECRET"); click("METHOD\nGET");
        clean_app(); open_app(); pump_for(100);
        assert(http_tool_busy() && http_host_open_files == 1 && storage_errors == 0);
        InterlockedExchange(&http_host_sync_blocked, 0);
        finish(); expect("HTTP 200"); expect("Metadata appended"); expect("SD LOG\nOFF");
        char endpoint[256]; snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%s/evidence,\"quoted\"", port);
        expect_request("GET", endpoint);
    } else if (!strcmp(mode, "log-pending-error")) {
        http_storage_fault(HTTP_STORAGE_SYNC); start();
        /* A stale WORKING tap must show the result before accepting another SEND. */
        wait_without_ui();
        assert(storage_errors == 0 && http_host_allocations == 1);
        click("WORKING"); expect("SD log failed"); expect("SD LOG\nOFF");
        assert(!http_host_allocations && host_tasks_started == 1 && storage_errors == 1);
        assert(lv_obj_has_state(lv_obj_get_parent(find_text(content, "CANCEL", true)), LV_STATE_DISABLED));
        start();
        finish(); expect("HTTP 200"); expect("SD LOG\nOFF");
        assert(storage_errors == 1 && last_storage_error == ENOSPC);
    } else if (!strcmp(mode, "log-pending-clear")) {
        http_storage_fault(HTTP_STORAGE_SYNC); start(); wait_without_ui();
        click("CLEAR"); pump_for(250); expect("Cleared"); expect("SD log failed"); expect("SD LOG\nOFF");
        assert(!*lv_textarea_get_text(nth(&lv_textarea_class, 3)) && !find_text(content, "Request: ", false));
        assert(!http_host_allocations && storage_errors == 1 && last_storage_error == ENOSPC);
    } else if (!strcmp(mode, "log-cancel") || !strcmp(mode, "log-cancel-fault")) {
        if (!strcmp(mode, "log-cancel-fault")) http_storage_fault(HTTP_STORAGE_SYNC);
        start(); pump_for(250); assert(http_tool_busy()); click("CANCEL"); finish(); expect("Cancelled after");
        if (!strcmp(mode, "log-cancel-fault")) { expect_log_error(1); assert(last_storage_error == ENOSPC); }
        else expect("Metadata appended");
    } else if (!strcmp(mode, "log-deadline") || !strcmp(mode, "log-invalid")) {
        start(); finish();
        expect(!strcmp(mode, "log-deadline") ? "15-second request deadline reached" : "Response headers/trailers invalid");
        expect("Metadata appended");
    } else {
        http_storage_fault_t failure;
        if (!strcmp(mode, "log-mkdir")) failure = HTTP_STORAGE_MKDIR;
        else if (!strcmp(mode, "log-open")) failure = HTTP_STORAGE_OPEN;
        else if (!strcmp(mode, "log-flush")) failure = HTTP_STORAGE_FLUSH;
        else if (!strcmp(mode, "log-close")) failure = HTTP_STORAGE_CLOSE;
        else if (!strcmp(mode, "log-sync") || !strcmp(mode, "log-incomplete-sync")) failure = HTTP_STORAGE_SYNC;
        else {
            assert(!strcmp(mode, "log-repair-error")); failure = HTTP_STORAGE_REPAIR;
            http_storage_seed("unix_time,method,url,status,response_bytes,duration_ms,outcome\n"
                              "1,GET,\"http://example.invalid/\",200,2,1,complete\n");
        }
        http_storage_fault(failure); start(); finish();
        expect(!strcmp(mode, "log-incomplete-sync") ? "Incomplete HTTP 200 response" : "HTTP 200");
        expect_log_error(1);
        assert(last_storage_error == (failure == HTTP_STORAGE_SYNC ? ENOSPC :
                                    failure == HTTP_STORAGE_OPEN || failure == HTTP_STORAGE_MKDIR ? EROFS : EIO));
    }
    assert(!http_host_open_files);
}
int main(int argc, char **argv)
{
    assert(argc == 4);
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argv[2]; output_directory = argv[3]; ui_thread = GetCurrentThreadId();
    sd_available = !strncmp(mode, "log-", 4);
    http_storage_setup(output_directory, mode, sd_available);
    WSADATA data; assert(!WSAStartup(MAKEWORD(2, 2), &data)); http_dns_test_init(mode); setup(); http_tool_self_test();
    char url[128]; snprintf(url, sizeof(url), "http://127.0.0.1:%s/?test=discarded#fragment", argv[1]); input(0, url);
    int64_t began = esp_timer_get_time();
    if (!strncmp(mode, "dns-", 4) || !strcmp(mode, "ipv6-literal")) {
        dns_case(mode, argv[1]);
    } else if (!strncmp(mode, "log-", 4)) {
        logging_case(mode, argv[1]); shot("http-log-result");
    } else if (!strcmp(mode, "limits")) {
        char headers[609] = "X-Test: ";
        for (unsigned i = 0; i < 300; i++) memcpy(headers + 8 + i * 2, "\xc3\xa9", 2);
        headers[608] = 0; input(1, headers); click("SEND"); expect("511-byte limit");
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 1)), headers));
        input(1, ""); char body[1025];
        for (unsigned i = 0; i < 256; i++) memcpy(body + i * 4, "\xf0\x9f\x98\x80", 4);
        body[1024] = 0; input(2, body); click("SEND"); expect("1023-byte limit");
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 2)), body));
        assert(!host_tasks_started && !http_host_allocations);
    } else if (!strcmp(mode, "utf8-body")) {
        char body[1021];
        for (unsigned i = 0; i < 255; i++) memcpy(body + i * 4, "\xf0\x9f\x98\x80", 4);
        body[1020] = 0; input(2, body); click("METHOD\nGET"); start(); finish(); expect("HTTP 200");
    } else if (!strcmp(mode, "gates")) {
        click("SEND"); expect("Tap SEND again"); input(2, "{}"); click("SEND"); assert(!host_tasks_started);
        pump_for(5300); expect("confirmation expired");
        click("SEND"); assert(!host_tasks_started); clean_app(); open_app(); input(0, url);
        click("SEND"); assert(!host_tasks_started);
        click("CANCEL"); expect("confirmation cancelled");
        click("SEND"); assert(!host_tasks_started);
        click("CLEAR"); expect("Cleared; ready for another request");
        assert(lv_obj_has_state(lv_obj_get_parent(find_text(content, "CANCEL", true)), LV_STATE_DISABLED));
        click("SEND"); expect("Tap SEND again"); assert(!host_tasks_started);
    } else if (!strcmp(mode, "allocation")) {
        InterlockedExchange(&http_host_fail_allocation, 1); click("SEND"); expect("Request allocation failed");
        InterlockedExchange(&host_fail_next_task, 1); start(); expect("Could not start HTTP worker");
        assert(!http_tool_busy() && !http_host_allocations);
        assert(lv_obj_has_state(lv_obj_get_parent(find_text(content, "CANCEL", true)), LV_STATE_DISABLED));
        InterlockedExchange(&http_host_fail_transport, 1); start(); finish(); expect("Request failed");
        start(); finish(); expect("HTTP 200");
    } else if (!strcmp(mode, "methods")) {
        const char *labels[] = {"METHOD\nGET", "METHOD\nPOST", "METHOD\nPUT", "METHOD\nDELETE"};
        input(1, "X-Tab5-Test: fixture\nAuthorization: Bearer disposable\nX-Second: 2\nX-Third: 3");
        input(2, "{\"test\":true}");
        for (unsigned i = 0; i < 4; i++) { start(); finish(); expect("HTTP 200"); click(labels[i]); }
    } else if (!strcmp(mode, "source-edits")) {
        start();
        char endpoint[128]; snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%s/", argv[1]);
        char next[256]; snprintf(next, sizeof(next), "%ssecond?token=SOURCE_QUERY_SECRET#SOURCE_FRAGMENT_SECRET", endpoint);
        input(0, next); input(1, "X-Changed: captured"); input(2, "{\"changed\":true}"); click("METHOD\nGET");
        finish(); expect_request("GET", endpoint);
        click("SEND"); expect("Tap SEND again"); expect_request("GET", endpoint);
        assert(*lv_textarea_get_text(nth(&lv_textarea_class, 3)));
        click("SEND"); assert(!*lv_textarea_get_text(nth(&lv_textarea_class, 3)));
        snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%s/second", argv[1]);
        expect_request("POST", endpoint); finish(); expect("HTTP 200"); expect_request("POST", endpoint);
        click("CLEAR"); assert(!find_text(content, "Request: ", false));
    } else if (!strcmp(mode, "pending-clear")) {
        start(); wait_without_ui(); click("CLEAR"); pump_for(250);
        expect("Cleared; ready for another request");
        assert(!*lv_textarea_get_text(nth(&lv_textarea_class, 3)) && !find_text(content, "Request: ", false));
        assert(!http_host_allocations);
    } else if (!strcmp(mode, "pending-send")) {
        start(); wait_without_ui(); click("WORKING"); expect("HTTP 200");
        assert(!http_host_allocations && host_tasks_started == 1);
        assert(lv_obj_has_state(lv_obj_get_parent(find_text(content, "CANCEL", true)), LV_STATE_DISABLED));
        pump_for(250); expect("HTTP 200"); start(); finish(); expect("HTTP 200");
    } else if (!strcmp(mode, "tls-fail-closed")) {
        snprintf(url, sizeof(url), "https://127.0.0.1:%s/", argv[1]); input(0, url);
        click("SEND"); finish(); expect("Request failed");
        assert(host_tasks_started == 1 && host_sockets_opened == 0 && http_host_certificates >= 2);
    } else if (!strcmp(mode, "repeat")) {
        start(); finish(); expect("HTTP 200");
        /* Compare the same closed screen: result/duration text varies in size. */
        clean_app(); pump_for(30);
        DWORD before, after; lv_mem_monitor_t initial, final; lv_mem_monitor(&initial);
        GetProcessHandleCount(GetCurrentProcess(), &before);
        for (unsigned i = 0; i < 25; i++) {
            open_app(); input(0, url); start(); finish(); expect("HTTP 200"); clean_app(); pump_for(30);
        }
        lv_mem_monitor(&final); GetProcessHandleCount(GetCurrentProcess(), &after);
        printf("heap_free=%zu->%zu allocations=%lu->%lu handles=%lu->%lu ", initial.free_size, final.free_size,
               (unsigned long)initial.used_cnt, (unsigned long)final.used_cnt, before, after);
        assert(before == after && final.free_size >= initial.free_size && final.used_cnt == initial.used_cnt);
    } else {
        start();
        if (!strncmp(mode, "cancel-", 7) || !strcmp(mode, "home")) {
            pump_for(250); assert(http_tool_busy());
            int64_t cancelled = esp_timer_get_time();
            if (!strcmp(mode, "home")) {
                input(0, "http://changed.invalid/?CHANGED_QUERY_SECRET"); click("METHOD\nGET");
                clean_app(); assert(esp_timer_get_time() - cancelled < 100000); open_app();
            } else click("CANCEL");
            finish(); assert(esp_timer_get_time() - cancelled < 800000); expect("Cancelled after");
            if (!strcmp(mode, "home")) {
                char endpoint[128]; snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%s/", argv[1]);
                expect_request("GET", endpoint); expect("METHOD\nPOST");
            }
            if (!strcmp(mode, "cancel-trailers"))
                assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "OK"));
            assert(!host_active_tasks && host_tasks_started == 1); shot("http-cancelled");
        } else {
            finish();
            if (!strncmp(mode, "deadline-", 9)) {
                expect("15-second request deadline reached");
                assert(esp_timer_get_time() - began >= 15000000 && esp_timer_get_time() - began < 16000000);
                if (!strcmp(mode, "deadline-stream")) expect("drip");
                if (!strcmp(mode, "deadline-trailers"))
                    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "OK"));
            } else if (!strcmp(mode, "short") || !strcmp(mode, "short-trailers") ||
                       !strcmp(mode, "invalid-chunk") || !strcmp(mode, "eof-silent")) {
                expect("Incomplete HTTP 200 response");
                if (strcmp(mode, "short")) assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "OK"));
                if (!strcmp(mode, "eof-silent"))
                    assert(esp_timer_get_time() - began >= 10000000 && esp_timer_get_time() - began < 11000000);
            }
            else if (!strcmp(mode, "large")) { expect("preview capped"); assert(strlen(lv_textarea_get_text(nth(&lv_textarea_class, 3))) == 4096); }
            else if (!strcmp(mode, "redirect")) { expect("HTTP 302"); expect("not followed"); }
            else if (!strcmp(mode, "invalid-headers") || !strcmp(mode, "large-headers") ||
                     !strcmp(mode, "invalid-trailers") || !strcmp(mode, "large-trailers") ||
                     !strcmp(mode, "switch-protocol")) expect("Response headers/trailers invalid");
            else if (!strcmp(mode, "silent") || !strcmp(mode, "short-headers") ||
                     !strcmp(mode, "short-informational")) expect("Request failed");
            else if (!strcmp(mode, "no-content") || !strcmp(mode, "not-modified")) {
                expect(!strcmp(mode, "no-content") ? "HTTP 204 | 0 bytes" : "HTTP 304 | 0 bytes");
                assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "(empty response body)"));
            }
            else {
                assert(!strcmp(mode, "echo") || !strcmp(mode, "stream") || !strcmp(mode, "slow-headers") ||
                       !strcmp(mode, "boundary-headers") || !strcmp(mode, "fragmented") ||
                       !strcmp(mode, "informational") || !strcmp(mode, "trailers") ||
                       !strcmp(mode, "informational-coalesced") || !strcmp(mode, "fragmented-trailers") ||
                       !strcmp(mode, "boundary-trailers") || !strcmp(mode, "eof"));
                expect("HTTP 200");
                if (!strcmp(mode, "boundary-headers") || !strcmp(mode, "fragmented") ||
                    !strcmp(mode, "informational") || !strcmp(mode, "informational-coalesced") ||
                    !strcmp(mode, "trailers") || !strcmp(mode, "fragmented-trailers") ||
                    !strcmp(mode, "boundary-trailers") || !strcmp(mode, "eof"))
                    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "OK"));
                if (!strcmp(mode, "informational") || !strcmp(mode, "informational-coalesced")) {
                    expect("Type: text/plain");
                    assert(!find_text(content, "wrong/interim", false) && !find_text(content, "/wrong", false));
                }
            }
            shot("http-result");
        }
    }
    clean_app(); lv_deinit(); http_dns_test_close(); WSACleanup();
    assert(!http_host_allocations && !http_host_sdk_allocations && !host_open_sockets && !host_active_tasks);
    assert(!http_host_open_files);
    printf("%s PASS elapsed_ms=%lld tasks=%ld sockets=%ld\n", mode,
           (long long)((esp_timer_get_time() - began) / 1000), host_tasks_started, host_sockets_opened);
    return 0;
}
