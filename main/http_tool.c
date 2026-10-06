#include "http_tool.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage_io.h"
#include "http_transport.h"

#if !CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
#error "HTTP Console requires CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT=y"
#endif
/* ESP-IDF 5.4.2 assigns custom transports inside its HTTPS configuration block. */
#if !CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS
#error "HTTP Console requires CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y"
#endif

#define HTTP_URL_MAX 255
#define HTTP_HEADERS_MAX 511
#define HTTP_BODY_MAX 1023
#define HTTP_PREVIEW_MAX 4096
#define HTTP_HEADER_LIMIT 4
#define HTTP_CONFIRM_MS 5000U
#define HTTP_REQUEST_MS 15000U
#define HTTP_LOG_PATH "/sdcard/HTTP/HTTPLOG.CSV"

typedef struct {
    esp_http_client_method_t method;
    bool log_enabled;
    char url[HTTP_URL_MAX + 1];
    char headers[HTTP_HEADERS_MAX + 1];
    char body[HTTP_BODY_MAX + 1];
    esp_err_t error;
    int socket_error;
    int status;
    int log_error;
    uint32_t duration_ms;
    size_t response_bytes;
    size_t preview_length;
    bool truncated;
    bool incomplete;
    bool cancelled;
    bool deadline_expired;
    bool invalid_headers;
    int64_t started_us;
    char content_type[96];
    char location[160];
    char preview[HTTP_PREVIEW_MAX + 1];
} http_job_t;

static const esp_http_client_method_t methods[] = {
    HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_METHOD_PUT, HTTP_METHOD_DELETE,
};
static const char *const method_names[] = {"GET", "POST", "PUT", "DELETE"};

static portMUX_TYPE http_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool request_busy;
static bool cancel_requested;
static http_job_t *completed_job;
static lv_timer_t *ui_timer;
static lv_obj_t *url_area;
static lv_obj_t *headers_area;
static lv_obj_t *body_area;
static lv_obj_t *keyboard;
static lv_obj_t *method_label;
static lv_obj_t *log_label;
static lv_obj_t *send_label;
static lv_obj_t *cancel_button;
static lv_obj_t *request_label;
static lv_obj_t *status_label;
static lv_obj_t *response_area;
static unsigned method_index;
static bool logging_enabled;
static bool screen_connected;
static bool screen_sd_available;
static bool cleartext_armed;
static uint32_t cleartext_hash;
static uint32_t cleartext_armed_at;
static http_tool_storage_error_cb_t storage_error_cb;

static bool prefix_equal(const char *text, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*text++) != tolower((unsigned char)*prefix++)) return false;
    }
    return true;
}

static bool normalize_url(const char *input, char output[HTTP_URL_MAX + 1], bool *secure)
{
    while (*input == ' ' || *input == '\t') input++;
    size_t length = strlen(input);
    while (length && (input[length - 1] == ' ' || input[length - 1] == '\t')) length--;
    if (!length || length > HTTP_URL_MAX) return false;
    memcpy(output, input, length);
    output[length] = '\0';

    size_t scheme_length;
    if (prefix_equal(output, "https://")) {
        *secure = true;
        scheme_length = 8;
    } else if (prefix_equal(output, "http://")) {
        *secure = false;
        scheme_length = 7;
    } else {
        return false;
    }
    const char *authority = output + scheme_length;
    const char *authority_end = strpbrk(authority, "/?#");
    if (!authority_end) authority_end = output + length;
    if (authority == authority_end || memchr(authority, '@', (size_t)(authority_end - authority))) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char)output[i];
        if (value <= 0x20 || value == 0x7f) return false;
    }
    return true;
}

static void safe_log_url(const char *url, char output[HTTP_URL_MAX + 1])
{
    size_t length = strcspn(url, "?#");
    if (length > HTTP_URL_MAX) length = HTTP_URL_MAX;
    memcpy(output, url, length);
    output[length] = '\0';
}

static const char *method_name(esp_http_client_method_t method)
{
    return method_names[method == HTTP_METHOD_GET ? 0 :
                        method == HTTP_METHOD_POST ? 1 :
                        method == HTTP_METHOD_PUT ? 2 : 3];
}

static bool header_name_character(unsigned char value)
{
    return isalnum(value) || strchr("!#$%&'*+-.^_`|~", value) != NULL;
}

static bool forbidden_header(const char *name)
{
    return !strcasecmp(name, "Host") || !strcasecmp(name, "Content-Length") ||
           !strcasecmp(name, "Transfer-Encoding") || !strcasecmp(name, "Connection");
}

static bool parse_headers(char *text, esp_http_client_handle_t client,
                          bool *has_content_type, char *message, size_t message_size)
{
    unsigned count = 0;
    *has_content_type = false;
    char *cursor = text;
    while (*cursor) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        if (newline) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor += strlen(cursor);
        }
        size_t line_length = strlen(line);
        if (line_length && line[line_length - 1] == '\r') line[--line_length] = '\0';
        while (*line == ' ' || *line == '\t') line++;
        if (!*line) continue;
        if (++count > HTTP_HEADER_LIMIT) {
            snprintf(message, message_size, "Use no more than %u custom headers", HTTP_HEADER_LIMIT);
            return false;
        }
        char *separator = strchr(line, ':');
        if (!separator || separator == line) {
            snprintf(message, message_size, "Header %u must be Name: value", count);
            return false;
        }
        *separator = '\0';
        char *name_end = separator;
        while (name_end > line && (name_end[-1] == ' ' || name_end[-1] == '\t')) *--name_end = '\0';
        for (const unsigned char *name = (const unsigned char *)line; *name; name++) {
            if (!header_name_character(*name)) {
                snprintf(message, message_size, "Header %u has an invalid name", count);
                return false;
            }
        }
        if (forbidden_header(line)) {
            snprintf(message, message_size, "%s is managed by the HTTP client", line);
            return false;
        }
        char *value = separator + 1;
        while (*value == ' ' || *value == '\t') value++;
        size_t value_length = strlen(value);
        while (value_length && (value[value_length - 1] == ' ' || value[value_length - 1] == '\t'))
            value[--value_length] = '\0';
        for (const unsigned char *byte = (const unsigned char *)value; *byte; byte++) {
            if ((*byte < 0x20 && *byte != '\t') || *byte == 0x7f) {
                snprintf(message, message_size, "Header %u contains a control character", count);
                return false;
            }
        }
        if (!strcasecmp(line, "Content-Type")) *has_content_type = true;
        if (client && esp_http_client_set_header(client, line, value) != ESP_OK) {
            snprintf(message, message_size, "Could not set header %s", line);
            return false;
        }
    }
    return true;
}

static uint32_t hash_bytes(uint32_t hash, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    while (length--) {
        hash ^= *bytes++;
        hash *= 16777619U;
    }
    return hash;
}

static uint32_t request_hash(esp_http_client_method_t method, const char *url,
                             const char *headers, const char *body)
{
    uint32_t hash = hash_bytes(2166136261U, &method, sizeof(method));
    hash = hash_bytes(hash, url, strlen(url) + 1);
    hash = hash_bytes(hash, headers, strlen(headers) + 1);
    return hash_bytes(hash, body, strlen(body) + 1);
}

static bool confirmation_valid(bool armed, uint32_t armed_hash, uint32_t expected_hash,
                               uint32_t armed_at, uint32_t now)
{
    return armed && armed_hash == expected_hash && (uint32_t)(now - armed_at) <= HTTP_CONFIRM_MS;
}

static char preview_byte(unsigned char value)
{
    if (value == '\r') return '\n';
    if (value == '\n' || value == '\t' || (value >= 0x20 && value <= 0x7e)) return (char)value;
    return '.';
}

static void copy_header_value(char *output, size_t output_size, const char *input)
{
    size_t used = 0;
    while (*input && used + 1 < output_size) output[used++] = preview_byte((unsigned char)*input++);
    output[used] = '\0';
}

void http_tool_self_test(void)
{
    char url[HTTP_URL_MAX + 1];
    bool secure;
    assert(normalize_url(" https://example.com/a?secret=1 ", url, &secure) && secure);
    assert(!strcmp(url, "https://example.com/a?secret=1"));
    assert(normalize_url("http://192.0.2.1:8080/", url, &secure) && !secure);
    assert(!normalize_url("ftp://example.com", url, &secure));
    assert(!normalize_url("https://user:pass@example.com", url, &secure));
    assert(!normalize_url("https://", url, &secure));
    char safe[HTTP_URL_MAX + 1];
    safe_log_url("https://example.com/a?token=secret#part", safe);
    assert(!strcmp(safe, "https://example.com/a"));

    char headers[] = "Authorization: Bearer secret\nContent-Type: application/json";
    char message[96];
    bool content_type;
    assert(parse_headers(headers, NULL, &content_type, message, sizeof(message)) && content_type);
    char forbidden[] = "Content-Length: 4";
    assert(!parse_headers(forbidden, NULL, &content_type, message, sizeof(message)));
    char injected[] = "X-Test: okay\rbad";
    assert(!parse_headers(injected, NULL, &content_type, message, sizeof(message)));
    char too_many[] = "A: 1\nB: 2\nC: 3\nD: 4\nE: 5";
    assert(!parse_headers(too_many, NULL, &content_type, message, sizeof(message)));

    uint32_t hash = request_hash(HTTP_METHOD_POST, "http://example.com", "", "{}");
    assert(confirmation_valid(true, hash, hash, 100, 5100));
    assert(!confirmation_valid(true, hash, hash, 100, 5101));
    assert(confirmation_valid(true, hash, hash, UINT32_MAX - 1000, 1000));
    assert(!confirmation_valid(true, hash, hash + 1, 100, 200));
    assert(preview_byte(0xff) == '.' && preview_byte('\r') == '\n');
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    http_job_t *job = event->user_data;
    if (!job) return ESP_OK;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value) {
        if (!strcasecmp(event->header_key, "Content-Type"))
            copy_header_value(job->content_type, sizeof(job->content_type), event->header_value);
        else if (!strcasecmp(event->header_key, "Location"))
            copy_header_value(job->location, sizeof(job->location), event->header_value);
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data && event->data_len > 0) {
        const unsigned char *data = event->data;
        job->response_bytes += (size_t)event->data_len;
        for (int i = 0; i < event->data_len; i++) {
            if (job->preview_length == HTTP_PREVIEW_MAX) {
                job->truncated = true;
                break;
            }
            job->preview[job->preview_length++] = preview_byte(data[i]);
        }
        job->preview[job->preview_length] = '\0';
    }
    return ESP_OK;
}

static int csv_field(FILE *file, const char *text)
{
    if (fputc('"', file) == EOF) return -1;
    while (*text) {
        if (*text == '"' && fputc('"', file) == EOF) return -1;
        if (fputc(*text++, file) == EOF) return -1;
    }
    return fputc('"', file) == EOF ? -1 : 0;
}

static int append_log(const http_job_t *job)
{
    if (mkdir("/sdcard/HTTP", 0775) != 0 && errno != EEXIST) return errno ? errno : EIO;
    if (storage_repair_csv_tail(HTTP_LOG_PATH) != 0) return errno ? errno : EIO;
    FILE *file = fopen(HTTP_LOG_PATH, "a+");
    if (!file) return errno ? errno : EIO;
    int first_error = 0;
    if (fseek(file, 0, SEEK_END) != 0) first_error = errno ? errno : EIO;
    long length = first_error ? -1 : ftell(file);
    if (length < 0 && !first_error) first_error = errno ? errno : EIO;
    if (!first_error && length == 0 &&
        fputs("unix_time,method,url,status,response_bytes,duration_ms,outcome\n", file) < 0)
        first_error = errno ? errno : EIO;

    char safe_url[HTTP_URL_MAX + 1];
    safe_log_url(job->url, safe_url);
    const char *outcome = job->cancelled ? "cancelled" :
                          job->deadline_expired ? "request_deadline" :
                          job->invalid_headers ? "invalid_response_headers" :
                          job->incomplete ? "incomplete_response" :
                          job->error != ESP_OK ? esp_err_to_name(job->error) :
                          job->truncated ? "preview_truncated" : "complete";
    if (!first_error && (fprintf(file, "%lld,%s,", (long long)time(NULL),
                                 method_name(job->method)) < 0 ||
                         csv_field(file, safe_url) != 0 ||
                         fprintf(file, ",%d,%u,%lu,%s\n", job->status,
                                 (unsigned)job->response_bytes,
                                 (unsigned long)job->duration_ms, outcome) < 0))
        first_error = errno ? errno : EIO;
    if (!first_error && storage_sync_file(file) != 0) first_error = errno ? errno : EIO;
    if (fclose(file) != 0 && !first_error) first_error = errno ? errno : EIO;
    return first_error;
}

static bool request_cancelled(void *context)
{
    (void)context;
    portENTER_CRITICAL(&http_lock);
    bool cancelled = cancel_requested;
    portEXIT_CRITICAL(&http_lock);
    return cancelled;
}

static void perform_request(http_job_t *job)
{
    bool secure = prefix_equal(job->url, "https://");
    esp_transport_handle_t transport = http_transport_init(secure,
        job->started_us + (int64_t)HTTP_REQUEST_MS * 1000, request_cancelled, NULL);
    esp_http_client_config_t config = {
        .url = job->url,
        .method = job->method,
        .timeout_ms = 10000,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
        .event_handler = http_event,
        .user_data = job,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .user_agent = "Tab5OS/1.0",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .transport = transport,
    };
    esp_http_client_handle_t client = transport ? esp_http_client_init(&config) : NULL;
    if (!client) {
        job->error = ESP_ERR_NO_MEM;
    } else {
        esp_http_client_set_header(client, "Accept", "*/*");
        char header_error[96];
        bool has_content_type;
        if (!parse_headers(job->headers, client, &has_content_type,
                           header_error, sizeof(header_error))) {
            ESP_LOGE("http-tool", "%s", header_error);
            job->error = ESP_ERR_INVALID_ARG;
        } else {
            bool sends_body = job->method == HTTP_METHOD_POST || job->method == HTTP_METHOD_PUT;
            if (sends_body && !has_content_type)
                esp_http_client_set_header(client, "Content-Type", "application/json");
            if (sends_body)
                esp_http_client_set_post_field(client, job->body, (int)strlen(job->body));
            job->error = esp_http_client_perform(client);
            job->status = esp_http_client_get_status_code(client);
            job->socket_error = http_transport_get_errno(transport);
            http_transport_stop_t stopped = http_transport_stop_reason(transport);
            job->cancelled = stopped == HTTP_TRANSPORT_CANCELLED;
            job->deadline_expired = stopped == HTTP_TRANSPORT_DEADLINE;
            job->invalid_headers = stopped == HTTP_TRANSPORT_INVALID_HEADERS;
            if (job->cancelled) job->error = ESP_ERR_INVALID_STATE;
            else if (job->deadline_expired) job->error = ESP_ERR_TIMEOUT;
            else if (job->invalid_headers) job->error = ESP_ERR_INVALID_RESPONSE;
            else if (stopped == HTTP_TRANSPORT_INVALID_BODY) {
                job->incomplete = true;
                job->error = ESP_ERR_INVALID_RESPONSE;
            }
            else if (job->error == ESP_OK &&
                     (!http_transport_response_complete(transport) ||
                      (job->status != 304 && !esp_http_client_is_complete_data_received(client)))) {
                job->incomplete = true;
                job->error = ESP_ERR_INVALID_RESPONSE;
            }
        }
        esp_http_client_cleanup(client);
    }
    /* The SDK closes but does not destroy an injected custom transport. */
    if (transport) esp_transport_destroy(transport);
    job->duration_ms = (uint32_t)((esp_timer_get_time() - job->started_us) / 1000);
    if (job->log_enabled) job->log_error = append_log(job);
}

static void http_worker(void *argument)
{
    http_job_t *job = argument;
    perform_request(job);
    portENTER_CRITICAL(&http_lock);
    completed_job = job;
    request_busy = false;
    portEXIT_CRITICAL(&http_lock);
    vTaskDeleteWithCaps(NULL);
}

static void set_status(const char *text)
{
    if (status_label) lv_label_set_text(status_label, text);
}

static void show_request(const http_job_t *job)
{
    if (!request_label) return;
    if (!job) {
        lv_label_set_text(request_label, "");
        lv_obj_add_flag(request_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char endpoint[HTTP_URL_MAX + 1];
    safe_log_url(job->url, endpoint);
    lv_label_set_text_fmt(request_label, "Request: %s %s%s", method_name(job->method), endpoint,
                          strpbrk(job->url, "?#") ? "\nQuery/fragment omitted" : "");
    lv_obj_remove_flag(request_label, LV_OBJ_FLAG_HIDDEN);
}

static void update_controls(void)
{
    bool busy = http_tool_busy();
    if (method_label) lv_label_set_text_fmt(method_label, "METHOD\n%s", method_names[method_index]);
    if (log_label) lv_label_set_text(log_label, logging_enabled ? "SD LOG\nON" : "SD LOG\nOFF");
    if (send_label) lv_label_set_text(send_label, busy ? "WORKING" : "SEND");
    if (cancel_button) {
        if (busy || cleartext_armed) lv_obj_remove_state(cancel_button, LV_STATE_DISABLED);
        else lv_obj_add_state(cancel_button, LV_STATE_DISABLED);
    }
}

static void report_log_error(int error)
{
    if (!error) return;
    logging_enabled = false;
    screen_sd_available = false;
    if (storage_error_cb) storage_error_cb(error);
}

static http_job_t *take_completed_job(void)
{
    portENTER_CRITICAL(&http_lock);
    http_job_t *job = completed_job;
    completed_job = NULL;
    portEXIT_CRITICAL(&http_lock);
    return job;
}

static void append_log_status(char *message, size_t capacity, const http_job_t *job)
{
    size_t used = strlen(message);
    if (job->log_error)
        snprintf(message + used, capacity - used,
                 "\nSD log failed: %s; metadata was not confirmed saved. SD LOG is now OFF.",
                 strerror(job->log_error));
    else if (job->log_enabled)
        snprintf(message + used, capacity - used, "\nMetadata appended to /HTTP/HTTPLOG.CSV");
    else
        snprintf(message + used, capacity - used, "\nSD LOG was OFF for this request.");
}

static void show_result(http_job_t *job)
{
    show_request(job);
    if (response_area)
        lv_textarea_set_text(response_area, job->preview_length ? job->preview : "(empty response body)");
    if (status_label) {
        char message[640];
        if (job->cancelled || job->deadline_expired) {
            snprintf(message, sizeof(message),
                                  "%s after %lu ms; %u body bytes received. Partial preview retained.\n"
                                  "The connection is closed; a sent request may already have taken effect.",
                                  job->cancelled ? "Cancelled" : "15-second request deadline reached",
                                  (unsigned long)job->duration_ms, (unsigned)job->response_bytes);
        } else if (job->invalid_headers) {
            snprintf(message, sizeof(message),
                                  "Response headers/trailers invalid, unsupported or exceed the 8 KiB limit after %lu ms",
                                  (unsigned long)job->duration_ms);
        } else if (job->incomplete) {
            snprintf(message, sizeof(message),
                                  "Incomplete HTTP %d response after %lu ms: %u body bytes received",
                                  job->status, (unsigned long)job->duration_ms,
                                  (unsigned)job->response_bytes);
        } else if (job->error != ESP_OK) {
            snprintf(message, sizeof(message), "Request failed after %lu ms: %s%s%s",
                                  (unsigned long)job->duration_ms, esp_err_to_name(job->error),
                                  job->socket_error ? " / " : "",
                                  job->socket_error ? strerror(job->socket_error) : "");
        } else {
            snprintf(message, sizeof(message),
                                  "HTTP %d | %u bytes | %lu ms%s%s%s%s%s",
                                  job->status, (unsigned)job->response_bytes,
                                  (unsigned long)job->duration_ms,
                                  job->truncated ? " | preview capped" : "",
                                  job->content_type[0] ? "\nType: " : "",
                                  job->content_type,
                                  job->location[0] ? "\nRedirect target (not followed): " : "",
                                  job->location);
        }
        append_log_status(message, sizeof(message), job);
        lv_label_set_text(status_label, message);
    }
    report_log_error(job->log_error);
    heap_caps_free(job);
}

static void http_tick(lv_timer_t *timer)
{
    (void)timer;
    if (cleartext_armed &&
        (uint32_t)(lv_tick_get() - cleartext_armed_at) > HTTP_CONFIRM_MS) {
        cleartext_armed = false;
        set_status("Plain HTTP confirmation expired");
    }
    http_job_t *job = take_completed_job();
    if (job) show_result(job);
    update_controls();
}

static lv_obj_t *action_button(lv_obj_t *parent, const char *text, int width,
                               lv_event_cb_t callback, lv_obj_t **label_out)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_size(control, width, 68);
    lv_obj_add_event_cb(control, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(control);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    if (label_out) *label_out = label;
    return control;
}

static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, 640, 78);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return container;
}

static void keyboard_hide(lv_event_t *event)
{
    (void)event;
    if (keyboard) {
        lv_keyboard_set_textarea(keyboard, NULL);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (response_area) lv_obj_remove_flag(response_area, LV_OBJ_FLAG_HIDDEN);
}

static void textarea_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (keyboard && (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED)) {
        lv_keyboard_set_textarea(keyboard, lv_event_get_target_obj(event));
        lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        if (response_area) lv_obj_add_flag(response_area, LV_OBJ_FLAG_HIDDEN);
    }
    cleartext_armed = false;
}

static void method_clicked(lv_event_t *event)
{
    (void)event;
    method_index = (method_index + 1) % (sizeof(methods) / sizeof(methods[0]));
    cleartext_armed = false;
    update_controls();
}

static void log_clicked(lv_event_t *event)
{
    (void)event;
    if (!screen_sd_available) {
        set_status("Insert a writable SD card before enabling metadata logging");
        return;
    }
    logging_enabled = !logging_enabled;
    update_controls();
}

static void clear_clicked(lv_event_t *event)
{
    (void)event;
    cleartext_armed = false;
    bool busy = http_tool_busy();
    if (response_area) lv_textarea_set_text(response_area, "");
    if (busy) set_status("Preview cleared; request is still in progress.");
    else {
        http_job_t *job = take_completed_job();
        char message[256] = "Cleared; ready for another request";
        if (job) {
            if (job->log_error) append_log_status(message, sizeof(message), job);
            report_log_error(job->log_error);
            heap_caps_free(job);
        }
        show_request(NULL);
        set_status(message);
    }
    update_controls();
}

static void cancel_clicked(lv_event_t *event)
{
    (void)event;
    bool armed = cleartext_armed;
    cleartext_armed = false;
    portENTER_CRITICAL(&http_lock);
    bool busy = request_busy;
    if (busy) cancel_requested = true;
    portEXIT_CRITICAL(&http_lock);
    if (busy) set_status("Cancellation requested; waiting for connection cleanup...");
    else if (armed) set_status("Plain HTTP confirmation cancelled");
    update_controls();
}

static void send_clicked(lv_event_t *event)
{
    (void)event;
    if (!screen_connected) {
        set_status("Connect to Wi-Fi first");
        return;
    }
    if (http_tool_busy()) return;
    http_job_t *pending = take_completed_job();
    if (pending) {
        cleartext_armed = false;
        show_result(pending);
        update_controls();
        return;
    }
    const char *header_text = lv_textarea_get_text(headers_area);
    const char *body_text = lv_textarea_get_text(body_area);
    if (strlen(header_text) > HTTP_HEADERS_MAX) {
        set_status("Custom headers exceed the 511-byte limit");
        return;
    }
    if (strlen(body_text) > HTTP_BODY_MAX) {
        set_status("Body exceeds the 1023-byte limit");
        return;
    }
    http_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) {
        set_status("Request allocation failed");
        return;
    }
    job->method = methods[method_index];
    job->log_enabled = logging_enabled && screen_sd_available;
    snprintf(job->headers, sizeof(job->headers), "%s", header_text);
    snprintf(job->body, sizeof(job->body), "%s", body_text);
    bool secure;
    if (!normalize_url(lv_textarea_get_text(url_area), job->url, &secure)) {
        heap_caps_free(job);
        set_status("Use http:// or https:// with a host; spaces and embedded credentials are rejected");
        return;
    }
    char headers_copy[HTTP_HEADERS_MAX + 1];
    snprintf(headers_copy, sizeof(headers_copy), "%s", job->headers);
    char header_error[96];
    bool has_content_type;
    if (!parse_headers(headers_copy, NULL, &has_content_type,
                       header_error, sizeof(header_error))) {
        heap_caps_free(job);
        set_status(header_error);
        return;
    }
    uint32_t hash = request_hash(job->method, job->url, job->headers, job->body);
    uint32_t now = lv_tick_get();
    keyboard_hide(NULL);
    if (!secure && !confirmation_valid(cleartext_armed, cleartext_hash, hash,
                                       cleartext_armed_at, now)) {
        cleartext_armed = true;
        cleartext_hash = hash;
        cleartext_armed_at = now;
        heap_caps_free(job);
        set_status("Plain HTTP exposes headers and body. Tap SEND again within 5 seconds for this exact request.");
        update_controls();
        return;
    }
    cleartext_armed = false;
    job->started_us = esp_timer_get_time();
    portENTER_CRITICAL(&http_lock);
    cancel_requested = false;
    request_busy = true;
    portEXIT_CRITICAL(&http_lock);
    show_request(job);
    if (response_area) lv_textarea_set_text(response_area, "");
    set_status("Request in progress (15-second deadline); automatic redirects are disabled...");
    if (xTaskCreateWithCaps(http_worker, "http-console", 12288, job, 4,
                            NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        portENTER_CRITICAL(&http_lock);
        request_busy = false;
        portEXIT_CRITICAL(&http_lock);
        heap_caps_free(job);
        set_status("Could not start HTTP worker");
        update_controls();
        return;
    }
    update_controls();
}

bool http_tool_busy(void)
{
    portENTER_CRITICAL(&http_lock);
    bool busy = request_busy;
    portEXIT_CRITICAL(&http_lock);
    return busy;
}

void http_tool_stop(void)
{
    cancel_clicked(NULL);
    if (ui_timer) {
        lv_timer_delete(ui_timer);
        ui_timer = NULL;
    }
    cleartext_armed = false;
    url_area = NULL;
    headers_area = NULL;
    body_area = NULL;
    if (keyboard) lv_keyboard_set_textarea(keyboard, NULL);
    keyboard = NULL;
    method_label = NULL;
    log_label = NULL;
    send_label = NULL;
    cancel_button = NULL;
    request_label = NULL;
    status_label = NULL;
    response_area = NULL;
    storage_error_cb = NULL;
}

void http_tool_show(lv_obj_t *parent, bool connected, bool sd_available,
                    http_tool_storage_error_cb_t error_cb)
{
    screen_connected = connected;
    screen_sd_available = sd_available;
    logging_enabled = false;
    storage_error_cb = error_cb;

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "HTTP Console");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *help = lv_label_create(parent);
    lv_obj_set_width(help, 640);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(help, "HTTPS certificates are verified; redirects are shown but not followed.\n"
                            "4 KiB body preview; 8 KiB response headers; four custom headers maximum.\n"
                            "15-second request budget; Cancel/Home stops cooperatively, including DNS waits.\n"
                            "SD logs are opt-in metadata only: no headers, bodies, queries, or response content.");

    lv_obj_t *controls = row(parent);
    action_button(controls, "", 110, method_clicked, &method_label);
    action_button(controls, "", 110, log_clicked, &log_label);
    action_button(controls, "CLEAR", 110, clear_clicked, NULL);
    cancel_button = action_button(controls, "CANCEL", 110, cancel_clicked, NULL);
    lv_obj_t *send = action_button(controls, "", 110, send_clicked, &send_label);
    if (!connected) lv_obj_add_state(send, LV_STATE_DISABLED);

    url_area = lv_textarea_create(parent);
    lv_obj_set_size(url_area, 640, 72);
    lv_textarea_set_one_line(url_area, true);
    lv_textarea_set_max_length(url_area, HTTP_URL_MAX);
    lv_textarea_set_text(url_area, "https://example.com/");
    lv_textarea_set_placeholder_text(url_area, "https://device.local/api");

    headers_area = lv_textarea_create(parent);
    lv_obj_set_size(headers_area, 640, 132);
    lv_textarea_set_max_length(headers_area, HTTP_HEADERS_MAX);
    lv_textarea_set_placeholder_text(headers_area, "Optional headers, one Name: value per line (maximum four)");

    body_area = lv_textarea_create(parent);
    lv_obj_set_size(body_area, 640, 170);
    lv_textarea_set_max_length(body_area, HTTP_BODY_MAX);
    lv_textarea_set_placeholder_text(body_area, "POST/PUT body (default Content-Type: application/json)");

    request_label = lv_label_create(parent);
    lv_obj_set_width(request_label, 640);
    lv_label_set_long_mode(request_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(request_label, LV_TEXT_ALIGN_CENTER, 0);
    show_request(NULL);

    status_label = lv_label_create(parent);
    lv_obj_set_width(status_label, 640);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label, http_tool_busy() ? "Finishing the previous request..." :
                                    connected ? "Ready" : "Connect to Wi-Fi first");

    response_area = lv_textarea_create(parent);
    lv_obj_set_size(response_area, 640, 300);
    lv_textarea_set_cursor_click_pos(response_area, false);
    lv_textarea_set_placeholder_text(response_area, "Response body preview");

    keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(keyboard, 640, 300);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_hide, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_hide, LV_EVENT_CANCEL, NULL);
    lv_obj_t *inputs[] = {url_area, headers_area, body_area};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        lv_obj_add_event_cb(inputs[i], textarea_event, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_event_cb(inputs[i], textarea_event, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(inputs[i], textarea_event, LV_EVENT_VALUE_CHANGED, NULL);
    }
    update_controls();
    ui_timer = lv_timer_create(http_tick, 200, NULL);
    http_tick(ui_timer);
}
