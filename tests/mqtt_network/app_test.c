/* Actual MQTT console/LVGL/storage and the unmodified SDK client over TCP. */
#include "app_services.h"
#include "mqtt_client.h"
#include "mqtt_tool.h"
#include "payload_clipboard.h"
#include "byte_tool.h"
#include "src/display/lv_display_private.h"
#include "src/stdlib/builtin/lv_tlsf.h"
#include <errno.h>

static uint16_t framebuffer[720 * 1280];
static lv_obj_t *content;
static const char *mode, *output;
static unsigned qos;
static int current_cycle;
static uint32_t ticks(void) { return (uint32_t)GetTickCount64(); }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels; lv_display_flush_ready(display);
}
static void pump(void) { lv_timer_handler(); Sleep(2); }
static lv_obj_t *find_text(lv_obj_t *object, const char *text, bool exact)
{
    if (lv_obj_check_type(object, &lv_label_class) &&
        (exact ? !strcmp(lv_label_get_text(object), text) : strstr(lv_label_get_text(object), text) != NULL)) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = find_text(lv_obj_get_child(object, i), text, exact); if (found) return found;
    }
    return NULL;
}
static lv_obj_t *button(const char *text)
{
    lv_obj_t *label = find_text(content, text, true); assert(label); return lv_obj_get_parent(label);
}
static void click(const char *text)
{
    lv_obj_t *control = button(text); assert(!lv_obj_has_state(control, LV_STATE_DISABLED));
    lv_obj_send_event(control, LV_EVENT_CLICKED, NULL);
}
static lv_obj_t *find_area(lv_obj_t *object, unsigned *index)
{
    if (lv_obj_check_type(object, &lv_textarea_class) && (*index)-- == 0) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = find_area(lv_obj_get_child(object, i), index); if (found) return found;
    }
    return NULL;
}
static lv_obj_t *area(unsigned index)
{
    lv_obj_t *found = find_area(content, &index); assert(found); return found;
}
static void wait_text(const char *text)
{
    uint64_t until = GetTickCount64() + 3000;
    while (!find_text(content, text, false)) {
        if (GetTickCount64() >= until) {
            fprintf(stderr, "Missing UI text: %s; SDK SUBACKs=%ld PUBACKs=%ld held returns=%ld\n",
                    text, mqtt_net_subscribed_events, mqtt_net_published_events, mqtt_net_held_api_returns);
            for (uint32_t i = 0; i < lv_obj_get_child_count(content); ++i) {
                lv_obj_t *child = lv_obj_get_child(content, i);
                if (lv_obj_check_type(child, &lv_label_class)) fprintf(stderr, "Console label: %.192s\n", lv_label_get_text(child));
            }
        }
        assert(GetTickCount64() < until); pump();
    }
}
static void wait_action(void)
{
    uint64_t until = GetTickCount64() + 3000;
    while (lv_obj_has_state(button("PUBLISH"), LV_STATE_DISABLED)) {
        assert(GetTickCount64() < until); pump();
    }
}
static void wait_history(const char *text)
{
    uint64_t until = GetTickCount64() + 3000;
    while (!strstr(lv_textarea_get_text(area(5)), text)) {
        if (GetTickCount64() >= until) fprintf(stderr, "Missing history: %s\n", text);
        assert(GetTickCount64() < until); pump();
    }
}
static void open_app(void)
{
    mqtt_tool_show(content, true, true, mqtt_app_storage_error); lv_obj_update_layout(content);
}
static void setup(void)
{
    lv_init(); lv_tick_set_cb(ticks); lv_display_t *display = lv_display_create(720, 1280);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, framebuffer, NULL, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    content = lv_obj_create(lv_screen_active()); lv_obj_set_size(content, 720, 1180);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(content, 28, 0); lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}
static void wait_cleanup(void)
{
    uint64_t until = GetTickCount64() + 15000;
    while (mqtt_tool_busy()) { assert(GetTickCount64() < until); pump(); }
    assert(mqtt_net_threads == 1 && !mqtt_net_transports && !host_open_sockets && !mqtt_app_open_files);
}
static void close_screen(void)
{
    int64_t start = esp_timer_get_time(); mqtt_tool_stop(); mqtt_tool_stop();
    int64_t elapsed = esp_timer_get_time() - start;
    printf("HOME returned_us=%" PRId64 " busy=%d\n", elapsed, mqtt_tool_busy());
    assert(elapsed < 100000);
    lv_obj_clean(content); lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
}
static void set_qos(unsigned selected)
{
    unsigned current = 1;
    while (current != selected) {
        char label[16]; snprintf(label, sizeof(label), "QoS %u", current); click(label); current = (current + 1) % 3;
    }
}
static void phase_signal(const char *phase)
{
    char path[1100]; int length;
    if (!strncmp(mode, "app-log-", 8))
        length = snprintf(path, sizeof(path), "%s/%s-%s-%d.flag", output, mode, phase, current_cycle);
    else length = snprintf(path, sizeof(path), "%s/%s-%s.flag", output, mode, phase);
    assert(length < (int)sizeof(path));
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fputs("Actual console phase completed\n", file) >= 0 && fclose(file) == 0);
}
static void wait_counter(volatile LONG *counter, LONG value)
{
    uint64_t until = GetTickCount64() + 3000;
    while (InterlockedCompareExchange(counter, 0, 0) != value) {
        assert(GetTickCount64() < until); pump();
    }
}
static void reenter_stopping(void)
{
    close_screen(); assert(mqtt_tool_busy()); open_app();
    assert(lv_obj_has_state(button("STOPPING"), LV_STATE_DISABLED));
    assert(lv_obj_has_state(button("PUBLISH"), LV_STATE_DISABLED) && lv_obj_has_state(button("SUBSCRIBE"), LV_STATE_DISABLED));
    assert(!strlen(lv_textarea_get_text(area(2))));
}
static void check_log_disabled(void)
{
    wait_text("SD metadata log not confirmed saved:");
    lv_obj_t *log = button("SD LOG\nOFF"); assert(lv_obj_has_state(log, LV_STATE_DISABLED));
    lv_obj_send_event(log, LV_EVENT_CLICKED, NULL); /* A queued/stale tap cannot restart this failed session. */
    assert(find_text(content, "SD LOG\nOFF", true) && mqtt_app_storage_reports == 1);
}
static void storage_session(void)
{
    bool overflow = !strcmp(mode, "app-log-overflow");
    bool active_error = !strcmp(mode, "app-log-active-error");
    bool home_error = !strcmp(mode, "app-log-home-error");
    bool held_file = overflow || home_error || !strcmp(mode, "app-log-sync-home") || !strcmp(mode, "app-log-close-home");
    if (held_file) {
        wait_counter(&mqtt_app_file_waiting, 1);
        assert(mqtt_app_open_files == 1 && mqtt_tool_busy()); phase_signal("storage");
    } else if (active_error) {
        wait_counter(&mqtt_app_report_waiting, 1);
        assert(mqtt_app_storage_reports == 1 && mqtt_net_transports); phase_signal("callback");
    } else {
        wait_counter(&mqtt_app_storage_reports, 1); check_log_disabled(); phase_signal("fault");
    }
    wait_history("RX QoS 0 | 2 bytes"); /* The SDK continues receiving and acknowledging while storage waits. */
    if (overflow) {
        wait_history("FIXTURE_BURST_PAYLOAD_SECRET_19"); wait_text("Dropped 13 SD metadata entries");
    } else { wait_history("TX QoS"); wait_history("A.\n\n\t...Z"); }
    assert(mqtt_app_append_attempts == 1);
    if (mqtt_app_fault_mode() && !home_error) check_log_disabled();
    if (held_file || active_error) {
        wait_action(); lv_textarea_set_text(area(3), "cancelled/state");
        lv_textarea_set_text(area(4), "FIXTURE_CANCELLED_PAYLOAD_SECRET"); click("PUBLISH");
        assert(lv_obj_has_state(button("PUBLISH"), LV_STATE_DISABLED));
        reenter_stopping();
        uint64_t until = GetTickCount64() + 200;
        while (GetTickCount64() < until) {
            assert(mqtt_tool_busy() && mqtt_net_allocations > 1 && mqtt_app_append_attempts == 1);
            assert(held_file ? mqtt_app_file_waiting && mqtt_app_open_files == 1 : mqtt_app_report_waiting && !mqtt_app_open_files);
            pump();
        }
        printf("HOME_WAIT cancelled_action=1 file_wait=%ld report_wait=%ld busy=%d\n",
               mqtt_app_file_waiting, mqtt_app_report_waiting, mqtt_tool_busy());
        if (held_file) mqtt_app_release_file(); else mqtt_app_release_report();
        if (home_error) {
            wait_counter(&mqtt_app_report_waiting, 1);
            assert(mqtt_tool_busy() && mqtt_net_allocations == 1 && !mqtt_net_transports);
            /* Re-entry must remain disabled after client/buffer release, through error reporting. */
            reenter_stopping();
            until = GetTickCount64() + 200;
            while (GetTickCount64() < until) { assert(mqtt_tool_busy() && mqtt_app_report_waiting); pump(); }
            printf("HOME_REPORT_WAIT busy=%d allocations=%ld\n", mqtt_tool_busy(), mqtt_net_allocations);
            mqtt_app_release_report();
        }
    } else click("DISCONNECT");
    wait_cleanup();
    if (mqtt_app_fault_mode()) {
        wait_text("Disconnected; SD metadata log not confirmed saved:");
        assert(mqtt_app_storage_reports == 1 && mqtt_app_file_faults == 1 && mqtt_app_append_attempts == 1);
    } else if (overflow) wait_text("Disconnected; SD metadata omitted: 13 entries");
    close_screen();
}
static size_t lab_payload(char hex[PAYLOAD_CLIPBOARD_HEX_SIZE], uint8_t raw[128])
{
    size_t length = !strcmp(mode, "app-paste-empty") ? 0 : 128, used = 0;
    hex[0] = '\0';
    for (size_t i = 0; i < length; ++i) {
        raw[i] = (uint8_t)(i * 37U);
        used += (size_t)snprintf(hex + used, PAYLOAD_CLIPBOARD_HEX_SIZE - used, "%02X%s", (unsigned)raw[i],
                                 i + 1 == length ? "" : " ");
    }
    return length;
}
static void prepare_lab_payload(void)
{
    char hex[PAYLOAD_CLIPBOARD_HEX_SIZE]; uint8_t raw[128]; size_t length = lab_payload(hex, raw);
    LONG opened = host_sockets_opened;
    byte_tool_show(content); lv_obj_update_layout(content);
    lv_textarea_set_text(area(0), hex); click("Copy bytes");
    const payload_clipboard_t *copy = payload_clipboard_peek();
    assert(copy && copy->length == length && !memcmp(copy->bytes, raw, length));
    byte_tool_stop(); lv_obj_clean(content); lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
    assert(host_sockets_opened == opened && !mqtt_app_open_files);
    printf("BYTE_LAB_PREPARE bytes=%zu exact=1 no_network=1\n", length);
}
static void one_session(int port)
{
    bool pasted = !strncmp(mode, "app-paste-", 10);
    if (pasted) prepare_lab_payload();
    open_app();
    char uri[96]; snprintf(uri, sizeof(uri), "mqtt://127.0.0.1:%d", port);
    lv_textarea_set_text(area(0), uri);
    lv_textarea_set_text(area(1), "FIXTURE_APP_USER_SECRET");
    lv_textarea_set_text(area(2), "FIXTURE_APP_PASSWORD_SECRET");
    lv_textarea_set_text(area(3), "site/+/state");
    lv_textarea_set_text(area(4), "µFIXTURE_APP_PAYLOAD_SECRET");
    if (pasted) {
        LONG opened = host_sockets_opened;
        click("PASTE BYTES"); assert(lv_obj_has_flag(area(4), LV_OBJ_FLAG_HIDDEN));
        assert(!strcmp(lv_textarea_get_text(area(4)), "µFIXTURE_APP_PAYLOAD_SECRET"));
        assert(host_sockets_opened == opened && !mqtt_app_open_files && !mqtt_net_published_events);
        const uint8_t replacement[] = {0xFF, 0x80, 0xA5};
        assert(payload_clipboard_store(replacement, sizeof(replacement))); /* The prepared draft must survive this. */
    }
    set_qos(qos); click("RETAIN\nOFF"); click("SD LOG\nOFF");
    LONG opened = host_sockets_opened;
    click("CONNECT"); assert(!mqtt_tool_busy() && host_sockets_opened == opened);
    assert(find_text(content, "Tap CONNECT again", false));
    click("CONNECT"); assert(mqtt_tool_busy());
    if (!strcmp(mode, "app-refused")) {
        wait_text("Broker refused connection (5)");
        assert(lv_obj_has_state(button("SUBSCRIBE"), LV_STATE_DISABLED) && lv_obj_has_state(button("PUBLISH"), LV_STATE_DISABLED));
        lv_obj_send_event(button("SUBSCRIBE"), LV_EVENT_CLICKED, NULL);
        lv_obj_send_event(button("PUBLISH"), LV_EVENT_CLICKED, NULL);
        assert(!mqtt_net_published_events && !mqtt_net_subscribed_events);
        click("DISCONNECT"); wait_cleanup(); close_screen(); return;
    }
    wait_text("Connected over confirmed cleartext");
    if (!strncmp(mode, "app-early-subscribe", 19)) mqtt_net_hold_api_return(MQTT_EVENT_SUBSCRIBED);
    click("SUBSCRIBE");
    wait_action();
    wait_text(strstr(mode, "subscribe-rejected") ? "Broker rejected subscription" : "Subscription acknowledged");
    assert(!lv_obj_has_state(button("PUBLISH"), LV_STATE_DISABLED));
    lv_textarea_set_text(area(3), "site/state");
    LONG first_data = InterlockedCompareExchange(&mqtt_net_data_events, 0, 0);
    if (!strncmp(mode, "app-early-publish", 17)) mqtt_net_hold_api_return(MQTT_EVENT_PUBLISHED);
    click("PUBLISH");
    if (!strncmp(mode, "app-early-publish", 17)) {
        wait_action(); wait_text("Publish acknowledged"); phase_signal("ack");
    }
    if (!strncmp(mode, "app-log-", 8) && !current_cycle) { storage_session(); return; }
    if (pasted) {
        char hex[PAYLOAD_CLIPBOARD_HEX_SIZE], text[80]; uint8_t raw[128]; size_t length = lab_payload(hex, raw);
        snprintf(text, sizeof(text), "Latest complete RX: %u bytes", (unsigned)length); wait_text(text); wait_action();
        click("COPY RX"); const payload_clipboard_t *copy = payload_clipboard_peek();
        assert(copy && copy->length == length && !memcmp(copy->bytes, raw, length));
        phase_signal("copy"); wait_history("RX QoS 0 | 2 bytes"); wait_history("ASCII preview");
        click("DISCONNECT"); wait_cleanup(); close_screen();
        byte_tool_show(content); lv_obj_update_layout(content); click("Paste hex");
        assert(!strcmp(lv_textarea_get_text(area(0)), hex));
        snprintf(text, sizeof(text), "%u bytes | SUM8:", (unsigned)length); wait_text(text);
        byte_tool_stop(); lv_obj_clean(content); lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
        printf("BYTE_LAB_ROUNDTRIP bytes=%zu exact=1 qos=%u\n", length, qos); return;
    }
    if (!strncmp(mode, "app-copy-", 9)) {
        uint8_t raw[128]; size_t length;
        const uint8_t binary[] = {'A', 0, '\r', '\n', '\t', 0x7F, 0xFF, 0x80, 'Z'};
        if (!strcmp(mode, "app-copy-fragments")) {
            for (unsigned i = 0; i < sizeof(raw); ++i) raw[i] = (uint8_t)(i * 37U);
            length = sizeof(raw);
        } else { length = !strcmp(mode, "app-copy-empty") ? 0 : sizeof(binary); memcpy(raw, binary, sizeof(binary)); }
        char text[80]; snprintf(text, sizeof(text), "Latest complete RX: %u bytes", (unsigned)length); wait_text(text);
        if (!strcmp(mode, "app-copy-fragments")) {
            assert(mqtt_net_data_events >= first_data + 9);
            mqtt_net_data_record_t first = mqtt_net_data_records[first_data];
            assert(first.offset == 0 && first.length == 0 && first.total == 128 && first.topic_length == 1017);
            assert(first.qos == 2 && first.retain && first.dup);
            int received = 0;
            for (LONG i = first_data + 1; i < mqtt_net_data_events; ++i) {
                mqtt_net_data_record_t next = mqtt_net_data_records[i];
                assert(next.offset == received && next.length > 0 && next.length <= 17);
                assert(next.total == 128 && next.topic_length == 0 && next.qos == 2 && next.retain && next.dup);
                received += next.length;
            }
            assert(received == 128);
            wait_text("topic preview");
        }
        wait_action(); LONG before_data = mqtt_net_data_events, published = mqtt_net_published_events;
        click("COPY RX"); const payload_clipboard_t *copy = payload_clipboard_peek();
        assert(copy && copy->length == length && !memcmp(copy->bytes, raw, length));
        assert(mqtt_net_data_events == before_data && mqtt_net_published_events == published);
        wait_text("Copied to the byte clipboard"); phase_signal("copy");
        printf("COPY_RX bytes=%zu exact=1 no_sdk_action=1\n", length);
        if (!strcmp(mode, "app-copy-oversize")) {
            wait_text("Latest complete RX: 129 bytes");
            assert(lv_obj_has_state(button("COPY RX"), LV_STATE_DISABLED));
            lv_obj_send_event(button("COPY RX"), LV_EVENT_CLICKED, NULL);
            assert(copy->length == length && !memcmp(copy->bytes, raw, length));
            phase_signal("oversize"); printf("COPY_RX over_128_rejected=1 previous_clipboard_preserved=1\n");
        }
        wait_history("RX QoS 0 | 2 bytes"); click("DISCONNECT"); wait_cleanup(); close_screen(); return;
    }
    if (!strcmp(mode, "app-home") || !strcmp(mode, "app-abandoned")) {
        uint64_t until = GetTickCount64() + 3000;
        while (InterlockedCompareExchange(&mqtt_net_data_events, 0, 0) == first_data) {
            assert(GetTickCount64() < until); pump();
        }
        assert(mqtt_net_data_records[first_data].offset == 0 && mqtt_net_data_records[first_data].total == 2048);
        assert(!strstr(lv_textarea_get_text(area(5)), "RX QoS"));
        reenter_stopping();
        phase_signal("home");
        wait_cleanup(); close_screen(); return;
    }
    wait_history("RX QoS 0 | 2 bytes"); /* Broker marker follows all acknowledgment exchanges. */
    wait_history("TX QoS");
    if (!strcmp(mode, "app-zero-start")) {
        assert(mqtt_net_data_records[first_data].length == 0 && mqtt_net_data_records[first_data].topic_length == 1017);
        assert(mqtt_net_data_records[first_data + 1].offset == 0 && mqtt_net_data_records[first_data + 1].topic_length == 0);
        wait_history("kkkkkkkkkkkkkkkkk"); wait_history("topic preview");
    } else if (!strcmp(mode, "app-fragments")) {
        assert(mqtt_net_data_records[first_data].total == 1024 && mqtt_net_data_records[first_data + 1].offset > 0);
        wait_history("preview capped"); wait_history("1024 bytes");
    } else if (!strcmp(mode, "app-qos0")) {
        wait_history("RX QoS 0 | retained | 0 bytes"); assert(!mqtt_net_published_events);
    } else {
        wait_history("A.\n\n\t...Z"); wait_history("9 bytes");
    }
    click("DISCONNECT"); wait_cleanup(); close_screen();
}

int main(int argc, char **argv)
{
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX); setvbuf(stdout, NULL, _IONBF, 0);
    assert(argc == 4); int port = atoi(argv[1]); mode = argv[2]; output = argv[3];
    assert(port > 0 && port <= 65535); mqtt_net_ui_thread = GetCurrentThreadId();
    qos = !strcmp(mode, "app-qos0") || !strcmp(mode, "app-paste-qos0") ? 0 : strstr(mode, "qos2") ? 2 : 1;
    if (!strcmp(mode, "app-fragments") || !strcmp(mode, "app-copy-fragments")) mqtt_net_read_cap = 17;
    mqtt_app_storage_setup(output, mode);
    WSADATA sockets; assert(WSAStartup(MAKEWORD(2, 2), &sockets) == 0);
    mqtt_tool_self_test(); setup();
    int cycles = !strcmp(mode, "app-soak") ? 25 : mqtt_app_fault_mode() ? 2 : 1;
    lv_mem_monitor_t baseline, after; DWORD handles = 0, last = 0;
    size_t baseline_free = 0, free_with_padding = 0, raw_min = SIZE_MAX, raw_max = 0;
    uint32_t event_count = 0, event_capacity = 0;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        current_cycle = cycle;
        one_session(port); assert(!mqtt_tool_busy() && mqtt_net_allocations == 1 && mqtt_net_threads == 1);
        if (mqtt_app_fault_mode()) {
            assert(mqtt_app_file_faults == 1 && mqtt_app_storage_reports == 1);
            assert(mqtt_app_append_attempts == (cycle ? 4 : 1));
        }
        lv_mem_monitor(&after); assert(GetProcessHandleCount(GetCurrentProcess(), &last));
        /* The pinned renderer resizes this array as animations register/remove
         * display callbacks. TLSF keeps 8..32 bytes of padding in its 16-byte
         * request. Account for only that measured padding, not a heap tolerance. */
        lv_array_t *events = &lv_display_get_default()->event_list.array;
        size_t requested = (size_t)events->capacity * events->element_size;
        size_t allocated = lv_tlsf_block_size(events->data);
        assert(requested == 16 && allocated >= requested && allocated - requested <= 32);
        size_t padding = allocated - requested;
        free_with_padding = after.free_size + padding;
        if (after.free_size < raw_min) raw_min = after.free_size;
        if (after.free_size > raw_max) raw_max = after.free_size;
        if (!cycle) {
            baseline = after; handles = last; baseline_free = free_with_padding;
            event_count = events->size; event_capacity = events->capacity;
        }
        if (free_with_padding != baseline_free || after.used_cnt != baseline.used_cnt || handles != last)
            printf("APP_MEMORY cycle=%d free_plus_display_padding=%zu->%zu used=%zu->%zu handles=%lu->%lu\n",
                   cycle, baseline_free, free_with_padding, baseline.used_cnt, after.used_cnt, handles, last);
        assert(free_with_padding == baseline_free && after.used_cnt == baseline.used_cnt && handles == last);
        assert(events->size == event_count && events->capacity == event_capacity && lv_mem_test() == LV_RESULT_OK);
        printf("APP_SESSION cycle=%d allocations=%ld files=%ld sockets=%ld handles=%lu free=%zu raw_free=%zu display_padding=%zu used=%zu events=%u\n",
               cycle, mqtt_net_allocations, mqtt_app_open_files, host_open_sockets, last, free_with_padding,
               after.free_size, padding, after.used_cnt, events->size);
    }
    lv_deinit(); mqtt_net_shutdown_worker(); mqtt_app_storage_shutdown();
    assert(mqtt_net_held_api_returns == (!strncmp(mode, "app-early-", 10) ? 1 : 0));
    assert(!mqtt_net_allocations && !mqtt_net_transports && !mqtt_net_threads && !host_open_sockets && !mqtt_app_open_files);
    assert(WSACleanup() == 0);
    printf("PASS %s cycles=%d LVGL_free_plus_display_padding=%zu->%zu raw_free_range=%zu..%zu handles=%lu->%lu allocations=0 files=0 sockets=0 tasks=0\n",
           mode, cycles, baseline_free, free_with_padding, raw_min, raw_max, handles, last);
    return 0;
}
