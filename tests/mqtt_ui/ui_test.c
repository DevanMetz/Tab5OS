/* Actual MQTT app, LVGL and CSV helper with controlled SDK/file service stalls. */
#include "adapter.h"
#include "mqtt_tool.h"
#include "mqtt_msg.h"
#include "payload_clipboard.h"
#include "byte_tool.h"
#include "src/display/lv_display_private.h"
#include "src/stdlib/builtin/lv_tlsf.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef assert
#define assert(condition) do { if (!(condition)) { \
    fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
    fflush(stderr); ExitProcess(3); } } while (0)

static uint16_t framebuffer[720 * 1280];
static lv_obj_t *content;
static const char *mode, *output;
static volatile LONG reported_errors, reporting;
static bool hold_report;
static HANDLE report_release;
static DWORD ui_thread;
static uint32_t tick_offset;
static void storage_error(int error)
{
    assert(error == EIO && mqtt_tool_busy() && GetCurrentThreadId() != ui_thread);
    InterlockedIncrement(&reporting);
    if (hold_report) assert(WaitForSingleObject(report_release, 5000) == WAIT_OBJECT_0);
    InterlockedIncrement(&reported_errors);
}
static uint32_t ticks(void) { return (uint32_t)GetTickCount64() + tick_offset; }
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
static void expect(const char *text)
{
    if (!find_text(content, text, false)) { fprintf(stderr, "Missing expected text: %s\n", text); assert(0); }
}
static void wait_text(const char *text)
{
    int64_t until = esp_timer_get_time() + 1000000;
    while (!find_text(content, text, false)) {
        if (esp_timer_get_time() >= until) fprintf(stderr, "Timed out waiting for: %s\n", text);
        assert(esp_timer_get_time() < until); pump();
    }
}
static lv_obj_t *button(const char *text)
{ lv_obj_t *label = find_text(content, text, true); assert(label); return lv_obj_get_parent(label); }
static void click(const char *text)
{ lv_obj_t *control = button(text); assert(!lv_obj_has_state(control, LV_STATE_DISABLED)); lv_obj_send_event(control, LV_EVENT_CLICKED, NULL); }
static void forced_click(const char *text) { lv_obj_send_event(button(text), LV_EVENT_CLICKED, NULL); }
static lv_obj_t *find_area(lv_obj_t *object, unsigned *index)
{
    if (lv_obj_check_type(object, &lv_textarea_class) && (*index)-- == 0) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find_area(lv_obj_get_child(object, i), index); if (found) return found;
    }
    return NULL;
}
static lv_obj_t *area(unsigned index) { lv_obj_t *found = find_area(content, &index); assert(found); return found; }
static void wait_count(volatile LONG *counter, LONG count, bool with_ui)
{
    int64_t until = esp_timer_get_time() + 1000000;
    while (*counter < count) { assert(esp_timer_get_time() < until); if (with_ui) pump(); else Sleep(2); }
}
static void finish(bool with_ui)
{
    int64_t until = esp_timer_get_time() + 2000000;
    while (mqtt_tool_busy()) { assert(esp_timer_get_time() < until); if (with_ui) pump(); else Sleep(2); }
    assert(!mqtt_host_clients && !mqtt_host_open_files);
    if (with_ui) {
        until = esp_timer_get_time() + 1000000;
        lv_obj_t *connect_label;
        while (!(connect_label = find_text(content, "CONNECT", true)) ||
               lv_obj_has_state(lv_obj_get_parent(connect_label), LV_STATE_DISABLED)) {
            assert(esp_timer_get_time() < until); pump();
        }
    }
}
static void close_app(void)
{
    int64_t began = esp_timer_get_time(); mqtt_tool_stop(); mqtt_tool_stop();
    assert(esp_timer_get_time() - began < 100000);
    lv_obj_clean(content); lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
}
static void open_app(void)
{ mqtt_tool_show(content, true, true, storage_error); lv_obj_update_layout(content); }
static void connect_session(void)
{
    lv_textarea_set_text(area(0), "mqtts://fixture.test:8883");
    lv_textarea_set_text(area(1), "FIXTURE_USER_SECRET");
    lv_textarea_set_text(area(2), "FIXTURE_PASSWORD_SECRET");
    lv_textarea_set_text(area(3), "fixture/topic");
    lv_textarea_set_text(area(4), "FIXTURE_PAYLOAD_SECRET");
    click("CONNECT");
    if (strstr(mode, "-failure")) return;
    assert(mqtt_tool_busy() && mqtt_host_clients == 1);
    mqtt_host_event(MQTT_EVENT_CONNECTED);
    int64_t until = esp_timer_get_time() + 1000000;
    while (lv_obj_has_state(button("PUBLISH"), LV_STATE_DISABLED)) {
        assert(esp_timer_get_time() < until); pump();
    }
}
static void wait_action(bool with_ui)
{
    int64_t until = esp_timer_get_time() + 1000000;
    while (mqtt_host_actions_active || mqtt_host_allocations != 3) {
        assert(esp_timer_get_time() < until); Sleep(2);
    }
    assert(!mqtt_host_actions_active && mqtt_host_allocations == 3);
    if (with_ui) {
        /* A fast worker can finish before the click callback refreshes controls.
         * Wait for this action's result, so its ring entry is also drained by the
         * real UI timer before inspecting history or heap usage. */
        assert(mqtt_host_actions_entered);
        mqtt_host_action_record_t *record = &mqtt_host_actions[mqtt_host_actions_entered - 1];
        char expected[192];
        if (record->result < 0)
            snprintf(expected, sizeof(expected), "%s", record->result == -2 ? "MQTT outbox full; wait before retrying" :
                     record->publish ? "Could not queue publish" : "Could not queue subscription");
        else
            snprintf(expected, sizeof(expected), record->publish ? "Publish queued on %s (message %d)" :
                                                                  "Subscribing to %s (message %d)...",
                     record->topic, record->result);
        while (lv_obj_has_state(button("PUBLISH"), LV_STATE_DISABLED) || !find_text(content, expected, true)) {
            assert(esp_timer_get_time() < until); pump();
        }
    }
}
static void settle_ui(void)
{
    lv_obj_update_layout(content);
    uint16_t animations = lv_anim_count_running();
    int64_t stable_since = esp_timer_get_time(), until = stable_since + 1500000;
    /* Theme transitions include a delay; wait for their allocation count to
     * settle across the 400 ms maximum scroll duration. Cursor blinking remains
     * active. Do not delete animations to make a memory comparison pass. */
    while (esp_timer_get_time() - stable_since < 500000) {
        assert(esp_timer_get_time() < until); pump();
        uint16_t current = lv_anim_count_running();
        if (current != animations) { animations = current; stable_since = esp_timer_get_time(); }
    }
    assert(!lv_obj_is_scrolling(area(5)) && !lv_obj_is_scrolling(content));
}
static void wait_logging(LONG entries, bool with_ui)
{
    int64_t until = esp_timer_get_time() + 1000000;
    while (mqtt_host_close_entered < entries || mqtt_host_open_files) {
        assert(esp_timer_get_time() < until); if (with_ui) pump(); else Sleep(2);
    }
}
static void queue_messages(bool with_log)
{
    click("PUBLISH"); wait_action(true);
    mqtt_host_receive("fixture/rx", "FIXTURE_RX_SECRET");
    int64_t until = esp_timer_get_time() + 1000000;
    while (!find_text(content, "Received 17 bytes on fixture/rx", true)) {
        assert(esp_timer_get_time() < until); pump();
    }
    if (with_log) { wait_logging(2, false); mqtt_host_hold_log_dispatch = true; }
    click("PUBLISH"); wait_action(false); mqtt_host_receive("fixture/rx", "FIXTURE_RX_SECRET");
    if (with_log) wait_count(&mqtt_host_log_dispatch_entered, 1, false);
    assert(mqtt_host_publishes == 2);
}
static void assert_stopping(void)
{
    pump_for(220); assert(mqtt_tool_busy());
    assert(lv_obj_has_state(button("STOPPING"), LV_STATE_DISABLED));
    assert(lv_obj_has_state(button("PUBLISH"), LV_STATE_DISABLED));
    assert(lv_obj_has_state(button("SUBSCRIBE"), LV_STATE_DISABLED));
    assert(lv_obj_has_state(button("SD LOG\nOFF"), LV_STATE_DISABLED));
    assert(lv_obj_has_state(button("COPY RX"), LV_STATE_DISABLED));
    assert(lv_obj_has_state(button("PASTE BYTES"), LV_STATE_DISABLED));
    lv_obj_t *payload_mode = find_text(content, "PAYLOAD: BYTES", true);
    if (!payload_mode) payload_mode = find_text(content, "PAYLOAD: TEXT", true);
    assert(payload_mode && lv_obj_has_state(lv_obj_get_parent(payload_mode), LV_STATE_DISABLED));
    LONG inits = mqtt_host_initializations, publishes = mqtt_host_publishes, subscribes = mqtt_host_subscribes;
    forced_click("STOPPING"); forced_click("PUBLISH"); forced_click("SUBSCRIBE");
    forced_click("SAVE PROFILE"); forced_click("DELETE PROFILE"); forced_click("SD LOG\nOFF");
    forced_click("COPY RX");
    forced_click("PASTE BYTES"); lv_obj_send_event(lv_obj_get_parent(payload_mode), LV_EVENT_CLICKED, NULL);
    assert(mqtt_host_initializations == inits && mqtt_host_publishes == publishes &&
           mqtt_host_subscribes == subscribes && !mqtt_host_nvs_writes);
    assert(!strcmp(lv_textarea_get_text(area(2)), ""));
}
static void snapshot(void)
{
    lv_obj_update_layout(content); lv_refr_now(NULL);
    char path[1024]; snprintf(path, sizeof(path), "%s/mqtt-%s.ppm", output, mode);
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
}
static void geometry(void)
{
    const char *controls[] = {"CONNECT", "SUBSCRIBE", "PUBLISH", "SAVE PROFILE", "DELETE PROFILE", "CLEAR", "COPY RX",
                              "PAYLOAD: TEXT", "PASTE BYTES"};
    for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        lv_obj_t *control = button(controls[i]); lv_area_t parent, bounds;
        lv_obj_get_content_coords(lv_obj_get_parent(control), &parent); lv_obj_get_coords(control, &bounds);
        if (bounds.x1 < parent.x1 || bounds.x2 > parent.x2 || bounds.y1 < parent.y1 || bounds.y2 > parent.y2)
            fprintf(stderr, "%s: control=(%ld,%ld)-(%ld,%ld), row=(%ld,%ld)-(%ld,%ld)\n", controls[i],
                    (long)bounds.x1, (long)bounds.y1, (long)bounds.x2, (long)bounds.y2,
                    (long)parent.x1, (long)parent.y1, (long)parent.x2, (long)parent.y2);
        assert(bounds.x1 >= parent.x1 && bounds.x2 <= parent.x2 && bounds.y1 >= parent.y1 && bounds.y2 <= parent.y2);
    }
}
static mqtt_host_profile_record_t profile_fixture(bool websocket)
{
    mqtt_host_profile_record_t profile = {.version = 1};
    strcpy(profile.uri, websocket ? "wss://fixture.test:443/mqtt?test=1" : "mqtts://fixture.test:8883");
    memset(profile.username, 'u', 64);
    for (unsigned i = 0; i < 16; i++) memcpy(profile.password + i * 4, "\xf0\x9f\x98\x80", 4);
    strcpy(profile.topic, "fixture/+/profile"); return profile;
}
static void profile_draft(const mqtt_host_profile_record_t *profile)
{
    lv_textarea_set_text(area(0), profile->uri); lv_textarea_set_text(area(1), profile->username);
    lv_textarea_set_text(area(2), profile->password); lv_textarea_set_text(area(3), profile->topic);
}
static void profile_fields(const mqtt_host_profile_record_t *profile)
{
    assert(!strcmp(lv_textarea_get_text(area(0)), profile->uri));
    assert(!strcmp(lv_textarea_get_text(area(1)), profile->username));
    assert(!strcmp(lv_textarea_get_text(area(2)), profile->password));
    assert(!strcmp(lv_textarea_get_text(area(3)), profile->topic));
    assert(lv_textarea_get_password_mode(area(2)) && !mqtt_host_nvs_handles);
}
static void profile_defaults(void)
{
    assert(!strcmp(lv_textarea_get_text(area(0)), "mqtts://test.mosquitto.org:8886"));
    assert(!strcmp(lv_textarea_get_text(area(1)), "") && !strcmp(lv_textarea_get_text(area(2)), ""));
    assert(!strcmp(lv_textarea_get_text(area(3)), "tab5os/010203/loopback"));
    assert(lv_textarea_get_password_mode(area(2)) && !mqtt_host_nvs_handles);
}
static void reenter_profile(void) { close_app(); finish(false); open_app(); }
static void delete_saved_profile(void)
{
    LONG erases = mqtt_host_nvs_erases;
    click("DELETE PROFILE"); expect("Tap DELETE PROFILE again"); assert(mqtt_host_nvs_erases == erases);
    click("DELETE PROFILE"); expect("profile deleted");
    assert(mqtt_host_nvs_erases == erases + 1 && !mqtt_host_nvs_present()); profile_defaults();
    assert(lv_obj_has_state(button("DELETE PROFILE"), LV_STATE_DISABLED));
}
static bool run_profile_case(void)
{
    if (strncmp(mode, "profile-", 8)) return false;
    mqtt_host_profile_record_t profile = profile_fixture(strstr(mode, "-wss") != NULL);
    if (!strcmp(mode, "profile-empty")) {
        mqtt_host_nvs_seed(NULL, 0, false); reenter_profile(); profile_defaults();
        assert(lv_obj_has_state(button("DELETE PROFILE"), LV_STATE_DISABLED));
        forced_click("DELETE PROFILE"); assert(!mqtt_host_nvs_erases && !mqtt_host_nvs_handles); return true;
    }
    if (!strcmp(mode, "profile-overwrite-errors")) {
        mqtt_nvs_fault_t faults[] = {MQTT_NVS_OPEN_WRITE, MQTT_NVS_SET_BEFORE, MQTT_NVS_SET_AFTER, MQTT_NVS_COMMIT};
        mqtt_host_profile_record_t replacement = profile_fixture(true);
        strcpy(replacement.topic, "fixture/+/replacement");
        for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
            mqtt_host_nvs_seed(&profile, sizeof(profile), false); reenter_profile(); profile_fields(&profile);
            profile_draft(&replacement); mqtt_host_nvs_fault(faults[i]); click("SAVE PROFILE");
            expect("profile save not confirmed"); profile_fields(&replacement);
            assert(!lv_obj_has_state(button("DELETE PROFILE"), LV_STATE_DISABLED));
            reenter_profile(); profile_fields(i >= 2 ? &replacement : &profile); assert(!mqtt_host_initializations);
        }
        return true;
    }
    if (!strcmp(mode, "profile-repair")) {
        for (unsigned i = 0; i < 2; i++) {
            mqtt_host_profile_record_t bad = profile; bad.version = 2;
            mqtt_host_nvs_seed(&bad, sizeof(bad), i == 0); reenter_profile(); profile_defaults();
            expect("Saved MQTT profile unavailable"); profile_draft(&profile); click("SAVE PROFILE");
            expect("TLS profile saved"); reenter_profile(); profile_fields(&profile);
        }
        return true;
    }
    if (!strncmp(mode, "profile-save-", 13)) {
        mqtt_nvs_fault_t fault = strstr(mode, "-open") ? MQTT_NVS_OPEN_WRITE :
            strstr(mode, "-before") ? MQTT_NVS_SET_BEFORE : strstr(mode, "-after") ? MQTT_NVS_SET_AFTER : MQTT_NVS_COMMIT;
        profile_draft(&profile); mqtt_host_nvs_fault(fault); click("SAVE PROFILE");
        expect("profile save not confirmed"); profile_fields(&profile);
        assert(!mqtt_host_nvs_handles && !mqtt_host_initializations);
        bool effect = fault == MQTT_NVS_SET_AFTER || fault == MQTT_NVS_COMMIT;
        assert(mqtt_host_nvs_present() == effect);
        assert(mqtt_host_nvs_commits == (fault == MQTT_NVS_COMMIT ? 1 : 0));
        if (fault == MQTT_NVS_OPEN_WRITE) {
            assert(!mqtt_host_nvs_writes && lv_obj_has_state(button("DELETE PROFILE"), LV_STATE_DISABLED));
        } else {
            assert(mqtt_host_nvs_writes == 1 && !lv_obj_has_state(button("DELETE PROFILE"), LV_STATE_DISABLED));
            delete_saved_profile();
        }
        reenter_profile(); profile_defaults(); assert(!mqtt_host_nvs_present()); return true;
    }
    if (!strcmp(mode, "profile-invalid")) {
        for (unsigned i = 0; i < 12; i++) {
            mqtt_host_profile_record_t bad = profile;
            size_t size = sizeof(bad);
            switch (i) {
            case 0: bad.version = 2; break;
            case 1: strcpy(bad.uri, "mqtt://fixture.test:1883"); break;
            case 2: memset(bad.uri, 'x', sizeof(bad.uri)); break;
            case 3: memset(bad.username, 'x', sizeof(bad.username)); break;
            case 4: memset(bad.password, 'x', sizeof(bad.password)); break;
            case 5: memset(bad.topic, 'x', sizeof(bad.topic)); break;
            case 6: strcpy(bad.topic, "bad+filter"); break;
            case 7: bad.topic[0] = 0; break;
            case 8: strcpy(bad.uri, " mqtts://fixture.test:8883"); break;
            case 9: strcpy(bad.uri, "mqtts://fixture.test:0"); break;
            case 10: size--; break;
            case 11: size = 0; break;
            }
            mqtt_host_nvs_seed(&bad, size, false); reenter_profile(); profile_defaults();
            expect("Saved MQTT profile unavailable"); assert(!mqtt_host_initializations);
            assert(!lv_obj_has_state(button("DELETE PROFILE"), LV_STATE_DISABLED));
            delete_saved_profile();
        }
        unsigned char oversized[sizeof(profile) + 1] = {0};
        memcpy(oversized, &profile, sizeof(profile)); mqtt_host_nvs_seed(oversized, sizeof(oversized), false);
        reenter_profile(); profile_defaults(); expect("Saved MQTT profile unavailable"); delete_saved_profile();
        printf("13 malformed profile blobs rejected, retained for explicit key deletion\n"); return true;
    }
    if (!strcmp(mode, "profile-read-errors")) {
        mqtt_nvs_fault_t faults[] = {MQTT_NVS_OPEN_READ, MQTT_NVS_SIZE, MQTT_NVS_READ};
        for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
            mqtt_host_nvs_seed(&profile, sizeof(profile), false); mqtt_host_nvs_fault(faults[i]);
            reenter_profile(); profile_defaults(); expect("Saved MQTT profile unavailable");
            assert(!lv_obj_has_state(button("DELETE PROFILE"), LV_STATE_DISABLED)); delete_saved_profile();
        }
        return true;
    }
    mqtt_host_nvs_seed(&profile, sizeof(profile), !strcmp(mode, "profile-wrong-type"));
    if (!strcmp(mode, "profile-read-short")) mqtt_host_nvs_fault(MQTT_NVS_READ_SHORT);
    if (!strcmp(mode, "profile-read-gone")) mqtt_host_nvs_fault(MQTT_NVS_READ_GONE);
    reenter_profile();
    if (!strcmp(mode, "profile-wrong-type") || !strcmp(mode, "profile-read-short")) {
        profile_defaults(); expect("Saved MQTT profile unavailable"); assert(!mqtt_host_initializations);
        assert(!lv_obj_has_state(button("DELETE PROFILE"), LV_STATE_DISABLED)); delete_saved_profile(); return true;
    }
    if (!strcmp(mode, "profile-read-gone")) {
        profile_defaults(); assert(!mqtt_host_nvs_present() && !mqtt_host_nvs_handles);
        assert(lv_obj_has_state(button("DELETE PROFILE"), LV_STATE_DISABLED)); return true;
    }
    profile_fields(&profile); expect("Saved TLS profile loaded");
    assert(!mqtt_host_initializations && !mqtt_host_nvs_writes && !mqtt_tool_busy());
    assert(find_text(content, "SD LOG\nOFF", true) && find_text(content, "RETAIN\nOFF", true));
    if (!strcmp(mode, "profile-load-tls") || !strcmp(mode, "profile-load-wss")) {
        click("CONNECT"); assert(mqtt_host_clients == 1 && mqtt_host_connection.certificate_bundle);
        assert(!strcmp(mqtt_host_connection.uri, profile.uri) && !strcmp(mqtt_host_connection.username, profile.username) &&
               !strcmp(mqtt_host_connection.password, profile.password));
        mqtt_host_event(MQTT_EVENT_CONNECTED); wait_text("Connected with TLS verification");
        click("SUBSCRIBE"); wait_action(true); assert(!strcmp(mqtt_host_actions[0].topic, profile.topic));
        click("DISCONNECT"); finish(true); assert(!strlen(lv_textarea_get_text(area(2))));
        reenter_profile(); profile_fields(&profile); assert(mqtt_host_initializations == 1); return true;
    }
    if (!strcmp(mode, "profile-overwrite")) {
        profile = profile_fixture(true); strcpy(profile.topic, "fixture/+/replacement");
        profile_draft(&profile); click("SAVE PROFILE"); expect("TLS profile saved");
        assert(mqtt_host_nvs_writes == 1 && mqtt_host_nvs_commits == 1);
        reenter_profile(); profile_fields(&profile); delete_saved_profile();
        reenter_profile(); profile_defaults(); assert(!mqtt_host_nvs_present()); return true;
    }
    if (!strcmp(mode, "profile-delete-expiry")) {
        click("DELETE PROFILE"); assert(!mqtt_host_nvs_erases);
        tick_offset += 5001; pump_for(220); expect("deletion confirmation expired");
        click("DELETE PROFILE"); assert(!mqtt_host_nvs_erases && mqtt_host_nvs_present());
        click("CLEAR"); click("DELETE PROFILE"); assert(!mqtt_host_nvs_erases);
        lv_obj_send_event(area(3), LV_EVENT_FOCUSED, NULL); click("DELETE PROFILE"); assert(!mqtt_host_nvs_erases);
        click("DELETE PROFILE"); assert(mqtt_host_nvs_erases == 1 && !mqtt_host_nvs_present());
        profile_defaults(); return true;
    }
    if (!strcmp(mode, "profile-delete-handoffs")) {
        click("DELETE PROFILE"); lv_textarea_set_text(area(0), "https://invalid.test"); click("SAVE PROFILE");
        click("DELETE PROFILE"); assert(!mqtt_host_nvs_erases); expect("Tap DELETE PROFILE again");
        profile_draft(&profile); click("CONNECT"); mqtt_host_event(MQTT_EVENT_CONNECTED);
        wait_text("Connected with TLS verification"); click("DISCONNECT"); finish(true);
        click("DELETE PROFILE"); assert(!mqtt_host_nvs_erases); expect("Tap DELETE PROFILE again");
        click("CLEAR"); lv_textarea_set_text(area(0), "mqtt://fixture.test:1883");
        click("CONNECT"); expect("Tap CONNECT again"); assert(mqtt_host_initializations == 1);
        click("DELETE PROFILE"); click("CONNECT"); expect("Tap CONNECT again");
        assert(mqtt_host_initializations == 1 && !mqtt_host_nvs_erases && mqtt_host_nvs_present()); return true;
    }
    if (!strncmp(mode, "profile-delete-", 15)) {
        mqtt_nvs_fault_t fault = strstr(mode, "-open") ? MQTT_NVS_OPEN_WRITE :
            strstr(mode, "-before") ? MQTT_NVS_ERASE_BEFORE : strstr(mode, "-after") ? MQTT_NVS_ERASE_AFTER : MQTT_NVS_COMMIT;
        mqtt_host_nvs_fault(fault); click("DELETE PROFILE"); click("DELETE PROFILE");
        expect("profile deletion not confirmed"); profile_fields(&profile);
        bool effect = fault == MQTT_NVS_ERASE_AFTER || fault == MQTT_NVS_COMMIT;
        assert(mqtt_host_nvs_present() != effect && !mqtt_host_nvs_handles);
        assert(!lv_obj_has_state(button("DELETE PROFILE"), LV_STATE_DISABLED));
        assert(mqtt_host_nvs_erases == (fault == MQTT_NVS_OPEN_WRITE ? 0 : 1));
        delete_saved_profile(); reenter_profile(); profile_defaults(); return true;
    }
    if (!strcmp(mode, "profile-live-actions")) {
        click("CONNECT"); mqtt_host_event(MQTT_EVENT_CONNECTED); wait_text("Connected with TLS verification");
        forced_click("SAVE PROFILE"); forced_click("DELETE PROFILE"); assert(!mqtt_host_nvs_writes);
        mqtt_host_hold_stop = true; click("DISCONNECT"); wait_count(&mqtt_host_stop_entered, 1, false);
        forced_click("SAVE PROFILE"); forced_click("DELETE PROFILE"); assert(!mqtt_host_nvs_writes);
        SetEvent(mqtt_host_stop_release); finish(true); delete_saved_profile(); return true;
    }
    if (!strcmp(mode, "profile-pending-actions")) {
        click("CONNECT"); mqtt_host_event(MQTT_EVENT_CONNECTED); wait_text("Connected with TLS verification");
        click("DISCONNECT"); finish(false);
        assert(lv_obj_has_state(button("SAVE PROFILE"), LV_STATE_DISABLED) &&
               lv_obj_has_state(button("DELETE PROFILE"), LV_STATE_DISABLED));
        forced_click("SAVE PROFILE"); forced_click("DELETE PROFILE");
        assert(!mqtt_host_nvs_writes && !mqtt_host_nvs_erases);
        expect("Disconnecting in the background"); finish(true); reenter_profile(); profile_fields(&profile); return true;
    }
    assert(!strcmp(mode, "profile-repeat"));
    for (unsigned i = 0; i < 3; i++) {
        delete_saved_profile(); profile_draft(&profile); click("SAVE PROFILE"); close_app(); finish(false);
        if (i < 2) { open_app(); profile_fields(&profile); }
    }
    /* Warm-up and measured cycles end after Save/Home; ending warm-up after
     * Load/Home leaves a different small LVGL allocator tail in the parent. */
    lv_mem_monitor_t before, after; lv_mem_monitor(&before);
    DWORD handles_before, handles_after; assert(GetProcessHandleCount(GetCurrentProcess(), &handles_before));
    LONG tasks = host_tasks_started;
    for (unsigned i = 0; i < 25; i++) {
        open_app(); profile_fields(&profile); delete_saved_profile();
        profile_draft(&profile); click("SAVE PROFILE"); close_app(); finish(false);
        assert(!mqtt_host_nvs_handles && !mqtt_host_allocations && mqtt_host_nvs_present());
    }
    lv_mem_monitor(&after); assert(GetProcessHandleCount(GetCurrentProcess(), &handles_after));
    printf("Profile memory before assertion: free=%zu -> %zu, allocations=%zu -> %zu, animations=%u\n",
           before.free_size, after.free_size, before.used_cnt, after.used_cnt, lv_anim_count_running()); fflush(stdout);
    assert(before.free_size == after.free_size && before.used_cnt == after.used_cnt);
    assert(handles_before == handles_after && host_tasks_started == tasks && host_active_tasks == 1);
    printf("25 profile reload/delete/save cycles: LVGL free=%zu -> %zu, allocations=%zu -> %zu, handles=%lu -> %lu, standby workers=%ld\n",
           before.free_size, after.free_size, before.used_cnt, after.used_cnt,
           (unsigned long)handles_before, (unsigned long)handles_after, host_active_tasks);
    open_app(); profile_fields(&profile); return true;
}
static void lifecycle(void)
{
    bool logged = !strcmp(mode, "logged-lifecycle");
    connect_session(); if (logged) click("SD LOG\nOFF"); click("PUBLISH"); wait_action(false);
    mqtt_host_receive("fixture/rx", "bounded preview"); close_app(); finish(false);
    lv_mem_monitor_t before, after; lv_mem_monitor(&before); DWORD handles_before, handles_after;
    assert(GetProcessHandleCount(GetCurrentProcess(), &handles_before));
    LONG tasks_before = host_tasks_started;
    for (unsigned i = 0; i < 25; i++) {
        open_app(); connect_session(); if (logged) click("SD LOG\nOFF"); click("PUBLISH"); wait_action(false);
        mqtt_host_receive("fixture/rx", "bounded preview"); close_app();
        finish(false); assert(!mqtt_host_allocations && host_active_tasks == 1);
    }
    lv_mem_monitor(&after); assert(GetProcessHandleCount(GetCurrentProcess(), &handles_after));
    printf("Session memory: free=%zu -> %zu, allocations=%zu -> %zu, animations=%u\n",
           before.free_size, after.free_size, before.used_cnt, after.used_cnt, lv_anim_count_running()); fflush(stdout);
    assert(before.free_size == after.free_size && before.used_cnt == after.used_cnt);
    assert(handles_before == handles_after && host_tasks_started == tasks_before);
    printf("25 sessions: LVGL free=%zu -> %zu, allocations=%zu, handles=%lu -> %lu, standby workers=%ld\n",
           before.free_size, after.free_size, after.used_cnt, (unsigned long)handles_before, (unsigned long)handles_after, host_active_tasks);
    open_app();
}
/* Only for proving these synthetic fixtures collide in the removed fingerprint. */
static uint32_t legacy_connection_fingerprint(const char *const fields[3])
{
    uint32_t hash = 2166136261U;
    for (unsigned i = 0; i < 3; i++) {
        const unsigned char *text = (const unsigned char *)fields[i];
        do { hash = (hash ^ *text) * 16777619U; } while (*text++);
    }
    return hash;
}
static bool run_connection_case(void)
{
    if (strncmp(mode, "connection-", 11)) return false;
    lv_textarea_set_text(area(0), "mqtts://fixture.test:8883");
    lv_textarea_set_text(area(3), "fixture/topic");
    if (!strncmp(mode, "connection-confirmation-collision-", 34)) {
        unsigned field = strstr(mode, "-uri") ? 0 : strstr(mode, "-username") ? 1 : 2;
        const char *pairs[3][2] = {
            {"mqtt://863b7860cf71494a.test:1883", "mqtt://295ddb5da61a6de2.test:1883"},
            {"2cc499a6969fd39d", "3baf159ee8c13e60"},
            {"01ee6126d21d92c2", "310581bed07745c6"},
        };
        const uint32_t fingerprints[] = {0xd1503d50U, 0xfcfb6dedU, 0xb9c73043U};
        const char *first[] = {"mqtt://fixture.test:1883", "FIXTURE_USER_SECRET", "FIXTURE_PASSWORD_SECRET"};
        const char *second[] = {first[0], first[1], first[2]};
        first[field] = pairs[field][0]; second[field] = pairs[field][1];
        assert(strcmp(first[field], second[field]) && legacy_connection_fingerprint(first) == fingerprints[field]);
        assert(legacy_connection_fingerprint(first) == legacy_connection_fingerprint(second));
        for (unsigned i = 0; i < 3; i++) lv_textarea_set_text(area(i), first[i]);
        click("CONNECT"); expect("Tap CONNECT again"); assert(!mqtt_host_initializations);
        lv_textarea_set_text(area(field), second[field]); click("CONNECT");
        assert(!mqtt_host_initializations && !mqtt_host_clients); expect("Tap CONNECT again");
        click("CONNECT"); assert(mqtt_host_initializations == 1 && mqtt_host_clients == 1);
        assert(!strcmp(mqtt_host_connection.uri, second[0]) && !strcmp(mqtt_host_connection.username, second[1]) &&
               !strcmp(mqtt_host_connection.password, second[2]) && !mqtt_host_connection.certificate_bundle);
        mqtt_host_event(MQTT_EVENT_CONNECTED); wait_text("Connected over confirmed cleartext");
        printf("Changed field %u with legacy fingerprint %08x required a fresh confirmation\n", field, fingerprints[field]);
        return true;
    }
    if (!strcmp(mode, "connection-confirmation-edits")) {
        const char *original[] = {"mqtt://fixture.test:1883", "FIXTURE_USER_SECRET", "FIXTURE_PASSWORD_SECRET"};
        for (unsigned i = 0; i < 3; i++) lv_textarea_set_text(area(i), original[i]);
        for (unsigned field = 0; field < 3; field++) {
            for (unsigned edit = 0; edit < 3; edit++) {
                click("CONNECT"); expect("Tap CONNECT again"); assert(!mqtt_host_initializations);
                if (edit == 0) {
                    lv_textarea_set_cursor_pos(area(field), LV_TEXTAREA_CURSOR_LAST);
                    lv_textarea_add_text(area(field), field ? "x" : "/"); lv_textarea_delete_char(area(field));
                } else if (edit == 1) {
                    lv_textarea_set_text(area(field), field ? "replacement" : "mqtt://replacement.test:1883");
                    lv_textarea_set_text(area(field), original[field]);
                } else {
                    lv_textarea_set_cursor_pos(area(field), 0); lv_textarea_delete_char_forward(area(field));
                    lv_textarea_add_char(area(field), (uint32_t)(unsigned char)original[field][0]);
                }
                assert(!strcmp(lv_textarea_get_text(area(field)), original[field]));
                click("CONNECT"); assert(!mqtt_host_initializations && !mqtt_host_clients); expect("Tap CONNECT again");
                click("CLEAR");
            }
        }
        click("CONNECT"); expect("Tap CONNECT again"); assert(!mqtt_host_initializations);
        click("CONNECT"); assert(mqtt_host_clients == 1 && mqtt_host_initializations == 1);
        mqtt_host_event(MQTT_EVENT_CONNECTED); wait_text("Connected over confirmed cleartext");
        printf("Nine unfocused edit/restore sequences required a fresh confirmation\n"); return true;
    }
    if (!strcmp(mode, "connection-uri-invalid")) {
        const char *invalid[] = {"", "https://fixture.test", "mqtts://", "mqtts:///mqtt",
            "mqtts://:8883", "mqtts://fixture.test:", "mqtts://fixture.test:0",
            "mqtts://fixture.test:65536", "mqtts://fixture.test:-1", "mqtts://fixture.test:abc",
            "mqtts://fixture.test:8883x", "mqtts://user:pass@fixture.test", "mqtts://@fixture.test",
            "mqtts://fixture.test#private", "mqtts://fixture.test#", "mqtts://fixture.test/topic",
            "mqtt://fixture.test?token=value", "mqtt://fixture.test?", "mqtts://[::1",
            "mqtts://[bad]:8883", "mqtts://2001:db8::1", "mqtts://fixture .test",
            "wss://fixture.test:65536/mqtt", "ws://user:pass@fixture.test/mqtt",
            "wss://fixture.test/mqtt#ignored"};
        for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            lv_textarea_set_text(area(0), invalid[i]); click("CONNECT");
            if (mqtt_host_initializations) fprintf(stderr, "Accepted invalid broker URI: %s\n", invalid[i]);
            assert(!mqtt_host_initializations); click("CONNECT");
            assert(!mqtt_host_initializations && !mqtt_tool_busy()); expect("valid MQTT broker URI");
            click("SAVE PROFILE"); assert(!mqtt_host_nvs_writes);
        }
        printf("25 invalid broker URIs rejected before SDK initialization or NVS writes\n"); return true;
    }
    if (!strcmp(mode, "connection-uri-bounds")) {
        const char *valid[] = {"mqtts://fixture.test", "mqtts://fixture.test/", "MQTTS://fixture.test:1",
            "mqtts://fixture.test:65535", "mqtts://[::1]:8883", "wss://fixture.test?test=1",
            "WSS://fixture.test:443/mqtt?test=1"};
        for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
            lv_textarea_set_text(area(0), valid[i]); click("CONNECT");
            assert(mqtt_host_clients == 1 && !strcmp(mqtt_host_connection.uri, valid[i]));
            mqtt_host_event(MQTT_EVENT_CONNECTED); wait_text("Connected with TLS verification");
            click("DISCONNECT"); finish(true);
        }
        char uri[257]; const char *prefix = "wss://fixture.test/mqtt/";
        strcpy(uri, prefix); memset(uri + strlen(prefix), 'x', 256 - strlen(prefix)); uri[256] = 0;
        lv_textarea_set_text(area(0), uri); assert(strlen(lv_textarea_get_text(area(0))) == 256);
        click("CONNECT"); expect("valid MQTT broker URI"); assert(mqtt_host_initializations == 7);
        click("SAVE PROFILE"); assert(!mqtt_host_nvs_writes);
        char padded[260]; snprintf(padded, sizeof(padded), "  %s ", uri);
        lv_textarea_set_text(area(0), padded); assert(strlen(lv_textarea_get_text(area(0))) == 256);
        click("CONNECT"); expect("valid MQTT broker URI"); assert(mqtt_host_initializations == 7);
        click("SAVE PROFILE"); assert(!mqtt_host_nvs_writes);
        uri[255] = 0; lv_textarea_set_text(area(0), uri); click("CONNECT");
        assert(mqtt_host_initializations == 8 && !strcmp(mqtt_host_connection.uri, uri));
        mqtt_host_event(MQTT_EVENT_CONNECTED); wait_text("Connected with TLS verification"); return true;
    }
    if (!strcmp(mode, "connection-credential-bounds") || !strcmp(mode, "connection-profile-bounds")) {
        bool save = !strcmp(mode, "connection-profile-bounds");
        char ascii[66], unicode[69], mixed[66]; memset(ascii, 'c', 65); ascii[65] = 0;
        for (unsigned i = 0; i < 17; i++) memcpy(unicode + i * 4, "\xf0\x9f\x98\x80", 4);
        unicode[68] = 0;
        mixed[0] = 'a'; memcpy(mixed + 1, unicode, 64); mixed[65] = 0;
        for (unsigned field = 1; field <= 2; field++) {
            lv_textarea_set_text(area(1), ""); lv_textarea_set_text(area(2), "");
            lv_textarea_set_text(area(field), ascii); assert(strlen(lv_textarea_get_text(area(field))) == 65);
            click(save ? "SAVE PROFILE" : "CONNECT"); expect("limited to 64 UTF-8 bytes");
            assert(!mqtt_host_initializations && !mqtt_host_nvs_writes);
            lv_textarea_set_text(area(field), mixed); assert(strlen(lv_textarea_get_text(area(field))) == 65);
            click(save ? "SAVE PROFILE" : "CONNECT"); expect("limited to 64 UTF-8 bytes");
            assert(!mqtt_host_initializations && !mqtt_host_nvs_writes);
            lv_textarea_set_text(area(field), unicode); assert(strlen(lv_textarea_get_text(area(field))) == 68);
            click(save ? "SAVE PROFILE" : "CONNECT"); expect("limited to 64 UTF-8 bytes");
            assert(!mqtt_host_initializations && !mqtt_host_nvs_writes);
        }
        ascii[64] = 0; unicode[64] = 0;
        lv_textarea_set_text(area(1), ascii); lv_textarea_set_text(area(2), unicode);
        click(save ? "SAVE PROFILE" : "CONNECT");
        if (save) {
            expect("TLS profile saved"); assert(mqtt_host_nvs_writes == 1);
            assert(!strcmp(mqtt_host_profile.username, ascii) && !strcmp(mqtt_host_profile.password, unicode));
            assert(mqtt_host_profile.version == 1 && !strcmp(mqtt_host_profile.uri, "mqtts://fixture.test:8883"));
            assert(!strcmp(mqtt_host_profile.topic, "fixture/topic"));
        } else {
            assert(mqtt_host_clients == 1 && !strcmp(mqtt_host_connection.username, ascii) &&
                   !strcmp(mqtt_host_connection.password, unicode));
            mqtt_host_event(MQTT_EVENT_CONNECTED); wait_text("Connected with TLS verification");
        }
        return true;
    }
    if (!strcmp(mode, "connection-confirmation")) {
        lv_textarea_set_text(area(0), "mqtt://fixture.test:1883"); click("CONNECT"); expect("Tap CONNECT again");
        tick_offset += 5001; pump_for(220); expect("confirmation expired"); assert(!mqtt_host_initializations);
        click("CONNECT"); expect("Tap CONNECT again"); click("CLEAR"); click("CONNECT");
        expect("Tap CONNECT again"); assert(!mqtt_host_initializations);
        lv_textarea_set_text(area(1), "changed-user"); click("CONNECT"); assert(!mqtt_host_initializations);
        lv_textarea_set_text(area(2), "changed-password"); click("CONNECT"); assert(!mqtt_host_initializations);
        lv_textarea_set_text(area(0), "ws://fixture.test:80/mqtt?test=1"); click("CONNECT");
        assert(!mqtt_host_initializations); click("CONNECT"); assert(mqtt_host_clients == 1);
        assert(!strcmp(mqtt_host_connection.username, "changed-user") &&
               !strcmp(mqtt_host_connection.password, "changed-password"));
        mqtt_host_event(MQTT_EVENT_CONNECTED); wait_text("Connected over confirmed cleartext");
        click("DISCONNECT"); finish(true); click("CONNECT"); assert(mqtt_host_initializations == 1);
        expect("Tap CONNECT again"); close_app(); open_app();
        lv_textarea_set_text(area(0), "mqtt://fixture.test:1883"); click("CONNECT");
        assert(mqtt_host_initializations == 1); expect("Tap CONNECT again"); return true;
    }
    if (!strcmp(mode, "connection-error-storage") || !strcmp(mode, "connection-error-overflow") ||
        !strcmp(mode, "connection-error-history")) {
        connect_session();
        bool storage = !strcmp(mode, "connection-error-storage");
        bool overflow = !strcmp(mode, "connection-error-overflow");
        if (storage || overflow) click("SD LOG\nOFF");
        if (storage) {
            mqtt_host_file_fault(MQTT_FILE_SYNC);
            mqtt_host_receive("fixture/error/0", "FIXTURE_RX_SECRET");
            wait_logging(1, false); wait_count(&reported_errors, 1, false);
        } else {
            mqtt_host_hold_log_dispatch = overflow;
            for (unsigned i = 0; i < (overflow ? 21U : 12U); i++) {
                char topic[48]; snprintf(topic, sizeof(topic), "fixture/error/%u", i);
                mqtt_host_receive(topic, "FIXTURE_RX_SECRET");
                if (overflow && !i) wait_count(&mqtt_host_log_dispatch_entered, 1, false);
            }
        }
        esp_mqtt_error_codes_t error = {.error_type = MQTT_ERROR_TYPE_CONNECTION_REFUSED, .connect_return_code = 5};
        esp_mqtt_event_t event = {.error_handle = &error};
        mqtt_host_event_data(MQTT_EVENT_ERROR, &event); mqtt_host_event(MQTT_EVENT_DISCONNECTED);
        wait_text("Broker refused connection (5)");
        expect(storage ? "not confirmed saved" : overflow ? "Dropped 12 SD metadata entries" : "Dropped 4 UI events");
        assert(lv_obj_has_state(button("PUBLISH"), LV_STATE_DISABLED));
        if (overflow) { SetEvent(mqtt_host_log_dispatch_release); wait_logging(9, true); }
        pump_for(220); expect("Broker refused connection (5)");
        click("CLEAR"); expect("Broker refused connection (5)");
        click("DISCONNECT"); finish(true);
        assert(reported_errors == (storage ? 1 : 0)); return true;
    }
    if (!strncmp(mode, "connection-transport-", 21) || !strcmp(mode, "connection-refused") ||
        !strcmp(mode, "connection-generic-error")) {
        connect_session();
        esp_mqtt_error_codes_t error = {.error_type = MQTT_ERROR_TYPE_TCP_TRANSPORT,
            .esp_tls_last_esp_err = ESP_ERR_TIMEOUT, .esp_transport_sock_errno = ECONNRESET};
        const char *expected = "Transport error: ESP_ERR_TIMEOUT";
        if (strcmp(mode, "connection-transport-error") && strcmp(mode, "connection-transport-combined")) {
            error.esp_tls_last_esp_err = 0; error.esp_transport_sock_errno = 0;
        }
        if (!strcmp(mode, "connection-transport-socket")) {
            error.esp_transport_sock_errno = ECONNRESET; expected = "Transport error: socket errno";
        } else if (!strcmp(mode, "connection-transport-tls")) {
            error.esp_tls_stack_err = -0x2700; expected = "Transport error: TLS stack -0x2700";
        } else if (!strcmp(mode, "connection-transport-verify")) {
            error.esp_tls_cert_verify_flags = 8; expected = "Transport error: cert flags 0x8";
        } else if (!strcmp(mode, "connection-transport-combined")) {
            error.esp_tls_stack_err = -0x2700; error.esp_tls_cert_verify_flags = 8;
        } else if (!strcmp(mode, "connection-transport-empty")) {
            expected = "Transport error: SDK supplied no error details";
        } else if (!strcmp(mode, "connection-transport-extremes")) {
            error.esp_tls_last_esp_err = ESP_FAIL; error.esp_tls_stack_err = INT_MIN;
            error.esp_tls_cert_verify_flags = INT_MIN; error.esp_transport_sock_errno = INT_MAX;
            expected = "Transport error: ESP_FAIL (0xFFFFFFFF)";
        }
        if (!strcmp(mode, "connection-refused")) {
            error.error_type = MQTT_ERROR_TYPE_CONNECTION_REFUSED; error.connect_return_code = 5;
            expected = "Broker refused connection (5)";
        }
        esp_mqtt_event_t event = {.error_handle = &error};
        if (!strcmp(mode, "connection-generic-error")) { event.error_handle = NULL; expected = "MQTT connection error"; }
        mqtt_host_event_data(MQTT_EVENT_ERROR, &event); mqtt_host_event(MQTT_EVENT_DISCONNECTED);
        if (!strncmp(mode, "connection-transport-", 21)) {
            wait_text("Transport error:");
            printf("Diagnostic: %s\n", lv_label_get_text(find_text(content, "Transport error:", false)));
            fflush(stdout);
        }
        wait_text(expected); assert(mqtt_tool_busy() && mqtt_host_clients == 1);
        char observed[192];
        snprintf(observed, sizeof(observed), "%s", lv_label_get_text(find_text(content, expected, false)));
        if (!strncmp(mode, "connection-transport-", 21)) {
            lv_obj_t *diagnostic = find_text(content, expected, false); assert(diagnostic);
            const char *text = lv_label_get_text(diagnostic);
            assert(!strstr(text, "ESP_OK") && strlen(text) < 192);
            if (error.esp_tls_stack_err) expect(error.esp_tls_stack_err == INT_MIN ? "TLS stack -0x80000000" : "TLS stack -0x2700");
            if (error.esp_tls_cert_verify_flags) expect(error.esp_tls_cert_verify_flags == INT_MIN ? "cert flags 0x80000000" : "cert flags 0x8");
            if (error.esp_tls_last_esp_err) {
                char code[24]; snprintf(code, sizeof(code), "(0x%X)", (unsigned)error.esp_tls_last_esp_err);
                expect(code);
            }
            if (error.esp_transport_sock_errno) {
                char code[48]; snprintf(code, sizeof(code), "socket errno %d:", error.esp_transport_sock_errno);
                expect(code);
                assert(strstr(text, strerror(error.esp_transport_sock_errno)));
            }
        }
        assert(lv_obj_has_state(button("SUBSCRIBE"), LV_STATE_DISABLED) &&
               lv_obj_has_state(button("PUBLISH"), LV_STATE_DISABLED));
        forced_click("SUBSCRIBE"); forced_click("PUBLISH"); assert(!mqtt_host_actions_entered);
        mqtt_host_event(MQTT_EVENT_SUBSCRIBED); mqtt_host_event(MQTT_EVENT_PUBLISHED); pump_for(220);
        assert(find_text(content, observed, true)); click("CLEAR"); assert(find_text(content, observed, true));
        click("DISCONNECT"); finish(true); expect("Disconnected; form password cleared");
        connect_session(); expect("Connected with TLS verification");
        assert(!find_text(content, expected, false)); return true;
    }
    if (!strcmp(mode, "connection-subscription-rejected")) {
        connect_session(); click("SUBSCRIBE"); wait_action(true);
        esp_mqtt_error_codes_t error = {.error_type = MQTT_ERROR_TYPE_SUBSCRIBE_FAILED};
        esp_mqtt_event_t event = {.msg_id = 7, .error_handle = &error};
        mqtt_host_event_data(MQTT_EVENT_SUBSCRIBED, &event); wait_text("Broker rejected subscription (message 7)");
        assert(!find_text(content, "Subscription acknowledged", false));
        assert(!lv_obj_has_state(button("PUBLISH"), LV_STATE_DISABLED));
        error.error_type = 0; mqtt_host_event_data(MQTT_EVENT_SUBSCRIBED, &event);
        wait_text("Subscription acknowledged (message 7)"); click("PUBLISH"); wait_action(true);
        assert(mqtt_host_publishes == 1 && mqtt_host_subscribes == 1); return true;
    }
    bool websocket = !strcmp(mode, "connection-wss") || !strcmp(mode, "connection-ws");
    bool secure = !strcmp(mode, "connection-tls") || !strcmp(mode, "connection-wss");
    const char *uri = websocket ? (secure ? "wss://fixture.test:443/mqtt?test=1" : "ws://fixture.test:80/mqtt?test=1") :
                                 secure ? "mqtts://fixture.test:8883" : "mqtt://fixture.test:1883";
    char draft[260]; snprintf(draft, sizeof(draft), " \t%s\t ", uri); lv_textarea_set_text(area(0), draft);
    click("CONNECT");
    if (!secure) { assert(!mqtt_host_initializations); expect("Tap CONNECT again"); click("CONNECT"); }
    assert(mqtt_host_clients == 1 && !strcmp(mqtt_host_connection.uri, uri));
    /* The status must use the captured transport even if the disabled draft is edited. */
    lv_textarea_set_text(area(0), secure ? "mqtt://changed.test:1883" : "mqtts://changed.test:8883");
    mqtt_host_event(MQTT_EVENT_CONNECTED);
    wait_text(secure ? "Connected with TLS verification" : "Connected over confirmed cleartext");
    assert(mqtt_host_connection.certificate_bundle == secure);
    if (!secure) assert(!find_text(content, "Connected with TLS verification", false));
    assert(!strcmp(mqtt_host_connection.username, "") && !strcmp(mqtt_host_connection.password, ""));
    click("PUBLISH"); wait_action(true); assert(mqtt_host_publishes == 1); return true;
}
static void check_clipboard(const uint8_t *bytes, size_t length)
{
    const payload_clipboard_t *copy = payload_clipboard_peek();
    assert(copy && copy->length == length && !memcmp(copy->bytes, bytes, length));
}
static esp_mqtt_event_t copy_message(const uint8_t *bytes, int length)
{
    return (esp_mqtt_event_t){.topic = "fixture/copy", .topic_len = 12, .data = (char *)bytes,
        .data_len = length, .total_data_len = length, .msg_id = 42, .qos = 2, .retain = true};
}
static void expect_copy(const uint8_t *bytes, size_t length)
{
    LONG publishes = mqtt_host_publishes, subscribes = mqtt_host_subscribes, appends = mqtt_host_appends;
    click("COPY RX"); check_clipboard(bytes, length);
    expect("Copied to the byte clipboard"); expect("fixture/copy");
    assert(mqtt_host_publishes == publishes && mqtt_host_subscribes == subscribes && mqtt_host_appends == appends);
}
static void paste_clipboard(size_t length)
{
    int64_t until = esp_timer_get_time() + 1000000;
    while (lv_obj_has_state(button("PASTE BYTES"), LV_STATE_DISABLED)) {
        assert(esp_timer_get_time() < until); pump();
    }
    LONG publishes = mqtt_host_publishes, subscribes = mqtt_host_subscribes;
    click("PASTE BYTES");
    char text[80]; snprintf(text, sizeof(text), "Prepared byte payload: %u bytes (Hex)", (unsigned)length);
    expect(text); expect("PAYLOAD: BYTES");
    assert(lv_obj_has_flag(area(4), LV_OBJ_FLAG_HIDDEN));
    assert(mqtt_host_publishes == publishes && mqtt_host_subscribes == subscribes);
}
static void prepare_bytes(const uint8_t *bytes, size_t length)
{
    assert(payload_clipboard_store(bytes, length));
    paste_clipboard(length);
}
static void check_published_bytes(const uint8_t *bytes, size_t length, LONG index,
                                  const char *topic, unsigned qos, bool retained)
{
    assert(index >= 0 && index < mqtt_host_actions_entered);
    mqtt_host_action_record_t *record = &mqtt_host_actions[index];
    assert(record->publish && record->store && record->length == (int)length);
    assert(!memcmp(record->payload, bytes, length) && !strcmp(record->topic, topic));
    assert(record->qos == (int)qos && record->retained == retained);
}
static bool run_paste_case(void)
{
    if (strncmp(mode, "paste-", 6)) return false;
    uint8_t bytes[129], replacement[] = {0xFF, 0, 0x80, 0x7F, '\n', '\r', '\t'};
    for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)(i * 37U);
    payload_clipboard_clear(); pump_for(220);
    assert(lv_obj_has_state(button("PASTE BYTES"), LV_STATE_DISABLED));
    assert(lv_obj_has_state(button("PAYLOAD: TEXT"), LV_STATE_DISABLED));
    forced_click("PASTE BYTES"); forced_click("PAYLOAD: TEXT");
    assert(!mqtt_host_initializations && !mqtt_host_publishes && !mqtt_host_appends);
    assert(!lv_obj_has_flag(area(4), LV_OBJ_FLAG_HIDDEN));
    if (!strcmp(mode, "paste-binary")) {
        close_app(); byte_tool_show(content); lv_obj_update_layout(content);
        lv_textarea_set_text(area(0), "00 25 4A 6F 94 B9 DE 03 28"); click("Copy bytes");
        check_clipboard(bytes, 9); byte_tool_stop(); lv_obj_clean(content); open_app();
        printf("Byte Lab Hex -> MQTT prepared byte payload passed\n");
    }
    if (!strcmp(mode, "paste-confirmation")) {
        lv_textarea_set_text(area(0), "mqtt://fixture.test:1883"); click("CONNECT"); expect("Tap CONNECT again");
        prepare_bytes(bytes, 9); click("CONNECT"); expect("Tap CONNECT again");
        assert(!mqtt_host_initializations && !mqtt_tool_busy());
        click("PAYLOAD: BYTES"); click("CONNECT"); expect("Tap CONNECT again");
        assert(!mqtt_host_initializations && !mqtt_tool_busy());
        click("CONNECT"); assert(mqtt_host_initializations == 1); return true;
    }
    size_t length = !strcmp(mode, "paste-empty") ? 0 : 9;
    if (!strcmp(mode, "paste-binary")) paste_clipboard(length);
    else prepare_bytes(bytes, length);
    assert(!mqtt_host_initializations && !mqtt_host_appends);
    expect(length ? "00 25 4A 6F 94 B9 DE 03 28" : "(empty payload)");
    connect_session();
    if (!strcmp(mode, "paste-empty")) { click("QoS 1"); click("QoS 2"); }
    if (!strcmp(mode, "paste-binary") || !strcmp(mode, "paste-empty") || !strcmp(mode, "paste-bounds"))
        click("SD LOG\nOFF");
    if (!strcmp(mode, "paste-bounds")) {
        for (unsigned count = 0; count <= 128; ++count) {
            prepare_bytes(bytes, count); click("PUBLISH"); wait_action(true);
            check_published_bytes(bytes, count, (LONG)count, "fixture/topic", 1, false);
        }
        assert(!payload_clipboard_store(bytes, 129)); check_clipboard(bytes, 128);
        click("PASTE BYTES"); expect("Prepared byte payload: 128 bytes");
        assert(mqtt_host_publishes == 129); wait_logging(129, false);
        printf("All 0-128 prepared lengths published exactly; 129-byte clipboard store rejected\n"); return true;
    }
    if (!strcmp(mode, "paste-capture") || !strcmp(mode, "paste-queued")) {
        assert(payload_clipboard_store(replacement, sizeof(replacement)));
        /* Replacing the clipboard without pasting must not replace the draft. */
        mqtt_host_hold_dispatch = true; click("PUBLISH"); wait_count(&mqtt_host_dispatch_entered, 1, false);
        assert(!mqtt_host_actions_entered && mqtt_host_allocations == 4);
        if (!strcmp(mode, "paste-capture")) {
            prepare_bytes(replacement, sizeof(replacement)); lv_textarea_set_text(area(3), "changed/topic");
            click("QoS 1"); click("RETAIN\nOFF");
            SetEvent(mqtt_host_dispatch_release); wait_action(true);
            check_published_bytes(bytes, 9, 0, "fixture/topic", 1, false);
            click("PUBLISH"); wait_action(true);
            check_published_bytes(replacement, sizeof(replacement), 1, "changed/topic", 2, true); return true;
        }
        close_app(); open_app(); assert_stopping(); expect("Prepared byte payload: 9 bytes");
        SetEvent(mqtt_host_dispatch_release); finish(false);
        forced_click("PASTE BYTES"); forced_click("PAYLOAD: BYTES"); expect("Prepared byte payload: 9 bytes");
        assert(!mqtt_host_actions_entered && !mqtt_host_appends);
        pump_for(220); connect_session(); click("PUBLISH"); wait_action(true);
        check_published_bytes(bytes, 9, 0, "fixture/topic", 1, false); return true;
    }
    if (!strcmp(mode, "paste-home")) {
        mqtt_host_hold_action = true; click("PUBLISH"); wait_count(&mqtt_host_actions_entered, 1, false);
        check_published_bytes(bytes, 9, 0, "fixture/topic", 1, false);
        prepare_bytes(replacement, 3); close_app(); open_app(); assert_stopping();
        assert(payload_clipboard_store(bytes, 128)); forced_click("PASTE BYTES"); expect("Prepared byte payload: 3 bytes");
        SetEvent(mqtt_host_action_release); finish(true);
        assert(mqtt_host_publishes == 1 && !mqtt_host_appends);
        connect_session(); click("PUBLISH"); wait_action(true);
        check_published_bytes(replacement, 3, 1, "fixture/topic", 1, false); return true;
    }
    if (!strcmp(mode, "paste-error")) {
        click("PUBLISH"); wait_action(true); expect("MQTT outbox full");
        check_published_bytes(bytes, 9, 0, "fixture/topic", 1, false);
        assert(!mqtt_host_appends && !strlen(lv_textarea_get_text(area(5))));
        mqtt_host_event(MQTT_EVENT_ERROR); pump_for(220); expect("MQTT connection error");
        prepare_bytes(replacement, 3); expect("MQTT connection error");
        click("PAYLOAD: BYTES"); expect("MQTT connection error"); forced_click("PUBLISH");
        assert(mqtt_host_publishes == 1); return true;
    }
    if (!strcmp(mode, "paste-modes")) {
        lv_obj_t *keys = lv_obj_get_child(content, lv_obj_get_child_count(content) - 1);
        assert(lv_obj_check_type(keys, &lv_keyboard_class));
        lv_obj_send_event(area(4), LV_EVENT_FOCUSED, NULL);
        assert(lv_keyboard_get_textarea(keys) != area(4));
        char overlong[514]; memset(overlong, 'x', 513); overlong[513] = '\0';
        lv_textarea_set_text(area(4), overlong);
        click("PAYLOAD: BYTES"); assert(!lv_obj_has_flag(area(4), LV_OBJ_FLAG_HIDDEN));
        assert(!strcmp(lv_textarea_get_text(area(4)), overlong)); click("PUBLISH"); expect("exceeds 512 bytes");
        assert(!mqtt_host_actions_entered);
        click("PAYLOAD: TEXT"); click("PUBLISH"); wait_action(true);
        check_published_bytes(bytes, 9, 0, "fixture/topic", 1, false);
        click("CLEAR"); expect("Prepared byte payload: 9 bytes"); click("PAYLOAD: BYTES");
        assert(!strcmp(lv_textarea_get_text(area(4)), overlong));
        const char *text = "\xC2\xB5text draft"; lv_textarea_set_text(area(4), text);
        click("PUBLISH"); wait_action(true);
        check_published_bytes((const uint8_t *)text, strlen(text), 1, "fixture/topic", 1, false);
        click("PAYLOAD: TEXT"); click("PUBLISH"); wait_action(true);
        check_published_bytes(bytes, 9, 2, "fixture/topic", 1, false); return true;
    }
    click("PUBLISH"); wait_action(true);
    check_published_bytes(bytes, length, 0, "fixture/topic", !strcmp(mode, "paste-empty") ? 0 : 1, false);
    assert(strstr(lv_textarea_get_text(area(5)), "ASCII preview"));
    if (!strcmp(mode, "paste-binary") || !strcmp(mode, "paste-empty")) {
        wait_logging(1, false); snapshot();
        printf("Prepared bytes published exactly: length=%zu, metadata_only=1\n", length); return true;
    }
    assert(!strcmp(mode, "paste-soak"));
    close_app(); finish(false); lv_mem_monitor_t before, after; lv_mem_monitor(&before);
    lv_array_t *events = &lv_display_get_default()->event_list.array;
    size_t first_display = lv_tlsf_block_size(events->data);
    uint32_t event_count = events->size, event_capacity = events->capacity;
    assert((size_t)event_capacity * events->element_size == 16);
    DWORD before_handles, after_handles; assert(GetProcessHandleCount(GetCurrentProcess(), &before_handles));
    LONG tasks = host_tasks_started;
    for (unsigned i = 0; i < 25; ++i) {
        open_app(); prepare_bytes(bytes, i % 2 ? 9 : 128); connect_session();
        click("PAYLOAD: BYTES"); assert(!lv_obj_has_flag(area(4), LV_OBJ_FLAG_HIDDEN)); click("PAYLOAD: TEXT");
        click("PUBLISH"); wait_action(true);
        check_published_bytes(bytes, i % 2 ? 9 : 128, (LONG)i + 1, "fixture/topic", 1, false);
        close_app(); finish(false);
    }
    lv_mem_monitor(&after); assert(GetProcessHandleCount(GetCurrentProcess(), &after_handles));
    size_t last_display = lv_tlsf_block_size(events->data);
    printf("PASTE_MEMORY free=%zu -> %zu used=%zu -> %zu display_array=%zu -> %zu requested=16 handles=%lu -> %lu\n",
           before.free_size, after.free_size, before.used_cnt, after.used_cnt, first_display, last_display,
           before_handles, after_handles); fflush(stdout);
    assert(first_display >= 16 && first_display - 16 <= 32 && last_display >= 16 && last_display - 16 <= 32);
    assert(before.free_size + first_display == after.free_size + last_display && before.used_cnt == after.used_cnt);
    assert(events->size == event_count && events->capacity == event_capacity && lv_mem_test() == LV_RESULT_OK);
    assert(before_handles == after_handles && tasks == host_tasks_started && !mqtt_host_allocations);
    return true;
}
static bool run_copy_case(void)
{
    if (strncmp(mode, "copy-", 5)) return false;
    uint8_t bytes[1024], sentinel[] = {0x00, 0xFF, 0xA5};
    for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)(i * 37U);
    payload_clipboard_store(sentinel, sizeof(sentinel));
    assert(lv_obj_has_state(button("COPY RX"), LV_STATE_DISABLED));
    forced_click("COPY RX"); check_clipboard(sentinel, sizeof(sentinel));
    esp_mqtt_event_t event = copy_message(bytes, !strcmp(mode, "copy-empty") ? 0 : 9);
    if (!strcmp(mode, "copy-fragments") || !strcmp(mode, "copy-home") || !strcmp(mode, "copy-abandoned")) {
        event.data_len = 0; event.total_data_len = 128; mqtt_host_data(&event);
        pump_for(220); forced_click("COPY RX"); check_clipboard(sentinel, sizeof(sentinel));
        assert(lv_obj_has_state(button("COPY RX"), LV_STATE_DISABLED));
        event.topic = NULL; event.topic_len = 0; event.data_len = 65;
        mqtt_host_data(&event); event.data = (char *)bytes + 65; event.current_data_offset = 65; event.data_len = 63;
        bool home = strcmp(mode, "copy-fragments") != 0;
        if (home) { mqtt_host_hold_stop = true; close_app(); wait_count(&mqtt_host_stop_entered, 1, false); open_app(); assert_stopping(); }
        if (strcmp(mode, "copy-abandoned")) mqtt_host_data(&event);
        if (home) { SetEvent(mqtt_host_stop_release); finish(true); }
        if (!strcmp(mode, "copy-abandoned")) {
            assert(lv_obj_has_state(button("COPY RX"), LV_STATE_DISABLED));
            forced_click("COPY RX"); check_clipboard(sentinel, sizeof(sentinel)); return true;
        }
        wait_text("Latest complete RX: 128 bytes"); expect_copy(bytes, 128);
        if (home) {
            close_app(); open_app(); expect_copy(bytes, 128);
            connect_session(); assert(lv_obj_has_state(button("COPY RX"), LV_STATE_DISABLED));
            check_clipboard(bytes, 128);
        }
        return true;
    }
    if (!strcmp(mode, "copy-bounds")) {
        for (int length = 0; length <= 128; ++length) {
            event = copy_message(bytes, length); mqtt_host_data(&event);
            if (!length) wait_text("Latest complete RX: 0 bytes");
            expect_copy(bytes, (size_t)length);
        }
        const int large[] = {129, 512, 513, 1024};
        for (unsigned i = 0; i < sizeof(large) / sizeof(large[0]); ++i) {
            event = copy_message(bytes, large[i]); mqtt_host_data(&event); pump_for(220);
            expect("Over 128 bytes; copy unavailable");
            assert(lv_obj_has_state(button("COPY RX"), LV_STATE_DISABLED));
            forced_click("COPY RX"); check_clipboard(bytes, 128);
        }
        event = copy_message(bytes, 1); mqtt_host_data(&event); wait_text("Latest complete RX: 1 bytes"); expect_copy(bytes, 1);
        return true;
    }
    if (!strcmp(mode, "copy-overflow")) {
        for (unsigned i = 0; i < 20; ++i) { bytes[0] = (uint8_t)i; event = copy_message(bytes, 4); mqtt_host_data(&event); }
        wait_text("Latest complete RX: 4 bytes"); expect_copy(bytes, 4); return true;
    }
    if (!strcmp(mode, "copy-clear")) {
        mqtt_host_data(&event); /* Clear before the timer drains the receive ring. */
        click("CLEAR"); pump_for(220); expect("No complete RX saved");
        assert(!strlen(lv_textarea_get_text(area(5))) && lv_obj_has_state(button("COPY RX"), LV_STATE_DISABLED));
        forced_click("COPY RX"); check_clipboard(sentinel, sizeof(sentinel));
        mqtt_host_data(&event); wait_text("Latest complete RX: 9 bytes"); expect_copy(bytes, 9);
        click("CLEAR"); forced_click("COPY RX"); check_clipboard(bytes, 9); return true;
    }
    mqtt_host_data(&event); wait_text(!strcmp(mode, "copy-empty") ? "Latest complete RX: 0 bytes" : "Latest complete RX: 9 bytes");
    expect_copy(bytes, (size_t)event.total_data_len);
    if (!strcmp(mode, "copy-binary")) {
        click("DISCONNECT"); finish(true); close_app(); byte_tool_show(content); lv_obj_update_layout(content);
        click("Paste hex");
        assert(!strcmp(lv_textarea_get_text(area(0)), "00 25 4A 6F 94 B9 DE 03 28"));
        wait_text("9 bytes | SUM8:");
        snapshot(); byte_tool_stop(); lv_obj_clean(content);
        printf("MQTT raw binary -> Byte Lab Hex handoff passed\n"); return true;
    }
    if (!strcmp(mode, "copy-latest")) {
        lv_textarea_set_text(area(3), "changed/topic");
        click("PUBLISH"); wait_action(true); expect_copy(bytes, 9);
        assert(mqtt_host_publishes == 1); /* A transmit must not replace the receive source. */
        return true;
    }
    if (!strcmp(mode, "copy-invalid")) {
        event.data_len = 4; event.total_data_len = 17; mqtt_host_data(&event);
        event.topic = NULL; event.topic_len = 0; event.data += 5; event.current_data_offset = 5; event.data_len = 12;
        mqtt_host_data(&event); pump_for(220); expect_copy(bytes, 9);
        mqtt_host_data(&event); mqtt_host_event(MQTT_EVENT_ERROR); pump_for(220);
        expect_copy(bytes, 9); expect("MQTT connection error"); return true;
    }
    if (!strcmp(mode, "copy-confirmation")) {
        click("DISCONNECT"); finish(true); lv_textarea_set_text(area(0), "mqtt://fixture.test:1883");
        LONG initialized = mqtt_host_initializations;
        click("CONNECT"); expect("Tap CONNECT again"); click("COPY RX"); click("CONNECT");
        assert(mqtt_host_initializations == initialized && !mqtt_tool_busy()); expect("Tap CONNECT again");
        click("CONNECT"); assert(mqtt_host_initializations == initialized + 1);
        return true;
    }
    if (!strcmp(mode, "copy-soak")) {
        click("CLEAR"); settle_ui(); close_app(); finish(false); lv_mem_monitor_t before, after;
        lv_mem_monitor(&before); DWORD handles_before, handles_after;
        lv_array_t *display_events = &lv_display_get_default()->event_list.array;
        size_t before_display_bytes = lv_tlsf_block_size(display_events->data);
        uint32_t before_events = display_events->size, before_capacity = display_events->capacity;
        assert((size_t)display_events->capacity * display_events->element_size == 16);
        assert(before_display_bytes >= 16 && before_display_bytes - 16 <= 32);
        assert(GetProcessHandleCount(GetCurrentProcess(), &handles_before)); LONG tasks = host_tasks_started;
        for (unsigned i = 0; i < 25; ++i) {
            open_app(); connect_session(); event = copy_message(bytes, 128); mqtt_host_data(&event);
            wait_text("Latest complete RX: 128 bytes"); expect_copy(bytes, 128);
            close_app(); finish(false);
        }
        lv_mem_monitor(&after); assert(GetProcessHandleCount(GetCurrentProcess(), &handles_after));
        printf("COPY_MEMORY free=%zu -> %zu used=%zu -> %zu display_array=%zu -> %zu requested=%zu handles=%lu -> %lu\n",
               before.free_size, after.free_size, before.used_cnt, after.used_cnt,
               before_display_bytes, lv_tlsf_block_size(display_events->data),
               (size_t)display_events->capacity * display_events->element_size, handles_before, handles_after);
        fflush(stdout);
        size_t after_display_bytes = lv_tlsf_block_size(display_events->data);
        assert(after_display_bytes >= 16 && after_display_bytes - 16 <= 32);
        assert(before.free_size + before_display_bytes == after.free_size + after_display_bytes && before.used_cnt == after.used_cnt);
        assert(display_events->size == before_events && display_events->capacity == before_capacity && lv_mem_test() == LV_RESULT_OK);
        assert(handles_before == handles_after && host_tasks_started == tasks);
        printf("25 copied receives: LVGL free=%zu -> %zu, handles=%lu -> %lu\n", before.free_size, after.free_size, handles_before, handles_after);
    }
    return true;
}

static bool run_receive_case(void)
{
    if (strncmp(mode, "receive-", 8)) return false;
    click("SD LOG\nOFF");
    esp_mqtt_event_t part = {.topic = "fixture/fragment", .topic_len = 16,
        .data = "FIXTURE_RX_SECRET", .data_len = 5, .total_data_len = 17,
        .msg_id = 42, .qos = 2, .retain = true};
    if (!strcmp(mode, "receive-empty")) {
        part.data = NULL; part.data_len = part.total_data_len = 0; part.qos = 0; part.retain = false;
        mqtt_host_data(&part); wait_logging(1, false); wait_text("Received 0 bytes on fixture/fragment");
        assert(strstr(lv_textarea_get_text(area(5)), "RX QoS 0 | 0 bytes")); return true;
    }
    if (!strcmp(mode, "receive-binary")) {
        char data[] = {'A', 0, '\r', '\n', '\t', 0x7f, (char)0xff, (char)0x80, 'Z'};
        part.data = data; part.data_len = part.total_data_len = sizeof(data);
        mqtt_host_data(&part); wait_logging(1, false); wait_text("Received 9 bytes on fixture/fragment");
        assert(strstr(lv_textarea_get_text(area(5)), "A.\n\n\t...Z")); expect("ASCII preview"); return true;
    }
    if (!strcmp(mode, "receive-cap")) {
        char data[1024]; memset(data, 'x', sizeof(data));
        for (unsigned i = 0; i < 3; i++) {
            int lengths[] = {512, 513, 1024};
            part.topic = "fixture/fragment"; part.topic_len = 16; part.current_data_offset = 0;
            part.data = data; part.data_len = 511; part.total_data_len = lengths[i];
            mqtt_host_data(&part);
            part.topic = NULL; part.topic_len = 0; part.data = data + 511; part.data_len = 1; part.current_data_offset = 511;
            mqtt_host_data(&part);
            if (lengths[i] > 512) {
                part.data = data + 512; part.current_data_offset = 512; part.data_len = lengths[i] - 512;
                mqtt_host_data(&part);
            }
            wait_logging((LONG)i + 1, false); pump_for(220);
            const char *history = lv_textarea_get_text(area(5));
            char expected[513]; memset(expected, 'x', 512); expected[512] = 0;
            assert(strstr(history, expected));
            assert(!strstr(strstr(history, expected) + 512, "x"));
            if (i) expect("preview capped");
            click("CLEAR");
        }
        return true;
    }
    if (!strcmp(mode, "receive-topic-view")) {
        char short_topic[128], long_topic[129]; memset(short_topic, 't', 127); short_topic[127] = 0;
        memset(long_topic, 't', 128); long_topic[128] = 0;
        const char *topics[] = {short_topic, long_topic, "fixture/\xc3\xa9", "fixture/line\r\n\t", "fixture/\"quoted\""};
        for (unsigned i = 0; i < sizeof(topics) / sizeof(topics[0]); i++) {
            part.topic = (char *)topics[i]; part.topic_len = (int)strlen(part.topic); part.data_len = 17;
            mqtt_host_data(&part); wait_logging((LONG)i + 1, false); pump_for(220);
            if (i >= 1 && i <= 3) expect("topic preview");
            else assert(!strstr(lv_textarea_get_text(area(5)), "topic preview"));
            click("CLEAR");
        }
        return true;
    }
    if (!strcmp(mode, "receive-invalid")) {
        for (unsigned i = 0; i < 14; i++) {
            esp_mqtt_event_t bad = part;
            switch (i) {
            case 0: mqtt_host_data(NULL); continue;
            case 1: bad.current_data_offset = -1; break;
            case 2: bad.total_data_len = -1; break;
            case 3: bad.data_len = -1; break;
            case 4: bad.topic_len = -1; break;
            case 5: bad.msg_id = -1; break;
            case 6: bad.qos = 3; break;
            case 7: bad.data_len = 18; break;
            case 8: bad.current_data_offset = 18; bad.data_len = 0; break;
            case 9: bad.data = NULL; break;
            case 10: bad.topic = NULL; break;
            case 11: bad.topic_len = 0; break;
            case 12: bad.current_data_offset = 1; break;
            case 13: bad.topic = NULL; bad.topic_len = 0; bad.data_len = 17; break;
            }
            mqtt_host_data(&bad);
        }
        for (unsigned i = 0; i < 11; i++) {
            mqtt_host_data(&part);
            esp_mqtt_event_t tail = part; tail.topic = NULL; tail.topic_len = 0;
            tail.data += 5; tail.data_len = 12; tail.current_data_offset = 5;
            esp_mqtt_event_t bad = tail;
            switch (i) {
            case 0: bad.current_data_offset = 6; bad.data_len = 11; break;
            case 1: bad.current_data_offset = 4; bad.data_len = 13; break;
            case 2: bad.total_data_len = 18; bad.data_len = 13; break;
            case 3: bad.msg_id++; break;
            case 4: bad.qos = 1; break;
            case 5: bad.retain = false; break;
            case 6: bad.topic = part.topic; bad.topic_len = part.topic_len; break;
            case 7: bad.current_data_offset = 0; bad.data_len = 17; break;
            case 8: bad.data = NULL; break;
            case 9: bad.data_len = 0; break;
            case 10: bad.current_data_offset = INT_MAX; bad.data_len = 1; bad.total_data_len = INT_MAX; break;
            }
            mqtt_host_data(&bad); mqtt_host_data(&tail);
        }
        pump_for(220); expect("Ignored invalid or incomplete MQTT receive");
        assert(!mqtt_host_appends && !strlen(lv_textarea_get_text(area(5))) && mqtt_host_allocations == 3);
        part.data_len = 17; mqtt_host_data(&part); wait_logging(1, false); wait_text("Received 17 bytes on fixture/fragment");
        return true;
    }
    if (!strcmp(mode, "receive-replace")) {
        mqtt_host_data(&part);
        part.topic = "fixture/replacement"; part.topic_len = 19; part.msg_id++;
        part.data = "replacement body"; part.data_len = part.total_data_len = 16;
        mqtt_host_data(&part); wait_logging(1, false); wait_text("Received 16 bytes on fixture/replacement");
        assert(!strstr(lv_textarea_get_text(area(5)), "FIXTURE_RX_SECRET")); return true;
    }
    if (!strcmp(mode, "receive-interruption")) {
        mqtt_host_data(&part); mqtt_host_event(MQTT_EVENT_ERROR);
        esp_mqtt_event_t tail = part; tail.topic = NULL; tail.topic_len = 0;
        tail.data += 5; tail.data_len = 12; tail.current_data_offset = 5;
        mqtt_host_data(&tail); mqtt_host_data(&part); mqtt_host_event(MQTT_EVENT_DISCONNECTED);
        mqtt_host_data(&tail); mqtt_host_data(&part); mqtt_host_event(MQTT_EVENT_CONNECTED); mqtt_host_data(&tail);
        pump_for(220); assert(!mqtt_host_appends && !strlen(lv_textarea_get_text(area(5))));
        part.data_len = 17; mqtt_host_data(&part); wait_logging(1, false); wait_text("Received 17 bytes on fixture/fragment");
        return true;
    }
    if (!strcmp(mode, "receive-soak")) {
        for (unsigned i = 0; i < 5; i++) {
            mqtt_host_data(&part); esp_mqtt_event_t tail = part; tail.topic = NULL; tail.topic_len = 0;
            tail.data += 5; tail.data_len = 12; tail.current_data_offset = 5; mqtt_host_data(&tail);
            wait_logging((LONG)i + 1, false); pump_for(220);
        }
        click("CLEAR"); settle_ui(); lv_mem_monitor_t live_before, live_after, before, after;
        lv_mem_monitor(&live_before); close_app(); finish(false); lv_mem_monitor(&before);
        DWORD handles_before, handles_after; assert(GetProcessHandleCount(GetCurrentProcess(), &handles_before));
        LONG tasks = host_tasks_started; open_app(); connect_session(); click("SD LOG\nOFF");
        for (unsigned i = 0; i < 50; i++) {
            mqtt_host_data(&part); esp_mqtt_event_t tail = part; tail.topic = NULL; tail.topic_len = 0;
            tail.data += 5; tail.data_len = 12; tail.current_data_offset = 5; mqtt_host_data(&tail);
            wait_logging((LONG)i + 6, false); pump_for(220);
        }
        click("CLEAR"); settle_ui(); lv_mem_monitor(&live_after);
        assert(live_before.used_cnt == live_after.used_cnt && mqtt_host_allocations == 3);
        close_app(); finish(false); lv_mem_monitor(&after); assert(GetProcessHandleCount(GetCurrentProcess(), &handles_after));
        assert(before.free_size == after.free_size && before.used_cnt == after.used_cnt);
        assert(handles_before == handles_after && host_tasks_started == tasks);
        printf("50 fragmented receives: LVGL free=%zu -> %zu, handles=%lu -> %lu, standby workers=%ld\n",
               before.free_size, after.free_size, (unsigned long)handles_before, (unsigned long)handles_after, host_active_tasks);
        return true;
    }
    if (!strcmp(mode, "receive-zero-start")) { part.data = NULL; part.data_len = 0; }
    mqtt_host_data(&part); pump_for(220);
    assert(!mqtt_host_appends && !strlen(lv_textarea_get_text(area(5))));
    bool home = !strcmp(mode, "receive-home") || !strcmp(mode, "receive-abandoned");
    if (home) { mqtt_host_hold_stop = true; close_app(); wait_count(&mqtt_host_stop_entered, 1, false); open_app(); assert_stopping(); }
    if (strcmp(mode, "receive-abandoned")) {
        int initial = part.data_len;
        part.topic = NULL; part.topic_len = 0;
        part.data = &"FIXTURE_RX_SECRET"[initial]; part.data_len = 17 - initial; part.current_data_offset = initial;
        mqtt_host_data(&part);
    }
    if (home) {
        SetEvent(mqtt_host_stop_release); finish(true);
        assert(mqtt_host_appends == (!strcmp(mode, "receive-abandoned") ? 0 : 1)); return true;
    }
    wait_logging(1, false); wait_text("Received 17 bytes on fixture/fragment");
    assert(strstr(lv_textarea_get_text(area(5)), "fixture/fragment"));
    assert(strstr(lv_textarea_get_text(area(5)), "FIXTURE_RX_SECRET"));
    assert(strstr(lv_textarea_get_text(area(5)), "RX QoS 2 | retained | 17 bytes"));
    esp_mqtt_event_t duplicate = part; mqtt_host_data(&duplicate);
    wait_text("Ignored invalid or incomplete MQTT receive"); assert(mqtt_host_appends == 1);
    return true;
}
static bool run_storage_case(void)
{
    if (strncmp(mode, "active-log-", 11)) return false;
    click("SD LOG\nOFF");
    if (!strcmp(mode, "active-log-before-ui")) {
        mqtt_host_receive("fixture/log/accepted", "FIXTURE_RX_SECRET"); wait_logging(1, false);
        assert(!strlen(lv_textarea_get_text(area(5))));
        click("SD LOG\nON"); mqtt_host_receive("fixture/log/off", "FIXTURE_RX_SECRET");
        pump_for(220); assert(mqtt_host_appends == 1); return true;
    }
    if (!strcmp(mode, "active-log-ui-drop")) {
        for (unsigned i = 0; i < 12; i++) {
            char topic[32]; snprintf(topic, sizeof(topic), "fixture/burst/%u", i);
            mqtt_host_receive(topic, "FIXTURE_RX_SECRET"); wait_logging((LONG)i + 1, false);
        }
        pump_for(220); expect("Dropped 4 UI events"); assert(mqtt_host_appends == 12); return true;
    }
    if (!strcmp(mode, "active-log-error")) {
        mqtt_host_file_fault(MQTT_FILE_SYNC); mqtt_host_receive("fixture/log/failed", "FIXTURE_RX_SECRET");
        wait_count(&reported_errors, 1, false); assert(!mqtt_host_open_files && mqtt_host_allocations == 3);
        pump_for(220); expect("not confirmed saved"); assert(lv_obj_has_state(button("SD LOG\nOFF"), LV_STATE_DISABLED));
        forced_click("SD LOG\nOFF"); mqtt_host_receive("fixture/log/ignored", "FIXTURE_RX_SECRET");
        pump_for(220); assert(mqtt_host_appends == 1);
        click("DISCONNECT"); finish(true); expect("not confirmed saved"); assert(reported_errors == 1);
        connect_session(); click("SD LOG\nOFF"); mqtt_host_receive("fixture/log/recovered", "FIXTURE_RX_SECRET");
        wait_logging(2, true); assert(reported_errors == 1);
        click("DISCONNECT"); finish(true); expect("Disconnected; form password cleared"); return true;
    }
    if (!strcmp(mode, "active-log-error-report")) {
        hold_report = true; mqtt_host_file_fault(MQTT_FILE_SYNC);
        mqtt_host_receive("fixture/log/failed", "FIXTURE_RX_SECRET"); wait_count(&reporting, 1, false);
        assert(!mqtt_host_open_files && mqtt_host_allocations == 3 && mqtt_tool_busy());
        pump_for(220); expect("not confirmed saved");
        click("PUBLISH"); assert(mqtt_host_allocations == 4 && !mqtt_host_actions_entered);
        close_app(); open_app(); assert_stopping();
        SetEvent(report_release); finish(true); assert(reported_errors == 1 && !mqtt_host_actions_entered);
        expect("not confirmed saved"); return true;
    }
    bool error_home = !strcmp(mode, "active-log-error-home");
    if (error_home) { mqtt_host_hold_log_dispatch = true; mqtt_host_file_fault(MQTT_FILE_SYNC); }
    else if (!strcmp(mode, "active-log-close")) mqtt_host_hold_close = true;
    else mqtt_host_hold_sync = true;
    mqtt_host_receive(error_home ? "fixture/log/failed" : "fixture/burst/0", "FIXTURE_RX_SECRET");
    if (error_home) wait_count(&mqtt_host_log_dispatch_entered, 1, false);
    else {
        wait_count(mqtt_host_hold_close ? &mqtt_host_close_entered : &mqtt_host_sync_entered, 1, false);
        assert(mqtt_host_open_files == 1);
    }
    int64_t began = esp_timer_get_time(); pump_for(220);
    assert(esp_timer_get_time() - began < 400000 && mqtt_tool_busy());
    if (!strcmp(mode, "active-log-overflow")) {
        for (unsigned i = 1; i <= 20; i++) {
            char topic[32]; snprintf(topic, sizeof(topic), "fixture/burst/%u", i);
            mqtt_host_receive(topic, "FIXTURE_RX_SECRET");
        }
        pump_for(220); expect("Dropped 12 SD metadata entries"); assert(mqtt_host_appends == 1 && mqtt_host_allocations == 3);
    } else if (!strcmp(mode, "active-log-off")) {
        click("SD LOG\nON"); mqtt_host_receive("fixture/log/off", "FIXTURE_RX_SECRET");
        click("CLEAR"); assert(mqtt_host_open_files == 1 && mqtt_tool_busy());
    } else if (!strcmp(mode, "active-log-action")) {
        click("PUBLISH"); assert(!mqtt_host_actions_entered && mqtt_host_allocations == 4);
        forced_click("PUBLISH"); forced_click("SUBSCRIBE"); assert(!mqtt_host_actions_entered);
        lv_textarea_set_text(area(3), "changed/topic"); lv_textarea_set_text(area(4), "changed payload");
    } else if (!strcmp(mode, "active-log-sync") || !strcmp(mode, "active-log-close")) {
        click("CLEAR"); assert(mqtt_host_open_files == 1 && mqtt_tool_busy());
    }
    bool home = !strcmp(mode, "active-log-home") || !strcmp(mode, "active-log-overflow") || error_home;
    bool stop = !strcmp(mode, "active-log-stop");
    if (home) { close_app(); open_app(); assert_stopping(); }
    else if (stop) {
        began = esp_timer_get_time(); click("DISCONNECT"); assert(esp_timer_get_time() - began < 100000); assert_stopping();
    }
    assert(!mqtt_host_stop_entered && !mqtt_host_destroy_entered);
    if (error_home) { mqtt_host_hold_log_dispatch = false; SetEvent(mqtt_host_log_dispatch_release); }
    else SetEvent(mqtt_host_file_release);
    if (home || stop) {
        finish(true);
        expect(error_home ? "not confirmed saved" : !strcmp(mode, "active-log-overflow") ?
               "SD metadata omitted: 12 entries" : "Disconnected; form password cleared");
        assert(reported_errors == (error_home ? 1 : 0));
    } else {
        if (!strcmp(mode, "active-log-action")) {
            wait_action(true); assert(mqtt_host_actions_entered == 1);
            assert(!strcmp(mqtt_host_actions[0].topic, "fixture/topic") && !strcmp(mqtt_host_actions[0].payload, "FIXTURE_PAYLOAD_SECRET"));
        }
        wait_logging(!strcmp(mode, "active-log-action") ? 2 : 1, true);
    }
    return true;
}
static void change_qos(unsigned target)
{
    for (unsigned qos = 1; qos != target; qos = (qos + 1) % 3) {
        char text[16]; snprintf(text, sizeof(text), "QoS %u", qos); click(text);
    }
}
static size_t wire_packet(const char *fixture, uint8_t *packet, size_t capacity)
{
    char path[1200]; snprintf(path, sizeof(path), "%s/wire-fixtures/%s.bin", output, fixture);
    FILE *file = fopen(path, "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long length = ftell(file);
    assert(length > 0 && (size_t)length <= capacity && !fseek(file, 0, SEEK_SET));
    assert(fread(packet, 1, (size_t)length, file) == (size_t)length && !fclose(file));
    return (size_t)length;
}
static esp_mqtt_event_t wire_first_event(uint8_t *packet, size_t length, size_t first)
{
    assert(first <= length && mqtt_has_valid_msg_hdr(packet, first) && mqtt_header_complete(packet, first));
    size_t topic_length = first, data_length = first;
    char *topic = mqtt_get_publish_topic(packet, &topic_length);
    char *data = mqtt_get_publish_data(packet, &data_length);
    size_t total = mqtt_get_total_length(packet, first, NULL);
    assert(topic && (!data_length || data) && total == length && data_length + total - first <= INT_MAX);
    /* Selected 3.1.1 event mapping from the pinned SDK's deliver_publish().
     * SDK codec functions run above; client task/transport dispatch is modeled. */
    esp_mqtt_event_t event = {.topic = topic, .topic_len = (int)topic_length,
        .data = data_length ? data : NULL, .data_len = (int)data_length,
        .total_data_len = (int)(data_length + total - first), .current_data_offset = 0,
        .msg_id = mqtt_get_id(packet, first), .qos = mqtt_get_qos(packet),
        .retain = mqtt_get_retain(packet), .dup = mqtt_get_dup(packet)};
    printf("SDK decoded first event: wire=%zu first=%zu topic=%d data=%d total=%d id=%d QoS=%d retain=%d dup=%d\n",
           length, first, event.topic_len, event.data_len, event.total_data_len, event.msg_id,
           event.qos, event.retain, event.dup); fflush(stdout);
    return event;
}
static bool run_wire_case(void)
{
    if (strncmp(mode, "wire-", 5)) return false;
    if (!strcmp(mode, "wire-header-flags")) {
        const uint8_t valid[] = {0x10, 0x20, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x3a, 0x3b,
            0x3c, 0x3d, 0x40, 0x50, 0x62, 0x70, 0x82, 0x90, 0xa2, 0xb0, 0xc0, 0xd0, 0xe0};
        for (unsigned value = 0; value <= 255; value++) {
            uint8_t byte = (uint8_t)value; bool expected = false;
            for (size_t i = 0; i < sizeof(valid); i++) expected |= byte == valid[i];
            assert((mqtt_has_valid_msg_hdr(&byte, 1) != 0) == expected);
            assert(!mqtt_has_valid_msg_hdr(&byte, 0));
        }
        printf("All 256 fixed-header bytes match independent MQTT 3.1.1 flag fixtures\n"); return true;
    }
    if (!strcmp(mode, "wire-lengths")) {
        const struct { uint8_t header[5]; size_t size, remaining; } fixtures[] = {
            {{0x30, 0x00}, 2, 0}, {{0x30, 0x7f}, 2, 127}, {{0x30, 0x80, 0x01}, 3, 128},
            {{0x30, 0xff, 0x7f}, 3, 16383}, {{0x30, 0x80, 0x80, 0x01}, 4, 16384},
            {{0x30, 0xff, 0xff, 0x7f}, 4, 2097151}, {{0x30, 0x80, 0x80, 0x80, 0x01}, 5, 2097152},
            {{0x30, 0xff, 0xff, 0xff, 0x7f}, 5, 268435455},
        };
        for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
            int fixed = 0;
            assert(mqtt_get_total_length(fixtures[i].header, fixtures[i].size, &fixed) ==
                   fixtures[i].remaining + fixtures[i].size);
            assert(fixed == (int)fixtures[i].size);
        }
        printf("Eight remaining-length boundary fixtures decoded (one to four encoded bytes)\n"); return true;
    }
    uint8_t packet[2048], buffer[128];
    mqtt_connection_t connection = {.buffer = buffer, .buffer_length = sizeof(buffer)};
    uint16_t id = 0;
    if (!strcmp(mode, "wire-connect")) {
        size_t length = wire_packet(mode, packet, sizeof(packet));
        mqtt_connect_info_t info = {.client_id = "tab5", .username = "u", .password = "p",
            .keepalive = 60, .clean_session = 1, .protocol_ver = MQTT_PROTOCOL_V_3_1_1};
        mqtt_message_t *message = mqtt_msg_connect(&connection, &info);
        assert(message->length == length && !memcmp(message->data, packet, length));
        printf("SDK CONNECT: %zu exact fixture bytes (standalone codec input)\n", length); return true;
    }
    if (!strcmp(mode, "wire-subscribe") || !strncmp(mode, "wire-publish-", 13)) {
        bool subscribe = !strcmp(mode, "wire-subscribe"), empty = !strcmp(mode, "wire-publish-empty");
        unsigned qos = subscribe ? 2 : empty ? 1 : (unsigned)(mode[strlen(mode) - 1] - '0');
        size_t length = wire_packet(mode, packet, sizeof(packet));
        lv_textarea_set_text(area(3), subscribe ? "site/+/state" : "a/b");
        lv_textarea_set_text(area(4), empty ? "" : "\xc2\xb5Z");
        change_qos(qos); if (!subscribe && !empty) click("RETAIN\nOFF");
        if (!subscribe) click("SD LOG\nOFF");
        click(subscribe ? "SUBSCRIBE" : "PUBLISH"); wait_action(true);
        assert(mqtt_host_actions_entered == 1);
        const mqtt_host_action_record_t *action = &mqtt_host_actions[0];
        mqtt_message_t *message;
        if (subscribe) {
            esp_mqtt_topic_t topic = {.filter = action->topic, .qos = action->qos};
            message = mqtt_msg_subscribe(&connection, &topic, 1, &id);
        } else {
            assert(action->store && action->length == (empty ? 0 : 3));
            message = mqtt_msg_publish(&connection, action->topic, action->payload, action->length,
                                       action->qos, action->retained, &id);
            wait_logging(1, true);
        }
        assert(id == (qos ? 0x1234 : 0));
        assert(message->length == length && !memcmp(message->data, packet, length));
        printf("SDK %s: %zu exact fixture bytes from captured app action, id=%u\n",
               subscribe ? "SUBSCRIBE" : "PUBLISH", length, id); return true;
    }
    click("SD LOG\nOFF");
    if (!strcmp(mode, "wire-receive-small")) {
        const char *fixtures[] = {"wire-receive-empty", "wire-receive-binary", "wire-receive-dup"};
        for (unsigned i = 0; i < 3; i++) {
            size_t length = wire_packet(fixtures[i], packet, sizeof(packet));
            esp_mqtt_event_t event = wire_first_event(packet, length, length);
            assert(event.qos == (int)i && event.retain && event.dup == (i == 2));
            assert(event.total_data_len == (i ? 9 : 0) && event.msg_id == (i ? 0x1234 : 0));
            mqtt_host_data(&event); wait_logging((LONG)i + 1, true);
            wait_text(i ? "Received 9 bytes on fixture/wire" : "Received 0 bytes on fixture/wire");
            if (i) assert(strstr(lv_textarea_get_text(area(5)), "A.\n\n\t...Z"));
        }
        return true;
    }
    size_t length = wire_packet(mode, packet, sizeof(packet));
    bool zero = !strcmp(mode, "wire-receive-zero-start"), large = !strcmp(mode, "wire-receive-fragments");
    bool home = !strcmp(mode, "wire-receive-home") || !strcmp(mode, "wire-receive-abandoned");
    size_t first = zero ? 1024 : large ? 30 : 23;
    esp_mqtt_event_t event = wire_first_event(packet, length, first);
    assert(event.qos == 2 && event.retain && event.msg_id == 0x1234);
    assert(event.total_data_len == (large ? 1024 : 17));
    assert(event.data_len == (zero ? 0 : large ? 11 : 5));
    mqtt_host_data(&event); pump_for(220);
    assert(!mqtt_host_appends && !strlen(lv_textarea_get_text(area(5))));
    if (home) { mqtt_host_hold_stop = true; close_app(); wait_count(&mqtt_host_stop_entered, 1, false); open_app(); assert_stopping(); }
    if (strcmp(mode, "wire-receive-abandoned")) {
        size_t read = first;
        while (read < length) {
            size_t chunk = length - read; if (chunk > 17) chunk = 17;
            event.current_data_offset += event.data_len;
            event.topic = NULL; event.topic_len = 0; event.data = (char *)(packet + read); event.data_len = (int)chunk;
            mqtt_host_data(&event); read += chunk;
        }
    }
    if (home) {
        SetEvent(mqtt_host_stop_release); finish(true);
        assert(mqtt_host_appends == (!strcmp(mode, "wire-receive-abandoned") ? 0 : 1)); return true;
    }
    wait_logging(1, true);
    wait_text(zero ? "Received 17 bytes on topic preview t" : "Received 1024 bytes on fixture/wire");
    assert(strstr(lv_textarea_get_text(area(5)), "FIXTURE_RX_SECRET"));
    expect(zero ? "topic preview" : "preview capped");
    assert(mqtt_host_appends == 1); return true;
}
static bool run_action_case(void)
{
    bool subscribe = !strncmp(mode, "subscribe-", 10);
    bool publish = !strncmp(mode, "publish-", 8);
    bool stall = strstr(mode, "-stall") || strstr(mode, "-home") || strstr(mode, "-stop");
    if ((subscribe || publish) && stall) {
        bool leaving = strstr(mode, "-home") != NULL, stopping = strstr(mode, "-stop") != NULL;
        mqtt_host_hold_action = true;
        if (publish) click("RETAIN\nOFF");
        if (publish && (leaving || stopping)) click("SD LOG\nOFF");
        int64_t began = esp_timer_get_time(); click(publish ? "PUBLISH" : "SUBSCRIBE");
        assert(esp_timer_get_time() - began < 100000); wait_count(&mqtt_host_actions_entered, 1, false);
        assert(mqtt_host_actions_active == 1 && mqtt_host_allocations == 4);
        assert(lv_obj_has_state(button("PUBLISH"), LV_STATE_DISABLED));
        assert(lv_obj_has_state(button("SUBSCRIBE"), LV_STATE_DISABLED));
        forced_click("PUBLISH"); forced_click("SUBSCRIBE"); assert(mqtt_host_actions_entered == 1);
        lv_textarea_set_text(area(3), "changed/topic"); lv_textarea_set_text(area(4), "changed payload");
        change_qos(2); if (publish) click("RETAIN\nON");
        if (leaving) { close_app(); open_app(); assert_stopping(); }
        else if (stopping) { click("DISCONNECT"); assert_stopping(); }
        else { pump_for(220); expect("in the background"); snapshot(); }
        assert(!mqtt_host_stop_entered && !mqtt_host_destroy_entered && mqtt_tool_busy());
        mqtt_host_action_record_t *record = &mqtt_host_actions[0];
        assert(record->publish == publish && record->qos == 1 && !strcmp(record->topic, "fixture/topic"));
        if (publish) assert(record->retained == 1 && record->length == 22 && !strcmp(record->payload, "FIXTURE_PAYLOAD_SECRET") && record->store);
        SetEvent(mqtt_host_action_release);
        if (leaving || stopping) { finish(true); expect("Disconnected; form password cleared"); }
        else { wait_action(true); expect(publish ? "Publish queued on fixture/topic" : "Subscribing to fixture/topic"); }
        return true;
    }
    if (!strcmp(mode, "queued-subscribe") || !strcmp(mode, "queued-publish")) {
        mqtt_host_hold_dispatch = true; click(!strcmp(mode, "queued-publish") ? "PUBLISH" : "SUBSCRIBE");
        wait_count(&mqtt_host_dispatch_entered, 1, false); assert(!mqtt_host_actions_entered);
        close_app(); open_app(); assert_stopping(); SetEvent(mqtt_host_dispatch_release);
        finish(true); assert(!mqtt_host_actions_entered && !mqtt_host_publishes && !mqtt_host_subscribes);
        expect("Disconnected; form password cleared"); return true;
    }
    if (!strcmp(mode, "action-allocation")) {
        mqtt_host_fail_allocation = 1; click("PUBLISH"); expect("action allocation failed");
        assert(!mqtt_host_actions_entered && mqtt_host_allocations == 3);
        click("PUBLISH"); wait_action(true); assert(mqtt_host_actions_entered == 1); expect("Publish queued"); return true;
    }
    if (!strcmp(mode, "subscribe-error") || !strcmp(mode, "publish-error") || !strcmp(mode, "publish-full")) {
        click(subscribe ? "SUBSCRIBE" : "PUBLISH"); wait_action(true);
        expect(!strcmp(mode, "publish-full") ? "outbox full" : subscribe ? "Could not queue subscription" : "Could not queue publish");
        assert(!strlen(lv_textarea_get_text(area(5))) && mqtt_host_actions_entered == 1); return true;
    }
    if (!strcmp(mode, "publish-qos0") || !strcmp(mode, "publish-qos2")) {
        unsigned qos = !strcmp(mode, "publish-qos0") ? 0 : 2; change_qos(qos);
        if (qos == 2) click("RETAIN\nOFF");
        click("SD LOG\nOFF");
        click("PUBLISH"); wait_action(true); expect("Publish queued");
        if (!qos) expect("message 0");
        assert(mqtt_host_actions_entered == 1 && mqtt_host_actions[0].qos == (int)qos && mqtt_host_actions[0].store);
        assert(mqtt_host_actions[0].retained == (qos == 2)); return true;
    }
    if (!strcmp(mode, "publish-bounds")) {
        char payload[517]; memset(payload, 'x', 513); payload[513] = 0;
        lv_textarea_set_text(area(4), payload); assert(strlen(lv_textarea_get_text(area(4))) == 513);
        click("PUBLISH"); expect("exceeds 512 bytes"); assert(!mqtt_host_actions_entered);
        payload[512] = 0; lv_textarea_set_text(area(4), payload); click("PUBLISH"); wait_action(true);
        assert(mqtt_host_actions_entered == 1 && mqtt_host_actions[0].length == 512 && !strcmp(mqtt_host_actions[0].payload, payload));
        for (unsigned i = 0; i < 129; i++) memcpy(payload + i * 4, "\xf0\x9f\x98\x80", 4);
        payload[516] = 0; lv_textarea_set_text(area(4), payload); assert(strlen(lv_textarea_get_text(area(4))) == 516);
        click("PUBLISH"); expect("exceeds 512 bytes"); assert(mqtt_host_actions_entered == 1);
        payload[512] = 0; lv_textarea_set_text(area(4), payload); click("PUBLISH"); wait_action(true);
        assert(mqtt_host_actions_entered == 2 && mqtt_host_actions[1].length == 512 && !strcmp(mqtt_host_actions[1].payload, payload));
        lv_textarea_set_text(area(4), ""); click("PUBLISH"); wait_action(true);
        assert(mqtt_host_actions_entered == 3 && !mqtt_host_actions[2].length && !mqtt_host_actions[2].payload[0]);
        char topic[129]; memset(topic, 't', 128); topic[128] = 0; lv_textarea_set_text(area(3), topic);
        assert(strlen(lv_textarea_get_text(area(3))) == 128); click("SUBSCRIBE"); expect("1-127");
        click("PUBLISH"); expect("1-127"); assert(mqtt_host_actions_entered == 3);
        topic[127] = 0; lv_textarea_set_text(area(3), topic); click("SUBSCRIBE"); wait_action(true);
        assert(mqtt_host_actions_entered == 4 && !strcmp(mqtt_host_actions[3].topic, topic)); return true;
    }
    if (!strcmp(mode, "pending-action")) {
        click("PUBLISH"); wait_action(false); assert(lv_obj_has_state(button("PUBLISH"), LV_STATE_DISABLED));
        forced_click("PUBLISH"); forced_click("SUBSCRIBE"); assert(mqtt_host_actions_entered == 1);
        wait_action(true); expect("Publish queued on fixture/topic"); return true;
    }
    if (!strcmp(mode, "actions-repeat")) {
        /* Exercise scrolling history and both action types before comparing
         * allocations; LVGL retains scroll state for a widget's life and its
         * allocator may retain different small tails in resized text blocks.
         * Compare exact free bytes only after Home releases those widgets. */
        for (unsigned i = 0; i < 10; i++) { click(i % 2 ? "PUBLISH" : "SUBSCRIBE"); wait_action(true); }
        click("CLEAR"); settle_ui();
        lv_mem_monitor_t before, after, warm_ui, used_ui;
        lv_mem_monitor(&warm_ui); close_app(); finish(false);
        lv_mem_monitor(&before); DWORD handles_before, handles_after;
        assert(GetProcessHandleCount(GetCurrentProcess(), &handles_before)); LONG tasks = host_tasks_started;
        open_app(); connect_session();
        for (unsigned i = 0; i < 50; i++) { click(i % 2 ? "PUBLISH" : "SUBSCRIBE"); wait_action(true); }
        click("CLEAR"); settle_ui(); lv_mem_monitor(&used_ui);
        assert(warm_ui.used_cnt == used_ui.used_cnt && mqtt_host_allocations == 3);
        assert(mqtt_host_actions_entered == 60);
        close_app(); finish(false); lv_mem_monitor(&after);
        assert(GetProcessHandleCount(GetCurrentProcess(), &handles_after));
        printf("Action memory after Home: free=%zu -> %zu, allocations=%zu -> %zu; active UI allocations=%zu -> %zu\n",
               before.free_size, after.free_size, before.used_cnt, after.used_cnt,
               warm_ui.used_cnt, used_ui.used_cnt); fflush(stdout);
        assert(before.free_size == after.free_size && before.used_cnt == after.used_cnt);
        assert(handles_before == handles_after && host_tasks_started == tasks && !mqtt_host_allocations);
        printf("50 actions: LVGL free=%zu -> %zu, handles=%lu -> %lu, standby workers=%ld\n",
               before.free_size, after.free_size, (unsigned long)handles_before, (unsigned long)handles_after, host_active_tasks);
        return true;
    }
    return false;
}
static void run_case(void)
{
    if (!strcmp(mode, "idle")) {
        forced_click("SUBSCRIBE"); forced_click("PUBLISH");
        assert(!mqtt_host_subscribes && !mqtt_host_publishes && !mqtt_tool_busy()); geometry(); snapshot(); return;
    }
    if (!strcmp(mode, "task-failure")) {
        assert(lv_obj_has_state(button("CONNECT"), LV_STATE_DISABLED)); forced_click("CONNECT");
        assert(!mqtt_host_initializations && !host_active_tasks); close_app(); open_app();
        assert(!lv_obj_has_state(button("CONNECT"), LV_STATE_DISABLED)); return;
    }
    if (!strcmp(mode, "buffer-failure")) {
        mqtt_host_fail_allocation = 1;
        click("CONNECT"); expect("buffer allocation failed"); assert(!mqtt_host_initializations);
        pump_for(220); assert(mqtt_host_allocations == 3); return;
    }
    if (run_connection_case()) return;
    if (run_profile_case()) return;
    if (run_paste_case()) return;
    if (!strcmp(mode, "lifecycle") || !strcmp(mode, "logged-lifecycle")) { lifecycle(); return; }
    connect_session();
    if (strstr(mode, "-failure")) {
        assert(!mqtt_tool_busy() && !mqtt_host_clients); expect(!strcmp(mode, "start-failure") ? "Could not start" : "Could not initialize"); return;
    }
    if (run_copy_case() || run_wire_case() || run_receive_case() || run_storage_case() || run_action_case()) return;
    if (!strcmp(mode, "subscribe-publish")) {
        click("SUBSCRIBE"); wait_action(true); click("PUBLISH"); wait_action(true);
        assert(mqtt_host_subscribes == 1 && mqtt_host_publishes == 1);
        mqtt_host_event(MQTT_EVENT_SUBSCRIBED); mqtt_host_event(MQTT_EVENT_PUBLISHED); pump_for(220);
        expect("Publish acknowledged"); click("DISCONNECT"); finish(true); return;
    }
    if (!strcmp(mode, "pending-stop")) {
        click("DISCONNECT"); finish(false);
        assert(!mqtt_tool_busy() && !mqtt_host_allocations);
        expect("Disconnecting in the background");
        assert(lv_obj_has_state(button("STOPPING"), LV_STATE_DISABLED));
        LONG initialized = mqtt_host_initializations;
        forced_click("STOPPING"); forced_click("PUBLISH"); forced_click("SUBSCRIBE");
        forced_click("SD LOG\nOFF");
        expect("Disconnecting in the background");
        assert(lv_obj_has_state(button("STOPPING"), LV_STATE_DISABLED));
        assert(!mqtt_host_subscribes && !mqtt_host_publishes && mqtt_host_initializations == initialized);
        finish(true); expect("Disconnected; form password cleared");
        assert(find_text(content, "SD LOG\nOFF", true)); return;
    }
    if (!strcmp(mode, "pending-connect-refresh")) {
        click("DISCONNECT"); finish(false);
        assert(lv_obj_has_state(button("STOPPING"), LV_STATE_DISABLED));
        click("QoS 1"); assert(!lv_obj_has_state(button("CONNECT"), LV_STATE_DISABLED));
        click("CONNECT"); assert(mqtt_host_clients == 1 && mqtt_tool_busy()); pump_for(220);
        if (find_text(content, "Disconnected; form password cleared", true))
            fputs("Old cleanup status replaced the new client's connecting status\n", stderr);
        expect("Connecting with certificate verification");
        assert(!find_text(content, "Disconnected;", false));
        mqtt_host_event(MQTT_EVENT_CONNECTED); pump_for(220); expect("Connected with TLS verification");
        return;
    }
    bool with_log = (!strncmp(mode, "log-", 4) && strcmp(mode, "log-optout")) || !strcmp(mode, "error-report");
    if (with_log) click("SD LOG\nOFF");
    if (strcmp(mode, "log-empty")) queue_messages(with_log);
    InterlockedExchange(&mqtt_host_sync_entered, 0);
    InterlockedExchange(&mqtt_host_close_entered, 0);
    if (!strcmp(mode, "stop") || !strcmp(mode, "home") || !strcmp(mode, "late-events")) mqtt_host_hold_stop = true;
    if (!strcmp(mode, "destroy")) mqtt_host_hold_destroy = true;
    if (!strcmp(mode, "log-sync") || !strcmp(mode, "log-home") || !strcmp(mode, "log-reentry")) mqtt_host_hold_sync = true;
    if (!strcmp(mode, "log-close")) mqtt_host_hold_close = true;
    if (!strcmp(mode, "error-report")) { hold_report = true; mqtt_host_file_fault(MQTT_FILE_SYNC); }
    if (!strcmp(mode, "log-repair-error")) mqtt_host_file_fault(MQTT_FILE_REPAIR);
    if (!strcmp(mode, "log-open-error")) mqtt_host_file_fault(MQTT_FILE_OPEN);
    if (!strcmp(mode, "log-flush-error")) mqtt_host_file_fault(MQTT_FILE_FLUSH);
    if (!strcmp(mode, "log-sync-error")) mqtt_host_file_fault(MQTT_FILE_SYNC);
    if (!strcmp(mode, "log-close-error")) mqtt_host_file_fault(MQTT_FILE_CLOSE);
    if (!strcmp(mode, "home") || !strcmp(mode, "log-home") || !strcmp(mode, "log-reentry")) close_app();
    else {
        int64_t began = esp_timer_get_time(); click("DISCONNECT"); assert(esp_timer_get_time() - began < 100000);
    }
    if (mqtt_host_hold_log_dispatch) {
        mqtt_host_hold_log_dispatch = false; SetEvent(mqtt_host_log_dispatch_release);
    }
    if (mqtt_host_hold_stop) {
        wait_count(&mqtt_host_stop_entered, 1, false);
        if (!strcmp(mode, "home")) open_app();
        if (!strcmp(mode, "late-events")) {
            mqtt_host_event(MQTT_EVENT_CONNECTED); mqtt_host_event(MQTT_EVENT_SUBSCRIBED);
            mqtt_host_event(MQTT_EVENT_PUBLISHED); mqtt_host_event(MQTT_EVENT_ERROR);
            mqtt_host_receive("fixture/late", "another bounded preview");
        }
        assert_stopping(); snapshot(); SetEvent(mqtt_host_stop_release);
    }
    if (mqtt_host_hold_destroy) {
        wait_count(&mqtt_host_destroy_entered, 1, false); assert_stopping(); SetEvent(mqtt_host_destroy_release);
    }
    if (mqtt_host_hold_sync || mqtt_host_hold_close) {
        wait_count(mqtt_host_hold_sync ? &mqtt_host_sync_entered : &mqtt_host_close_entered, 1, false);
        assert(mqtt_host_open_files == 1 && mqtt_tool_busy());
        if (!strcmp(mode, "log-home") || !strcmp(mode, "log-reentry")) open_app();
        assert_stopping();
        if (!strcmp(mode, "log-reentry")) {
            close_app(); assert(mqtt_tool_busy()); open_app(); assert_stopping(); snapshot();
        }
        SetEvent(mqtt_host_file_release);
    }
    if (hold_report) {
        wait_count(&reporting, 1, false); assert(!mqtt_host_open_files && !mqtt_host_allocations);
        assert_stopping(); SetEvent(report_release);
    }
    finish(true); expect(strstr(mode, "-error") || hold_report ? "not confirmed saved" : "Disconnected; form password cleared");
    assert(!lv_obj_has_state(button("CONNECT"), LV_STATE_DISABLED));
    assert(reported_errors == (strstr(mode, "-error") || hold_report ? 1 : 0));
    if (reported_errors) assert(mqtt_host_appends >= 2 && mqtt_host_appends <= 3);
    if (!with_log) assert(!mqtt_host_appends);
    if (strcmp(mode, "log-home") && strcmp(mode, "log-reentry")) {
        lv_textarea_set_text(area(0), "mqtts://fixture.test:8883"); click("CONNECT");
        assert(mqtt_tool_busy() && mqtt_host_clients == 1); mqtt_host_event(MQTT_EVENT_CONNECTED);
        pump_for(220); expect("Connected with TLS verification");
        assert(!find_text(content, "not confirmed saved", false)); click("DISCONNECT"); finish(true);
    }
}
int main(int argc, char **argv)
{
    _set_error_mode(_OUT_TO_STDERR); SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    assert(argc == 3); mode = argv[1]; output = argv[2]; ui_thread = GetCurrentThreadId();
    mqtt_host_setup(output, mode); report_release = CreateEvent(NULL, TRUE, FALSE, NULL); assert(report_release);
    if (!strcmp(mode, "task-failure")) host_fail_next_task = 1;
    if (!strcmp(mode, "buffer-failure")) mqtt_host_fail_allocation = 1;
    mqtt_tool_self_test(); setup(); run_case(); close_app(); finish(false);
    assert(!mqtt_host_allocations && !mqtt_host_clients && !mqtt_host_open_files);
    lv_deinit(); mqtt_host_shutdown(); CloseHandle(report_release);
    printf("%s PASS clients=0 buffers=0 files=0 workers=0 errors=%ld\n", mode, reported_errors);
    return 0;
}
