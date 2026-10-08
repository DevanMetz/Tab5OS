#include "udp_tool.h"
#include "ipv4_data.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "byte_data.h"
#include "payload_clipboard.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define UDP_REPLY_MAX 512
#define UDP_CONFIRM_MS 5000U
#define UDP_DEADLINE_US 3000000LL

typedef struct {
    char host[16];
    uint16_t port;
    uint16_t source_port;
    byte_data_mode_t mode;
    size_t length;
    uint8_t bytes[BYTE_DATA_MAX_BYTES];
} udp_request_t;

typedef struct {
    udp_request_t request;
    uint16_t actual_port;
    bool present;
    bool sent;
    bool received;
    bool truncated;
    size_t reply_length;
    uint8_t reply[UDP_REPLY_MAX];
    int64_t accepted_us;
    uint32_t duration_ms;
    uint32_t reply_ms;
    char status[192];
} udp_job_t;

static portMUX_TYPE job_lock = portMUX_INITIALIZER_UNLOCKED;
static udp_job_t published;
static bool job_busy;
static bool job_cancelled;
static uint32_t job_revision;
static udp_request_t confirmation;
static bool confirmation_armed;
static uint32_t confirmation_at;
static byte_data_mode_t input_mode = BYTE_DATA_HEX;
static char saved_fields[3][129] = {"", "19007", "0"};
/* One extra character makes overlong input invalid, even after Home/re-entry.
 * LVGL counts Unicode characters, while the byte parser rejects non-ASCII. */
static char saved_hex[(BYTE_DATA_MAX_TEXT + 1) * 4 + 1] = "00 01 02 03";
static char saved_ascii[(BYTE_DATA_MAX_BYTES + 1) * 4 + 1] = "Hello Tab5";
static bool loading_input;
static lv_obj_t *fields[3];
static lv_obj_t *payload;
static lv_obj_t *mode_select;
static lv_obj_t *keyboard;
static lv_obj_t *send_button;
static lv_obj_t *send_label;
static lv_obj_t *cancel_button;
static lv_obj_t *paste_button;
static lv_obj_t *copy_reply_button;
static lv_obj_t *status_label;
static lv_obj_t *identity_label;
static lv_obj_t *result_label;
static lv_obj_t *preview_panel;
static lv_obj_t *tx_label;
static lv_obj_t *rx_label;
static lv_timer_t *ui_timer;
static uint32_t rendered_revision;

static const char *const hex_keys[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "\n",
    "8", "9", "A", "B", "C", "D", "E", "F", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t hex_controls[] = {
    2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2,
    LV_BUTTONMATRIX_CTRL_CHECKED | 2, 6, LV_BUTTONMATRIX_CTRL_CHECKED | 2,
    LV_BUTTONMATRIX_CTRL_CHECKED | 3, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 3
};
_Static_assert(sizeof(hex_keys) / sizeof(hex_keys[0]) ==
               sizeof(hex_controls) / sizeof(hex_controls[0]) + 3, "UDP keyboard map mismatch");

bool udp_tool_busy(void)
{
    portENTER_CRITICAL(&job_lock);
    bool busy = job_busy;
    portEXIT_CRITICAL(&job_lock);
    return busy;
}

static bool wifi_ready(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    wifi_ap_record_t ap;
    return netif && esp_netif_is_netif_up(netif) &&
           esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0 &&
           esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

static bool local_broadcast(uint32_t destination)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) return false;
    uint32_t mask = ntohl(ip.netmask.addr);
    uint32_t wildcard = ~mask;
    return mask != 0 && wildcard > 1 &&
           destination == ((ntohl(ip.ip.addr) & mask) | wildcard);
}

static int operation_error(int64_t deadline)
{
    portENTER_CRITICAL(&job_lock);
    bool cancel = job_cancelled;
    portEXIT_CRITICAL(&job_lock);
    if (cancel) return ECANCELED;
    if (!wifi_ready()) return ENETDOWN;
    return esp_timer_get_time() >= deadline ? ETIMEDOUT : 0;
}

static int wait_socket(int fd, bool writing, int64_t deadline)
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
        int count = select(fd + 1, writing ? NULL : &ready, writing ? &ready : NULL, NULL, &timeout);
        if (count > 0) return operation_error(deadline);
        if (count < 0 && errno != EINTR) return errno ? errno : EIO;
    }
}

static void udp_worker(void *argument)
{
    (void)argument;
    udp_job_t job;
    portENTER_CRITICAL(&job_lock);
    job = published;
    portEXIT_CRITICAL(&job_lock);
    int64_t started = job.accepted_us;
    int64_t deadline = started + UDP_DEADLINE_US;
    int64_t sent_at = 0;
    int error = operation_error(deadline);
    const char *stage = "Socket setup";
    int fd = -1;
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(job.request.port)};
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(job.request.source_port),
                                .sin_addr.s_addr = htonl(INADDR_ANY)};
    uint32_t ip;
    if (!error && !ipv4_parse(job.request.host, &ip)) error = EINVAL;
    if (!error) peer.sin_addr.s_addr = htonl(ip);
    if (!error && local_broadcast(ntohl(peer.sin_addr.s_addr))) error = EINVAL;
    if (!error) {
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) error = errno ? errno : EIO;
    }
    if (!error && fcntl(fd, F_SETFL, O_NONBLOCK) < 0) error = errno ? errno : EIO;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
    if (!error && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
        error = errno ? errno : EIO;
    if (!error) {
        stage = "Local port bind";
        /* No address/port reuse: an occupied port must fail rather than sharing
         * another app's receive queue. Zero requests an ephemeral source port. */
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) error = errno ? errno : EIO;
    }
    if (!error) {
        stage = "Peer selection";
        /* UDP connect sends no handshake. It filters replies to this exact peer. */
        if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) error = errno ? errno : EIO;
    }
    if (!error) {
        socklen_t size = sizeof(local);
        if (getsockname(fd, (struct sockaddr *)&local, &size) < 0) error = errno ? errno : EIO;
        else job.actual_port = ntohs(local.sin_port);
    }
    if (!error) stage = "Send";
    while (!error && !job.sent) {
        error = wait_socket(fd, true, deadline);
        if (error) break;
        ssize_t count = send(fd, job.request.bytes, job.request.length, 0);
        if (count == (ssize_t)job.request.length) {
            job.sent = true; /* Zero bytes is a valid empty UDP datagram. */
            sent_at = esp_timer_get_time();
            snprintf(job.status, sizeof(job.status), "Sent %u bytes. Waiting for one reply...", (unsigned)job.request.length);
            portENTER_CRITICAL(&job_lock);
            published = job;
            job_revision++;
            portEXIT_CRITICAL(&job_lock);
        } else if (count >= 0) error = EIO; /* Never retry a partial datagram. */
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) error = errno ? errno : EIO;
    }
    if (!error) stage = "Receive";
    while (!error && !job.received) {
        error = wait_socket(fd, false, deadline);
        if (error) break;
        /* One sentinel byte distinguishes a full 512-byte datagram from a
         * truncated prefix. The full length of an oversized packet is unknown. */
        uint8_t reply[UDP_REPLY_MAX + 1];
        ssize_t count = recv(fd, reply, sizeof(reply), 0);
        if (count >= 0) {
            job.received = true;
            job.truncated = count > UDP_REPLY_MAX;
            job.reply_length = job.truncated ? UDP_REPLY_MAX : (size_t)count;
            memcpy(job.reply, reply, job.reply_length);
            job.reply_ms = (uint32_t)((esp_timer_get_time() - sent_at) / 1000);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) error = errno ? errno : EIO;
    }
    /* The worker is the sole socket owner; Home never closes it concurrently. */
    if (fd >= 0) close(fd);
    job.duration_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    if (job.received) {
        if (job.truncated) snprintf(job.status, sizeof(job.status),
            "Reply exceeds 512 bytes; showing the first 512 only.\nFull datagram length unknown. Socket closed.");
        else snprintf(job.status, sizeof(job.status), "Received %u-byte datagram after %lu ms. Socket closed.",
                      (unsigned)job.reply_length, (unsigned long)job.reply_ms);
    } else if (error == ECANCELED) {
        snprintf(job.status, sizeof(job.status), job.sent ?
                 "Cancelled waiting for a reply. Socket closed.\nThe sent datagram cannot be recalled." :
                 "Cancelled before sending. Socket closed.");
    } else if (error == ETIMEDOUT) {
        snprintf(job.status, sizeof(job.status), job.sent ?
                 "No reply within the 3-second deadline. Socket closed.\nSend success does not prove delivery." :
                 "Send deadline reached. Socket closed.");
    } else if (error == ENETDOWN) {
        snprintf(job.status, sizeof(job.status), job.sent ?
                 "Wi-Fi lost while waiting for a reply. Socket closed." :
                 "Wi-Fi unavailable before sending. Socket closed.");
    } else {
        snprintf(job.status, sizeof(job.status), "%s failed (socket error %d). Socket closed.\n%s",
                 stage, error, job.sent ? "A datagram was sent; no reply was received." : "Send did not complete.");
    }
    portENTER_CRITICAL(&job_lock);
    published = job;
    job_busy = false;
    job_revision++;
    portEXIT_CRITICAL(&job_lock);
    vTaskDelete(NULL);
}

static void enabled(lv_obj_t *object, bool value)
{
    if (value) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static void disarm(void)
{
    confirmation_armed = false;
    if (send_label) lv_label_set_text(send_label, "SEND");
}

static void keyboard_visible(bool visible)
{
    if (visible) {
        lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(preview_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void preview(lv_obj_t *label, const char *caption, const uint8_t *bytes, size_t length)
{
    /* LVGL copies the text. A reusable bounded buffer keeps this off its stack. */
    static char text[UDP_REPLY_MAX * 4 + 96];
    size_t used = (size_t)snprintf(text, sizeof(text), "%s (%u bytes)\nHEX\n", caption, (unsigned)length);
    for (size_t i = 0; i < length; i++)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%02X%s", bytes[i], i + 1 == length ? "" : " ");
    used += (size_t)snprintf(text + used, sizeof(text) - used, "%s\nASCII (non-printable = .)\n", length ? "" : "(empty)");
    for (size_t i = 0; i < length; i++) text[used++] = bytes[i] >= 32 && bytes[i] <= 126 ? (char)bytes[i] : '.';
    if (!length) used += (size_t)snprintf(text + used, sizeof(text) - used, "(empty)");
    text[used] = '\0';
    lv_label_set_text(label, text);
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if (!status_label) return;
    if (confirmation_armed && (uint32_t)(lv_tick_get() - confirmation_at) >= UDP_CONFIRM_MS) {
        disarm();
        lv_label_set_text(status_label, "Confirmation expired. Tap SEND to review the request again.");
    }
    udp_job_t job;
    bool busy;
    uint32_t revision;
    portENTER_CRITICAL(&job_lock);
    job = published;
    busy = job_busy;
    revision = job_revision;
    portEXIT_CRITICAL(&job_lock);
    for (unsigned i = 0; i < 3; i++) enabled(fields[i], !busy);
    enabled(payload, !busy);
    enabled(mode_select, !busy);
    enabled(send_button, !busy && ui_timer != NULL);
    enabled(cancel_button, busy || confirmation_armed);
    enabled(paste_button, !busy && payload_clipboard_peek() != NULL);
    enabled(copy_reply_button, !busy && job.received && !job.truncated &&
                              job.reply_length <= PAYLOAD_CLIPBOARD_MAX_BYTES);
    if (rendered_revision == revision) return;
    rendered_revision = revision;
    if (!job.present) return;
    char text[192];
    snprintf(text, sizeof(text), "Result for %s:%u | source port %u\n%s input, %u bytes | %s | total %lu ms",
             job.request.host, job.request.port, job.actual_port,
             job.request.mode == BYTE_DATA_HEX ? "Hex" : "ASCII", (unsigned)job.request.length,
             busy ? "Working" : "Socket closed", (unsigned long)job.duration_ms);
    lv_label_set_text(identity_label, text);
    lv_label_set_text(result_label, job.status);
    preview(tx_label, job.sent ? "TX sent" : "TX requested (not sent)", job.request.bytes, job.request.length);
    if (job.received) preview(rx_label, job.truncated ? "RX truncated prefix" : "RX", job.reply, job.reply_length);
    else lv_label_set_text(rx_label, "RX: no reply received.");
    lv_label_set_text(status_label, busy ? "CANCEL or Home stops waiting. A sent packet cannot be recalled." :
                      "Tap SEND to review another request. No automatic retries.");
}

static bool parse_port(const char *text, bool allow_zero, uint16_t *port)
{
    unsigned value = 0, digits = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || ++digits > 5) return false;
        value = value * 10 + (unsigned)(*text - '0');
    }
    if (!digits || value > 65535 || (!allow_zero && value == 0)) return false;
    *port = (uint16_t)value;
    return true;
}

static bool form_request(udp_request_t *request)
{
    memset(request, 0, sizeof(*request));
    uint32_t ip;
    if (!ipv4_parse(saved_fields[0], &ip)) {
        lv_label_set_text(status_label, "Enter a literal unicast IPv4 address, without spaces or leading zeros.");
        return false;
    }
    ipv4_format(ip, request->host);
    if ((ip >> 24) == 0 || (ip >> 28) >= 14 || local_broadcast(ip)) {
        lv_label_set_text(status_label, "Choose a unicast IPv4 peer. Broadcast and multicast are not supported.");
        return false;
    }
    if (!parse_port(saved_fields[1], false, &request->port) ||
        !parse_port(saved_fields[2], true, &request->source_port)) {
        lv_label_set_text(status_label, "Peer port: 1-65535. Source port: 0 (automatic) or 1-65535. Decimal digits only.");
        return false;
    }
    byte_data_t data;
    byte_data_status_t status = byte_data_parse(input_mode == BYTE_DATA_HEX ? saved_hex : saved_ascii, input_mode, &data);
    if (status != BYTE_DATA_OK) {
        lv_label_set_text(status_label, byte_data_error(status));
        return false;
    }
    request->length = data.length;
    memcpy(request->bytes, data.bytes, data.length);
    request->mode = input_mode;
    return true;
}

static void send_clicked(lv_event_t *event)
{
    (void)event;
    if (udp_tool_busy() || !ui_timer) return;
    udp_request_t request;
    if (!form_request(&request)) { disarm(); return; }
    if (!wifi_ready()) {
        disarm();
        lv_label_set_text(status_label, "Connect to Wi-Fi with an IPv4 address before sending.");
        return;
    }
    bool unchanged = !strcmp(request.host, confirmation.host) && request.port == confirmation.port &&
        request.source_port == confirmation.source_port && request.mode == confirmation.mode &&
        request.length == confirmation.length && !memcmp(request.bytes, confirmation.bytes, request.length);
    keyboard_visible(false);
    if (!confirmation_armed || !unchanged || (uint32_t)(lv_tick_get() - confirmation_at) >= UDP_CONFIRM_MS) {
        confirmation = request;
        confirmation_armed = true;
        confirmation_at = lv_tick_get();
        lv_label_set_text(send_label, "CONFIRM");
        char text[192];
        snprintf(text, sizeof(text), "Send ONE cleartext datagram (%u bytes) to %s:%u?\n"
                 "Source port %u (0 = automatic). CONFIRM within 5 seconds.",
                 (unsigned)request.length, request.host, request.port, request.source_port);
        lv_label_set_text(status_label, text);
        refresh(NULL);
        return;
    }
    disarm();
    lv_keyboard_set_textarea(keyboard, NULL);
    portENTER_CRITICAL(&job_lock);
    memset(&published, 0, sizeof(published));
    published.accepted_us = esp_timer_get_time();
    published.request = request;
    published.present = true;
    strcpy(published.status, "Starting one UDP exchange...");
    job_cancelled = false;
    job_busy = true;
    job_revision++;
    portEXIT_CRITICAL(&job_lock);
    if (xTaskCreate(udp_worker, "udp-exchange", 6144, NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&job_lock);
        strcpy(published.status, "Could not allocate the worker. No datagram sent.");
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
    lv_label_set_text(status_label, busy ? "Cancelling the exchange. Sent data cannot be recalled." :
                      "Confirmation cancelled. No new datagram sent.");
    refresh(NULL);
}

static void input_event(lv_event_t *event)
{
    if (loading_input || !keyboard || udp_tool_busy()) return;
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    lv_obj_t *object = lv_event_get_target_obj(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_mode_t mode = index < 3 ? LV_KEYBOARD_MODE_NUMBER :
            input_mode == BYTE_DATA_HEX ? LV_KEYBOARD_MODE_USER_1 : LV_KEYBOARD_MODE_TEXT_LOWER;
        lv_keyboard_set_mode(keyboard, mode);
        lv_obj_set_style_text_font(keyboard, mode == LV_KEYBOARD_MODE_TEXT_LOWER ?
                                   &lv_font_montserrat_14 : &lv_font_montserrat_28, 0);
        lv_keyboard_set_textarea(keyboard, object);
        keyboard_visible(true);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        if (index < 3) snprintf(saved_fields[index], sizeof(saved_fields[index]), "%s", lv_textarea_get_text(object));
        else if (input_mode == BYTE_DATA_HEX) snprintf(saved_hex, sizeof(saved_hex), "%s", lv_textarea_get_text(object));
        else snprintf(saved_ascii, sizeof(saved_ascii), "%s", lv_textarea_get_text(object));
        disarm();
        lv_label_set_text(status_label, "Inputs changed. The previous result below retains its original request.");
    }
}

static void mode_changed(lv_event_t *event)
{
    (void)event;
    if (udp_tool_busy()) return;
    input_mode = lv_dropdown_get_selected(mode_select) == 0 ? BYTE_DATA_HEX : BYTE_DATA_ASCII;
    loading_input = true;
    lv_textarea_set_max_length(payload, input_mode == BYTE_DATA_HEX ? BYTE_DATA_MAX_TEXT + 1 : BYTE_DATA_MAX_BYTES + 1);
    lv_textarea_set_text(payload, input_mode == BYTE_DATA_HEX ? saved_hex : saved_ascii);
    loading_input = false;
    disarm();
    keyboard_visible(false);
    lv_keyboard_set_textarea(keyboard, NULL);
    lv_label_set_text(status_label, "Mode changed. Review the payload before sending. No escapes are expanded.");
}

static void keyboard_done(lv_event_t *event)
{
    (void)event;
    keyboard_visible(false); /* Done never sends or confirms a datagram. */
}

static void paste_clicked(lv_event_t *event)
{
    (void)event;
    if (udp_tool_busy() || !payload_clipboard_peek()) return;
    if (!payload_clipboard_hex(saved_hex, sizeof(saved_hex))) return;
    lv_dropdown_set_selected(mode_select, BYTE_DATA_HEX);
    mode_changed(NULL); /* Replaces only Hex, disarms confirmation, hides keys. */
    refresh(NULL);
    lv_label_set_text(status_label, "Pasted bytes into the Hex draft. Review the peer and payload, then SEND. Nothing sent.");
}

static void copy_reply_clicked(lv_event_t *event)
{
    (void)event;
    udp_job_t job;
    portENTER_CRITICAL(&job_lock);
    bool busy = job_busy;
    job = published;
    portEXIT_CRITICAL(&job_lock);
    if (busy) return;
    if (!job.received || job.truncated || job.reply_length > PAYLOAD_CLIPBOARD_MAX_BYTES) {
        lv_label_set_text(status_label, "Copy needs a complete reply of 0-128 bytes. Clipboard unchanged.");
        return;
    }
    payload_clipboard_store(job.reply, job.reply_length);
    disarm();
    refresh(NULL);
    char text[128];
    snprintf(text, sizeof(text), "Copied %u reply bytes. Open Byte Lab and Paste hex to inspect them. Nothing sent.",
             (unsigned)job.reply_length);
    lv_label_set_text(status_label, text);
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

static void field(lv_obj_t *parent, const char *caption, unsigned index, int width)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_size(block, width, 82);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, 4, 0);
    lv_obj_set_width(label(block, caption, true), width);
    fields[index] = lv_textarea_create(block);
    lv_obj_set_size(fields[index], width, 60);
    /* Keep pasted newlines so the parser can reject the original draft. */
    lv_textarea_set_max_length(fields[index], 32);
    lv_textarea_set_text(fields[index], saved_fields[index]);
    lv_obj_add_event_cb(fields[index], input_event, LV_EVENT_ALL, (void *)(uintptr_t)index);
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_set_size(object, 310, 64);
    lv_obj_t *caption = lv_label_create(object);
    lv_label_set_text(caption, text);
    lv_obj_center(caption);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, NULL);
    return object;
}

void udp_tool_stop(void)
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
    payload = NULL;
    mode_select = NULL;
    send_button = NULL;
    send_label = NULL;
    cancel_button = NULL;
    paste_button = copy_reply_button = NULL;
    status_label = NULL;
    identity_label = NULL;
    result_label = NULL;
    preview_panel = NULL;
    tx_label = NULL;
    rx_label = NULL;
    loading_input = false;
}

void udp_tool_show(lv_obj_t *parent, bool connected)
{
    lv_obj_set_style_pad_row(parent, 12, 0);
    lv_obj_t *column = lv_obj_create(parent);
    lv_obj_remove_style_all(column);
    lv_obj_set_size(column, 640, LV_SIZE_CONTENT);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 12, 0);
    lv_obj_set_style_text_font(column, &lv_font_montserrat_28, 0);
    label(column, "UDP Console", false);
    label(column, "One unencrypted datagram, then one reply from the same IPv4/port.\n"
                  "0-128 TX bytes; 512-byte RX preview; 3-second deadline; no retries.", true);
    lv_obj_t *target = row(column, 82);
    field(target, "Peer IPv4 (unicast)", 0, 430);
    field(target, "Peer port", 1, 198);
    lv_obj_t *options = row(column, 82);
    field(options, "Source port (0 = automatic)", 2, 310);
    lv_obj_t *mode_block = lv_obj_create(options);
    lv_obj_remove_style_all(mode_block);
    lv_obj_set_size(mode_block, 310, 82);
    lv_obj_remove_flag(mode_block, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(mode_block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mode_block, 4, 0);
    lv_obj_set_width(label(mode_block, "Payload format", true), 310);
    mode_select = lv_dropdown_create(mode_block);
    lv_obj_set_size(mode_select, 310, 60);
    lv_dropdown_set_options(mode_select, "Hex byte pairs\nLiteral ASCII");
    lv_dropdown_set_selected(mode_select, input_mode);
    lv_obj_add_event_cb(mode_select, mode_changed, LV_EVENT_VALUE_CHANGED, NULL);
    payload = lv_textarea_create(column);
    lv_obj_set_size(payload, 640, 104);
    lv_textarea_set_max_length(payload, input_mode == BYTE_DATA_HEX ? BYTE_DATA_MAX_TEXT + 1 : BYTE_DATA_MAX_BYTES + 1);
    lv_textarea_set_text(payload, input_mode == BYTE_DATA_HEX ? saved_hex : saved_ascii);
    lv_obj_add_event_cb(payload, input_event, LV_EVENT_ALL, (void *)(uintptr_t)3);
    lv_obj_t *clipboard_actions = row(column, 64);
    paste_button = button(clipboard_actions, "PASTE BYTES", paste_clicked);
    copy_reply_button = button(clipboard_actions, "COPY REPLY", copy_reply_clicked);
    keyboard = lv_keyboard_create(column);
    lv_obj_set_size(keyboard, 640, 240);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1, hex_keys, hex_controls);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_CANCEL, NULL);
    lv_obj_t *actions = row(column, 64);
    send_button = button(actions, "SEND", send_clicked);
    send_label = lv_obj_get_child(send_button, 0);
    cancel_button = button(actions, "CANCEL", cancel_clicked);
    status_label = label(column, connected ? "Enter a peer. SEND reviews the request; a second tap confirms it." :
                         "Connect to Wi-Fi before sending. You can prepare the request now.", true);
    lv_obj_set_height(status_label, 56);
    identity_label = label(column, "No UDP exchange yet.", true);
    lv_obj_set_height(identity_label, 42);
    result_label = label(column, "Replies are unauthenticated. Empty input sends an empty datagram.", true);
    lv_obj_set_height(result_label, 48);
    preview_panel = lv_obj_create(column);
    lv_obj_set_size(preview_panel, 640, 220);
    lv_obj_set_flex_flow(preview_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(preview_panel, 12, 0);
    tx_label = label(preview_panel, "Sent bytes and received bytes appear here in hex and ASCII.", true);
    rx_label = label(preview_panel, "", true);
    lv_obj_set_width(tx_label, LV_PCT(100));
    lv_obj_set_width(rx_label, LV_PCT(100));
    label(column, "Paste replaces the Hex draft; copy accepts complete replies up to 128 bytes.\n"
                  "Inputs, clipboard and last exchange stay in RAM until restart.\n"
                  "CANCEL/Home closes the exchange; transmitted data cannot be recalled.", true);
    rendered_revision = UINT32_MAX;
    ui_timer = lv_timer_create(refresh, 100, NULL);
    refresh(NULL);
    if (!ui_timer) lv_label_set_text(status_label, "Could not allocate the display timer. Return Home and reopen the app.");
}
