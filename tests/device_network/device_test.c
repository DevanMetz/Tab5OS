/* Real LVGL and unchanged app workers; SDK services alone use host adapters. */
#include "host.h"
#include "lvgl.h"
#include "ntp_tool.h"
#include "wol_tool.h"
#include "udp_tool.h"
#include "byte_tool.h"
#include "payload_clipboard.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t framebuffer[720 * 1280];
static lv_obj_t *content;
static bool ntp;
static bool udp;
static const char *output_directory;
static uint32_t ticks(void) { return (uint32_t)GetTickCount64(); }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels;
    lv_display_flush_ready(display);
}
static void pump(void) { lv_timer_handler(); Sleep(2); }
static void pump_for(unsigned ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < until) pump();
}
static lv_obj_t *find_type(lv_obj_t *o, const lv_obj_class_t *type, unsigned *index)
{
    if (lv_obj_check_type(o, type) && (*index)-- == 0) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *found = find_type(lv_obj_get_child(o, i), type, index);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *nth(const lv_obj_class_t *type, unsigned index)
{
    lv_obj_t *o = find_type(content, type, &index);
    assert(o); return o;
}
static lv_obj_t *find_text(lv_obj_t *o, const char *text, bool exact)
{
    if (lv_obj_check_type(o, &lv_label_class) &&
        (exact ? !strcmp(lv_label_get_text(o), text) : strstr(lv_label_get_text(o), text) != NULL)) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *found = find_text(lv_obj_get_child(o, i), text, exact);
        if (found) return found;
    }
    return NULL;
}
static void dump_text(lv_obj_t *o)
{
    if (lv_obj_check_type(o, &lv_label_class)) printf("[%s]\n", lv_label_get_text(o));
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) dump_text(lv_obj_get_child(o, i));
}
static void expect(const char *text)
{
    if (!find_text(content, text, false)) {
        fprintf(stderr, "Missing UI text: %s\n", text); dump_text(content); assert(0);
    }
}
static void click(const char *text)
{
    lv_obj_t *label = find_text(content, text, true);
    assert(label);
    lv_obj_t *button = lv_obj_get_parent(label);
    assert(!lv_obj_has_state(button, LV_STATE_DISABLED));
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
}
static void input(unsigned index, const char *text) { lv_textarea_set_text(nth(&lv_textarea_class, index), text); }
static bool busy(void) { return udp ? udp_tool_busy() : ntp ? ntp_tool_busy() : wol_tool_busy(); }
static void open_app(void)
{
    if (udp) udp_tool_show(content, host_online != 0);
    else if (ntp) ntp_tool_show(content, host_online != 0); else wol_tool_show(content, host_online != 0);
    pump_for(120);
}
static void clean_app(void)
{
    if (udp) udp_tool_stop(); else if (ntp) ntp_tool_stop(); else wol_tool_stop();
    lv_obj_clean(content);
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
}
static void finish(void)
{
    int64_t until = esp_timer_get_time() + 11500000;
    while (busy() || InterlockedCompareExchange(&host_active_tasks, 0, 0)) {
        assert(esp_timer_get_time() < until); pump();
    }
    pump_for(120);
    assert(host_open_sockets == 0 && host_active_tasks == 0);
}
static void check_bounds(lv_obj_t *o)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(o, &a);
    /* Textarea internal labels intentionally scroll inside their field. */
    if (lv_obj_check_type(o, &lv_textarea_class) || lv_obj_check_type(o, &lv_keyboard_class) ||
        lv_obj_check_type(o, &lv_button_class) || lv_obj_get_parent(o) == content) {
        if (a.x1 < 28 || a.x2 > 691 || a.y1 < 128 || a.y2 > 1251) {
            fprintf(stderr, "Object overflow: (%ld,%ld)-(%ld,%ld)\n", (long)a.x1, (long)a.y1, (long)a.x2, (long)a.y2);
            assert(0);
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) check_bounds(lv_obj_get_child(o, i));
}
static void shot(const char *name)
{
    pump_for(300); lv_obj_update_layout(content); lv_refr_now(NULL);
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
static void setup_screen(void)
{
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
}
static void start_wol(void)
{
    LONG before = host_sockets_opened;
    click("SEND"); expect("CONFIRM"); assert(host_sockets_opened == before);
    click("CONFIRM");
}
static void wol_success(void)
{
    finish(); expect("102-byte packet handed to the network."); expect("Device wake state is unverified.");
    expect("Result for 02:11:22:33:44:55"); expect("Socket closed");
}
static void wol_case(const char *scenario, const char *port)
{
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), ""));
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 1)), ""));
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 2)), "9"));
    assert(host_sockets_opened == 0);
    if (!strcmp(scenario, "wol-ui")) {
        shot("wol-default");
        lv_obj_send_event(nth(&lv_textarea_class, 0), LV_EVENT_FOCUSED, NULL);
        shot("wol-keyboard");
        lv_obj_t *keyboard = nth(&lv_keyboard_class, 0);
        assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_USER_2);
        lv_buttonmatrix_set_selected_button(keyboard, 10);
        lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, NULL);
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "A"));
        lv_obj_send_event(keyboard, LV_EVENT_READY, NULL);
        assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        lv_obj_send_event(nth(&lv_textarea_class, 1), LV_EVENT_CLICKED, NULL);
        assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_NUMBER);
        assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        return;
    }
    input(0, "02:11:22:33:44:55"); input(1, "127.0.0.1"); input(2, port);
    if (!strcmp(scenario, "wol-gates")) {
        click("SEND"); expect("CONFIRM"); click("CANCEL"); expect("Confirmation cancelled.");
        assert(host_sockets_opened == 0);
        click("SEND"); input(0, "02:11:22:33:44:56"); expect("Inputs changed.");
        assert(find_text(content, "SEND", true)); assert(host_sockets_opened == 0);
        input(0, "02:11:22:33:44:55"); click("SEND"); pump_for(5200);
        expect("Confirmation expired."); assert(host_sockets_opened == 0);
        click("SEND"); clean_app(); open_app(); assert(host_sockets_opened == 0);
        assert(find_text(content, "SEND", true));
        start_wol(); wol_success();
        input(0, "02:11:22:33:44:56"); expect("Result for 02:11:22:33:44:55");
    } else if (!strcmp(scenario, "wol-offline")) {
        InterlockedExchange(&host_online, 0); click("SEND"); expect("Connect to Wi-Fi");
        assert(host_sockets_opened == 0);
        InterlockedExchange(&host_online, 1); click("SEND"); expect("CONFIRM");
        InterlockedExchange(&host_online, 0); click("CONFIRM"); expect("Connect to Wi-Fi");
        assert(host_sockets_opened == 0);
        InterlockedExchange(&host_online, 1); start_wol(); wol_success();
    } else if (!strcmp(scenario, "wol-invalid")) {
        const char *bad[] = {"FF:FF:FF:FF:FF:FF", "00:00:00:00:00:00", "02:11:22:33:44:55junk", "02:11:22:\n33:44:55"};
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            input(0, bad[i]);
            assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), bad[i]));
            click("SEND"); expect("nonzero unicast MAC");
        }
        input(0, "02:11:22:33:44:55"); input(1, "224.0.0.1"); click("SEND"); expect("unicast or broadcast IPv4");
        input(1, "127.0.\n0.1"); click("SEND"); expect("unicast or broadcast IPv4");
        input(1, "127.0.0.1"); input(2, "1\n23"); click("SEND"); expect("1 to 65535");
        input(1, "127.0.0.1"); input(2, "0"); click("SEND"); expect("1 to 65535");
        assert(host_sockets_opened == 0);
    } else if (!strcmp(scenario, "wol-repeated")) {
        DWORD before = 0, after = 0; lv_mem_monitor_t memory_before, memory_after;
        for (unsigned i = 0; i < 25; i++) {
            start_wol(); wol_success(); clean_app(); pump_for(30);
            if (i == 0) { GetProcessHandleCount(GetCurrentProcess(), &before); lv_mem_monitor(&memory_before); }
            open_app(); assert(host_sockets_opened == (LONG)i + 1);
        }
        clean_app(); pump_for(30); GetProcessHandleCount(GetCurrentProcess(), &after); lv_mem_monitor(&memory_after);
        assert(before == after); assert(memory_before.free_size == memory_after.free_size);
        printf("heap_free=%zu handles=%lu->%lu ", memory_after.free_size, before, after);
        open_app();
    } else { assert(!strcmp(scenario, "wol-send")); start_wol(); wol_success(); }
}
static void ntp_case(const char *scenario, const char *port)
{
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), ""));
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 1)), "123"));
    assert(host_sockets_opened == 0);
    if (!strcmp(scenario, "ntp-ui")) {
        shot("ntp-default");
        lv_obj_send_event(nth(&lv_textarea_class, 0), LV_EVENT_FOCUSED, NULL);
        shot("ntp-keyboard");
        lv_obj_t *keyboard = nth(&lv_keyboard_class, 0);
        assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_NUMBER);
        lv_obj_send_event(keyboard, LV_EVENT_READY, NULL);
        assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        lv_obj_send_event(nth(&lv_textarea_class, 1), LV_EVENT_CLICKED, NULL);
        assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        return;
    }
    input(0, "127.0.0.1"); input(1, port);
    if (!strcmp(scenario, "ntp-inputs")) {
        const char *bad_hosts[] = {"127.1", "0177.0.0.1", "0x7f.0.0.1", "2130706433",
            "127.0.0.01", "127.0.0.1 ", "+127.0.0.1", "127.0.0.-1", "127.0.0.1/24",
            "127.\n0.0.1", "127.0.0.1\t", "255.255.255.2551", "127.0.0.\xc2\xb9"};
        for (size_t i = 0; i < sizeof(bad_hosts) / sizeof(bad_hosts[0]); i++) {
            input(0, bad_hosts[i]);
            assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), bad_hosts[i]));
            click("SAMPLE 4"); expect("full decimal IPv4");
            assert(host_sockets_opened == 0 && host_tasks_started == 0 && !busy());
        }
        input(0, "127.0.0.1");
        const char *bad_ports[] = {"-9", "+9", "9x", "9.0", "1e2", "1\n23", "650001", "000001"};
        for (size_t i = 0; i < sizeof(bad_ports) / sizeof(bad_ports[0]); i++) {
            input(1, bad_ports[i]);
            assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 1)), bad_ports[i]));
            click("SAMPLE 4"); expect("1 to 65535");
            assert(host_sockets_opened == 0 && host_tasks_started == 0);
        }
        clean_app(); open_app();
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 1)), "000001"));
        click("SAMPLE 4"); expect("1 to 65535");
        char unicode[65];
        for (unsigned i = 0; i < 16; i++) memcpy(unicode + 4 * i, "\xf0\x9f\x94\xa5", 4);
        unicode[64] = 0;
        input(0, unicode); clean_app(); open_app();
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), unicode));
        click("SAMPLE 4"); expect("full decimal IPv4");
        input(0, "127.0.0.1"); unicode[24] = 0;
        input(1, unicode); clean_app(); open_app();
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 1)), unicode));
        click("SAMPLE 4"); expect("1 to 65535");
        assert(host_sockets_opened == 0 && host_tasks_started == 0 && !busy());
        return;
    }
    if (!strcmp(scenario, "ntp-gates")) {
        InterlockedExchange(&host_online, 0); click("SAMPLE 4"); expect("Connect to Wi-Fi");
        assert(host_sockets_opened == 0);
        InterlockedExchange(&host_online, 1);
        struct timeval time; host_gettimeofday(&time, NULL);
        InterlockedExchange64(&host_wall_offset_us, -(int64_t)time.tv_sec * 1000000);
        click("SAMPLE 4"); expect("Tablet clock is not set"); assert(host_sockets_opened == 0);
        InterlockedExchange64(&host_wall_offset_us, 0);
        input(0, "999.0.0.1"); click("SAMPLE 4"); expect("full decimal IPv4");
        input(0, "127.0.0.1"); input(1, "0"); click("SAMPLE 4"); expect("1 to 65535");
        assert(host_sockets_opened == 0); return;
    }
    if (!strcmp(scenario, "ntp-repeated")) {
        DWORD before = 0, after = 0; lv_mem_monitor_t memory_before, memory_after;
        for (unsigned i = 0; i < 25; i++) {
            click("SAMPLE 4"); pump_for(200); expect("1 valid");
            clean_app(); finish();
            if (i == 0) { GetProcessHandleCount(GetCurrentProcess(), &before); lv_mem_monitor(&memory_before); }
            open_app(); expect("Cancelled; UDP socket closed.");
        }
        clean_app(); pump_for(30); GetProcessHandleCount(GetCurrentProcess(), &after); lv_mem_monitor(&memory_after);
        assert(before == after); assert(memory_before.free_size == memory_after.free_size);
        printf("heap_free=%zu handles=%lu->%lu ", memory_after.free_size, before, after);
        open_app(); return;
    }
    int64_t started = esp_timer_get_time();
    click("SAMPLE 4");
    if (!strcmp(scenario, "ntp-stop") || !strcmp(scenario, "ntp-home")) {
        pump_for(250); assert(busy()); int64_t cancel_at = esp_timer_get_time();
        if (!strcmp(scenario, "ntp-stop")) click("STOP"); else clean_app();
        finish(); assert(esp_timer_get_time() - cancel_at < 1200000);
        if (!strcmp(scenario, "ntp-home")) open_app();
        expect("Cancelled; UDP socket closed.");
        printf("cancel_ms=%lld ", (long long)((esp_timer_get_time() - cancel_at) / 1000));
        click("SAMPLE 4"); finish(); expect("KoD RATE");
    } else if (!strcmp(scenario, "ntp-clockstep") || !strcmp(scenario, "ntp-clockgap")) {
        pump_for(200);
        if (!strcmp(scenario, "ntp-clockgap")) expect("1 valid");
        InterlockedExchange64(&host_wall_offset_us, 2000000);
        finish(); expect("clock");
        assert(esp_timer_get_time() - started < 3000000);
        if (!strcmp(scenario, "ntp-clockstep")) expect("No valid timing samples");
        else expect("1 valid");
        InterlockedExchange64(&host_wall_offset_us, 0);
    } else {
        finish();
        if (!strcmp(scenario, "ntp-valid") || !strcmp(scenario, "ntp-unmatched")) {
            expect("4 valid | 0 failed/missing | 0 not sampled"); expect("Min delay");
            expect("NTPv4 | stratum 2 | leap 0 | RefID 54455354");
            lv_obj_t *summary = find_text(content, "Min delay", false);
            const char *offset_text = strstr(lv_label_get_text(summary), "\nOffset "); assert(offset_text);
            double offset = strtod(offset_text + 8, NULL); assert(offset > 75 && offset < 125);
            shot("ntp-results");
            input(0, "127.0.0.2"); expect("Results for 127.0.0.1:");
        } else if (!strcmp(scenario, "ntp-invalid")) {
            expect("0 valid | 4 failed/missing | 0 not sampled"); expect("No valid timing samples");
            expect("Server clock is unsynchronized"); expect("Unsupported reply length");
        } else if (!strcmp(scenario, "ntp-timeout")) {
            expect("0 valid | 4 failed/missing | 0 not sampled"); expect("No reply within 2 seconds");
            int64_t elapsed = esp_timer_get_time() - started;
            assert(elapsed >= 7900000 && elapsed < 11000000);
        } else if (!strcmp(scenario, "ntp-kod")) {
            expect("KoD RATE"); expect("0 valid | 1 failed/missing | 3 not sampled");
            assert(esp_timer_get_time() - started < 1200000);
        } else assert(0);
    }
}
static void payload_mode(unsigned mode)
{
    lv_obj_t *select = nth(&lv_dropdown_class, 0);
    lv_dropdown_set_selected(select, mode);
    lv_obj_send_event(select, LV_EVENT_VALUE_CHANGED, NULL);
}
static void start_udp(void)
{
    LONG before = host_sockets_opened;
    click("SEND"); expect("CONFIRM"); assert(host_sockets_opened == before);
    click("CONFIRM");
}
static void udp_echo(void)
{
    finish(); expect("Received 4-byte datagram"); expect("Socket closed");
    expect("TX sent (4 bytes)"); expect("RX (4 bytes)"); expect("00 01 02 03");
}
static SOCKET reserve_port(char text[8])
{
    SOCKET guard = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(guard != INVALID_SOCKET);
    int exclusive = 1;
    assert(!setsockopt(guard, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive)));
    /* Match the app's wildcard bind. Windows permits some specific-address /
     * wildcard pairs that lwIP without SO_REUSEADDR rejects. */
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
    assert(!bind(guard, (struct sockaddr *)&address, sizeof(address)));
    int length = sizeof(address);
    assert(!getsockname(guard, (struct sockaddr *)&address, &length));
    snprintf(text, 8, "%u", ntohs(address.sin_port));
    return guard;
}

static void udp_clipboard_case(void)
{
    clean_app();
    byte_tool_show(content); pump_for(120);
    payload_mode(2);
    lv_obj_t *select = nth(&lv_dropdown_class, 1);
    lv_dropdown_set_selected(select, 6); lv_obj_send_event(select, LV_EVENT_VALUE_CHANGED, NULL);
    select = nth(&lv_dropdown_class, 2);
    lv_dropdown_set_selected(select, 1); lv_obj_send_event(select, LV_EVENT_VALUE_CHANGED, NULL);
    input(0, "1.5"); click("Copy bytes");
    const payload_clipboard_t *copy = payload_clipboard_peek();
    assert(copy && copy->length == 4 && !memcmp(copy->bytes, "\x00\x00\xc0\x3f", 4));
    byte_tool_stop(); lv_obj_clean(content); open_app();
    payload_mode(1); input(3, "keep ASCII"); click("SEND"); expect("CONFIRM");
    click("PASTE BYTES"); expect("Nothing sent"); expect("SEND");
    assert(!find_text(content, "CONFIRM", true));
    assert(lv_dropdown_get_selected(nth(&lv_dropdown_class, 0)) == 0);
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "00 00 C0 3F"));
    assert(host_sockets_opened == 0 && host_tasks_started == 0);
    payload_mode(1); assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "keep ASCII"));
    click("PASTE BYTES"); click("SEND"); expect("CONFIRM");
    click("PASTE BYTES"); expect("SEND"); /* Identical paste also disarms. */
    start_udp();
    lv_obj_t *paste = lv_obj_get_parent(find_text(content, "PASTE BYTES", true));
    lv_obj_t *reply = lv_obj_get_parent(find_text(content, "COPY REPLY", true));
    assert(lv_obj_has_state(paste, LV_STATE_DISABLED) && lv_obj_has_state(reply, LV_STATE_DISABLED));
    lv_obj_send_event(paste, LV_EVENT_CLICKED, NULL);
    lv_obj_send_event(reply, LV_EVENT_CLICKED, NULL); /* Guarded even if called directly. */
    finish(); expect("Received 4-byte datagram");
    input(3, "FF"); input(0, "127.0.0.2");
    click("COPY REPLY");
    assert(host_tasks_started == 1 && host_sockets_opened == 1);
    assert(copy->length == 4 && !memcmp(copy->bytes, "\x00\x00\xc0\x3f", 4));
    expect("Result for 127.0.0.1:"); shot("udp-clipboard-reply");
    clean_app(); byte_tool_show(content); pump_for(120); click("Paste hex");
    expect("Float32 LE: 1.5");
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), "00 00 C0 3F"));
    byte_tool_stop(); lv_obj_clean(content); open_app();
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "FF"));
    input(0, "127.0.0.1");
    uint8_t maximum[128]; for (unsigned i = 0; i < sizeof(maximum); i++) maximum[i] = (uint8_t)i;
    assert(payload_clipboard_store(maximum, sizeof(maximum)));
    pump_for(120); click("PASTE BYTES");
    assert(strlen(lv_textarea_get_text(nth(&lv_textarea_class, 3))) == 383);
    start_udp(); finish(); expect("Received 128-byte datagram"); click("COPY REPLY");
    assert(copy->length == 128 && !memcmp(copy->bytes, maximum, 128));
    assert(host_tasks_started == 2 && host_sockets_opened == 2);
    payload_clipboard_clear(); pump_for(120);
    paste = lv_obj_get_parent(find_text(content, "PASTE BYTES", true));
    assert(lv_obj_has_state(paste, LV_STATE_DISABLED));
    lv_obj_send_event(paste, LV_EVENT_CLICKED, NULL);
    assert(strlen(lv_textarea_get_text(nth(&lv_textarea_class, 3))) == 383);
}

static void udp_case(const char *scenario, const char *port)
{
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 0)), ""));
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 2)), "0"));
    assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "00 01 02 03"));
    assert(host_sockets_opened == 0);
    if (!strcmp(scenario, "udp-ui")) {
        shot("udp-default");
        lv_obj_send_event(nth(&lv_textarea_class, 3), LV_EVENT_CLICKED, NULL);
        lv_obj_t *keyboard = nth(&lv_keyboard_class, 0);
        assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_USER_1);
        input(3, "");
        lv_buttonmatrix_set_selected_button(keyboard, 10); /* A */
        lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, NULL);
        lv_buttonmatrix_set_selected_button(keyboard, 11); /* B */
        lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, NULL);
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "AB"));
        shot("udp-hex-keyboard");
        lv_obj_send_event(keyboard, LV_EVENT_READY, NULL);
        assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        payload_mode(1); input(3, "Hi\n\\n"); payload_mode(0);
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "AB"));
        payload_mode(1);
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "Hi\n\\n"));
        lv_obj_send_event(nth(&lv_textarea_class, 3), LV_EVENT_CLICKED, NULL);
        assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_TEXT_LOWER);
        shot("udp-ascii-keyboard");
        lv_obj_send_event(keyboard, LV_EVENT_CANCEL, NULL);
        lv_obj_send_event(nth(&lv_textarea_class, 0), LV_EVENT_CLICKED, NULL);
        assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_NUMBER);
        assert(host_sockets_opened == 0 && host_tasks_started == 0);
        return;
    }
    input(0, "127.0.0.1"); input(1, port);
    if (!strcmp(scenario, "udp-clipboard")) { udp_clipboard_case(); return; }
    if (!strcmp(scenario, "udp-invalid")) {
        const char *addresses[] = {"", "127.1", "0177.0.0.1", "0x7f000001", "127.0.0.01", "127.0.0.1 ",
            "localhost", "256.0.0.1", "127.0.0.1/8", "0.0.0.0", "224.0.0.1", "255.255.255.255", "127.255.255.255", "127.0.\n0.1"};
        for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
            input(0, addresses[i]); click("SEND");
            assert(!find_text(content, "CONFIRM", true));
        }
        input(0, "127.0.0.1");
        const char *ports[] = {"", "0", "65536", "1.5", "-1", " 7", "7 ", "000007", "7e1", "1\n23"};
        for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
            input(1, ports[i]); click("SEND"); expect("Peer port: 1-65535");
        }
        input(1, port); input(2, "65536"); click("SEND"); expect("Source port: 0");
        input(2, "1\n23"); click("SEND"); expect("Source port: 0"); input(2, "0");
        const char *payloads[] = {"A", "A B", "0xAB", "12 GG", ("12\xc2\xa0" "34")};
        for (size_t i = 0; i < sizeof(payloads) / sizeof(payloads[0]); i++) {
            input(3, payloads[i]); click("SEND"); assert(!find_text(content, "CONFIRM", true));
        }
        char many[514]; memset(many, ' ', 513); many[513] = 0;
        input(3, many); click("SEND"); expect("512-character limit");
        payload_mode(1); memset(many, 'A', 129); many[129] = 0;
        input(3, many); click("SEND"); expect("128-byte limit");
        input(3, "caf\xc3\xa9"); click("SEND"); expect("7-bit characters");
        clean_app(); open_app();
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "caf\xc3\xa9"));
        click("SEND"); expect("7-bit characters");
        assert(host_sockets_opened == 0 && host_tasks_started == 0); return;
    }
    if (!strcmp(scenario, "udp-gates")) {
        click("SEND"); expect("CONFIRM");
        input(3, "01"); expect("SEND");
        click("SEND"); expect("CONFIRM"); payload_mode(1); expect("SEND");
        click("SEND"); click("CANCEL"); expect("Confirmation cancelled");
        click("SEND"); pump_for(5100); expect("Confirmation expired"); expect("SEND");
        click("SEND"); clean_app(); open_app(); expect("SEND");
        assert(host_sockets_opened == 0 && host_tasks_started == 0); return;
    }
    if (!strcmp(scenario, "udp-offline")) {
        InterlockedExchange(&host_online, 0); click("SEND"); expect("Connect to Wi-Fi");
        InterlockedExchange(&host_online, 1); click("SEND"); expect("CONFIRM");
        InterlockedExchange(&host_online, 0); click("CONFIRM"); expect("Connect to Wi-Fi");
        assert(host_sockets_opened == 0);
        InterlockedExchange(&host_online, 1); start_udp(); pump_for(160); assert(busy());
        InterlockedExchange(&host_online, 0); finish(); expect("Wi-Fi lost while waiting");
        InterlockedExchange(&host_online, 1); return;
    }
    if (!strcmp(scenario, "udp-task-failure")) {
        InterlockedExchange(&host_fail_next_task, 1); start_udp(); finish();
        expect("Could not allocate the worker"); assert(host_sockets_opened == 0);
        start_udp(); udp_echo(); return;
    }
    if (!strcmp(scenario, "udp-bind") || !strcmp(scenario, "udp-source")) {
        char fixed[8]; SOCKET guard = reserve_port(fixed); input(2, fixed);
        if (!strcmp(scenario, "udp-bind")) {
            start_udp(); finish(); expect("Local port bind failed"); expect("TX requested (not sent)");
            closesocket(guard); return;
        }
        closesocket(guard); start_udp(); udp_echo();
        char expected[48]; snprintf(expected, sizeof(expected), "source port %s", fixed); expect(expected);
        printf("fixed_port=%s ", fixed); return;
    }
    if (!strcmp(scenario, "udp-ascii")) { payload_mode(1); input(3, "Hi\n\\n"); }
    else if (!strcmp(scenario, "udp-empty")) input(3, "");
    else if (!strcmp(scenario, "udp-max")) {
        char maximum[385];
        for (unsigned i = 0; i < 128; i++) snprintf(maximum + i * 3, 4, "%02X ", i);
        input(3, maximum);
    }
    if (!strcmp(scenario, "udp-repeated")) {
        DWORD before = 0, after = 0; lv_mem_monitor_t initial, final;
        for (unsigned i = 0; i < 25; i++) {
            start_udp(); udp_echo(); clean_app(); pump_for(30);
            if (i == 0) { GetProcessHandleCount(GetCurrentProcess(), &before); lv_mem_monitor(&initial); }
            open_app(); expect("RX (4 bytes)");
        }
        clean_app(); pump_for(30); GetProcessHandleCount(GetCurrentProcess(), &after); lv_mem_monitor(&final);
        assert(before == after && final.free_size >= initial.free_size && initial.used_cnt == final.used_cnt);
        printf("heap_free=%zu->%zu handles=%lu->%lu ", initial.free_size, final.free_size, before, after);
        open_app(); return;
    }
    int64_t started = esp_timer_get_time(); start_udp();
    if (!strcmp(scenario, "udp-stop") || !strcmp(scenario, "udp-home")) {
        pump_for(160); assert(busy()); expect("Sent 4 bytes");
        assert(lv_obj_has_state(nth(&lv_textarea_class, 0), LV_STATE_DISABLED));
        int64_t cancel_at = esp_timer_get_time();
        if (!strcmp(scenario, "udp-home")) {
            clean_app(); assert(esp_timer_get_time() - cancel_at < 200000); open_app();
        } else click("CANCEL");
        finish(); assert(esp_timer_get_time() - cancel_at < 1200000);
        expect("Cancelled waiting"); expect("TX sent (4 bytes)");
        start_udp(); udp_echo(); return;
    }
    finish();
    if (!strcmp(scenario, "udp-timeout") || !strcmp(scenario, "udp-wrong-only")) {
        expect("No reply within the 3-second deadline"); expect("RX: no reply received");
        int64_t elapsed = esp_timer_get_time() - started;
        assert(elapsed >= 2900000 && elapsed < 4300000);
    } else if (!strcmp(scenario, "udp-truncated") || !strcmp(scenario, "udp-over-limit")) {
        expect("Reply exceeds 512 bytes"); expect("Full datagram length unknown");
        expect("RX truncated prefix (512 bytes)"); shot("udp-truncated");
    } else if (!strcmp(scenario, "udp-max")) {
        expect("TX sent (128 bytes)"); expect("Received 512-byte datagram"); expect("RX (512 bytes)");
        assert(!find_text(content, "truncated prefix", false)); shot("udp-maximum");
    } else if (!strcmp(scenario, "udp-empty")) {
        expect("TX sent (0 bytes)"); expect("Received 0-byte datagram"); expect("RX (0 bytes)"); expect("(empty)");
        click("COPY REPLY");
        assert(payload_clipboard_peek() && !payload_clipboard_peek()->length);
        input(3, "FF"); click("PASTE BYTES");
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), ""));
    } else if (!strcmp(scenario, "udp-ascii")) {
        expect("ASCII input, 5 bytes"); expect("48 69 0A 5C 6E"); expect("Received 5-byte datagram");
    } else if (!strcmp(scenario, "udp-wrong-peer")) {
        expect("Received 4-byte datagram"); expect("67 6F 6F 64");
        assert(!find_text(content, "wrong peer", false));
    } else {
        assert(!strcmp(scenario, "udp-echo")); udp_echo(); shot("udp-results");
        input(0, "127.0.0.2"); input(3, "FF"); expect("Result for 127.0.0.1:"); expect("TX sent (4 bytes)");
        clean_app(); open_app(); expect("Result for 127.0.0.1:"); expect("00 01 02 03");
        assert(!strcmp(lv_textarea_get_text(nth(&lv_textarea_class, 3)), "FF"));
    }
    if (!strcmp(scenario, "udp-max") || !strcmp(scenario, "udp-over-limit") || !strcmp(scenario, "udp-truncated")) {
        const uint8_t sentinel[] = {0xde, 0xad};
        assert(payload_clipboard_store(sentinel, sizeof(sentinel)));
        lv_obj_t *copy = lv_obj_get_parent(find_text(content, "COPY REPLY", true));
        assert(lv_obj_has_state(copy, LV_STATE_DISABLED));
        lv_obj_send_event(copy, LV_EVENT_CLICKED, NULL);
        expect("Clipboard unchanged");
        assert(payload_clipboard_peek()->length == 2 && !memcmp(payload_clipboard_peek()->bytes, sentinel, 2));
    }
}
int main(int argc, char **argv)
{
    assert(argc == 4);
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *scenario = argv[2]; output_directory = argv[3]; ntp = !strncmp(scenario, "ntp-", 4);
    udp = !strncmp(scenario, "udp-", 4);
    WSADATA data; assert(!WSAStartup(MAKEWORD(2, 2), &data));
    setup_screen(); open_app();
    int64_t began = esp_timer_get_time();
    if (udp) udp_case(scenario, argv[1]); else if (ntp) ntp_case(scenario, argv[1]); else wol_case(scenario, argv[1]);
    assert(host_open_sockets == 0 && host_active_tasks == 0);
    printf("%s PASS elapsed_ms=%lld tasks=%ld sockets=%ld\n", scenario,
           (long long)((esp_timer_get_time() - began) / 1000), host_tasks_started, host_sockets_opened);
    clean_app(); lv_deinit(); WSACleanup(); return 0;
}
