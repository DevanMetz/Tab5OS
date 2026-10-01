#include "ntp_tool.h"
#include "ipv4_data.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "ntp_data.h"

#define NTP_SAMPLE_US INT64_C(2000000)
#define NTP_SESSION_US INT64_C(10000000)

typedef struct {
    ntp_result_t result;
    bool attempted;
    bool valid;
    char note[96];
} sample_t;

typedef struct {
    char host[16];
    uint16_t port;
    int64_t clock_unix_us;
    uint32_t duration_ms;
    bool present;
    sample_t samples[NTP_SAMPLE_COUNT];
    char status[144];
} ntp_job_t;

static portMUX_TYPE job_lock = portMUX_INITIALIZER_UNLOCKED;
static ntp_job_t published;
static bool job_busy;
static bool job_cancelled;
static uint32_t job_revision;
/* Keep invalid drafts intact, including one excess character and UTF-8. */
static char saved_host[IPV4_TEXT_SIZE * 4 + 1];
static char saved_port[6 * 4 + 1] = "123";
static lv_timer_t *ui_timer;
static lv_obj_t *host_area;
static lv_obj_t *port_area;
static lv_obj_t *start_button;
static lv_obj_t *stop_button;
static lv_obj_t *status_label;
static lv_obj_t *identity_label;
static lv_obj_t *summary_label;
static lv_obj_t *metadata_label;
static lv_obj_t *samples_label;
static lv_obj_t *keyboard;
static uint32_t rendered_revision;

static int64_t unix_now(void)
{
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) return 0;
    return (int64_t)now.tv_sec * 1000000 + now.tv_usec;
}

bool ntp_tool_busy(void)
{
    portENTER_CRITICAL(&job_lock);
    bool busy = job_busy;
    portEXIT_CRITICAL(&job_lock);
    return busy;
}

static int operation_error(int64_t deadline)
{
    portENTER_CRITICAL(&job_lock);
    bool cancel = job_cancelled;
    portEXIT_CRITICAL(&job_lock);
    if (cancel) return ECANCELED;
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

static void publish_progress(const ntp_job_t *job)
{
    portENTER_CRITICAL(&job_lock);
    published = *job;
    job_revision++;
    portEXIT_CRITICAL(&job_lock);
}

static int take_sample(int fd, int64_t deadline, sample_t *sample, bool *stop_burst,
                       int64_t *sent_at)
{
    uint8_t request[NTP_PACKET_BYTES];
    int64_t sent_unix = 0, sent_mono = 0;
    for (;;) {
        int error = wait_socket(fd, true, deadline);
        if (error) return error;
        sent_mono = esp_timer_get_time();
        sent_unix = unix_now();
        if (!ntp_build_request(sent_unix, request)) {
            snprintf(sample->note, sizeof(sample->note), "%s", ntp_status_text(NTP_DATA_BAD_CLOCK));
            *stop_burst = true;
            return 0;
        }
        error = operation_error(deadline);
        if (error) return error;
        ssize_t count = send(fd, request, sizeof(request), 0);
        if (count == sizeof(request)) {
            *sent_at = esp_timer_get_time();
            break;
        }
        if (count >= 0) return EIO; /* Datagram sends must be complete. */
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return errno ? errno : EIO;
    }
    unsigned unmatched = 0;
    for (;;) {
        int error = wait_socket(fd, false, deadline);
        if (error) return error;
        /* The extra byte detects oversized UDP replies instead of silently
         * accepting the first 48 bytes of an extension/authentication packet. */
        uint8_t response[NTP_PACKET_BYTES + 1];
        ssize_t count = recv(fd, response, sizeof(response), 0);
        int64_t received_mono = esp_timer_get_time();
        int64_t received_unix = unix_now();
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return errno ? errno : EIO;
        }
        error = operation_error(deadline);
        if (error) return error;
        ntp_status_t status = ntp_parse_response(response, (size_t)count, request, sent_unix,
                                                received_unix, received_mono - sent_mono, &sample->result);
        if (status == NTP_DATA_WRONG_ORIGIN && ++unmatched < 16) continue;
        sample->valid = status == NTP_DATA_OK;
        if (status == NTP_DATA_KISS_OF_DEATH) {
            char code[5];
            for (unsigned i = 0; i < 4; i++) {
                uint8_t byte = sample->result.reference_id[i];
                code[i] = byte >= 32 && byte <= 126 ? (char)byte : '.';
            }
            code[4] = '\0';
            snprintf(sample->note, sizeof(sample->note), "KoD %s: server refused requests; burst stopped", code);
            *stop_burst = true;
        } else {
            snprintf(sample->note, sizeof(sample->note), "%s", ntp_status_text(status));
            if (status == NTP_DATA_BAD_CLOCK) *stop_burst = true;
        }
        return 0;
    }
}

static void ntp_worker(void *argument)
{
    (void)argument;
    ntp_job_t job;
    portENTER_CRITICAL(&job_lock);
    job = published;
    portEXIT_CRITICAL(&job_lock);
    int64_t started = esp_timer_get_time();
    job.clock_unix_us = unix_now();
    int64_t deadline = started + NTP_SESSION_US;
    int error = operation_error(deadline);
    int fd = -1;
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(job.port)};
    uint32_t ip;
    if (!error && !ipv4_parse(job.host, &ip)) error = EINVAL;
    if (!error) peer.sin_addr.s_addr = htonl(ip);
    if (!error) {
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) error = errno ? errno : EIO;
    }
    if (!error && fcntl(fd, F_SETFL, O_NONBLOCK) < 0) error = errno ? errno : EIO;
    /* Connected UDP restricts received datagrams to this address and port.
     * The network stack chooses the ephemeral source port. */
    if (!error && connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) error = errno ? errno : EIO;
    bool stop_burst = false;
    int64_t next_sample = started;
    for (unsigned i = 0; !error && !stop_burst && i < NTP_SAMPLE_COUNT; i++) {
        while (esp_timer_get_time() < next_sample && !(error = operation_error(deadline)))
            vTaskDelay(pdMS_TO_TICKS(25));
        if (error || (error = operation_error(deadline))) break;
        int64_t sample_started = esp_timer_get_time();
        int64_t sample_clock = unix_now();
        int64_t clock_drift = sample_clock - job.clock_unix_us - (sample_started - started);
        if (!ntp_clock_sane(sample_clock) || clock_drift < -5000 || clock_drift > 5000) {
            snprintf(job.status, sizeof(job.status), "Tablet clock changed between samples; burst stopped. UDP socket closed.");
            stop_burst = true;
            break;
        }
        int64_t sample_deadline = sample_started + NTP_SAMPLE_US;
        if (sample_deadline > deadline) sample_deadline = deadline;
        next_sample = sample_started + NTP_SAMPLE_US;
        sample_t *sample = &job.samples[i];
        sample->attempted = true;
        snprintf(sample->note, sizeof(sample->note), "Waiting for reply...");
        snprintf(job.status, sizeof(job.status), "Sampling %u/%u. STOP or Home cancels.", i + 1, NTP_SAMPLE_COUNT);
        publish_progress(&job);
        int64_t sent_at = 0;
        int sample_error = take_sample(fd, sample_deadline, sample, &stop_burst, &sent_at);
        if (sent_at) next_sample = sent_at + NTP_SAMPLE_US;
        if (sample_error) {
            if (sample_error == ETIMEDOUT) snprintf(sample->note, sizeof(sample->note), "No reply within 2 seconds");
            else if (sample_error == ECANCELED) {
                snprintf(sample->note, sizeof(sample->note), "Cancelled");
                error = sample_error;
            } else {
                snprintf(sample->note, sizeof(sample->note), "UDP error %d", sample_error);
                error = sample_error;
            }
        }
        if (stop_burst) snprintf(job.status, sizeof(job.status), "%s", sample->note);
        job.duration_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
        publish_progress(&job);
    }
    if (fd >= 0) close(fd);
    job.duration_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    if (error == ECANCELED) snprintf(job.status, sizeof(job.status), "Cancelled; UDP socket closed. Completed samples are retained.");
    else if (error == ETIMEDOUT) snprintf(job.status, sizeof(job.status), "Session deadline reached; UDP socket closed.");
    else if (error) snprintf(job.status, sizeof(job.status), "UDP operation failed (%d); socket closed.", error);
    else if (!stop_burst) snprintf(job.status, sizeof(job.status), "Finished; UDP socket closed. No clock settings changed.");
    portENTER_CRITICAL(&job_lock);
    if (job_cancelled) strcpy(job.status, "Cancelled; UDP socket closed. Completed samples are retained.");
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

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if (!status_label) return;
    ntp_job_t job;
    bool busy;
    uint32_t revision;
    portENTER_CRITICAL(&job_lock);
    job = published;
    busy = job_busy;
    revision = job_revision;
    portEXIT_CRITICAL(&job_lock);
    if (revision == rendered_revision) return;
    rendered_revision = revision;
    set_enabled(host_area, !busy);
    set_enabled(port_area, !busy);
    set_enabled(start_button, !busy);
    set_enabled(stop_button, busy);
    if (!job.present) return;
    char text[832];
    char when[32] = "unavailable";
    time_t seconds = (time_t)(job.clock_unix_us / 1000000);
    struct tm utc;
    if (gmtime_r(&seconds, &utc)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", &utc);
    snprintf(text, sizeof(text), "Results for %s:%u | %lu ms elapsed\nTablet reference at start: %s",
             job.host, job.port, (unsigned long)job.duration_ms, when);
    lv_label_set_text(identity_label, text);
    lv_label_set_text(status_label, job.status);
    unsigned attempted = 0, valid = 0, best = NTP_SAMPLE_COUNT;
    size_t used = 0;
    for (unsigned i = 0; i < NTP_SAMPLE_COUNT; i++) {
        const sample_t *sample = &job.samples[i];
        if (sample->attempted) attempted++;
        if (sample->valid) {
            valid++;
            if (best == NTP_SAMPLE_COUNT || sample->result.delay_ms < job.samples[best].result.delay_ms) best = i;
        }
        int written;
        if (sample->valid)
            written = snprintf(text + used, sizeof(text) - used,
                               "#%u  RTT %.3f ms | delay %.3f ms%s\n      Offset %+.6g ms\n",
                               i + 1, sample->result.round_trip_ms, sample->result.delay_ms,
                               sample->result.delay_clamped ? " (clamped)" : "", sample->result.offset_ms);
        else
            written = snprintf(text + used, sizeof(text) - used, "#%u  %s\n\n", i + 1,
                               sample->attempted ? sample->note : "Not sampled");
        if (written < 0 || (size_t)written >= sizeof(text) - used) break;
        used += (size_t)written;
    }
    lv_label_set_text(samples_label, text);
    if (best < NTP_SAMPLE_COUNT) {
        const ntp_result_t *result = &job.samples[best].result;
        snprintf(text, sizeof(text), "Min delay %.3f ms | sample %u\nOffset %+.6g ms", result->delay_ms, best + 1, result->offset_ms);
        lv_label_set_text(summary_label, text);
        snprintf(text, sizeof(text), "%u valid | %u failed/missing | %u not sampled\n"
                 "Best sample: NTPv%u | stratum %u | leap %u | RefID %02X%02X%02X%02X",
                 valid, attempted - valid, NTP_SAMPLE_COUNT - attempted, result->version, result->stratum,
                 result->leap, result->reference_id[0], result->reference_id[1], result->reference_id[2], result->reference_id[3]);
    } else {
        lv_label_set_text(summary_label, "No valid timing samples");
        snprintf(text, sizeof(text), "%u valid | %u failed/missing | %u not sampled\nNo server quality metadata from a valid sample.",
                 valid, attempted - valid, NTP_SAMPLE_COUNT - attempted);
    }
    lv_label_set_text(metadata_label, text);
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

static void start_clicked(lv_event_t *event)
{
    (void)event;
    if (ntp_tool_busy()) return;
    ntp_job_t job = {0};
    uint32_t address;
    if (!ipv4_parse(saved_host, &address)) {
        lv_label_set_text(status_label, "Enter the server's full decimal IPv4 address, without spaces or leading zeros.");
        return;
    }
    if (!ntp_parse_port(saved_port, &job.port)) {
        lv_label_set_text(status_label, "UDP port must be 1 to 65535.");
        return;
    }
    if (!wifi_ready()) {
        lv_label_set_text(status_label, "Connect to Wi-Fi with an IPv4 address before sampling.");
        return;
    }
    job.clock_unix_us = unix_now();
    if (!ntp_clock_sane(job.clock_unix_us)) {
        lv_label_set_text(status_label, "Tablet clock is not set (2020-2099 required). Connect Wi-Fi and wait for clock sync.");
        return;
    }
    ipv4_format(address, job.host);
    job.present = true;
    snprintf(job.status, sizeof(job.status), "Opening UDP socket; four samples, at most 10 seconds.");
    lv_keyboard_set_textarea(keyboard, NULL);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    portENTER_CRITICAL(&job_lock);
    published = job;
    job_busy = true;
    job_cancelled = false;
    job_revision++;
    portEXIT_CRITICAL(&job_lock);
    if (xTaskCreate(ntp_worker, "ntp-lab", 6144, NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&job_lock);
        strcpy(published.status, "Could not allocate the NTP worker; no request was sent.");
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
    if (busy) lv_label_set_text(status_label, "Cancelling; waiting for the worker to close its UDP socket...");
    else {
        rendered_revision = UINT32_MAX;
        refresh(NULL);
    }
}

static void field_event(lv_event_t *event)
{
    if (!keyboard || ntp_tool_busy()) return;
    lv_obj_t *field = lv_event_get_target_obj(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_textarea(keyboard, field);
        lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        bool host = field == host_area;
        snprintf(host ? saved_host : saved_port, host ? sizeof(saved_host) : sizeof(saved_port), "%s", lv_textarea_get_text(field));
        lv_label_set_text(status_label, "Inputs updated. Displayed samples still belong to the server identified below.");
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

static lv_obj_t *input(lv_obj_t *parent, const char *caption, const char *value, int width, uint32_t maximum)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_size(block, width, 80);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, 4, 0);
    lv_obj_t *name = label(block, caption, true);
    lv_obj_set_width(name, width);
    lv_obj_t *area = lv_textarea_create(block);
    /* Preserve pasted newlines; validation rejects them instead of joining lines. */
    lv_obj_set_size(area, width, 60);
    lv_textarea_set_max_length(area, maximum + 1);
    lv_textarea_set_text(area, value);
    lv_obj_add_event_cb(area, field_event, LV_EVENT_ALL, NULL);
    return area;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_set_size(object, 310, 60);
    lv_obj_t *caption = lv_label_create(object);
    lv_label_set_text(caption, text);
    lv_obj_center(caption);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, NULL);
    return object;
}

void ntp_tool_stop(void)
{
    portENTER_CRITICAL(&job_lock);
    if (job_busy) job_cancelled = true;
    portEXIT_CRITICAL(&job_lock);
    if (ui_timer) lv_timer_delete(ui_timer);
    ui_timer = NULL;
    if (keyboard) lv_keyboard_set_textarea(keyboard, NULL);
    keyboard = NULL;
    host_area = NULL;
    port_area = NULL;
    start_button = NULL;
    stop_button = NULL;
    status_label = NULL;
    identity_label = NULL;
    summary_label = NULL;
    metadata_label = NULL;
    samples_label = NULL;
}

void ntp_tool_show(lv_obj_t *parent, bool connected)
{
    lv_obj_set_style_pad_row(parent, 10, 0);
    lv_obj_t *column = lv_obj_create(parent);
    lv_obj_remove_style_all(column);
    lv_obj_set_size(column, 640, LV_SIZE_CONTENT);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 12, 0);
    lv_obj_set_style_text_font(column, &lv_font_montserrat_28, 0);
    label(column, "NTP Lab", false);
    label(column, "Compare a server's time with this tablet using four UDP exchanges.\n"
                  "Plain 48-byte NTPv3/v4 replies only; this app never sets the clock.", true);
    lv_obj_t *destination = row(column, 80);
    host_area = input(destination, "Server IPv4 address", saved_host, 430, 15);
    lv_textarea_set_placeholder_text(host_area, "e.g. 192.168.1.10");
    port_area = input(destination, "UDP port (1-65535)", saved_port, 198, 5);
    keyboard = lv_keyboard_create(column);
    lv_obj_set_size(keyboard, 640, 220);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_CANCEL, NULL);
    lv_obj_t *actions = row(column, 60);
    start_button = button(actions, "SAMPLE 4", start_clicked);
    stop_button = button(actions, "STOP", stop_clicked);
    status_label = label(column, connected ? "Ready. Choose a server and tap SAMPLE 4." : "Connect to Wi-Fi before sampling.", true);
    lv_obj_set_height(status_label, 44);
    identity_label = label(column, "No samples sent.", true);
    lv_obj_set_height(identity_label, 40);
    summary_label = label(column, "Minimum delay and clock offset\nwill appear here.", false);
    lv_obj_set_height(summary_label, 80);
    metadata_label = label(column, "Server stratum, leap indicator and reference ID appear after a valid reply.", true);
    lv_obj_set_height(metadata_label, 44);
    samples_label = label(column, "#1  Not sampled\n\n#2  Not sampled\n\n#3  Not sampled\n\n#4  Not sampled", true);
    lv_obj_set_height(samples_label, 168);
    label(column, "Positive offset means the server is ahead of the tablet. Delay removes\n"
                  "server processing time; RTT is measured with a monotonic clock.\n"
                  "Unauthenticated estimates, not a calibration. Local clock must be 2020-2099.\n"
                  "Clock steps over 5ms reject samples; small negative delay is clamped to 1us.", true);
    rendered_revision = UINT32_MAX;
    refresh(NULL);
    ui_timer = lv_timer_create(refresh, 100, NULL);
    if (!ui_timer) {
        set_enabled(start_button, false);
        lv_label_set_text(status_label, "Could not allocate the display timer. Return Home and reopen NTP Lab.");
    }
}
