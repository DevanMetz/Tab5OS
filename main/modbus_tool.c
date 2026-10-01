#include "modbus_tool.h"
#include "ipv4_data.h"

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
#include "modbus_data.h"

#define MODBUS_DEADLINE_US 5000000LL
#define MODBUS_CONFIRM_MS 5000U

typedef struct {
    char host[16];
    uint16_t port;
    modbus_request_t request;
    bool probe;
    bool present;
    uint32_t duration_ms;
    char status[160];
    modbus_result_t result;
    bool probe_connected;
} modbus_job_t;

static portMUX_TYPE job_lock = portMUX_INITIALIZER_UNLOCKED;
static bool job_busy;
static bool job_cancelled;
static uint32_t job_revision;
static modbus_job_t published;
static modbus_job_t confirmation;
static bool confirmation_armed;
static uint32_t confirmation_at;
static uint16_t next_transaction;
/* Retain one character beyond each accepted length, including rejected UTF-8.
 * Truncation or filtering must never turn an invalid request into a valid one. */
static char saved_inputs[5][IPV4_TEXT_SIZE * 4 + 1] = {"192.168.1.100", "502", "1", "0", "1"};
static unsigned function_index = 2;
static modbus_value_view_t value_view;
static modbus_byte_order_t byte_order;
static lv_timer_t *ui_timer;
static lv_obj_t *fields[5];
static lv_obj_t *function_select;
static lv_obj_t *view_select;
static lv_obj_t *order_select;
static lv_obj_t *read_button;
static lv_obj_t *test_button;
static lv_obj_t *stop_button;
static lv_obj_t *read_label;
static lv_obj_t *status_label;
static lv_obj_t *identity_label;
static lv_obj_t *result_label;
static lv_obj_t *result_box;
static lv_obj_t *keyboard;
static uint32_t rendered_revision;

bool modbus_tool_busy(void)
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

static int operation_error(int64_t deadline)
{
    if (cancelled()) return ECANCELED;
    if (esp_timer_get_time() >= deadline) return ETIMEDOUT;
    return 0;
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

static int transfer(int fd, uint8_t *bytes, size_t length, bool writing, int64_t deadline)
{
    size_t offset = 0;
    while (offset < length) {
        int error = wait_socket(fd, writing, deadline);
        if (error) return error;
        ssize_t count = writing ? send(fd, bytes + offset, length - offset, 0) :
                                  recv(fd, bytes + offset, length - offset, 0);
        if (count > 0) offset += (size_t)count;
        else if (count == 0) return ECONNRESET;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return errno ? errno : EIO;
    }
    return operation_error(deadline);
}

static void modbus_worker(void *argument)
{
    (void)argument;
    modbus_job_t job;
    portENTER_CRITICAL(&job_lock);
    job = published;
    portEXIT_CRITICAL(&job_lock);
    int64_t started = esp_timer_get_time();
    int64_t deadline = started + MODBUS_DEADLINE_US;
    int fd = -1;
    int error = operation_error(deadline);
    const char *stage = "Connect";
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(job.port)};
    uint32_t ip;
    if (!error && !ipv4_parse(job.host, &ip)) error = EINVAL;
    if (!error) peer.sin_addr.s_addr = htonl(ip);
    if (!error) {
        fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) error = errno ? errno : EIO;
    }
    if (!error && fcntl(fd, F_SETFL, O_NONBLOCK) < 0) error = errno ? errno : EIO;
    /* lwIP also uses SO_SNDTIMEO to bound close retries under memory pressure.
     * Nonblocking I/O itself uses the absolute operation deadline below. */
    struct timeval close_timeout = {.tv_sec = 0, .tv_usec = 100000};
    if (!error && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &close_timeout, sizeof(close_timeout)) != 0)
        error = errno ? errno : EIO;
    if (!error && connect(fd, (const struct sockaddr *)&peer, sizeof(peer)) < 0) {
        if (errno != EINPROGRESS && errno != EWOULDBLOCK) error = errno ? errno : EIO;
        else {
            error = wait_socket(fd, true, deadline);
            if (!error) {
                socklen_t size = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0) error = errno ? errno : EIO;
            }
        }
    }
    if (!error) error = operation_error(deadline);
    if (!error && job.probe) {
        snprintf(job.status, sizeof(job.status), "TCP connection succeeded; no application bytes sent.");
        job.probe_connected = true;
    } else if (!error) {
        uint8_t request[MODBUS_REQUEST_BYTES];
        uint8_t response[MODBUS_MAX_RESPONSE_BYTES];
        size_t response_size = 0;
        stage = "Send";
        if (!modbus_build_request(&job.request, request)) error = EINVAL;
        if (!error) error = transfer(fd, request, sizeof(request), true, deadline);
        if (!error) {
            stage = "Receive";
            error = transfer(fd, response, 6, false, deadline);
        }
        if (!error && !modbus_response_length(&job.request, response, &response_size)) {
            snprintf(job.status, sizeof(job.status), "Rejected response: transaction, protocol or length does not match.");
        } else if (!error) {
            error = transfer(fd, response + 6, response_size - 6, false, deadline);
            if (!error) {
                modbus_status_t parsed = modbus_parse_response(&job.request, response, response_size, &job.result);
                if (parsed == MODBUS_OK) {
                    snprintf(job.status, sizeof(job.status), "Read completed: %u value%s.",
                             (unsigned)job.result.count, job.result.count == 1 ? "" : "s");
                } else if (parsed == MODBUS_EXCEPTION) {
                    snprintf(job.status, sizeof(job.status), "Device returned Modbus exception 0x%02X.", job.result.exception_code);
                } else {
                    snprintf(job.status, sizeof(job.status), "Rejected response: %s.", modbus_data_error(parsed));
                }
            }
        }
    }
    /* Only this worker owns and closes the socket, including cancellation. */
    if (fd >= 0) close(fd);
    if (cancelled()) error = ECANCELED;
    if (error) {
        memset(&job.result, 0, sizeof(job.result));
        job.probe_connected = false;
        if (error == ECANCELED) snprintf(job.status, sizeof(job.status), "Cancelled; connection closed.");
        else if (error == ETIMEDOUT) snprintf(job.status, sizeof(job.status), "The 5-second exchange timed out; connection closed.");
        else snprintf(job.status, sizeof(job.status), "%s failed (socket error %d); connection closed.", stage, error);
    }
    job.duration_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    portENTER_CRITICAL(&job_lock);
    if (job_cancelled) {
        strcpy(job.status, "Cancelled; connection closed.");
        memset(&job.result, 0, sizeof(job.result));
        job.probe_connected = false;
    }
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

static void render_values(const modbus_job_t *job, bool busy)
{
    bool registers = !busy && !job->probe && job->result.count && job->request.function >= 3;
    set_enabled(view_select, registers);
    set_enabled(order_select, registers && value_view != MODBUS_VIEW_WORDS);
    if (!job->present) return;
    char output[MODBUS_VALUES_TEXT_SIZE];
    const char *text = busy ? "Waiting for result..." : "No values returned.";
    if (job->probe_connected)
        text = "An open TCP port does not establish that this is a Modbus device.";
    else if (job->result.count) {
        text = modbus_format_values(&job->request, &job->result, value_view, byte_order, output, sizeof(output)) ?
               output : "Could not format this result.";
    }
    lv_label_set_text(result_label, text);
    lv_obj_scroll_to_y(result_box, 0, LV_ANIM_OFF);
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if (!status_label) return;
    if (confirmation_armed && (uint32_t)(lv_tick_get() - confirmation_at) > MODBUS_CONFIRM_MS) {
        confirmation_armed = false;
        lv_label_set_text(read_label, "READ");
        lv_label_set_text(status_label, "Read confirmation expired. Tap READ to arm again.");
    }
    modbus_job_t job;
    bool busy;
    uint32_t revision;
    portENTER_CRITICAL(&job_lock);
    job = published;
    busy = job_busy;
    revision = job_revision;
    portEXIT_CRITICAL(&job_lock);
    if (revision == rendered_revision) return;
    rendered_revision = revision;
    for (unsigned i = 0; i < 5; i++) set_enabled(fields[i], !busy);
    set_enabled(function_select, !busy);
    set_enabled(read_button, !busy);
    set_enabled(test_button, !busy);
    set_enabled(stop_button, busy);
    render_values(&job, busy);
    if (!job.present) return;
    char identity[192];
    if (job.probe)
        snprintf(identity, sizeof(identity), "TCP test: %s:%u\n%s, %lu ms", job.host, job.port,
                 busy ? "In progress" : "Connection closed", (unsigned long)job.duration_ms);
    else
        snprintf(identity, sizeof(identity), "Result for %s:%u | unit %u | FC %02u | transaction %u\n"
                 "Zero-based address %u, count %u | %s | %lu ms", job.host, job.port, job.request.unit_id,
                 job.request.function, job.request.transaction_id, job.request.address, job.request.quantity,
                 busy ? "In progress" : "Connection closed", (unsigned long)job.duration_ms);
    lv_label_set_text(identity_label, identity);
    lv_label_set_text(status_label, job.status);
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

static bool form_job(modbus_job_t *job, bool probe)
{
    memset(job, 0, sizeof(*job));
    uint32_t address;
    uint16_t unit;
    if (!ipv4_parse(saved_inputs[0], &address)) {
        lv_label_set_text(status_label, "Enter a full decimal IPv4 address, without spaces or leading zeros.");
        return false;
    }
    ipv4_format(address, job->host);
    if (!modbus_parse_decimal(saved_inputs[1], 65535, &job->port) || !job->port) {
        lv_label_set_text(status_label, "TCP port must be 1 to 65535.");
        return false;
    }
    job->probe = probe;
    job->present = true;
    if (probe) return true;
    if (!modbus_parse_decimal(saved_inputs[2], 255, &unit)) {
        lv_label_set_text(status_label, "Unit ID must be 0 to 255.");
        return false;
    }
    job->request.unit_id = (uint8_t)unit;
    job->request.function = (uint8_t)(function_index + 1);
    if (!modbus_parse_decimal(saved_inputs[3], 65535, &job->request.address) ||
        !modbus_parse_decimal(saved_inputs[4], MODBUS_MAX_VALUES, &job->request.quantity) ||
        !job->request.quantity || (uint32_t)job->request.address + job->request.quantity > 65536) {
        lv_label_set_text(status_label, "Address must be 0 to 65535; count 1 to 16. Last address must not exceed 65535.");
        return false;
    }
    return true;
}

static bool same_request(const modbus_job_t *a, const modbus_job_t *b)
{
    return !strcmp(a->host, b->host) && a->port == b->port &&
           a->request.unit_id == b->request.unit_id && a->request.function == b->request.function &&
           a->request.address == b->request.address && a->request.quantity == b->request.quantity;
}

static void keyboard_visible(bool visible)
{
    if (visible) {
        lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(result_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(result_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void start_clicked(lv_event_t *event)
{
    if (modbus_tool_busy()) return;
    bool probe = (uintptr_t)lv_event_get_user_data(event) != 0;
    modbus_job_t job;
    if (!form_job(&job, probe)) return;
    if (!wifi_ready()) {
        confirmation_armed = false;
        lv_label_set_text(read_label, "READ");
        lv_label_set_text(status_label, "Connect to Wi-Fi with an IPv4 address before starting.");
        return;
    }
    uint32_t now = lv_tick_get();
    if (!probe && (!confirmation_armed || (uint32_t)(now - confirmation_at) > MODBUS_CONFIRM_MS ||
                   !same_request(&job, &confirmation))) {
        confirmation = job;
        confirmation_armed = true;
        confirmation_at = now;
        lv_label_set_text(read_label, "CONFIRM");
        lv_label_set_text(status_label, "Plain Modbus TCP exposes requests and replies. Tap CONFIRM within 5 seconds to send this read.");
        return;
    }
    confirmation_armed = false;
    lv_label_set_text(read_label, "READ");
    lv_keyboard_set_textarea(keyboard, NULL);
    keyboard_visible(false);
    job.request.transaction_id = ++next_transaction;
    snprintf(job.status, sizeof(job.status), "%s in progress; STOP or Home cancels. Exchange limit: 5 seconds.",
             probe ? "TCP test" : "Read");
    portENTER_CRITICAL(&job_lock);
    published = job;
    job_cancelled = false;
    job_busy = true;
    job_revision++;
    portEXIT_CRITICAL(&job_lock);
    if (xTaskCreate(modbus_worker, "modbus-read", 6144, NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&job_lock);
        snprintf(published.status, sizeof(published.status), "Could not allocate the Modbus worker. No connection was opened.");
        job_busy = false;
        job_revision++;
        portEXIT_CRITICAL(&job_lock);
    }
    refresh(NULL);
}

static void stop_clicked(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&job_lock);
    bool busy = job_busy;
    if (busy) job_cancelled = true;
    portEXIT_CRITICAL(&job_lock);
    if (busy) lv_label_set_text(status_label, "Cancelling; waiting for the worker to close its connection...");
    else {
        rendered_revision = UINT32_MAX;
        refresh(NULL);
    }
}

static void input_event(lv_event_t *event)
{
    if (!keyboard || modbus_tool_busy()) return;
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *object = lv_event_get_target_obj(event);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_textarea(keyboard, object);
        keyboard_visible(true);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
        snprintf(saved_inputs[index], sizeof(saved_inputs[index]), "%s", lv_textarea_get_text(object));
        confirmation_armed = false;
        lv_label_set_text(read_label, "READ");
        lv_label_set_text(status_label, "Inputs updated. Any displayed result belongs to the request identified below.");
    }
}

static void function_changed(lv_event_t *event)
{
    (void)event;
    function_index = lv_dropdown_get_selected(function_select);
    confirmation_armed = false;
    lv_label_set_text(read_label, "READ");
    lv_label_set_text(status_label, "Function updated. Any displayed result belongs to the request identified below.");
}

static void view_changed(lv_event_t *event)
{
    (void)event;
    if (modbus_tool_busy()) return;
    value_view = (modbus_value_view_t)lv_dropdown_get_selected(view_select);
    byte_order = (modbus_byte_order_t)lv_dropdown_get_selected(order_select);
    confirmation_armed = false;
    lv_label_set_text(read_label, "READ");
    modbus_job_t job;
    portENTER_CRITICAL(&job_lock);
    job = published;
    portEXIT_CRITICAL(&job_lock);
    render_values(&job, false);
    lv_label_set_text(status_label, "View updated for the saved reply. No new request was sent.");
}

static void keyboard_done(lv_event_t *event)
{
    (void)event;
    keyboard_visible(false);
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

static lv_obj_t *label(lv_obj_t *parent, const char *text, bool small)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_width(object, 640);
    if (small) lv_obj_set_style_text_font(object, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    lv_label_set_text(object, text);
    return object;
}

static lv_obj_t *field_block(lv_obj_t *parent, const char *name, int width)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_size(block, width, 80);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, 4, 0);
    lv_obj_t *caption = label(block, name, true);
    lv_obj_set_width(caption, width);
    return block;
}

static void input(lv_obj_t *parent, const char *name, unsigned index, int width)
{
    lv_obj_t *block = field_block(parent, name, width);
    fields[index] = lv_textarea_create(block);
    /* The default multiline mode preserves pasted newlines for validation. */
    lv_obj_set_size(fields[index], width, 60);
    lv_textarea_set_max_length(fields[index], index ? 6 : IPV4_TEXT_SIZE);
    lv_textarea_set_text(fields[index], saved_inputs[index]);
    lv_obj_add_event_cb(fields[index], input_event, LV_EVENT_ALL, (void *)(uintptr_t)index);
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_set_size(object, 206, 60);
    lv_obj_t *caption = lv_label_create(object);
    lv_label_set_text(caption, text);
    lv_obj_center(caption);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, user_data);
    return object;
}

static void readable_options(lv_obj_t *dropdown)
{
    /* Popup lists are attached to the screen, outside the column's font style. */
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_28, LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(list, 20, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 260, LV_PART_MAIN);
}

void modbus_tool_stop(void)
{
    portENTER_CRITICAL(&job_lock);
    if (job_busy) job_cancelled = true;
    portEXIT_CRITICAL(&job_lock);
    if (ui_timer) lv_timer_delete(ui_timer);
    ui_timer = NULL;
    if (keyboard) lv_keyboard_set_textarea(keyboard, NULL);
    keyboard = NULL;
    for (unsigned i = 0; i < 5; i++) fields[i] = NULL;
    function_select = NULL;
    view_select = NULL;
    order_select = NULL;
    read_button = NULL;
    test_button = NULL;
    stop_button = NULL;
    read_label = NULL;
    status_label = NULL;
    identity_label = NULL;
    result_label = NULL;
    result_box = NULL;
    confirmation_armed = false;
}

void modbus_tool_show(lv_obj_t *parent, bool connected)
{
    lv_obj_set_style_pad_row(parent, 10, 0);
    lv_obj_t *column = lv_obj_create(parent);
    lv_obj_remove_style_all(column);
    lv_obj_set_size(column, 640, LV_SIZE_CONTENT);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 10, 0);
    lv_obj_set_style_text_font(column, &lv_font_montserrat_28, 0);
    label(column, "Modbus TCP Inspector", false);
    label(column, "Read coils, discrete inputs or registers from a device you manage.\n"
                  "One request per read. TCP test opens a connection without sending data.", true);
    lv_obj_t *destination = row(column, 80);
    input(destination, "Device IPv4 address", 0, 430);
    input(destination, "TCP port (1-65535)", 1, 198);
    lv_obj_t *operation = row(column, 80);
    lv_obj_t *function = field_block(operation, "Read function", 430);
    function_select = lv_dropdown_create(function);
    readable_options(function_select);
    lv_obj_set_size(function_select, 430, 60);
    lv_dropdown_set_options(function_select, "01 Coils\n02 Discrete inputs\n03 Holding registers\n04 Input registers");
    lv_dropdown_set_selected(function_select, function_index);
    lv_obj_add_event_cb(function_select, function_changed, LV_EVENT_VALUE_CHANGED, NULL);
    input(operation, "Unit ID (0-255)", 2, 198);
    lv_obj_t *range = row(column, 80);
    input(range, "Zero-based start address (0-65535)", 3, 430);
    input(range, "Count (1-16)", 4, 198);
    keyboard = lv_keyboard_create(column);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_obj_set_size(keyboard, 640, 220);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_CANCEL, NULL);
    lv_obj_t *actions = row(column, 60);
    read_button = button(actions, "READ", start_clicked, NULL);
    read_label = lv_obj_get_child(read_button, 0);
    test_button = button(actions, "TEST TCP", start_clicked, (void *)1);
    stop_button = button(actions, "STOP", stop_clicked, NULL);
    status_label = label(column, connected ? "Ready. READ requires a second tap for cleartext confirmation." :
                         "Connect to Wi-Fi before starting. You can configure the request now.", true);
    lv_obj_set_height(status_label, 44);
    identity_label = label(column, "No request has been sent.", true);
    lv_obj_set_height(identity_label, 44);
    lv_obj_t *views = row(column, 80);
    lv_obj_t *view_block = field_block(views, "Saved reply: value view", 310);
    view_select = lv_dropdown_create(view_block);
    readable_options(view_select);
    lv_obj_set_size(view_select, 310, 60);
    lv_dropdown_set_options(view_select, "16-bit registers\nUnsigned 32-bit\nSigned 32-bit\nFloat32");
    lv_dropdown_set_selected(view_select, value_view);
    lv_obj_add_event_cb(view_select, view_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *order_block = field_block(views, "32-bit byte order", 310);
    order_select = lv_dropdown_create(order_block);
    readable_options(order_select);
    lv_obj_set_size(order_select, 310, 60);
    lv_dropdown_set_options(order_select, "ABCD\nCDAB\nBADC\nDCBA");
    lv_dropdown_set_selected(order_select, byte_order);
    lv_obj_add_event_cb(order_select, view_changed, LV_EVENT_VALUE_CHANGED, NULL);
    label(column, "A B = first register bytes; C D = next register bytes.\n"
                  "Pairs start at the read address. Check the device's mapping and scaling.", true);
    result_box = lv_obj_create(column);
    lv_obj_set_size(result_box, 640, 250);
    lv_obj_set_style_pad_all(result_box, 16, 0);
    lv_obj_set_scroll_dir(result_box, LV_DIR_VER);
    result_label = label(result_box, "Results appear here. After a register read, choose a value view.\n"
                         "Changing the view reinterprets the saved reply without sending a request.", true);
    lv_obj_set_width(result_label, 596);
    rendered_revision = UINT32_MAX;
    refresh(NULL);
    ui_timer = lv_timer_create(refresh, 100, NULL);
    if (!ui_timer) {
        set_enabled(read_button, false);
        set_enabled(test_button, false);
        lv_label_set_text(status_label, "Could not allocate the display timer. Return Home and reopen the app.");
    }
}
