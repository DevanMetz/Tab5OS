#include "wol_tool.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "wol_data.h"

#define WOL_CONFIRM_MS 5000U
#define WOL_DEADLINE_US 2000000LL

typedef struct {
    char destination[16];
    uint8_t mac[WOL_MAC_BYTES];
    uint16_t port;
    bool present;
    bool sent;
    uint32_t duration_ms;
    char status[192];
} wol_job_t;

static portMUX_TYPE job_lock = portMUX_INITIALIZER_UNLOCKED;
static wol_job_t published;
static wol_job_t confirmation;
static bool job_busy;
static bool job_cancelled;
static uint32_t job_revision;
static bool confirmation_armed;
static uint32_t confirmation_at;
/* Extra room retains rejected UTF-8 as typed; the parser validates full input. */
static char saved_inputs[3][129] = {"", "", "9"};
static lv_obj_t *fields[3];
static lv_obj_t *keyboard;
static lv_obj_t *send_button;
static lv_obj_t *send_label;
static lv_obj_t *cancel_button;
static lv_obj_t *status_label;
static lv_obj_t *identity_label;
static lv_obj_t *result_label;
static lv_timer_t *ui_timer;
static uint32_t rendered_revision;

static const char *const mac_keys[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "\n",
    "8", "9", "A", "B", "C", "D", "E", "F", "\n",
    ":", "-", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t mac_controls[] = {
    2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, LV_BUTTONMATRIX_CTRL_CHECKED | 3, LV_BUTTONMATRIX_CTRL_CHECKED | 3,
    LV_BUTTONMATRIX_CTRL_CHECKED | 3, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 3
};
_Static_assert(sizeof(mac_keys) / sizeof(mac_keys[0]) ==
               sizeof(mac_controls) / sizeof(mac_controls[0]) + 3, "MAC keyboard map mismatch");

bool wol_tool_busy(void)
{
    portENTER_CRITICAL(&job_lock);
    bool busy = job_busy;
    portEXIT_CRITICAL(&job_lock);
    return busy;
}

static bool cancelled(void)
{
    portENTER_CRITICAL(&job_lock);
    bool cancel = job_cancelled;
    portEXIT_CRITICAL(&job_lock);
    return cancel;
}

static bool wifi_ready(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    wifi_ap_record_t access_point;
    return netif && esp_netif_is_netif_up(netif) &&
           esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0 &&
           esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;
}

static int operation_error(int64_t deadline)
{
    if (cancelled()) return ECANCELED;
    if (esp_timer_get_time() >= deadline) return ETIMEDOUT;
    return 0;
}

static int wait_writable(int fd, int64_t deadline)
{
    for (;;) {
        int error = operation_error(deadline);
        if (error) return error;
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) return ETIMEDOUT;
        struct timeval timeout = {.tv_sec = 0, .tv_usec = (long)(remaining < 100000 ? remaining : 100000)};
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(fd, &ready);
        int count = select(fd + 1, NULL, &ready, NULL, &timeout);
        if (count > 0) return operation_error(deadline);
        if (count < 0 && errno != EINTR) return errno ? errno : EIO;
    }
}

static void wol_worker(void *argument)
{
    (void)argument;
    wol_job_t job;
    portENTER_CRITICAL(&job_lock);
    job = published;
    portEXIT_CRITICAL(&job_lock);
    int64_t started = esp_timer_get_time();
    int64_t deadline = started + WOL_DEADLINE_US;
    int error = operation_error(deadline);
    int fd = -1;
    uint8_t packet[WOL_PACKET_BYTES];
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(job.port)};
    if (!error && (!wol_build_packet(job.mac, packet) ||
                   inet_pton(AF_INET, job.destination, &peer.sin_addr) != 1)) error = EINVAL;
    if (!error && !wifi_ready()) error = ENETDOWN;
    if (!error) {
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) error = errno ? errno : EIO;
    }
    if (!error && fcntl(fd, F_SETFL, O_NONBLOCK) < 0) error = errno ? errno : EIO;
    int broadcast = 1;
    if (!error && setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) != 0)
        error = errno ? errno : EIO;
    struct timeval close_timeout = {.tv_sec = 0, .tv_usec = 100000};
    if (!error && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &close_timeout, sizeof(close_timeout)) != 0)
        error = errno ? errno : EIO;
    /* UDP connect only selects the explicitly confirmed peer; it sends no data. */
    if (!error && connect(fd, (const struct sockaddr *)&peer, sizeof(peer)) < 0) error = errno ? errno : EIO;
    while (!error && !job.sent) {
        error = wait_writable(fd, deadline);
        if (!error && !wifi_ready()) error = ENETDOWN;
        if (!error) error = operation_error(deadline);
        if (error) break;
        ssize_t count = send(fd, packet, sizeof(packet), 0);
        if (count == WOL_PACKET_BYTES) job.sent = true;
        else if (count >= 0) error = EIO; /* Never retry a partial datagram. */
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) error = errno ? errno : EIO;
    }
    /* This worker alone owns the socket. Cancellation cannot recall a sent packet. */
    if (fd >= 0) close(fd);
    if (job.sent) {
        snprintf(job.status, sizeof(job.status), "102-byte packet handed to the network.\nDevice wake state is unverified.");
    } else if (cancelled() || error == ECANCELED) {
        snprintf(job.status, sizeof(job.status), "Cancelled before the packet was handed to the network.");
    } else if (error == ETIMEDOUT) {
        snprintf(job.status, sizeof(job.status), "Send timed out after 2 seconds.\nDevice wake state is unverified.");
    } else if (error == ENETDOWN) {
        snprintf(job.status, sizeof(job.status), "Wi-Fi is unavailable. No packet sent.");
    } else {
        snprintf(job.status, sizeof(job.status), "UDP send failed (socket error %d).\nDevice wake state is unverified.", error);
    }
    job.duration_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    portENTER_CRITICAL(&job_lock);
    published = job;
    job_busy = false;
    job_revision++;
    portEXIT_CRITICAL(&job_lock);
    vTaskDelete(NULL);
}

static void set_enabled(lv_obj_t *object, bool enabled)
{
    if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static void disarm(void)
{
    confirmation_armed = false;
    if (send_label) lv_label_set_text(send_label, "SEND");
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if (!status_label) return;
    if (confirmation_armed && (uint32_t)(lv_tick_get() - confirmation_at) >= WOL_CONFIRM_MS) {
        disarm();
        lv_label_set_text(status_label, "Confirmation expired. Tap SEND to review the target again.");
    }
    wol_job_t job;
    bool busy;
    uint32_t revision;
    portENTER_CRITICAL(&job_lock);
    job = published;
    busy = job_busy;
    revision = job_revision;
    portEXIT_CRITICAL(&job_lock);
    for (unsigned i = 0; i < 3; i++) set_enabled(fields[i], !busy);
    set_enabled(send_button, !busy && ui_timer != NULL);
    set_enabled(cancel_button, busy || confirmation_armed);
    if (rendered_revision == revision) return;
    rendered_revision = revision;
    if (!job.present) return;
    char identity[160];
    snprintf(identity, sizeof(identity), "Result for %02X:%02X:%02X:%02X:%02X:%02X\n"
             "UDP destination %s:%u | %s | %lu ms", job.mac[0], job.mac[1], job.mac[2],
             job.mac[3], job.mac[4], job.mac[5], job.destination, job.port,
             busy ? "Sending" : "Socket closed", (unsigned long)job.duration_ms);
    lv_label_set_text(identity_label, identity);
    lv_label_set_text(result_label, job.status);
    lv_label_set_text(status_label, busy ? "CANCEL or Home stops an unsent packet." :
                      "Tap SEND again only if you want another packet.");
}

static bool form_job(wol_job_t *job)
{
    memset(job, 0, sizeof(*job));
    uint8_t ip[4];
    if (!wol_parse_mac(saved_inputs[0], job->mac)) {
        lv_label_set_text(status_label, "Enter a nonzero unicast MAC: six hex pairs with colons/hyphens, or 12 hex digits.");
        return false;
    }
    if (!wol_parse_ipv4(saved_inputs[1], ip)) {
        lv_label_set_text(status_label, "Enter a unicast or broadcast IPv4 destination, such as your target subnet's broadcast address.");
        return false;
    }
    if (!wol_parse_port(saved_inputs[2], &job->port)) {
        lv_label_set_text(status_label, "UDP port must be decimal 1 to 65535.");
        return false;
    }
    snprintf(job->destination, sizeof(job->destination), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    job->present = true;
    return true;
}

static void send_clicked(lv_event_t *event)
{
    (void)event;
    if (wol_tool_busy() || !ui_timer) return;
    wol_job_t job;
    if (!form_job(&job)) { disarm(); return; }
    if (!wifi_ready()) {
        disarm();
        lv_label_set_text(status_label, "Connect to Wi-Fi with an IPv4 address before sending.");
        return;
    }
    bool unchanged = !strcmp(job.destination, confirmation.destination) && job.port == confirmation.port &&
                     !memcmp(job.mac, confirmation.mac, WOL_MAC_BYTES);
    if (!confirmation_armed || !unchanged || (uint32_t)(lv_tick_get() - confirmation_at) >= WOL_CONFIRM_MS) {
        confirmation = job;
        confirmation_armed = true;
        confirmation_at = lv_tick_get();
        lv_label_set_text(send_label, "CONFIRM");
        char prompt[192];
        snprintf(prompt, sizeof(prompt), "Send ONE packet to %s:%u for\n%02X:%02X:%02X:%02X:%02X:%02X? Tap CONFIRM within 5 seconds.",
                 job.destination, job.port, job.mac[0], job.mac[1], job.mac[2], job.mac[3], job.mac[4], job.mac[5]);
        lv_label_set_text(status_label, prompt);
        refresh(NULL);
        return;
    }
    disarm();
    lv_keyboard_set_textarea(keyboard, NULL);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    snprintf(job.status, sizeof(job.status), "Sending one 102-byte packet...");
    portENTER_CRITICAL(&job_lock);
    published = job;
    job_cancelled = false;
    job_busy = true;
    job_revision++;
    portEXIT_CRITICAL(&job_lock);
    if (xTaskCreate(wol_worker, "wol-send", 4096, NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&job_lock);
        snprintf(published.status, sizeof(published.status), "Could not allocate the sender. No packet sent.");
        job_busy = false;
        job_revision++;
        portEXIT_CRITICAL(&job_lock);
    }
    refresh(NULL);
}

static void cancel_clicked(lv_event_t *event)
{
    (void)event;
    disarm();
    portENTER_CRITICAL(&job_lock);
    bool busy = job_busy;
    if (busy) job_cancelled = true;
    portEXIT_CRITICAL(&job_lock);
    lv_label_set_text(status_label, busy ? "Cancelling unsent work. A sent packet cannot be recalled." :
                      "Confirmation cancelled. No new packet sent.");
    refresh(NULL);
}

static void input_event(lv_event_t *event)
{
    if (!keyboard || wol_tool_busy()) return;
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    lv_obj_t *object = lv_event_get_target_obj(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_mode(keyboard, index == 0 ? LV_KEYBOARD_MODE_USER_2 : LV_KEYBOARD_MODE_NUMBER);
        lv_keyboard_set_textarea(keyboard, object);
        lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        snprintf(saved_inputs[index], sizeof(saved_inputs[index]), "%s", lv_textarea_get_text(object));
        disarm();
        lv_label_set_text(status_label, "Inputs changed. Any previous result keeps its original target below.");
    }
}

static void keyboard_done(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, bool small)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_width(object, 640);
    if (small) lv_obj_set_style_text_font(object, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    lv_label_set_text(object, text);
    return object;
}

static lv_obj_t *row(lv_obj_t *parent, int height)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, 640, height);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(object, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(object, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return object;
}

static void input(lv_obj_t *parent, const char *caption, unsigned index, int width)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_size(block, width, 82);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, 4, 0);
    lv_obj_set_width(label(block, caption, true), width);
    fields[index] = lv_textarea_create(block);
    lv_obj_set_size(fields[index], width, 60);
    /* Keep pasted newlines so the parser can reject the original draft. */
    lv_textarea_set_max_length(fields[index], 32);
    lv_textarea_set_text(fields[index], saved_inputs[index]);
    lv_obj_add_event_cb(fields[index], input_event, LV_EVENT_ALL, (void *)(uintptr_t)index);
}

static lv_obj_t *button(lv_obj_t *parent, const char *caption, lv_event_cb_t callback)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_set_size(object, 310, 64);
    lv_obj_t *text = lv_label_create(object);
    lv_label_set_text(text, caption);
    lv_obj_center(text);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, NULL);
    return object;
}

void wol_tool_stop(void)
{
    portENTER_CRITICAL(&job_lock);
    if (job_busy) job_cancelled = true;
    portEXIT_CRITICAL(&job_lock);
    confirmation_armed = false;
    if (ui_timer) lv_timer_delete(ui_timer);
    ui_timer = NULL;
    if (keyboard) lv_keyboard_set_textarea(keyboard, NULL);
    keyboard = NULL;
    for (unsigned i = 0; i < 3; i++) fields[i] = NULL;
    send_button = NULL;
    send_label = NULL;
    cancel_button = NULL;
    status_label = NULL;
    identity_label = NULL;
    result_label = NULL;
}

void wol_tool_show(lv_obj_t *parent, bool connected)
{
    lv_obj_set_style_pad_row(parent, 12, 0);
    lv_obj_t *column = lv_obj_create(parent);
    lv_obj_remove_style_all(column);
    lv_obj_set_size(column, 640, LV_SIZE_CONTENT);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 12, 0);
    lv_obj_set_style_text_font(column, &lv_font_montserrat_28, 0);
    label(column, "Wake-on-LAN", false);
    label(column, "Send one magic packet to a device you manage. The target needs Wake-on-LAN enabled.\n"
                  "Choose its subnet broadcast or an explicit IPv4 destination; no scanning or retries.", true);
    input(column, "Target MAC (colon, hyphen or 12 hex digits)", 0, 640);
    lv_obj_t *destination = row(column, 82);
    input(destination, "Destination IPv4", 1, 430);
    input(destination, "UDP port (1-65535)", 2, 198);
    keyboard = lv_keyboard_create(column);
    lv_obj_set_size(keyboard, 640, 240);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_2, mac_keys, mac_controls);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_2);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_CANCEL, NULL);
    lv_obj_t *actions = row(column, 64);
    send_button = button(actions, "SEND", send_clicked);
    send_label = lv_obj_get_child(send_button, 0);
    cancel_button = button(actions, "CANCEL", cancel_clicked);
    status_label = label(column, connected ? "Enter the target. SEND reviews it; a second tap confirms within 5 seconds." :
                         "Connect to Wi-Fi before sending. You can enter the target now.", true);
    lv_obj_set_height(status_label, 56);
    identity_label = label(column, "No packet has been sent.", true);
    lv_obj_set_height(identity_label, 50);
    result_label = label(column, "A successful UDP send cannot confirm that the device woke up.", false);
    label(column, "Inputs and the last result stay in RAM until restart.\n"
                  "No SecureOn password. CANCEL/Home can stop an unsent packet only.", true);
    rendered_revision = UINT32_MAX;
    ui_timer = lv_timer_create(refresh, 100, NULL);
    refresh(NULL);
    if (!ui_timer) lv_label_set_text(status_label, "Could not allocate the display timer. Return Home and reopen the app.");
}
