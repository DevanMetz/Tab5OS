/* Actual Diagnostics app/resolver/LVGL; synthetic DNS, ping and mDNS services.
 * Raw ping's actual socket loop is checked separately by network_ping_host. */
#include "host.h"
#include "dns_adapter.h"
#include "network_tool.h"
#include "network_ping.h"
#include "network_resolver.h"
#include "mdns.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A failed native assertion must exit into the runner, never open a CRT dialog. */
#undef assert
#define assert(condition) do { if (!(condition)) { \
    fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
    fflush(stderr); ExitProcess(3); } } while (0)

static uint16_t framebuffer[720 * 1280];
static lv_obj_t *content;
static const char *mode, *output;
static volatile LONG ping_calls, mdns_calls, mdns_inits, mdns_rows, mdns_freeing;
static bool mdns_hold, mdns_hold_free, ping_hold;
static HANDLE mdns_release, mdns_free_release;
static ip_addr_t ping_address;

esp_err_t mdns_init(void)
{
    InterlockedIncrement(&mdns_inits);
    return !strcmp(mode, "mdns-init-failure") ? ESP_ERR_NO_MEM : ESP_OK;
}
esp_err_t mdns_query_ptr(const char *service, const char *proto, uint32_t timeout,
                         size_t limit, mdns_result_t **results)
{
    assert(!strcmp(service, "_services._dns-sd") && !strcmp(proto, "_udp"));
    assert(timeout == 3000 && limit == 8);
    InterlockedIncrement(&mdns_calls);
    if (mdns_hold) assert(WaitForSingleObject(mdns_release, 5000) == WAIT_OBJECT_0);
    else Sleep(20);
    *results = NULL;
    if (strcmp(mode, "mdns-empty")) {
        for (unsigned i = 0; i < 3; i++) {
            mdns_result_t *row = calloc(1, sizeof(*row)); assert(row);
            row->service_type = i == 2 ? "_ipp" : "_http"; row->proto = "_tcp";
            row->next = *results; *results = row; InterlockedIncrement(&mdns_rows);
        }
    }
    return !strcmp(mode, "mdns-failure") ? ESP_ERR_TIMEOUT : ESP_OK;
}
void mdns_query_results_free(mdns_result_t *results)
{
    if (mdns_hold_free && results) {
        InterlockedIncrement(&mdns_freeing);
        assert(WaitForSingleObject(mdns_free_release, 5000) == WAIT_OBJECT_0);
    }
    while (results) {
        mdns_result_t *next = results->next;
        free(results); results = next; InterlockedDecrement(&mdns_rows);
    }
}
int network_ping_target(const ip_addr_t *address, uint32_t *sent, uint32_t *received,
                        uint32_t *total, network_ping_cancel_cb_t cancelled, void *context)
{
    assert(cancelled); ping_address = *address; InterlockedIncrement(&ping_calls);
    *sent = *received = *total = 0;
    if (ping_hold) {
        int64_t until = esp_timer_get_time() + 3000000;
        while (!cancelled(context)) { assert(esp_timer_get_time() < until); Sleep(10); }
        *sent = 1; return ECANCELED;
    }
    if (!strcmp(mode, "ping-failure")) return EACCES;
    *sent = 4; *received = 3; *total = 21; return 0;
}

static uint32_t ticks(void) { return (uint32_t)GetTickCount64(); }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }
static void pump(void) { lv_timer_handler(); Sleep(2); }
static void pump_for(unsigned ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < until) pump();
}
static lv_obj_t *find_text(lv_obj_t *object, const char *text, bool exact)
{
    if (lv_obj_check_type(object, &lv_label_class) &&
        (exact ? !strcmp(lv_label_get_text(object), text) : strstr(lv_label_get_text(object), text) != NULL)) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find_text(lv_obj_get_child(object, i), text, exact); if (found) return found;
    }
    return NULL;
}
static lv_obj_t *find_type(lv_obj_t *object, const lv_obj_class_t *type)
{
    if (lv_obj_check_type(object, type)) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find_type(lv_obj_get_child(object, i), type); if (found) return found;
    }
    return NULL;
}
static void dump(lv_obj_t *object)
{
    if (lv_obj_check_type(object, &lv_label_class)) fprintf(stderr, "[%s]\n", lv_label_get_text(object));
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) dump(lv_obj_get_child(object, i));
}
static void expect(const char *text)
{ if (!find_text(content, text, false)) { dump(content); fprintf(stderr, "Missing: %s\n", text); assert(0); } }
static lv_obj_t *button(const char *text)
{ lv_obj_t *label = find_text(content, text, true); assert(label); return lv_obj_get_parent(label); }
static void click(const char *text)
{ lv_obj_t *control = button(text); assert(!lv_obj_has_state(control, LV_STATE_DISABLED)); lv_obj_send_event(control, LV_EVENT_CLICKED, NULL); }
static void forced_click(const char *text) { lv_obj_send_event(button(text), LV_EVENT_CLICKED, NULL); }
static void input(const char *text)
{ lv_obj_t *area = find_type(content, &lv_textarea_class); assert(area); lv_textarea_set_text(area, text); }
static void wait_count(volatile LONG *counter, LONG count)
{
    int64_t until = esp_timer_get_time() + 1000000;
    while (*counter < count) { assert(esp_timer_get_time() < until); pump(); }
}
static void finish(bool with_ui)
{
    int64_t until = esp_timer_get_time() + 11000000;
    while (network_tool_busy() || host_active_tasks) {
        assert(esp_timer_get_time() < until); if (with_ui) pump(); else Sleep(2);
    }
    if (with_ui) {
        pump_for(220); assert(lv_obj_has_state(button("CANCEL"), LV_STATE_DISABLED));
    }
    assert(!mdns_rows && !host_open_sockets);
}
static void close_app(void)
{
    int64_t began = esp_timer_get_time(); network_tool_stop();
    assert(esp_timer_get_time() - began < 100000);
    lv_obj_clean(content); lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
}
static void open_app(void) { network_tool_show(content, host_online != 0); pump_for(20); }
static void snapshot(const char *name)
{
    lv_obj_update_layout(content); lv_refr_now(NULL);
    char path[1024]; snprintf(path, sizeof(path), "%s/%s-dns-%d.ppm", output, name, DNS_MAX_HOST_IP);
    FILE *file = fopen(path, "wb"); assert(file); fprintf(file, "P6\n720 1280\n255\n");
    for (size_t i = 0; i < 720U * 1280; i++) {
        uint16_t value = framebuffer[i];
        uint8_t rgb[] = {(uint8_t)(((value >> 11) & 31) * 255 / 31),
                         (uint8_t)(((value >> 5) & 63) * 255 / 63), (uint8_t)((value & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, file);
    }
    assert(fclose(file) == 0);
}
static void setup(void)
{
    lv_init(); lv_tick_set_cb(ticks); lv_display_t *display = lv_display_create(720, 1280);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, framebuffer, NULL, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x10141f), 0);
    lv_obj_set_style_text_color(lv_screen_active(), lv_color_white(), 0);
    content = lv_obj_create(lv_screen_active()); lv_obj_set_size(content, 720, 1180);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x10141f), 0); lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_text_color(content, lv_color_white(), 0);
    lv_obj_set_style_pad_all(content, 28, 0); lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    open_app();
    lv_obj_update_layout(content);
    const char *controls[] = {"LOOK UP", "PING x4", "mDNS", "REFRESH", "CANCEL"};
    lv_obj_t *row = lv_obj_get_parent(button(controls[0])); lv_area_t bounds; lv_obj_get_content_coords(row, &bounds);
    for (unsigned i = 0; i < 5; i++) {
        lv_area_t area; lv_obj_get_coords(button(controls[i]), &area);
        if (area.x1 < bounds.x1 || area.x2 > bounds.x2 || area.y1 < bounds.y1 || area.y2 > bounds.y2)
            fprintf(stderr, "%s: button=(%ld,%ld)-(%ld,%ld), row content=(%ld,%ld)-(%ld,%ld)\n", controls[i],
                    (long)area.x1, (long)area.y1, (long)area.x2, (long)area.y2,
                    (long)bounds.x1, (long)bounds.y1, (long)bounds.x2, (long)bounds.y2);
        assert(area.x1 >= bounds.x1 && area.x2 <= bounds.x2 && area.y1 >= bounds.y1 && area.y2 <= bounds.y2);
    }
}

typedef struct {
    const char *name;
    int error;
    size_t count;
    ip_addr_t addresses[4];
    volatile LONG done, cancel;
    int64_t deadline;
} resolver_job_t;
static bool job_cancelled(void *context)
{ return InterlockedCompareExchange(&((resolver_job_t *)context)->cancel, 0, 0) != 0; }
static void resolve_job(void *argument)
{
    resolver_job_t *job = argument;
    job->error = network_resolve_host(job->name, job->addresses, 4, &job->count, job->deadline, job_cancelled, job);
    InterlockedExchange(&job->done, 1);
}
static void start_job(resolver_job_t *job, const char *name, int64_t budget)
{
    memset(job, 0, sizeof(*job)); job->name = name; job->deadline = esp_timer_get_time() + budget;
    assert(host_task_create(resolve_job, "resolve", 0, job, 0, NULL));
}
static void await_job(resolver_job_t *job)
{
    int64_t until = esp_timer_get_time() + 1000000;
    while (!InterlockedCompareExchange(&job->done, 0, 0)) { assert(esp_timer_get_time() < until); pump(); }
}
static void resolver_case(void)
{
    if (!strcmp(mode, "resolver-concurrent")) {
        resolver_job_t first, second; start_job(&first, "hold.fixture.test", 3000000);
        wait_count(&http_host_dns_requests, 1); start_job(&second, "next.fixture.test", 3000000);
        wait_count(&http_host_dns_requests, 2);
        ip_addr_t address; size_t count; int64_t deadline = esp_timer_get_time() + 1000000;
        assert(network_resolve_host("cache.fixture.test", &address, 1, &count, deadline, NULL, NULL) == EBUSY && !count);
        assert(http_host_dns_posts == 2);
        assert(network_resolve_host("192.0.2.1", &address, 1, &count, deadline, NULL, NULL) == 0 && count == 1);
        http_dns_test_release_oldest(true); await_job(&first); assert(!first.error && first.count == 1);
        assert(!InterlockedCompareExchange(&second.done, 0, 0));
        http_dns_test_release_oldest(false); await_job(&second); assert(second.error == EHOSTUNREACH && !second.count);
    } else if (!strcmp(mode, "resolver-short-deadline")) {
        resolver_job_t job; int64_t began = esp_timer_get_time(); start_job(&job, "hold.fixture.test", 120000);
        wait_count(&http_host_dns_requests, 1); await_job(&job);
        assert(job.error == ETIMEDOUT && !job.count && esp_timer_get_time() - began < 300000);
        http_dns_test_release_oldest(true); wait_count(&http_host_dns_callbacks, 1);
        input("cache.fixture.test"); click("LOOK UP"); finish(true); expect("127.0.0.1");
    } else {
        assert(!strcmp(mode, "resolver-invalid"));
        ip_addr_t address; size_t count = 99; int64_t deadline = esp_timer_get_time() + 1000000;
        assert(network_resolve_host(NULL, &address, 1, &count, deadline, NULL, NULL) == EINVAL && !count);
        assert(network_resolve_host("", &address, 1, &count, deadline, NULL, NULL) == EINVAL);
        assert(network_resolve_host("cache.fixture.test", &address, 0, &count, deadline, NULL, NULL) == EINVAL);
        assert(network_resolve_host("cache.fixture.test", &address, 5, &count, deadline, NULL, NULL) == EINVAL);
        char long_host[257]; memset(long_host, 'a', 256); long_host[256] = 0;
        assert(network_resolve_host(long_host, &address, 1, &count, deadline, NULL, NULL) == EINVAL);
        assert(network_resolve_host("192.0.2.1", &address, 1, &count, esp_timer_get_time(), NULL, NULL) == ETIMEDOUT);
        resolver_job_t job = {.cancel = 1};
        assert(network_resolve_host("192.0.2.1", &address, 1, &count, deadline, job_cancelled, &job) == ECANCELED);
        assert(!http_host_dns_posts);
        struct { ip_addr_t address; unsigned char guard[64]; } one;
        memset(&one, 0xa5, sizeof(one));
        assert(network_resolve_host("multi.fixture.test", &one.address, 1, &count, deadline, NULL, NULL) == 0 && count == 1);
        for (unsigned i = 0; i < sizeof(one.guard); i++) assert(one.guard[i] == 0xa5);
    }
    finish(true);
}

static void run_case(void)
{
    if (!strncmp(mode, "resolver-", 9)) { resolver_case(); return; }
    if (!strcmp(mode, "input")) {
        const char *bad[] = {"", "https://example.com", "bad host", "bad/path", "bad\\path", "bad?query", "bad#fragment", "bad@name", "\xc3\xa9"};
        for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) { input(bad[i]); click("LOOK UP"); expect("127 ASCII bytes"); }
        char long_host[260]; memset(long_host, 'a', sizeof(long_host) - 1); long_host[sizeof(long_host) - 1] = 0;
        input(long_host); click("LOOK UP"); expect("127 ASCII bytes");
        assert(strlen(lv_textarea_get_text(find_type(content, &lv_textarea_class))) == 128);
        long_host[0] = ' '; input(long_host); click("PING x4"); expect("127 ASCII bytes");
        assert(!host_tasks_started && !http_host_dns_posts && !ping_calls);
        input(" 192.0.2.1 "); click("LOOK UP"); finish(true); expect("192.0.2.1"); return;
    }
    if (!strcmp(mode, "offline-refresh")) {
        host_online = 0; click("REFRESH"); expect("not connected");
        assert(lv_obj_has_state(button("LOOK UP"), LV_STATE_DISABLED)); forced_click("LOOK UP"); expect("Connect to Wi-Fi first");
        assert(!host_tasks_started); host_online = 1; click("REFRESH"); expect("fixture LAN");
        input("192.0.2.1"); click("LOOK UP"); finish(true); expect("192.0.2.1"); return;
    }
    if (!strcmp(mode, "task-failure")) {
        host_fail_next_task = 1; click("LOOK UP"); expect("Could not start network check");
        assert(!network_tool_busy() && !host_active_tasks && !http_host_dns_posts);
        input("192.0.2.1"); click("LOOK UP"); finish(true); expect("192.0.2.1"); return;
    }
    if (!strcmp(mode, "pending-start")) {
        input("cache.fixture.test"); click("LOOK UP"); finish(false);
        LONG started = host_tasks_started; input("192.0.2.9"); forced_click("PING x4");
        expect("cache.fixture.test"); assert(host_tasks_started == started && !ping_calls);
        click("PING x4"); finish(true); expect("192.0.2.9"); assert(ping_calls == 1); return;
    }
    if (!strcmp(mode, "lifecycle")) {
        close_app(); pump_for(30); lv_mem_monitor_t before, after; lv_mem_monitor(&before);
        DWORD initial, final; assert(GetProcessHandleCount(GetCurrentProcess(), &initial));
        for (unsigned i = 0; i < 25; i++) {
            open_app(); input("192.0.2.1"); click("LOOK UP"); finish(false);
            close_app(); pump_for(30); assert(!host_active_tasks && !network_tool_busy());
        }
        lv_mem_monitor(&after); assert(GetProcessHandleCount(GetCurrentProcess(), &final));
        assert(initial == final && before.free_size == after.free_size && before.used_cnt == after.used_cnt);
        printf("heap=%zu->%zu allocations=%lu->%lu handles=%lu->%lu ", before.free_size, after.free_size,
               (unsigned long)before.used_cnt, (unsigned long)after.used_cnt, initial, final);
        open_app(); return;
    }
    if (!strncmp(mode, "mdns-", 5)) {
        mdns_hold = !strcmp(mode, "mdns-cancel") || !strcmp(mode, "mdns-home");
        mdns_hold_free = !strcmp(mode, "mdns-cleanup"); click("mDNS");
        if (mdns_hold || mdns_hold_free) {
            wait_count(mdns_hold ? &mdns_calls : &mdns_freeing, 1); assert(network_tool_busy());
            if (!strcmp(mode, "mdns-cancel")) { click("CANCEL"); expect("3-second discovery"); snapshot("network-cancelling"); }
            else { close_app(); open_app(); expect("Cancelling the previous"); }
            assert(network_tool_busy() && host_active_tasks && lv_obj_has_state(button("LOOK UP"), LV_STATE_DISABLED));
            forced_click("PING x4"); assert(!ping_calls); SetEvent(mdns_hold ? mdns_release : mdns_free_release);
            finish(true); expect("Cancelled\nmDNS discovery");
        } else {
            finish(true);
            if (!strcmp(mode, "mdns-empty")) expect("No mDNS services answered");
            else if (!strcmp(mode, "mdns-failure") || !strcmp(mode, "mdns-init-failure")) expect("mDNS discovery failed");
            else { assert(!strcmp(mode, "mdns-result")); expect("_ipp._tcp\n_http._tcp"); }
        }
        assert(!http_host_dns_posts && !mdns_rows); return;
    }
    if (!strncmp(mode, "ping-", 5) || !strcmp(mode, "home-ping")) {
        ping_hold = !strcmp(mode, "ping-cancel") || !strcmp(mode, "home-ping");
        input("v6.fixture.test"); click("PING x4");
        if (ping_hold) {
            wait_count(&ping_calls, 1); int64_t began = esp_timer_get_time(); input("changed.fixture.test");
            if (!strcmp(mode, "home-ping")) { close_app(); open_app(); }
            else click("CANCEL");
            finish(true); assert(esp_timer_get_time() - began < 500000); expect("Cancelled\nv6.fixture.test");
        } else {
            finish(true); expect("v6.fixture.test"); expect("::1");
            if (!strcmp(mode, "ping-failure")) expect("Ping failed");
            else { assert(!strcmp(mode, "ping-result")); expect("Sent 4  Received 3  Loss 25%"); expect("Average reply 7 ms"); snapshot("network-ping"); }
        }
        assert(ping_address.family == AF_INET6 && ping_calls == 1); return;
    }
    if (!strcmp(mode, "cancel-dns") || !strcmp(mode, "home-dns") || !strcmp(mode, "cancel-queued") || !strcmp(mode, "late-dns")) {
        input("hold.fixture.test"); click("LOOK UP");
        wait_count(!strcmp(mode, "cancel-queued") ? &http_host_dns_posts : &http_host_dns_requests, 1);
        int64_t began = esp_timer_get_time(); input("changed.fixture.test");
        if (!strcmp(mode, "home-dns")) { close_app(); open_app(); } else click("CANCEL");
        finish(true); assert(esp_timer_get_time() - began < 500000); expect("Cancelled\nhold.fixture.test");
        if (!strcmp(mode, "cancel-queued")) { http_dns_test_release_posts(); pump_for(40); assert(!http_host_dns_requests); }
        else if (!strcmp(mode, "late-dns")) {
            input("next.fixture.test"); click("LOOK UP"); wait_count(&http_host_dns_requests, 2);
            http_dns_test_release_oldest(true); wait_count(&http_host_dns_callbacks, 1);
            assert(network_tool_busy()); expect("Resolving..."); http_dns_test_release_oldest(false);
            finish(true); expect("next.fixture.test"); expect("DNS lookup failed"); return;
        } else { http_dns_test_release_oldest(true); wait_count(&http_host_dns_callbacks, 1); }
        expect("Cancelled\nhold.fixture.test"); input("cache.fixture.test"); click("LOOK UP");
        if (!strcmp(mode, "cancel-queued")) { wait_count(&http_host_dns_posts, 2); http_dns_test_release_posts(); }
        finish(true); expect("127.0.0.1"); return;
    }
    const char *host = !strcmp(mode, "lookup-delayed") || !strcmp(mode, "source-edits") ? "slow.fixture.test" :
                       !strcmp(mode, "lookup-ipv6") ? "v6.fixture.test" : !strcmp(mode, "lookup-literal") ? "2001:db8::1" :
                       !strcmp(mode, "lookup-multiple") ? "multi.fixture.test" : !strcmp(mode, "lookup-multiple-delayed") ? "multi-slow.fixture.test" :
                       !strcmp(mode, "lookup-failure") ? "fail.fixture.test" : !strcmp(mode, "lookup-memory") ? "memory.fixture.test" :
                       !strcmp(mode, "lookup-timeout") ? "hold.fixture.test" : "cache.fixture.test";
    input(host); int64_t began = esp_timer_get_time(); click("LOOK UP");
    if (!strcmp(mode, "source-edits")) input("changed.fixture.test");
    finish(true); expect(host);
    if (!strcmp(mode, "lookup-timeout")) {
        expect("DNS lookup timed out (10 seconds)"); assert(esp_timer_get_time() - began >= 10000000 && esp_timer_get_time() - began < 10500000);
        http_dns_test_release_oldest(true); wait_count(&http_host_dns_callbacks, 1);
    } else if (!strcmp(mode, "lookup-failure") || !strcmp(mode, "lookup-memory") || !strcmp(mode, "lookup-post-failure")) expect("DNS lookup failed");
    else if (!strcmp(mode, "lookup-ipv6")) expect("::1");
    else if (!strcmp(mode, "lookup-literal")) { expect("2001:db8::1"); assert(!http_host_dns_posts); }
    else {
        expect("127.0.0.1");
        if (!strncmp(mode, "lookup-multiple", 15)) {
            for (unsigned i = 2; i <= DNS_MAX_HOST_IP; i++) { char text[32]; snprintf(text, sizeof(text), "127.0.0.%u", i); expect(text); }
            snapshot("network-lookup");
        }
    }
}

int main(int argc, char **argv)
{
    _set_error_mode(_OUT_TO_STDERR);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    assert(argc == 3); mode = argv[1]; output = argv[2]; WSADATA wsa; assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    mdns_release = CreateEvent(NULL, TRUE, FALSE, NULL); mdns_free_release = CreateEvent(NULL, TRUE, FALSE, NULL);
    assert(mdns_release && mdns_free_release);
    fprintf(stderr, "Diagnostics fixture: initializing DNS service\n"); fflush(stderr);
    http_dns_test_init(!strcmp(mode, "cancel-queued") ? "dns-queued-cancel" :
                       !strcmp(mode, "lookup-post-failure") ? "dns-post-failure" : mode);
    fprintf(stderr, "Diagnostics fixture: checking app helpers\n"); fflush(stderr);
    network_tool_self_test();
    fprintf(stderr, "Diagnostics fixture: creating LVGL screen\n"); fflush(stderr);
    setup();
    fprintf(stderr, "Diagnostics fixture: running %s\n", mode); fflush(stderr);
    int64_t began = esp_timer_get_time(); run_case();
    close_app(); finish(false); lv_deinit(); http_dns_test_close();
    CloseHandle(mdns_release); CloseHandle(mdns_free_release); WSACleanup();
    assert(!host_active_tasks && !host_open_sockets && !mdns_rows);
    printf("%s PASS dns_capacity=%d elapsed_ms=%lld tasks=%ld\n", mode, DNS_MAX_HOST_IP,
           (long long)((esp_timer_get_time() - began) / 1000), host_tasks_started);
    return 0;
}
