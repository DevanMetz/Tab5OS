#include "mqtt_tool.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "storage_io.h"

#define MQTT_URI_MAX 255
#define MQTT_CREDENTIAL_MAX 64
#define MQTT_TOPIC_MAX 127
#define MQTT_PAYLOAD_MAX 512
#define MQTT_MESSAGE_LIMIT 8
#define MQTT_HISTORY_MAX 4096
#define MQTT_CONFIRM_MS 5000U
#define MQTT_LOG_PATH "/sdcard/MQTT/MQTTLOG.CSV"
#define MQTT_PROFILE_VERSION 1
#define MQTT_PROFILE_NAMESPACE "mqtt"
#define MQTT_PROFILE_KEY "profile"

typedef struct {
    uint8_t version;
    char uri[MQTT_URI_MAX + 1];
    char username[MQTT_CREDENTIAL_MAX + 1];
    char password[MQTT_CREDENTIAL_MAX + 1];
    char topic[MQTT_TOPIC_MAX + 1];
} mqtt_profile_t;

typedef struct {
    time_t timestamp;
    bool received;
    bool retained;
    bool truncated;
    uint8_t qos;
    size_t payload_bytes;
    char topic[MQTT_TOPIC_MAX + 1];
    char payload[MQTT_PAYLOAD_MAX + 1];
} mqtt_message_t;

static portMUX_TYPE mqtt_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_mqtt_client_handle_t client;
static bool client_started;
static bool client_running;
static bool client_stopping;
static bool broker_connected;
static bool screen_visible;
static TaskHandle_t cleanup_worker_handle;
static esp_mqtt_client_handle_t cleanup_client;
static bool cleanup_client_started;
static bool cleanup_log_enabled;
static bool cleanup_sd_available;
static mqtt_tool_storage_error_cb_t cleanup_storage_error_cb;
static bool status_dirty;
static char async_status[192];
static mqtt_message_t *message_ring;
static mqtt_message_t *incoming_message;
static unsigned message_head;
static unsigned message_count;
static unsigned messages_dropped;
static char *history_text;
static char default_topic[MQTT_TOPIC_MAX + 1];

static lv_timer_t *ui_timer;
static lv_obj_t *uri_area;
static lv_obj_t *username_area;
static lv_obj_t *password_area;
static lv_obj_t *topic_area;
static lv_obj_t *payload_area;
static lv_obj_t *keyboard;
static lv_obj_t *connect_button;
static lv_obj_t *connect_label;
static lv_obj_t *qos_label;
static lv_obj_t *retain_label;
static lv_obj_t *log_label;
static lv_obj_t *subscribe_button;
static lv_obj_t *publish_button;
static lv_obj_t *save_profile_button;
static lv_obj_t *delete_profile_button;
static lv_obj_t *status_label;
static lv_obj_t *history_area;
static unsigned selected_qos = 1;
static bool publish_retained;
static bool logging_enabled;
static bool screen_wifi_connected;
static bool screen_sd_available;
static bool cleartext_armed;
static uint32_t cleartext_hash;
static uint32_t cleartext_armed_at;
static bool profile_saved;
static bool profile_delete_armed;
static uint32_t profile_delete_armed_at;
static mqtt_tool_storage_error_cb_t storage_error_cb;

static bool prefix_equal(const char *text, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*text++) != tolower((unsigned char)*prefix++)) return false;
    }
    return true;
}

static bool normalize_uri(const char *input, char output[MQTT_URI_MAX + 1], bool *secure)
{
    while (*input == ' ' || *input == '\t') input++;
    size_t length = strlen(input);
    while (length && (input[length - 1] == ' ' || input[length - 1] == '\t')) length--;
    if (!length || length > MQTT_URI_MAX) return false;
    memcpy(output, input, length);
    output[length] = '\0';

    size_t scheme_length;
    if (prefix_equal(output, "mqtts://")) {
        *secure = true;
        scheme_length = 8;
    } else if (prefix_equal(output, "wss://")) {
        *secure = true;
        scheme_length = 6;
    } else if (prefix_equal(output, "mqtt://")) {
        *secure = false;
        scheme_length = 7;
    } else if (prefix_equal(output, "ws://")) {
        *secure = false;
        scheme_length = 5;
    } else {
        return false;
    }
    const char *authority = output + scheme_length;
    const char *authority_end = strchr(authority, '/');
    if (!authority_end) authority_end = output + length;
    if (authority == authority_end || memchr(authority, '@', (size_t)(authority_end - authority))) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)output[i];
        if (byte <= 0x20 || byte == 0x7f) return false;
    }
    return true;
}

static bool topic_filter_valid(const char *topic)
{
    size_t length = strlen(topic);
    if (!length || length > MQTT_TOPIC_MAX) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)topic[i];
        if (byte < 0x20 || byte > 0x7e) return false;
        if (byte == '#' && (i + 1 != length || (i && topic[i - 1] != '/'))) return false;
        if (byte == '+' && ((i && topic[i - 1] != '/') ||
                            (i + 1 < length && topic[i + 1] != '/'))) return false;
    }
    return true;
}

static bool publish_topic_valid(const char *topic)
{
    return topic_filter_valid(topic) && !strchr(topic, '#') && !strchr(topic, '+');
}

static bool stored_string_valid(const char *text, size_t capacity)
{
    return memchr(text, '\0', capacity) != NULL;
}

static bool profile_valid(const mqtt_profile_t *profile)
{
    if (!profile || profile->version != MQTT_PROFILE_VERSION ||
        !stored_string_valid(profile->uri, sizeof(profile->uri)) ||
        !stored_string_valid(profile->username, sizeof(profile->username)) ||
        !stored_string_valid(profile->password, sizeof(profile->password)) ||
        !stored_string_valid(profile->topic, sizeof(profile->topic))) return false;
    char uri[MQTT_URI_MAX + 1];
    bool secure;
    return normalize_uri(profile->uri, uri, &secure) && secure &&
           !strcmp(uri, profile->uri) && topic_filter_valid(profile->topic);
}

static esp_err_t load_profile(mqtt_profile_t *profile, bool *present)
{
    *present = false;
    nvs_handle_t handle;
    esp_err_t error = nvs_open(MQTT_PROFILE_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) return error;
    size_t size = 0;
    error = nvs_get_blob(handle, MQTT_PROFILE_KEY, NULL, &size);
    if (error == ESP_OK) {
        *present = true;
        if (size != sizeof(*profile)) error = ESP_ERR_NVS_INVALID_LENGTH;
        else error = nvs_get_blob(handle, MQTT_PROFILE_KEY, profile, &size);
    }
    nvs_close(handle);
    if (error == ESP_OK && !profile_valid(profile)) return ESP_ERR_NVS_INVALID_LENGTH;
    return error;
}

static esp_err_t save_profile(const mqtt_profile_t *profile)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(MQTT_PROFILE_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(handle, MQTT_PROFILE_KEY, profile, sizeof(*profile));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

static esp_err_t delete_profile(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(MQTT_PROFILE_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_erase_key(handle, MQTT_PROFILE_KEY);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

static uint32_t hash_bytes(uint32_t hash, const char *text)
{
    do {
        hash ^= (unsigned char)*text;
        hash *= 16777619U;
    } while (*text++);
    return hash;
}

static uint32_t connection_hash(const char *uri, const char *username, const char *password)
{
    uint32_t hash = hash_bytes(2166136261U, uri);
    hash = hash_bytes(hash, username);
    return hash_bytes(hash, password);
}

static bool confirmation_valid(bool armed, uint32_t armed_hash, uint32_t expected_hash,
                               uint32_t armed_at, uint32_t now)
{
    return armed && armed_hash == expected_hash && (uint32_t)(now - armed_at) <= MQTT_CONFIRM_MS;
}

void mqtt_tool_self_test(void)
{
    char uri[MQTT_URI_MAX + 1];
    bool secure;
    assert(normalize_uri(" mqtts://broker.example:8883 ", uri, &secure) && secure);
    assert(!strcmp(uri, "mqtts://broker.example:8883"));
    assert(normalize_uri("ws://192.0.2.1:8080/mqtt", uri, &secure) && !secure);
    assert(!normalize_uri("https://broker.example", uri, &secure));
    assert(!normalize_uri("mqtts://user:pass@broker.example", uri, &secure));
    assert(!normalize_uri("mqtts://", uri, &secure));
    assert(topic_filter_valid("sensors/+/temperature"));
    assert(topic_filter_valid("site/#"));
    assert(!topic_filter_valid("bad/#/tail"));
    assert(!topic_filter_valid("bad+level"));
    assert(publish_topic_valid("site/device/state"));
    assert(!publish_topic_valid("site/+/state"));
    uint32_t hash = connection_hash("mqtt://broker", "user", "secret");
    assert(confirmation_valid(true, hash, hash, 100, 5100));
    assert(!confirmation_valid(true, hash, hash, 100, 5101));
    assert(confirmation_valid(true, hash, hash, UINT32_MAX - 1000, 1000));
    assert(!confirmation_valid(true, hash, hash + 1, 100, 200));
    mqtt_profile_t profile = {
        .version = MQTT_PROFILE_VERSION,
        .uri = "mqtts://broker.example:8883",
        .username = "user",
        .password = "secret",
        .topic = "site/+/state",
    };
    assert(profile_valid(&profile));
    snprintf(profile.uri, sizeof(profile.uri), "mqtt://broker.example:1883");
    assert(!profile_valid(&profile));
    snprintf(profile.uri, sizeof(profile.uri), "mqtts://broker.example:8883");
    memset(profile.topic, 'x', sizeof(profile.topic));
    assert(!profile_valid(&profile));
}

static void set_async_status(const char *format, ...)
{
    char text[sizeof(async_status)];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    portENTER_CRITICAL(&mqtt_lock);
    snprintf(async_status, sizeof(async_status), "%s", text);
    status_dirty = true;
    portEXIT_CRITICAL(&mqtt_lock);
}

static char display_byte(unsigned char byte)
{
    if (byte == '\r') return '\n';
    if (byte == '\n' || byte == '\t' || (byte >= 0x20 && byte <= 0x7e)) return (char)byte;
    return '.';
}

static void copy_event_text(char *output, size_t output_size, const char *input, size_t length)
{
    size_t used = length < output_size - 1 ? length : output_size - 1;
    for (size_t i = 0; i < used; i++) output[i] = display_byte((unsigned char)input[i]);
    output[used] = '\0';
}

static void push_message_locked(const mqtt_message_t *message)
{
    if (!message_ring) return;
    if (message_count == MQTT_MESSAGE_LIMIT) {
        message_head = (message_head + 1) % MQTT_MESSAGE_LIMIT;
        message_count--;
        messages_dropped++;
    }
    unsigned index = (message_head + message_count) % MQTT_MESSAGE_LIMIT;
    message_ring[index] = *message;
    message_count++;
}

static void handle_data(esp_mqtt_event_handle_t event)
{
    if (!incoming_message || !message_ring) return;
    if (event->current_data_offset == 0) {
        memset(incoming_message, 0, sizeof(*incoming_message));
        incoming_message->timestamp = time(NULL);
        incoming_message->received = true;
        incoming_message->retained = event->retain;
        incoming_message->qos = (uint8_t)event->qos;
        incoming_message->payload_bytes = (size_t)event->total_data_len;
        incoming_message->truncated = event->total_data_len > MQTT_PAYLOAD_MAX;
        if (event->topic && event->topic_len > 0)
            copy_event_text(incoming_message->topic, sizeof(incoming_message->topic),
                            event->topic, (size_t)event->topic_len);
    }
    if (event->data && event->data_len > 0 && event->current_data_offset < MQTT_PAYLOAD_MAX) {
        size_t offset = (size_t)event->current_data_offset;
        size_t available = MQTT_PAYLOAD_MAX - offset;
        size_t copy = (size_t)event->data_len < available ? (size_t)event->data_len : available;
        for (size_t i = 0; i < copy; i++)
            incoming_message->payload[offset + i] = display_byte((unsigned char)event->data[i]);
        incoming_message->payload[offset + copy] = '\0';
    }
    if (event->current_data_offset + event->data_len >= event->total_data_len) {
        portENTER_CRITICAL(&mqtt_lock);
        push_message_locked(incoming_message);
        portEXIT_CRITICAL(&mqtt_lock);
        set_async_status("Received %d bytes on %s", event->total_data_len,
                         incoming_message->topic[0] ? incoming_message->topic : "(topic unavailable)");
    }
}

static void mqtt_event(void *argument, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)argument;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        portENTER_CRITICAL(&mqtt_lock);
        broker_connected = true;
        portEXIT_CRITICAL(&mqtt_lock);
        set_async_status("Connected with TLS verification; choose SUBSCRIBE or PUBLISH");
        break;
    case MQTT_EVENT_DISCONNECTED:
        portENTER_CRITICAL(&mqtt_lock);
        broker_connected = false;
        portEXIT_CRITICAL(&mqtt_lock);
        set_async_status("Broker disconnected; press DISCONNECT before changing settings");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        set_async_status("Subscription acknowledged (message %d)", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        set_async_status("Publish acknowledged (message %d)", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        handle_data(event);
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle && event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
            set_async_status("Transport error: %s%s%s",
                             esp_err_to_name(event->error_handle->esp_tls_last_esp_err),
                             event->error_handle->esp_transport_sock_errno ? " / " : "",
                             event->error_handle->esp_transport_sock_errno ?
                                 strerror(event->error_handle->esp_transport_sock_errno) : "");
        else if (event->error_handle)
            set_async_status("Broker refused connection (%d)", event->error_handle->connect_return_code);
        else
            set_async_status("MQTT connection error");
        break;
    default:
        break;
    }
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

static int append_log(const mqtt_message_t *message)
{
    if (mkdir("/sdcard/MQTT", 0775) != 0 && errno != EEXIST) return errno ? errno : EIO;
    if (storage_repair_csv_tail(MQTT_LOG_PATH) != 0) return errno ? errno : EIO;
    FILE *file = fopen(MQTT_LOG_PATH, "a+");
    if (!file) return errno ? errno : EIO;
    int first_error = 0;
    if (fseek(file, 0, SEEK_END) != 0) first_error = errno ? errno : EIO;
    long length = first_error ? -1 : ftell(file);
    if (length < 0 && !first_error) first_error = errno ? errno : EIO;
    if (!first_error && length == 0 &&
        fputs("unix_time,direction,topic,qos,retained,payload_bytes,outcome\n", file) < 0)
        first_error = errno ? errno : EIO;
    if (!first_error && (fprintf(file, "%lld,%s,", (long long)message->timestamp,
                                 message->received ? "RX" : "TX") < 0 ||
                         csv_field(file, message->topic) != 0 ||
                         fprintf(file, ",%u,%u,%u,%s\n", message->qos,
                                 message->retained ? 1U : 0U,
                                 (unsigned)message->payload_bytes,
                                 message->truncated ? "preview_truncated" :
                                 message->received ? "received" : "queued") < 0))
        first_error = errno ? errno : EIO;
    if (!first_error && storage_sync_file(file) != 0) first_error = errno ? errno : EIO;
    if (fclose(file) != 0 && !first_error) first_error = errno ? errno : EIO;
    return first_error;
}

static void append_history(const mqtt_message_t *message)
{
    if (!history_text) return;
    size_t used = strlen(history_text);
    if (used > MQTT_HISTORY_MAX / 2) {
        char *keep = strchr(history_text + MQTT_HISTORY_MAX / 2, '\n');
        keep = keep ? keep + 1 : history_text + MQTT_HISTORY_MAX / 2;
        memmove(history_text, keep, strlen(keep) + 1);
        used = strlen(history_text);
    }
    struct tm local;
    localtime_r(&message->timestamp, &local);
    int written = snprintf(history_text + used, MQTT_HISTORY_MAX - used,
                           "[%02d:%02d:%02d] %s QoS %u%s | %u bytes%s\n%s\n%s\n\n",
                           local.tm_hour, local.tm_min, local.tm_sec,
                           message->received ? "RX" : "TX", message->qos,
                           message->retained ? " | retained" : "",
                           (unsigned)message->payload_bytes,
                           message->truncated ? " | preview capped" : "",
                           message->topic, message->payload);
    if (written < 0) history_text[used] = '\0';
    if (history_area) {
        lv_textarea_set_text(history_area, history_text);
        lv_textarea_set_cursor_pos(history_area, LV_TEXTAREA_CURSOR_LAST);
    }
}

static void drain_messages(void)
{
    mqtt_message_t message;
    for (;;) {
        portENTER_CRITICAL(&mqtt_lock);
        if (!message_count) {
            portEXIT_CRITICAL(&mqtt_lock);
            break;
        }
        message = message_ring[message_head];
        message_head = (message_head + 1) % MQTT_MESSAGE_LIMIT;
        message_count--;
        portEXIT_CRITICAL(&mqtt_lock);
        append_history(&message);
        if (logging_enabled && screen_sd_available) {
            int error = append_log(&message);
            if (error) {
                logging_enabled = false;
                if (storage_error_cb) storage_error_cb(error);
                if (status_label) lv_label_set_text_fmt(status_label, "SD metadata log failed: %s", strerror(error));
            }
        }
    }
}

static bool connected_snapshot(void)
{
    portENTER_CRITICAL(&mqtt_lock);
    bool connected = broker_connected;
    portEXIT_CRITICAL(&mqtt_lock);
    return connected;
}

static bool running_snapshot(void)
{
    portENTER_CRITICAL(&mqtt_lock);
    bool running = client_running;
    portEXIT_CRITICAL(&mqtt_lock);
    return running;
}

static bool stopping_snapshot(void)
{
    portENTER_CRITICAL(&mqtt_lock);
    bool stopping = client_stopping;
    portEXIT_CRITICAL(&mqtt_lock);
    return stopping;
}

static void update_controls(void)
{
    bool running = running_snapshot();
    bool stopping = stopping_snapshot();
    bool connected = connected_snapshot();
    if (connect_label) lv_label_set_text(connect_label, stopping ? "STOPPING" :
                                                       running ? "DISCONNECT" : "CONNECT");
    if (connect_button) {
        if (stopping || !screen_wifi_connected || !cleanup_worker_handle)
            lv_obj_add_state(connect_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(connect_button, LV_STATE_DISABLED);
    }
    if (qos_label) lv_label_set_text_fmt(qos_label, "QoS %u", selected_qos);
    if (retain_label) lv_label_set_text(retain_label, publish_retained ? "RETAIN\nON" : "RETAIN\nOFF");
    if (log_label) lv_label_set_text(log_label, logging_enabled ? "SD LOG\nON" : "SD LOG\nOFF");
    if (subscribe_button) {
        if (connected) lv_obj_remove_state(subscribe_button, LV_STATE_DISABLED);
        else lv_obj_add_state(subscribe_button, LV_STATE_DISABLED);
    }
    if (publish_button) {
        if (connected) lv_obj_remove_state(publish_button, LV_STATE_DISABLED);
        else lv_obj_add_state(publish_button, LV_STATE_DISABLED);
    }
    if (save_profile_button) {
        if (running || stopping) lv_obj_add_state(save_profile_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(save_profile_button, LV_STATE_DISABLED);
    }
    if (delete_profile_button) {
        if (running || stopping || !profile_saved) lv_obj_add_state(delete_profile_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(delete_profile_button, LV_STATE_DISABLED);
    }
    lv_obj_t *settings[] = {uri_area, username_area, password_area};
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
        if (!settings[i]) continue;
        if (running) lv_obj_add_state(settings[i], LV_STATE_DISABLED);
        else lv_obj_remove_state(settings[i], LV_STATE_DISABLED);
    }
}

static bool ensure_buffers(void)
{
    if (!message_ring)
        message_ring = heap_caps_calloc(MQTT_MESSAGE_LIMIT, sizeof(*message_ring),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!incoming_message)
        incoming_message = heap_caps_calloc(1, sizeof(*incoming_message),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!history_text) {
        history_text = heap_caps_calloc(1, MQTT_HISTORY_MAX + 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (history_text && history_area)
            snprintf(history_text, MQTT_HISTORY_MAX + 1, "%s", lv_textarea_get_text(history_area));
    }
    return message_ring && incoming_message && history_text;
}

static void release_buffers(void)
{
    heap_caps_free(message_ring);
    heap_caps_free(incoming_message);
    heap_caps_free(history_text);
    message_ring = NULL;
    incoming_message = NULL;
    history_text = NULL;
    message_head = message_count = messages_dropped = 0;
}

static void destroy_unstarted_client(void)
{
    esp_mqtt_client_handle_t handle = client;
    client = NULL;
    client_started = false;
    portENTER_CRITICAL(&mqtt_lock);
    client_running = false;
    client_stopping = false;
    broker_connected = false;
    portEXIT_CRITICAL(&mqtt_lock);
    if (handle) esp_mqtt_client_destroy(handle);
}

static void mqtt_cleanup_worker(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        portENTER_CRITICAL(&mqtt_lock);
        esp_mqtt_client_handle_t handle = cleanup_client;
        bool started = cleanup_client_started;
        bool write_log = cleanup_log_enabled && cleanup_sd_available;
        mqtt_tool_storage_error_cb_t error_cb = cleanup_storage_error_cb;
        portEXIT_CRITICAL(&mqtt_lock);

        if (handle) {
            if (started) {
                vTaskDelay(pdMS_TO_TICKS(50));
                esp_mqtt_client_stop(handle);
            }
            esp_mqtt_client_destroy(handle);
        }

        portENTER_CRITICAL(&mqtt_lock);
        mqtt_message_t *old_ring = message_ring;
        mqtt_message_t *old_incoming = incoming_message;
        char *old_history = history_text;
        unsigned old_head = message_head;
        unsigned old_count = message_count;
        message_ring = NULL;
        incoming_message = NULL;
        history_text = NULL;
        message_head = message_count = messages_dropped = 0;
        cleanup_client = NULL;
        cleanup_client_started = false;
        cleanup_log_enabled = false;
        cleanup_sd_available = false;
        cleanup_storage_error_cb = NULL;
        client_running = false;
        client_stopping = false;
        broker_connected = false;
        bool visible = screen_visible;
        portEXIT_CRITICAL(&mqtt_lock);

        int first_log_error = 0;
        if (write_log && old_ring) {
            for (unsigned i = 0; i < old_count; i++) {
                int error = append_log(&old_ring[(old_head + i) % MQTT_MESSAGE_LIMIT]);
                if (error && !first_log_error) first_log_error = error;
            }
        }
        heap_caps_free(old_ring);
        heap_caps_free(old_incoming);
        heap_caps_free(old_history);
        if (first_log_error && error_cb) error_cb(first_log_error);
        if (visible)
            set_async_status(first_log_error ? "Disconnected; SD metadata log failed: %s" :
                                               "Disconnected; form password cleared",
                             first_log_error ? strerror(first_log_error) : "");
    }
}

static void begin_client_stop(void)
{
    if (!client || !cleanup_worker_handle || stopping_snapshot()) return;
    esp_mqtt_client_disconnect(client);
    portENTER_CRITICAL(&mqtt_lock);
    cleanup_client = client;
    cleanup_client_started = client_started;
    cleanup_log_enabled = logging_enabled;
    cleanup_sd_available = screen_sd_available;
    cleanup_storage_error_cb = storage_error_cb;
    client = NULL;
    client_started = false;
    client_stopping = true;
    broker_connected = false;
    portEXIT_CRITICAL(&mqtt_lock);
    logging_enabled = false;
    publish_retained = false;
    xTaskNotifyGive(cleanup_worker_handle);
}

static void mqtt_tick(lv_timer_t *timer)
{
    (void)timer;
    if (cleartext_armed && (uint32_t)(lv_tick_get() - cleartext_armed_at) > MQTT_CONFIRM_MS) {
        cleartext_armed = false;
        if (status_label) lv_label_set_text(status_label, "Plain MQTT confirmation expired");
    }
    if (profile_delete_armed &&
        (uint32_t)(lv_tick_get() - profile_delete_armed_at) > MQTT_CONFIRM_MS) {
        profile_delete_armed = false;
        if (status_label) lv_label_set_text(status_label, "MQTT profile deletion confirmation expired");
    }
    if (stopping_snapshot()) {
        update_controls();
        return;
    }
    if (!ensure_buffers() && status_label) lv_label_set_text(status_label, "MQTT buffer allocation failed");
    char status[sizeof(async_status)];
    bool dirty;
    unsigned dropped;
    portENTER_CRITICAL(&mqtt_lock);
    dirty = status_dirty;
    if (dirty) {
        snprintf(status, sizeof(status), "%s", async_status);
        status_dirty = false;
    }
    dropped = messages_dropped;
    messages_dropped = 0;
    portEXIT_CRITICAL(&mqtt_lock);
    if (dirty && status_label) lv_label_set_text(status_label, status);
    drain_messages();
    if (dropped && status_label)
        lv_label_set_text_fmt(status_label, "Dropped %u UI/log events; narrow the subscription", dropped);
    update_controls();
}

static lv_obj_t *action_button(lv_obj_t *parent, const char *text, int width,
                               lv_event_cb_t callback, lv_obj_t **label_out)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 68);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    if (label_out) *label_out = label;
    return button;
}

static lv_obj_t *row(lv_obj_t *parent, int height)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, 640, height);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return container;
}

static void textarea_focused(lv_event_t *event)
{
    if (keyboard) lv_keyboard_set_textarea(keyboard, lv_event_get_target_obj(event));
    cleartext_armed = false;
    profile_delete_armed = false;
}

static void save_profile_clicked(lv_event_t *event)
{
    (void)event;
    if (running_snapshot() || stopping_snapshot()) return;
    mqtt_profile_t profile = {.version = MQTT_PROFILE_VERSION};
    bool secure;
    if (!normalize_uri(lv_textarea_get_text(uri_area), profile.uri, &secure) || !secure) {
        lv_label_set_text(status_label, "Only verified mqtts:// or wss:// profiles can be saved; cleartext settings remain memory-only");
        return;
    }
    const char *topic = lv_textarea_get_text(topic_area);
    if (!topic_filter_valid(topic)) {
        lv_label_set_text(status_label, "Enter a valid topic or subscription filter before saving");
        return;
    }
    snprintf(profile.username, sizeof(profile.username), "%s", lv_textarea_get_text(username_area));
    snprintf(profile.password, sizeof(profile.password), "%s", lv_textarea_get_text(password_area));
    snprintf(profile.topic, sizeof(profile.topic), "%s", topic);
    esp_err_t error = save_profile(&profile);
    mbedtls_platform_zeroize(&profile, sizeof(profile));
    if (error != ESP_OK) {
        lv_label_set_text_fmt(status_label, "Could not save MQTT profile: %s", esp_err_to_name(error));
        return;
    }
    profile_saved = true;
    profile_delete_armed = false;
    lv_label_set_text(status_label, "TLS profile saved in device settings; developer builds do not encrypt settings flash");
    update_controls();
}

static void delete_profile_clicked(lv_event_t *event)
{
    (void)event;
    if (!profile_saved || running_snapshot() || stopping_snapshot()) return;
    uint32_t now = lv_tick_get();
    if (!profile_delete_armed || (uint32_t)(now - profile_delete_armed_at) > MQTT_CONFIRM_MS) {
        profile_delete_armed = true;
        profile_delete_armed_at = now;
        lv_label_set_text(status_label, "Tap DELETE PROFILE again within 5 seconds to erase the saved broker credentials");
        return;
    }
    profile_delete_armed = false;
    esp_err_t error = delete_profile();
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        lv_label_set_text_fmt(status_label, "Could not delete MQTT profile: %s", esp_err_to_name(error));
        return;
    }
    profile_saved = false;
    lv_textarea_set_text(uri_area, "mqtts://test.mosquitto.org:8886");
    lv_textarea_set_text(username_area, "");
    lv_textarea_set_text(password_area, "");
    lv_textarea_set_text(topic_area, default_topic);
    lv_label_set_text(status_label, "Saved MQTT profile deleted; form reset to the public test broker");
    update_controls();
}

static void connect_clicked(lv_event_t *event)
{
    (void)event;
    if (running_snapshot()) {
        begin_client_stop();
        if (password_area) lv_textarea_set_text(password_area, "");
        if (status_label) lv_label_set_text(status_label, "Disconnecting in the background...");
        update_controls();
        return;
    }
    if (!screen_wifi_connected) {
        if (status_label) lv_label_set_text(status_label, "Connect to Wi-Fi first");
        return;
    }
    if (!message_ring || !incoming_message || !history_text) {
        lv_label_set_text(status_label, "MQTT buffer allocation failed");
        return;
    }
    char uri[MQTT_URI_MAX + 1];
    bool secure;
    if (!normalize_uri(lv_textarea_get_text(uri_area), uri, &secure)) {
        lv_label_set_text(status_label, "Use mqtts://, wss://, mqtt://, or ws:// with a host; embedded credentials are rejected");
        return;
    }
    const char *username = lv_textarea_get_text(username_area);
    const char *password = lv_textarea_get_text(password_area);
    uint32_t hash = connection_hash(uri, username, password);
    uint32_t now = lv_tick_get();
    if (!secure && !confirmation_valid(cleartext_armed, cleartext_hash, hash,
                                       cleartext_armed_at, now)) {
        cleartext_armed = true;
        cleartext_hash = hash;
        cleartext_armed_at = now;
        lv_label_set_text(status_label, "Plain MQTT exposes credentials and payloads. Tap CONNECT again within 5 seconds for these exact settings.");
        return;
    }
    cleartext_armed = false;
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char client_id[32];
    snprintf(client_id, sizeof(client_id), "tab5os-%02x%02x%02x", mac[3], mac[4], mac[5]);
    esp_mqtt_client_config_t config = {
        .broker = {
            .address.uri = uri,
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
        },
        .credentials = {
            .username = username[0] ? username : NULL,
            .client_id = client_id,
            .authentication.password = password[0] ? password : NULL,
        },
        .session = {
            .keepalive = 30,
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        },
        .network = {
            .timeout_ms = 10000,
            .disable_auto_reconnect = true,
        },
        .task = {.priority = 4, .stack_size = 6144},
        .buffer = {.size = 1024, .out_size = 1024},
        .outbox.limit = 2048,
    };
    client = esp_mqtt_client_init(&config);
    if (!client || esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event, NULL) != ESP_OK) {
        if (client) esp_mqtt_client_destroy(client);
        client = NULL;
        lv_label_set_text(status_label, "Could not initialize MQTT client");
        return;
    }
    portENTER_CRITICAL(&mqtt_lock);
    client_running = true;
    broker_connected = false;
    portEXIT_CRITICAL(&mqtt_lock);
    esp_err_t error = esp_mqtt_client_start(client);
    if (error != ESP_OK) {
        destroy_unstarted_client();
        lv_label_set_text_fmt(status_label, "Could not start MQTT: %s", esp_err_to_name(error));
        return;
    }
    client_started = true;
    lv_label_set_text(status_label, secure ? "Connecting with certificate verification..." : "Connecting over confirmed cleartext...");
    update_controls();
}

static void qos_clicked(lv_event_t *event)
{
    (void)event;
    selected_qos = (selected_qos + 1) % 3;
    update_controls();
}

static void retain_clicked(lv_event_t *event)
{
    (void)event;
    publish_retained = !publish_retained;
    update_controls();
}

static void log_clicked(lv_event_t *event)
{
    (void)event;
    if (!screen_sd_available) {
        lv_label_set_text(status_label, "Insert a writable SD card before enabling metadata logging");
        return;
    }
    logging_enabled = !logging_enabled;
    update_controls();
}

static void subscribe_clicked(lv_event_t *event)
{
    (void)event;
    const char *topic = lv_textarea_get_text(topic_area);
    if (!topic_filter_valid(topic)) {
        lv_label_set_text(status_label, "Enter a valid ASCII topic filter; + and # must occupy complete levels");
        return;
    }
    int message_id = esp_mqtt_client_subscribe(client, topic, (int)selected_qos);
    if (message_id < 0) lv_label_set_text(status_label, "Could not queue subscription");
    else lv_label_set_text_fmt(status_label, "Subscribing to %s (message %d)...", topic, message_id);
}

static void publish_clicked(lv_event_t *event)
{
    (void)event;
    const char *topic = lv_textarea_get_text(topic_area);
    const char *payload = lv_textarea_get_text(payload_area);
    if (!publish_topic_valid(topic)) {
        lv_label_set_text(status_label, "Publish topics cannot contain + or # and must be printable ASCII");
        return;
    }
    int message_id = esp_mqtt_client_enqueue(client, topic, payload, (int)strlen(payload),
                                             (int)selected_qos, publish_retained, false);
    if (message_id < 0) {
        lv_label_set_text(status_label, "Could not queue publish");
        return;
    }
    mqtt_message_t message = {
        .timestamp = time(NULL),
        .received = false,
        .retained = publish_retained,
        .qos = (uint8_t)selected_qos,
        .payload_bytes = strlen(payload),
    };
    snprintf(message.topic, sizeof(message.topic), "%s", topic);
    snprintf(message.payload, sizeof(message.payload), "%s", payload);
    portENTER_CRITICAL(&mqtt_lock);
    push_message_locked(&message);
    portEXIT_CRITICAL(&mqtt_lock);
    lv_label_set_text_fmt(status_label, "Publish queued (message %d)", message_id);
}

static void clear_clicked(lv_event_t *event)
{
    (void)event;
    if (history_text) history_text[0] = '\0';
    if (history_area) lv_textarea_set_text(history_area, "");
    lv_label_set_text(status_label, "On-screen history cleared; SD metadata log is unchanged");
}

bool mqtt_tool_busy(void)
{
    return running_snapshot();
}

void mqtt_tool_stop(void)
{
    if (ui_timer) {
        lv_timer_delete(ui_timer);
        ui_timer = NULL;
    }
    cleartext_armed = false;
    profile_delete_armed = false;
    if (password_area) lv_textarea_set_text(password_area, "");
    portENTER_CRITICAL(&mqtt_lock);
    screen_visible = false;
    bool active = client_running;
    bool stopping = client_stopping;
    portEXIT_CRITICAL(&mqtt_lock);
    if (active && !stopping) begin_client_stop();
    else if (!active) {
        drain_messages();
        release_buffers();
        logging_enabled = false;
        publish_retained = false;
    }
    uri_area = NULL;
    username_area = NULL;
    password_area = NULL;
    topic_area = NULL;
    payload_area = NULL;
    keyboard = NULL;
    connect_button = NULL;
    connect_label = NULL;
    qos_label = NULL;
    retain_label = NULL;
    log_label = NULL;
    subscribe_button = NULL;
    publish_button = NULL;
    save_profile_button = NULL;
    delete_profile_button = NULL;
    status_label = NULL;
    history_area = NULL;
    storage_error_cb = NULL;
}

void mqtt_tool_show(lv_obj_t *parent, bool wifi_connected, bool sd_available,
                    mqtt_tool_storage_error_cb_t error_cb)
{
    screen_wifi_connected = wifi_connected;
    screen_sd_available = sd_available;
    storage_error_cb = error_cb;
    logging_enabled = false;
    publish_retained = false;
    profile_delete_armed = false;
    mqtt_profile_t profile = {0};
    esp_err_t profile_error = load_profile(&profile, &profile_saved);
    bool profile_loaded = profile_error == ESP_OK;
    portENTER_CRITICAL(&mqtt_lock);
    screen_visible = true;
    bool stopping = client_stopping;
    if (!stopping) {
        status_dirty = false;
        async_status[0] = '\0';
    }
    portEXIT_CRITICAL(&mqtt_lock);
    if (!cleanup_worker_handle)
        xTaskCreateWithCaps(mqtt_cleanup_worker, "mqtt-cleanup", 4096, NULL, 4,
                            &cleanup_worker_handle, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stopping) ensure_buffers();
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(default_topic, sizeof(default_topic), "tab5os/%02x%02x%02x/loopback",
             mac[3], mac[4], mac[5]);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "MQTT Console");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_t *help = lv_label_create(parent);
    lv_obj_set_width(help, 640);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(help, "TLS/WSS certificates are verified; plain transports require two taps.\n"
                            "TLS profiles save explicitly to unencrypted developer settings; cleartext stays memory-only.\n"
                            "SD logs are opt-in metadata and never contain credentials or payloads.\n"
                            "The default public test broker/topic is observable: never send secrets.");

    lv_obj_t *controls = row(parent, 78);
    connect_button = action_button(controls, "", 140, connect_clicked, &connect_label);
    action_button(controls, "", 120, qos_clicked, &qos_label);
    action_button(controls, "", 140, retain_clicked, &retain_label);
    action_button(controls, "", 140, log_clicked, &log_label);
    if (!wifi_connected) lv_obj_add_state(connect_button, LV_STATE_DISABLED);

    uri_area = lv_textarea_create(parent);
    lv_obj_set_size(uri_area, 640, 72);
    lv_textarea_set_one_line(uri_area, true);
    lv_textarea_set_max_length(uri_area, MQTT_URI_MAX);
    lv_textarea_set_text(uri_area, "mqtts://test.mosquitto.org:8886");
    lv_textarea_set_placeholder_text(uri_area, "mqtts://broker.example:8883");

    lv_obj_t *credentials = row(parent, 78);
    username_area = lv_textarea_create(credentials);
    lv_obj_set_size(username_area, 292, 68);
    lv_textarea_set_one_line(username_area, true);
    lv_textarea_set_max_length(username_area, MQTT_CREDENTIAL_MAX);
    lv_textarea_set_placeholder_text(username_area, "Username (optional)");
    password_area = lv_textarea_create(credentials);
    lv_obj_set_size(password_area, 292, 68);
    lv_textarea_set_one_line(password_area, true);
    lv_textarea_set_password_mode(password_area, true);
    lv_textarea_set_max_length(password_area, MQTT_CREDENTIAL_MAX);
    lv_textarea_set_placeholder_text(password_area, "Password (never exported)");

    lv_obj_t *profile_actions = row(parent, 78);
    save_profile_button = action_button(profile_actions, "SAVE PROFILE", 270,
                                        save_profile_clicked, NULL);
    delete_profile_button = action_button(profile_actions, "DELETE PROFILE", 270,
                                          delete_profile_clicked, NULL);

    topic_area = lv_textarea_create(parent);
    lv_obj_set_size(topic_area, 640, 72);
    lv_textarea_set_one_line(topic_area, true);
    lv_textarea_set_max_length(topic_area, MQTT_TOPIC_MAX);
    lv_textarea_set_text(topic_area, profile_loaded ? profile.topic : default_topic);
    lv_textarea_set_placeholder_text(topic_area, "Topic or subscription filter");

    if (profile_loaded) {
        lv_textarea_set_text(uri_area, profile.uri);
        lv_textarea_set_text(username_area, profile.username);
        lv_textarea_set_text(password_area, profile.password);
    }
    mbedtls_platform_zeroize(&profile, sizeof(profile));

    lv_obj_t *actions = row(parent, 78);
    subscribe_button = action_button(actions, "SUBSCRIBE", 180, subscribe_clicked, NULL);
    publish_button = action_button(actions, "PUBLISH", 180, publish_clicked, NULL);
    action_button(actions, "CLEAR", 180, clear_clicked, NULL);

    payload_area = lv_textarea_create(parent);
    lv_obj_set_size(payload_area, 640, 140);
    lv_textarea_set_max_length(payload_area, MQTT_PAYLOAD_MAX);
    lv_textarea_set_text(payload_area, "hello from Tab5 OS");
    lv_textarea_set_placeholder_text(payload_area, "Publish payload (maximum 512 bytes)");

    status_label = lv_label_create(parent);
    lv_obj_set_width(status_label, 640);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    if (stopping) lv_label_set_text(status_label, "Previous MQTT session is stopping in the background...");
    else if (!cleanup_worker_handle) lv_label_set_text(status_label, "MQTT cleanup worker allocation failed");
    else if (!message_ring || !incoming_message || !history_text)
        lv_label_set_text(status_label, "MQTT buffer allocation failed");
    else if (profile_error != ESP_OK && profile_error != ESP_ERR_NVS_NOT_FOUND)
        lv_label_set_text_fmt(status_label, "Saved MQTT profile unavailable: %s", esp_err_to_name(profile_error));
    else if (profile_loaded)
        lv_label_set_text(status_label, "Saved TLS profile loaded; connect explicitly");
    else
        lv_label_set_text(status_label, wifi_connected ? "Ready; connect explicitly" : "Connect to Wi-Fi first");

    history_area = lv_textarea_create(parent);
    lv_obj_set_size(history_area, 640, 300);
    lv_textarea_set_cursor_click_pos(history_area, false);
    lv_textarea_set_placeholder_text(history_area, "Received and published messages");

    keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(keyboard, 640, 360);
    lv_keyboard_set_textarea(keyboard, uri_area);
    lv_obj_t *areas[] = {uri_area, username_area, password_area, topic_area, payload_area};
    for (size_t i = 0; i < sizeof(areas) / sizeof(areas[0]); i++)
        lv_obj_add_event_cb(areas[i], textarea_focused, LV_EVENT_FOCUSED, NULL);
    update_controls();
    ui_timer = lv_timer_create(mqtt_tick, 200, NULL);
}
