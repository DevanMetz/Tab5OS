#include "mqtt_tool.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
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
#include "http_parser.h"
#include "lwip/sockets.h"
#include "mbedtls/platform_util.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "payload_clipboard.h"
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
    bool topic_preview;
    bool byte_preview;
    uint8_t qos;
    size_t payload_bytes;
    char topic[MQTT_TOPIC_MAX + 1];
    char payload[MQTT_PAYLOAD_MAX + 1];
} mqtt_message_t;

/* Storage owns metadata only, independently of the UI's payload previews. */
typedef struct {
    time_t timestamp;
    bool received;
    bool retained;
    bool truncated;
    bool topic_preview;
    uint8_t qos;
    size_t payload_bytes;
    char topic[MQTT_TOPIC_MAX + 1];
} mqtt_log_entry_t;

typedef struct {
    mqtt_message_t messages[MQTT_MESSAGE_LIMIT];
    mqtt_log_entry_t logs[MQTT_MESSAGE_LIMIT];
    uint8_t receive_bytes[PAYLOAD_CLIPBOARD_MAX_BYTES];
} mqtt_rings_t;

/* The latest complete receive survives Home without retaining SDK buffers.
 * A topic preview is labeled; the copied payload is always exact. */
typedef struct {
    bool present, retained, topic_preview;
    uint8_t qos;
    size_t length;
    uint32_t revision;
    char topic[MQTT_TOPIC_MAX + 1];
    uint8_t bytes[PAYLOAD_CLIPBOARD_MAX_BYTES];
} mqtt_receive_copy_t;
static mqtt_receive_copy_t latest_receive;
static uint32_t copied_receive_revision;

/* UI-only prepared bytes are independent of the clipboard and SDK buffers.
 * Retain the reviewed draft in RAM through Home; only PUBLISH sends it. */
static payload_clipboard_t prepared_payload;
static bool prepared_payload_present;
static bool publish_bytes;

typedef struct {
    esp_mqtt_client_handle_t client;
    bool publish;
    mqtt_message_t message;
} mqtt_action_t;

static portMUX_TYPE mqtt_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_mqtt_client_handle_t client;
static bool client_started;
static bool client_running;
static bool client_stopping;
static bool broker_connected;
static bool client_secure;
static bool connection_failed;
static bool screen_visible;
static bool action_busy;
static mqtt_action_t *pending_action;
static TaskHandle_t cleanup_worker_handle;
static esp_mqtt_client_handle_t cleanup_client;
static bool cleanup_client_started;
static bool cleanup_log_enabled;
static bool cleanup_sd_available;
static mqtt_tool_storage_error_cb_t cleanup_storage_error_cb;
static bool status_dirty;
static char async_status[192];
static uint32_t event_status_revision;
static mqtt_rings_t *rings;
static mqtt_message_t *message_ring;
static mqtt_log_entry_t *log_ring;
static unsigned log_head;
static unsigned log_count;
static unsigned log_dropped;
static int log_error;
static bool log_error_reported;
static mqtt_message_t *incoming_message;
static bool incoming_active;
static size_t incoming_offset;
static int incoming_id;
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
static lv_obj_t *payload_mode_button;
static lv_obj_t *payload_mode_label;
static lv_obj_t *paste_payload_button;
static lv_obj_t *payload_bytes_label;
static lv_obj_t *keyboard;
static lv_obj_t *connect_button;
static lv_obj_t *connect_label;
static lv_obj_t *qos_label;
static lv_obj_t *retain_label;
static lv_obj_t *log_label;
static lv_obj_t *subscribe_button;
static lv_obj_t *publish_button;
static lv_obj_t *copy_receive_button;
static lv_obj_t *receive_label;
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
static uint32_t cleartext_armed_at;
/* Offer key deletion when it exists or a failed read/write leaves it uncertain. */
static bool profile_delete_available;
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
    /* Count before trimming: a clipped field with leading spaces must not turn
     * an oversized endpoint into an apparently valid shorter request. */
    if (strlen(input) > MQTT_URI_MAX) return false;
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
    const char *authority_end = authority + strcspn(authority, "/?#");
    if (authority == authority_end || authority_end[-1] == ':' ||
        memchr(authority, '@', (size_t)(authority_end - authority))) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)output[i];
        if (byte <= 0x20 || byte == 0x7f) return false;
    }
    struct http_parser_url parsed;
    http_parser_url_init(&parsed);
    if (http_parser_parse_url(output, length, 0, &parsed) ||
        !(parsed.field_set & (1U << UF_HOST)) || !parsed.field_data[UF_HOST].len ||
        (parsed.field_set & (1U << UF_USERINFO)) || strchr(output, '#') ||
        ((parsed.field_set & (1U << UF_PORT)) && !parsed.port)) return false;
    if (*authority == '[') {
        char host[INET6_ADDRSTRLEN];
        size_t host_length = parsed.field_data[UF_HOST].len;
        if (host_length >= sizeof(host)) return false;
        memcpy(host, output + parsed.field_data[UF_HOST].off, host_length);
        host[host_length] = '\0';
        struct in6_addr address;
        if (inet_pton(AF_INET6, host, &address) != 1) return false;
    }
    bool websocket = scheme_length == 5 || scheme_length == 6;
    if (!websocket && (strchr(output, '?') ||
        (parsed.field_data[UF_PATH].len &&
         (parsed.field_data[UF_PATH].len != 1 || output[parsed.field_data[UF_PATH].off] != '/')))) return false;
    return true;
}

static bool credentials_valid(const char *username, const char *password)
{
    return strlen(username) <= MQTT_CREDENTIAL_MAX && strlen(password) <= MQTT_CREDENTIAL_MAX;
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
    if (error != ESP_OK) {
        *present = error != ESP_ERR_NVS_NOT_FOUND;
        return error;
    }
    size_t size = 0;
    error = nvs_get_blob(handle, MQTT_PROFILE_KEY, NULL, &size);
    if (error == ESP_OK) {
        *present = true;
        if (size != sizeof(*profile)) error = ESP_ERR_NVS_INVALID_LENGTH;
        else error = nvs_get_blob(handle, MQTT_PROFILE_KEY, profile, &size);
        if (error == ESP_OK && size != sizeof(*profile)) error = ESP_ERR_NVS_INVALID_LENGTH;
    }
    *present = error != ESP_ERR_NVS_NOT_FOUND;
    nvs_close(handle);
    if (error == ESP_OK && !profile_valid(profile)) return ESP_ERR_NVS_INVALID_LENGTH;
    return error;
}

static esp_err_t save_profile(const mqtt_profile_t *profile, bool *write_attempted)
{
    *write_attempted = false;
    nvs_handle_t handle;
    esp_err_t error = nvs_open(MQTT_PROFILE_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    *write_attempted = true;
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

static bool confirmation_valid(bool armed, uint32_t armed_at, uint32_t now)
{
    return armed && (uint32_t)(now - armed_at) <= MQTT_CONFIRM_MS;
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
    assert(confirmation_valid(true, 100, 5100));
    assert(!confirmation_valid(true, 100, 5101));
    assert(confirmation_valid(true, UINT32_MAX - 1000, 1000));
    assert(!confirmation_valid(false, 100, 200));
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
    if (!client_stopping && !connection_failed) {
        snprintf(async_status, sizeof(async_status), "%s", text);
        status_dirty = true;
        event_status_revision++;
    }
    portEXIT_CRITICAL(&mqtt_lock);
}

static char display_byte(unsigned char byte)
{
    if (byte == '\r') return '\n';
    if (byte == '\n' || byte == '\t' || (byte >= 0x20 && byte <= 0x7e)) return (char)byte;
    return '.';
}

static bool copy_received_topic(char *output, size_t output_size, const char *input, size_t length)
{
    size_t used = length < output_size - 1 ? length : output_size - 1;
    bool preview = used != length;
    for (size_t i = 0; i < used; i++) {
        unsigned char byte = (unsigned char)input[i];
        bool printable = byte >= 0x20 && byte <= 0x7e;
        output[i] = printable ? (char)byte : '.';
        if (!printable) preview = true;
    }
    output[used] = '\0';
    return preview;
}

static bool push_message_locked(const mqtt_message_t *message)
{
    if (!message_ring) return false;
    if (message_count == MQTT_MESSAGE_LIMIT) {
        message_head = (message_head + 1) % MQTT_MESSAGE_LIMIT;
        message_count--;
        messages_dropped++;
    }
    unsigned index = (message_head + message_count) % MQTT_MESSAGE_LIMIT;
    message_ring[index] = *message;
    message_count++;

    bool write_log = client_stopping ? cleanup_log_enabled && cleanup_sd_available :
                                      logging_enabled && screen_sd_available;
    if (!write_log || log_error || !log_ring) return false;
    if (log_count == MQTT_MESSAGE_LIMIT) {
        log_head = (log_head + 1) % MQTT_MESSAGE_LIMIT;
        log_count--;
        if (log_dropped != UINT_MAX) log_dropped++;
    }
    mqtt_log_entry_t *entry = &log_ring[(log_head + log_count) % MQTT_MESSAGE_LIMIT];
    *entry = (mqtt_log_entry_t) {
        .timestamp = message->timestamp, .received = message->received,
        .retained = message->retained, .truncated = message->truncated,
        .topic_preview = message->topic_preview,
        .qos = message->qos, .payload_bytes = message->payload_bytes,
    };
    memcpy(entry->topic, message->topic, sizeof(entry->topic));
    log_count++;
    return true;
}

static void handle_data(esp_mqtt_event_handle_t event)
{
    if (!incoming_message || !message_ring) return;
    if (!event || event->current_data_offset < 0 || event->total_data_len < 0 ||
        event->data_len < 0 || event->topic_len < 0 || event->msg_id < 0 ||
        event->qos < 0 || event->qos > 2 ||
        event->current_data_offset > event->total_data_len ||
        event->data_len > event->total_data_len - event->current_data_offset ||
        (event->data_len && !event->data)) goto invalid;

    /* A header can fill the SDK buffer: both its empty first fragment and the
     * following data fragment then have offset zero. Only the first has a topic. */
    bool first = event->topic || event->topic_len;
    if (first) {
        if (event->current_data_offset != 0 || !event->topic || event->topic_len == 0)
            goto invalid;
        memset(incoming_message, 0, sizeof(*incoming_message));
        incoming_message->timestamp = time(NULL);
        incoming_message->received = true;
        incoming_message->retained = event->retain;
        incoming_message->qos = (uint8_t)event->qos;
        incoming_message->payload_bytes = (size_t)event->total_data_len;
        incoming_message->truncated = event->total_data_len > MQTT_PAYLOAD_MAX;
        incoming_message->topic_preview = copy_received_topic(incoming_message->topic,
            sizeof(incoming_message->topic), event->topic, (size_t)event->topic_len);
        incoming_active = true;
        incoming_offset = 0;
        incoming_id = event->msg_id;
    } else if (!incoming_active || !event->data_len ||
               incoming_offset != (size_t)event->current_data_offset ||
               incoming_message->payload_bytes != (size_t)event->total_data_len ||
               incoming_id != event->msg_id || incoming_message->qos != event->qos ||
               incoming_message->retained != event->retain) {
        goto invalid;
    }
    if (event->data && event->data_len > 0 && event->current_data_offset < MQTT_PAYLOAD_MAX) {
        size_t offset = (size_t)event->current_data_offset;
        size_t available = MQTT_PAYLOAD_MAX - offset;
        size_t copy = (size_t)event->data_len < available ? (size_t)event->data_len : available;
        for (size_t i = 0; i < copy; i++)
            incoming_message->payload[offset + i] = display_byte((unsigned char)event->data[i]);
        incoming_message->payload[offset + copy] = '\0';
        if (offset < sizeof(rings->receive_bytes)) {
            size_t raw_available = sizeof(rings->receive_bytes) - offset;
            size_t raw_copy = (size_t)event->data_len < raw_available ? (size_t)event->data_len : raw_available;
            memcpy(rings->receive_bytes + offset, event->data, raw_copy);
        }
    }
    incoming_offset = (size_t)event->current_data_offset + (size_t)event->data_len;
    if (incoming_offset == incoming_message->payload_bytes) {
        incoming_active = false;
        portENTER_CRITICAL(&mqtt_lock);
        uint32_t revision = latest_receive.revision + 1;
        latest_receive = (mqtt_receive_copy_t) {
            .present = true, .retained = incoming_message->retained,
            .topic_preview = incoming_message->topic_preview, .qos = incoming_message->qos,
            .length = incoming_message->payload_bytes, .revision = revision,
        };
        memcpy(latest_receive.topic, incoming_message->topic, sizeof(latest_receive.topic));
        if (latest_receive.length <= sizeof(latest_receive.bytes))
            memcpy(latest_receive.bytes, rings->receive_bytes, latest_receive.length);
        bool queued_log = push_message_locked(incoming_message);
        portEXIT_CRITICAL(&mqtt_lock);
        if (queued_log) xTaskNotifyGive(cleanup_worker_handle);
        set_async_status(incoming_message->topic_preview ? "Received %d bytes on topic preview %s" :
                                                          "Received %d bytes on %s",
                         event->total_data_len, incoming_message->topic);
    }
    return;
invalid:
    incoming_active = false;
    incoming_offset = 0;
    set_async_status("Ignored invalid or incomplete MQTT receive; awaiting a new message");
}

static void transport_error_text(char *text, size_t size, const esp_mqtt_error_codes_t *error)
{
    if (!error->esp_tls_last_esp_err && !error->esp_tls_stack_err &&
        !error->esp_tls_cert_verify_flags && !error->esp_transport_sock_errno) {
        snprintf(text, size, "Transport error: SDK supplied no error details");
        return;
    }
    snprintf(text, size, "Transport error:");
    if (error->esp_tls_last_esp_err)
        snprintf(text + strlen(text), size - strlen(text), " %.48s (0x%X)",
                 esp_err_to_name(error->esp_tls_last_esp_err), (unsigned)error->esp_tls_last_esp_err);
    if (error->esp_tls_stack_err) {
        unsigned code = (unsigned)error->esp_tls_stack_err;
        snprintf(text + strlen(text), size - strlen(text), " TLS stack %s0x%X",
                 error->esp_tls_stack_err < 0 ? "-" : "", error->esp_tls_stack_err < 0 ? 0U - code : code);
    }
    if (error->esp_tls_cert_verify_flags)
        snprintf(text + strlen(text), size - strlen(text), " cert flags 0x%X",
                 (unsigned)error->esp_tls_cert_verify_flags);
    /* Keep every numeric code ahead of the bounded socket description. */
    if (error->esp_transport_sock_errno)
        snprintf(text + strlen(text), size - strlen(text), " socket errno %d: %.32s",
                 error->esp_transport_sock_errno, strerror(error->esp_transport_sock_errno));
}

static void mqtt_event(void *argument, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)argument;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        incoming_active = false;
        portENTER_CRITICAL(&mqtt_lock);
        bool secure = client_secure;
        if (!client_stopping) {
            broker_connected = true;
            connection_failed = false;
        }
        portEXIT_CRITICAL(&mqtt_lock);
        set_async_status(secure ? "Connected with TLS verification; choose SUBSCRIBE or PUBLISH" :
                                 "Connected over confirmed cleartext; choose SUBSCRIBE or PUBLISH");
        break;
    case MQTT_EVENT_DISCONNECTED:
        incoming_active = false;
        portENTER_CRITICAL(&mqtt_lock);
        broker_connected = false;
        portEXIT_CRITICAL(&mqtt_lock);
        set_async_status("Broker disconnected; press DISCONNECT before changing settings");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        set_async_status(event->error_handle && event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED ?
                         "Broker rejected subscription (message %d)" : "Subscription acknowledged (message %d)",
                         event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        set_async_status("Publish acknowledged (message %d)", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        handle_data(event);
        break;
    case MQTT_EVENT_ERROR: {
        incoming_active = false;
        const esp_mqtt_error_codes_t *error = event ? event->error_handle : NULL;
        char text[sizeof(async_status)];
        if (error && error->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
            transport_error_text(text, sizeof(text), error);
        else if (error && error->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
            snprintf(text, sizeof(text), "Broker refused connection (%d)", error->connect_return_code);
        else
            snprintf(text, sizeof(text), "MQTT connection error");
        portENTER_CRITICAL(&mqtt_lock);
        if (!client_stopping) {
            broker_connected = false;
            connection_failed = true;
            snprintf(async_status, sizeof(async_status), "%s", text);
            status_dirty = true;
            event_status_revision++;
        }
        portEXIT_CRITICAL(&mqtt_lock);
        break;
    }
    default:
        break;
    }
}

static int csv_field(FILE *file, const char *text)
{
    errno = 0;
    if (fputc('"', file) == EOF || ferror(file)) return -1;
    while (*text) {
        if (*text == '"') {
            errno = 0;
            if (fputc('"', file) == EOF || ferror(file)) return -1;
        }
        errno = 0;
        if (fputc(*text++, file) == EOF || ferror(file)) return -1;
    }
    errno = 0;
    return fputc('"', file) == EOF || ferror(file) ? -1 : 0;
}

static int append_log(const mqtt_log_entry_t *message)
{
    errno = 0;
    if (mkdir("/sdcard/MQTT", 0775) != 0 && errno != EEXIST) return errno ? errno : EIO;
    errno = 0;
    if (storage_repair_csv_tail(MQTT_LOG_PATH) != 0) return errno ? errno : EIO;
    errno = 0;
    FILE *file = fopen(MQTT_LOG_PATH, "a+");
    if (!file) return errno ? errno : EIO;
    int first_error = 0;
    errno = 0;
    if (fseek(file, 0, SEEK_END) != 0) first_error = errno ? errno : EIO;
    long length = -1;
    if (!first_error) {
        errno = 0;
        length = ftell(file);
        if (length < 0) first_error = errno ? errno : EIO;
    }
    if (!first_error && length == 0) {
        errno = 0;
        if (fputs("unix_time,direction,topic,qos,retained,payload_bytes,outcome\n", file) < 0 || ferror(file))
            first_error = errno ? errno : EIO;
    }
    if (!first_error) {
        errno = 0;
        if (fprintf(file, "%lld,%s,", (long long)message->timestamp,
                    message->received ? "RX" : "TX") < 0 || ferror(file))
            first_error = errno ? errno : EIO;
    }
    if (!first_error && csv_field(file, message->topic) != 0)
        first_error = errno ? errno : EIO;
    if (!first_error) {
        errno = 0;
        if (fprintf(file, ",%u,%u,%u,%s\n", message->qos, message->retained ? 1U : 0U,
                    (unsigned)message->payload_bytes,
                    message->truncated || message->topic_preview ? "preview_truncated" :
                    message->received ? "received" : "queued") < 0 || ferror(file))
            first_error = errno ? errno : EIO;
    }
    if (!first_error && storage_sync_file(file) != 0) first_error = errno ? errno : EIO;
    errno = 0;
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
                           "[%02d:%02d:%02d] %s QoS %u%s | %u bytes%s%s%s\n%s\n%s\n\n",
                           local.tm_hour, local.tm_min, local.tm_sec,
                           message->received ? "RX" : "TX", message->qos,
                           message->retained ? " | retained" : "",
                           (unsigned)message->payload_bytes,
                           message->truncated ? " | preview capped" : "",
                           message->topic_preview ? " | topic preview" : "",
                           message->received || message->byte_preview ? " | ASCII preview" : "",
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

static bool action_busy_snapshot(void)
{
    portENTER_CRITICAL(&mqtt_lock);
    bool busy = action_busy;
    portEXIT_CRITICAL(&mqtt_lock);
    return busy;
}

static void update_controls(void)
{
    bool running = running_snapshot();
    bool stopping = stopping_snapshot();
    bool connected = running && !stopping && !action_busy_snapshot() && connected_snapshot();
    if (connect_label) lv_label_set_text(connect_label, stopping ? "STOPPING" :
                                                       running ? "DISCONNECT" : "CONNECT");
    if (connect_button) {
        if (stopping || !screen_wifi_connected || !cleanup_worker_handle)
            lv_obj_add_state(connect_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(connect_button, LV_STATE_DISABLED);
    }
    if (qos_label) lv_label_set_text_fmt(qos_label, "QoS %u", selected_qos);
    if (retain_label) lv_label_set_text(retain_label, publish_retained ? "RETAIN\nON" : "RETAIN\nOFF");
    if (log_label) {
        portENTER_CRITICAL(&mqtt_lock);
        bool enabled = logging_enabled;
        bool failed = log_error != 0;
        portEXIT_CRITICAL(&mqtt_lock);
        lv_label_set_text(log_label, enabled ? "SD LOG\nON" : "SD LOG\nOFF");
        lv_obj_t *button = lv_obj_get_parent(log_label);
        if (stopping || (running && failed)) lv_obj_add_state(button, LV_STATE_DISABLED);
        else lv_obj_remove_state(button, LV_STATE_DISABLED);
    }
    if (subscribe_button) {
        if (connected) lv_obj_remove_state(subscribe_button, LV_STATE_DISABLED);
        else lv_obj_add_state(subscribe_button, LV_STATE_DISABLED);
    }
    if (publish_button) {
        if (connected) lv_obj_remove_state(publish_button, LV_STATE_DISABLED);
        else lv_obj_add_state(publish_button, LV_STATE_DISABLED);
    }
    if (payload_mode_button) {
        if (stopping || !prepared_payload_present) lv_obj_add_state(payload_mode_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(payload_mode_button, LV_STATE_DISABLED);
    }
    if (paste_payload_button) {
        if (stopping || !payload_clipboard_peek()) lv_obj_add_state(paste_payload_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(paste_payload_button, LV_STATE_DISABLED);
    }
    if (copy_receive_button && receive_label) {
        portENTER_CRITICAL(&mqtt_lock);
        mqtt_receive_copy_t receive = latest_receive;
        portEXIT_CRITICAL(&mqtt_lock);
        bool copyable = receive.present && receive.length <= sizeof(receive.bytes) && !stopping;
        if (copyable) lv_obj_remove_state(copy_receive_button, LV_STATE_DISABLED);
        else lv_obj_add_state(copy_receive_button, LV_STATE_DISABLED);
        if (!receive.present)
            lv_label_set_text(receive_label, "No complete RX saved. COPY RX shares 0-128 raw bytes with Byte Lab.");
        else
            lv_label_set_text_fmt(receive_label, "Latest complete RX: %u bytes | QoS %u%s%s\n%s\n%s",
                                  (unsigned)receive.length, receive.qos, receive.retained ? " | retained" : "",
                                  receive.topic_preview ? " | topic preview" : "", receive.topic,
                                  receive.length > sizeof(receive.bytes) ? "Over 128 bytes; copy unavailable." :
                                  copied_receive_revision == receive.revision ? "Copied to the byte clipboard; paste in Byte Lab." :
                                  "COPY RX copies exact bytes to the RAM byte clipboard.");
    }
    if (save_profile_button) {
        if (running || stopping) lv_obj_add_state(save_profile_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(save_profile_button, LV_STATE_DISABLED);
    }
    if (delete_profile_button) {
        if (running || stopping || !profile_delete_available) lv_obj_add_state(delete_profile_button, LV_STATE_DISABLED);
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
    if (!rings) {
        rings = heap_caps_calloc(1, sizeof(*rings), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (rings) {
            message_ring = rings->messages;
            log_ring = rings->logs;
        }
    }
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
    portENTER_CRITICAL(&mqtt_lock);
    mqtt_rings_t *old_rings = rings;
    mqtt_message_t *old_incoming = incoming_message;
    char *old_history = history_text;
    rings = NULL;
    message_ring = NULL;
    log_ring = NULL;
    incoming_message = NULL;
    history_text = NULL;
    message_head = message_count = messages_dropped = 0;
    log_head = log_count = 0;
    incoming_active = false;
    incoming_offset = 0;
    portEXIT_CRITICAL(&mqtt_lock);
    heap_caps_free(old_rings);
    heap_caps_free(old_incoming);
    heap_caps_free(old_history);
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
    client_secure = false;
    connection_failed = false;
    portEXIT_CRITICAL(&mqtt_lock);
    if (handle) esp_mqtt_client_destroy(handle);
}

static void perform_action(mqtt_action_t *action)
{
    portENTER_CRITICAL(&mqtt_lock);
    bool stopping = client_stopping;
    uint32_t revision = event_status_revision;
    portEXIT_CRITICAL(&mqtt_lock);
    if (!stopping) {
        mqtt_message_t *message = &action->message;
        int message_id;
        if (action->publish) {
            /* QoS 0 requires store=true to enter the SDK outbox. Enqueue still
             * takes the network task's API lock, so it belongs on this worker. */
            message_id = esp_mqtt_client_enqueue(action->client, message->topic, message->payload,
                                                  (int)message->payload_bytes, message->qos,
                                                  message->retained, true);
            if (message_id >= 0) {
                message->timestamp = time(NULL);
                /* Enqueue has copied the original bytes into the SDK outbox.
                 * Only the bounded history preview is converted to text. */
                if (message->byte_preview) {
                    for (size_t i = 0; i < message->payload_bytes; ++i)
                        message->payload[i] = display_byte((unsigned char)message->payload[i]);
                    message->payload[message->payload_bytes] = '\0';
                }
                portENTER_CRITICAL(&mqtt_lock);
                bool queued_log = push_message_locked(message);
                portEXIT_CRITICAL(&mqtt_lock);
                if (queued_log) xTaskNotifyGive(cleanup_worker_handle);
            }
        } else {
            message_id = esp_mqtt_client_subscribe(action->client, message->topic, message->qos);
        }
        char text[sizeof(async_status)];
        if (message_id < 0)
            snprintf(text, sizeof(text), "%s", message_id == -2 ? "MQTT outbox full; wait before retrying" :
                     action->publish ? "Could not queue publish" : "Could not queue subscription");
        else
            snprintf(text, sizeof(text), action->publish ? "Publish queued on %s (message %d)" :
                                                         "Subscribing to %s (message %d)...",
                     message->topic, message_id);
        portENTER_CRITICAL(&mqtt_lock);
        /* The SDK releases its API lock before returning. An ACK, receive or
         * disconnect can already have supplied a newer status on its task. */
        if (!client_stopping && !connection_failed && revision == event_status_revision) {
            snprintf(async_status, sizeof(async_status), "%s", text);
            status_dirty = true;
        }
        portEXIT_CRITICAL(&mqtt_lock);
    }
    heap_caps_free(action);
    portENTER_CRITICAL(&mqtt_lock);
    action_busy = false;
    portEXIT_CRITICAL(&mqtt_lock);
}

static void notify_pending_work(void)
{
    portENTER_CRITICAL(&mqtt_lock);
    bool more_work = pending_action || cleanup_client || log_count;
    portEXIT_CRITICAL(&mqtt_lock);
    if (more_work) xTaskNotifyGive(cleanup_worker_handle);
}

static void record_log_error(int error)
{
    portENTER_CRITICAL(&mqtt_lock);
    if (!log_error) log_error = error;
    logging_enabled = false;
    cleanup_log_enabled = false;
    log_head = log_count = 0;
    portEXIT_CRITICAL(&mqtt_lock);
}

static void mqtt_cleanup_worker(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        portENTER_CRITICAL(&mqtt_lock);
        mqtt_action_t *action = pending_action;
        pending_action = NULL;
        /* The same worker owns action calls and destruction. Stop can detach
         * the UI while an SDK API waits, without destroying its borrowed client.
         * Select pending work before cleanup under the same lock: a queued
         * action must be released before its client can be destroyed. */
        esp_mqtt_client_handle_t handle = action ? NULL : cleanup_client;
        bool started = cleanup_client_started;
        mqtt_tool_storage_error_cb_t error_cb = cleanup_storage_error_cb;
        mqtt_log_entry_t entry;
        bool have_log = !action && !handle && log_count;
        if (have_log) {
            entry = log_ring[log_head];
            log_head = (log_head + 1) % MQTT_MESSAGE_LIMIT;
            log_count--;
            error_cb = storage_error_cb;
        }
        portEXIT_CRITICAL(&mqtt_lock);
        if (action) {
            perform_action(action);
            notify_pending_work();
            continue;
        }
        if (have_log) {
            int error = append_log(&entry);
            if (error) {
                record_log_error(error);
                portENTER_CRITICAL(&mqtt_lock);
                /* Once stopping begins, report after cleanup frees the buffers.
                 * An active-session callback also remains owned by this worker. */
                bool report = !client_stopping && !log_error_reported;
                if (report) log_error_reported = true;
                portEXIT_CRITICAL(&mqtt_lock);
                if (report && error_cb) error_cb(error);
            }
            notify_pending_work();
            continue;
        }
        /* A wakeup can also arrive after its work was already consumed. */
        if (!handle) continue;

        if (started) {
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_mqtt_client_stop(handle);
        }
        esp_mqtt_client_destroy(handle);

        portENTER_CRITICAL(&mqtt_lock);
        mqtt_rings_t *old_rings = rings;
        mqtt_message_t *old_incoming = incoming_message;
        char *old_history = history_text;
        unsigned old_head = log_head;
        unsigned old_count = log_count;
        int first_log_error = log_error;
        unsigned omitted = log_dropped;
        rings = NULL;
        message_ring = NULL;
        log_ring = NULL;
        incoming_message = NULL;
        history_text = NULL;
        message_head = message_count = messages_dropped = 0;
        log_head = log_count = 0;
        incoming_active = false;
        incoming_offset = 0;
        portEXIT_CRITICAL(&mqtt_lock);

        if (!first_log_error && old_rings) {
            for (unsigned i = 0; i < old_count; i++) {
                first_log_error = append_log(&old_rings->logs[(old_head + i) % MQTT_MESSAGE_LIMIT]);
                if (first_log_error) { record_log_error(first_log_error); break; }
            }
        }
        heap_caps_free(old_rings);
        heap_caps_free(old_incoming);
        heap_caps_free(old_history);
        portENTER_CRITICAL(&mqtt_lock);
        bool report = first_log_error && !log_error_reported;
        if (report) log_error_reported = true;
        portEXIT_CRITICAL(&mqtt_lock);
        if (report && error_cb) error_cb(first_log_error);

        /* OTA and re-entry must retain ownership through SD close, buffer release
         * and error reporting. Publish completion with the final state so a new
         * session cannot be overwritten by this session's delayed status. */
        portENTER_CRITICAL(&mqtt_lock);
        cleanup_client = NULL;
        cleanup_client_started = false;
        cleanup_log_enabled = false;
        cleanup_sd_available = false;
        cleanup_storage_error_cb = NULL;
        client_running = false;
        client_stopping = false;
        broker_connected = false;
        client_secure = false;
        connection_failed = false;
        if (screen_visible) {
            if (first_log_error)
                snprintf(async_status, sizeof(async_status), "Disconnected; SD metadata log not confirmed saved: %s", strerror(first_log_error));
            else if (omitted)
                snprintf(async_status, sizeof(async_status), "Disconnected; SD metadata omitted: %u entries", omitted);
            else
                snprintf(async_status, sizeof(async_status), "Disconnected; form password cleared");
            status_dirty = true;
        }
        portEXIT_CRITICAL(&mqtt_lock);
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
    logging_enabled = false;
    portEXIT_CRITICAL(&mqtt_lock);
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
    unsigned omitted;
    int storage_error;
    bool running;
    bool failed;
    portENTER_CRITICAL(&mqtt_lock);
    dirty = status_dirty;
    failed = connection_failed;
    if (dirty || failed) {
        snprintf(status, sizeof(status), "%s", async_status);
        status_dirty = false;
    }
    dropped = messages_dropped;
    messages_dropped = 0;
    omitted = log_dropped;
    storage_error = log_error;
    running = client_running;
    portEXIT_CRITICAL(&mqtt_lock);
    if (dirty && status_label) lv_label_set_text(status_label, status);
    drain_messages();
    if (running && storage_error && status_label) {
        if (failed) lv_label_set_text_fmt(status_label, "%s\nSD metadata log not confirmed saved: %s; disconnect before retrying",
                                          status, strerror(storage_error));
        else lv_label_set_text_fmt(status_label, "SD metadata log not confirmed saved: %s; disconnect before retrying", strerror(storage_error));
    } else if (running && omitted && status_label) {
        if (failed) lv_label_set_text_fmt(status_label, "%s\nDropped %u SD metadata entries; narrow the subscription", status, omitted);
        else lv_label_set_text_fmt(status_label, "Dropped %u SD metadata entries; narrow the subscription", omitted);
    } else if (dropped && status_label) {
        if (failed) lv_label_set_text_fmt(status_label, "%s\nDropped %u UI events; narrow the subscription", status, dropped);
        else lv_label_set_text_fmt(status_label, "Dropped %u UI events; narrow the subscription", dropped);
    }
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
    lv_obj_set_style_pad_all(container, 2, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return container;
}

static void textarea_event(lv_event_t *event)
{
    if (publish_bytes && lv_event_get_target_obj(event) == payload_area) return;
    if (lv_event_get_code(event) == LV_EVENT_FOCUSED && keyboard)
        lv_keyboard_set_textarea(keyboard, lv_event_get_target_obj(event));
    cleartext_armed = false;
    profile_delete_armed = false;
}

static void save_profile_clicked(lv_event_t *event)
{
    (void)event;
    if (!save_profile_button || lv_obj_has_state(save_profile_button, LV_STATE_DISABLED) ||
        running_snapshot() || stopping_snapshot()) return;
    cleartext_armed = false;
    profile_delete_armed = false;
    mqtt_profile_t profile = {.version = MQTT_PROFILE_VERSION};
    bool secure;
    if (!normalize_uri(lv_textarea_get_text(uri_area), profile.uri, &secure) || !secure) {
        lv_label_set_text(status_label, "Only valid mqtts:// or wss:// profiles can be saved; cleartext settings remain memory-only");
        return;
    }
    const char *username = lv_textarea_get_text(username_area);
    const char *password = lv_textarea_get_text(password_area);
    if (!credentials_valid(username, password)) {
        lv_label_set_text(status_label, "Username and password are limited to 64 UTF-8 bytes; excess input is retained");
        return;
    }
    const char *topic = lv_textarea_get_text(topic_area);
    if (!topic_filter_valid(topic)) {
        lv_label_set_text(status_label, "Enter a valid topic or subscription filter before saving");
        return;
    }
    snprintf(profile.username, sizeof(profile.username), "%s", username);
    snprintf(profile.password, sizeof(profile.password), "%s", password);
    snprintf(profile.topic, sizeof(profile.topic), "%s", topic);
    bool write_attempted;
    esp_err_t error = save_profile(&profile, &write_attempted);
    mbedtls_platform_zeroize(&profile, sizeof(profile));
    if (error != ESP_OK) {
        if (write_attempted) profile_delete_available = true;
        lv_label_set_text_fmt(status_label, profile_delete_available ?
                             "MQTT profile save not confirmed: %s; reopen to check or use DELETE PROFILE" :
                             "MQTT profile save not confirmed: %s; reopen to check", esp_err_to_name(error));
        update_controls();
        return;
    }
    profile_delete_available = true;
    profile_delete_armed = false;
    lv_label_set_text(status_label, "TLS profile saved in device settings; developer builds do not encrypt settings flash");
    update_controls();
}

static void delete_profile_clicked(lv_event_t *event)
{
    (void)event;
    if (!delete_profile_button || lv_obj_has_state(delete_profile_button, LV_STATE_DISABLED) ||
        !profile_delete_available || running_snapshot() || stopping_snapshot()) return;
    cleartext_armed = false;
    uint32_t now = lv_tick_get();
    if (!profile_delete_armed || (uint32_t)(now - profile_delete_armed_at) > MQTT_CONFIRM_MS) {
        profile_delete_armed = true;
        profile_delete_armed_at = now;
        lv_label_set_text(status_label, "Tap DELETE PROFILE again within 5 seconds to erase any saved broker credentials");
        return;
    }
    profile_delete_armed = false;
    esp_err_t error = delete_profile();
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        lv_label_set_text_fmt(status_label, "MQTT profile deletion not confirmed: %s; retry with two taps or reopen to check", esp_err_to_name(error));
        return;
    }
    profile_delete_available = false;
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
    if (!connect_button || lv_obj_has_state(connect_button, LV_STATE_DISABLED) ||
        stopping_snapshot() || !cleanup_worker_handle) return;
    profile_delete_armed = false;
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
    /* Cleanup can finish before the UI timer recreates its session buffers. */
    if (!ensure_buffers()) {
        lv_label_set_text(status_label, "MQTT buffer allocation failed");
        return;
    }
    char uri[MQTT_URI_MAX + 1];
    bool secure;
    if (!normalize_uri(lv_textarea_get_text(uri_area), uri, &secure)) {
        lv_label_set_text(status_label, "Use a valid MQTT broker URI and port 1-65535; only WS/WSS accepts paths/queries. No embedded credentials or fragments.");
        cleartext_armed = false;
        return;
    }
    const char *username = lv_textarea_get_text(username_area);
    const char *password = lv_textarea_get_text(password_area);
    if (!credentials_valid(username, password)) {
        lv_label_set_text(status_label, "Username and password are limited to 64 UTF-8 bytes; excess input is retained");
        cleartext_armed = false;
        return;
    }
    uint32_t now = lv_tick_get();
    if (!secure && !confirmation_valid(cleartext_armed, cleartext_armed_at, now)) {
        cleartext_armed = true;
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
            .verification.crt_bundle_attach = secure ? esp_crt_bundle_attach : NULL,
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
    portENTER_CRITICAL(&mqtt_lock);
    log_error = 0;
    log_error_reported = false;
    log_dropped = 0;
    status_dirty = false;
    async_status[0] = '\0';
    connection_failed = false;
    client_secure = secure;
    portEXIT_CRITICAL(&mqtt_lock);
    client = esp_mqtt_client_init(&config);
    if (!client || esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event, NULL) != ESP_OK) {
        if (client) esp_mqtt_client_destroy(client);
        client = NULL;
        lv_label_set_text(status_label, "Could not initialize MQTT client");
        return;
    }
    portENTER_CRITICAL(&mqtt_lock);
    latest_receive = (mqtt_receive_copy_t){.revision = latest_receive.revision + 1};
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
    if (!log_label || lv_obj_has_state(lv_obj_get_parent(log_label), LV_STATE_DISABLED) ||
        stopping_snapshot()) return;
    if (!screen_sd_available) {
        lv_label_set_text(status_label, "Insert a writable SD card before enabling metadata logging");
        return;
    }
    portENTER_CRITICAL(&mqtt_lock);
    bool failed = client_running && log_error;
    if (!failed) logging_enabled = !logging_enabled;
    portEXIT_CRITICAL(&mqtt_lock);
    if (failed) lv_label_set_text(status_label, "Disconnect before restarting SD metadata logging");
    update_controls();
}

static void queue_action(bool publish)
{
    mqtt_action_t *action = heap_caps_calloc(1, sizeof(*action), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!action) {
        lv_label_set_text(status_label, "MQTT action allocation failed; try again");
        return;
    }
    action->publish = publish;
    action->message.qos = (uint8_t)selected_qos;
    action->message.retained = publish && publish_retained;
    snprintf(action->message.topic, sizeof(action->message.topic), "%s", lv_textarea_get_text(topic_area));
    if (publish) {
        if (publish_bytes) {
            action->message.byte_preview = true;
            action->message.payload_bytes = prepared_payload.length;
            memcpy(action->message.payload, prepared_payload.bytes, prepared_payload.length);
            action->message.payload[prepared_payload.length] = '\0';
        } else {
            const char *payload = lv_textarea_get_text(payload_area);
            action->message.payload_bytes = strlen(payload);
            memcpy(action->message.payload, payload, action->message.payload_bytes + 1);
        }
    }

    portENTER_CRITICAL(&mqtt_lock);
    bool accepted = client && client_running && broker_connected && !client_stopping && !action_busy;
    if (accepted) {
        action->client = client;
        pending_action = action;
        action_busy = true;
    }
    portEXIT_CRITICAL(&mqtt_lock);
    if (!accepted) { heap_caps_free(action); return; }
    lv_label_set_text(status_label, publish ? "Queuing publish in the background..." :
                                            "Subscribing in the background...");
    update_controls();
    xTaskNotifyGive(cleanup_worker_handle);
}

static void subscribe_clicked(lv_event_t *event)
{
    (void)event;
    if (!subscribe_button || lv_obj_has_state(subscribe_button, LV_STATE_DISABLED) ||
        !client || stopping_snapshot() || action_busy_snapshot() || !connected_snapshot()) return;
    const char *topic = lv_textarea_get_text(topic_area);
    if (!topic_filter_valid(topic)) {
        lv_label_set_text(status_label, "Use 1-127 printable ASCII bytes; + and # must occupy complete levels");
        return;
    }
    queue_action(false);
}

static void publish_clicked(lv_event_t *event)
{
    (void)event;
    if (!publish_button || lv_obj_has_state(publish_button, LV_STATE_DISABLED) ||
        !client || stopping_snapshot() || action_busy_snapshot() || !connected_snapshot()) return;
    const char *topic = lv_textarea_get_text(topic_area);
    const char *payload = lv_textarea_get_text(payload_area);
    if (!publish_topic_valid(topic)) {
        lv_label_set_text(status_label, "Use 1-127 printable ASCII topic bytes without + or #");
        return;
    }
    if (publish_bytes && (!prepared_payload_present || prepared_payload.length > PAYLOAD_CLIPBOARD_MAX_BYTES)) {
        lv_label_set_text(status_label, "Paste complete clipboard bytes before publishing");
        return;
    }
    if (!publish_bytes && strlen(payload) > MQTT_PAYLOAD_MAX) {
        lv_label_set_text(status_label, "Publish payload exceeds 512 bytes; shorten it before sending");
        return;
    }
    queue_action(true);
}

static void refresh_payload(void)
{
    lv_label_set_text(payload_mode_label, publish_bytes ? "PAYLOAD: BYTES" : "PAYLOAD: TEXT");
    if (publish_bytes) {
        char hex[PAYLOAD_CLIPBOARD_HEX_SIZE];
        size_t used = 0;
        hex[0] = '\0';
        for (size_t i = 0; i < prepared_payload.length; ++i)
            used += (size_t)snprintf(hex + used, sizeof(hex) - used, "%02X%s", (unsigned)prepared_payload.bytes[i],
                                     i + 1 == prepared_payload.length ? "" : " ");
        lv_label_set_text_fmt(payload_bytes_label, "Prepared byte payload: %u bytes (Hex)\n%s\n"
                              "PUBLISH sends these exact bytes. Paste only prepares; it never sends.",
                              (unsigned)prepared_payload.length, used ? hex : "(empty payload)");
        lv_obj_add_flag(payload_area, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(payload_bytes_label, LV_OBJ_FLAG_HIDDEN);
        if (keyboard && lv_keyboard_get_textarea(keyboard) == payload_area)
            lv_keyboard_set_textarea(keyboard, NULL);
    } else {
        lv_obj_remove_flag(payload_area, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(payload_bytes_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void payload_mode_clicked(lv_event_t *event)
{
    (void)event;
    if (!payload_mode_button || lv_obj_has_state(payload_mode_button, LV_STATE_DISABLED) ||
        stopping_snapshot() || !prepared_payload_present) return;
    publish_bytes = !publish_bytes;
    cleartext_armed = profile_delete_armed = false;
    refresh_payload();
    update_controls();
}

static void paste_payload_clicked(lv_event_t *event)
{
    (void)event;
    if (!paste_payload_button || lv_obj_has_state(paste_payload_button, LV_STATE_DISABLED) || stopping_snapshot()) return;
    const payload_clipboard_t *copy = payload_clipboard_peek();
    if (!copy || copy->length > sizeof(copy->bytes)) return;
    prepared_payload = *copy;
    prepared_payload_present = publish_bytes = true;
    cleartext_armed = profile_delete_armed = false;
    refresh_payload();
    update_controls();
}

static void copy_receive_clicked(lv_event_t *event)
{
    (void)event;
    if (!copy_receive_button || lv_obj_has_state(copy_receive_button, LV_STATE_DISABLED)) return;
    portENTER_CRITICAL(&mqtt_lock);
    mqtt_receive_copy_t receive = latest_receive;
    bool stopping = client_stopping;
    portEXIT_CRITICAL(&mqtt_lock);
    if (stopping || !receive.present || receive.length > sizeof(receive.bytes)) return;
    if (payload_clipboard_store(receive.bytes, receive.length)) {
        copied_receive_revision = receive.revision;
        cleartext_armed = false;
        profile_delete_armed = false;
    }
    update_controls();
}

static void clear_clicked(lv_event_t *event)
{
    (void)event;
    cleartext_armed = false;
    profile_delete_armed = false;
    if (history_text) history_text[0] = '\0';
    if (history_area) lv_textarea_set_text(history_area, "");
    portENTER_CRITICAL(&mqtt_lock);
    bool failed = connection_failed;
    latest_receive = (mqtt_receive_copy_t){.revision = latest_receive.revision + 1};
    message_head = message_count = messages_dropped = 0;
    if (!failed) status_dirty = false;
    portEXIT_CRITICAL(&mqtt_lock);
    if (!failed) lv_label_set_text(status_label, "On-screen history cleared; SD metadata log is unchanged");
    update_controls();
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
        portENTER_CRITICAL(&mqtt_lock);
        logging_enabled = false;
        portEXIT_CRITICAL(&mqtt_lock);
        publish_retained = false;
    }
    uri_area = NULL;
    username_area = NULL;
    password_area = NULL;
    topic_area = NULL;
    payload_area = NULL;
    payload_mode_button = payload_mode_label = paste_payload_button = payload_bytes_label = NULL;
    keyboard = NULL;
    connect_button = NULL;
    connect_label = NULL;
    qos_label = NULL;
    retain_label = NULL;
    log_label = NULL;
    subscribe_button = NULL;
    publish_button = NULL;
    copy_receive_button = receive_label = NULL;
    save_profile_button = NULL;
    delete_profile_button = NULL;
    status_label = NULL;
    history_area = NULL;
    portENTER_CRITICAL(&mqtt_lock);
    storage_error_cb = NULL;
    portEXIT_CRITICAL(&mqtt_lock);
}

void mqtt_tool_show(lv_obj_t *parent, bool wifi_connected, bool sd_available,
                    mqtt_tool_storage_error_cb_t error_cb)
{
    screen_wifi_connected = wifi_connected;
    portENTER_CRITICAL(&mqtt_lock);
    screen_sd_available = sd_available;
    storage_error_cb = error_cb;
    logging_enabled = false;
    if (!client_running) {
        log_error = 0;
        log_error_reported = false;
        log_dropped = 0;
    }
    portEXIT_CRITICAL(&mqtt_lock);
    publish_retained = false;
    profile_delete_armed = false;
    mqtt_profile_t profile = {0};
    esp_err_t profile_error = load_profile(&profile, &profile_delete_available);
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
                            "PASTE BYTES prepares 0-128 bytes from Byte Lab; review, then PUBLISH.\n"
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
    lv_textarea_set_max_length(uri_area, MQTT_URI_MAX + 1);
    lv_textarea_set_text(uri_area, "mqtts://test.mosquitto.org:8886");
    lv_textarea_set_placeholder_text(uri_area, "mqtts://broker.example:8883");

    lv_obj_t *credentials = row(parent, 78);
    username_area = lv_textarea_create(credentials);
    lv_obj_set_size(username_area, 292, 68);
    lv_textarea_set_one_line(username_area, true);
    lv_textarea_set_max_length(username_area, MQTT_CREDENTIAL_MAX + 1);
    lv_textarea_set_placeholder_text(username_area, "Username (optional)");
    password_area = lv_textarea_create(credentials);
    lv_obj_set_size(password_area, 292, 68);
    lv_textarea_set_one_line(password_area, true);
    lv_textarea_set_password_mode(password_area, true);
    lv_textarea_set_max_length(password_area, MQTT_CREDENTIAL_MAX + 1);
    lv_textarea_set_placeholder_text(password_area, "Password (never exported)");

    lv_obj_t *profile_actions = row(parent, 78);
    save_profile_button = action_button(profile_actions, "SAVE PROFILE", 270,
                                        save_profile_clicked, NULL);
    delete_profile_button = action_button(profile_actions, "DELETE PROFILE", 270,
                                          delete_profile_clicked, NULL);

    topic_area = lv_textarea_create(parent);
    lv_obj_set_size(topic_area, 640, 72);
    lv_textarea_set_one_line(topic_area, true);
    lv_textarea_set_max_length(topic_area, MQTT_TOPIC_MAX + 1);
    lv_textarea_set_text(topic_area, profile_loaded ? profile.topic : default_topic);
    lv_textarea_set_placeholder_text(topic_area, "Topic or subscription filter");

    if (profile_loaded) {
        lv_textarea_set_text(uri_area, profile.uri);
        lv_textarea_set_text(username_area, profile.username);
        lv_textarea_set_text(password_area, profile.password);
    }
    mbedtls_platform_zeroize(&profile, sizeof(profile));

    lv_obj_t *actions = row(parent, 78);
    subscribe_button = action_button(actions, "SUBSCRIBE", 140, subscribe_clicked, NULL);
    publish_button = action_button(actions, "PUBLISH", 140, publish_clicked, NULL);
    action_button(actions, "CLEAR", 140, clear_clicked, NULL);
    copy_receive_button = action_button(actions, "COPY RX", 140, copy_receive_clicked, NULL);

    payload_area = lv_textarea_create(parent);
    lv_obj_set_size(payload_area, 640, 140);
    lv_textarea_set_max_length(payload_area, MQTT_PAYLOAD_MAX + 1);
    lv_textarea_set_text(payload_area, "hello from Tab5 OS");
    lv_textarea_set_placeholder_text(payload_area, "Text publish payload (maximum 512 UTF-8 bytes)");

    lv_obj_t *payload_actions = row(parent, 78);
    payload_mode_button = action_button(payload_actions, "", 270, payload_mode_clicked, &payload_mode_label);
    paste_payload_button = action_button(payload_actions, "PASTE BYTES", 270, paste_payload_clicked, NULL);
    payload_bytes_label = lv_label_create(parent);
    lv_obj_set_width(payload_bytes_label, 640);
    lv_obj_set_style_text_font(payload_bytes_label, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(payload_bytes_label, LV_LABEL_LONG_WRAP);
    refresh_payload();

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

    receive_label = lv_label_create(parent);
    lv_obj_set_width(receive_label, 640);
    lv_obj_set_style_text_font(receive_label, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(receive_label, LV_LABEL_LONG_WRAP);

    history_area = lv_textarea_create(parent);
    lv_obj_set_size(history_area, 640, 300);
    lv_textarea_set_cursor_click_pos(history_area, false);
    lv_textarea_set_placeholder_text(history_area, "Received and published messages");

    keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(keyboard, 640, 360);
    lv_keyboard_set_textarea(keyboard, uri_area);
    lv_obj_t *areas[] = {uri_area, username_area, password_area, topic_area, payload_area};
    for (size_t i = 0; i < sizeof(areas) / sizeof(areas[0]); i++) {
        lv_obj_add_event_cb(areas[i], textarea_event, LV_EVENT_FOCUSED, NULL);
        /* Every connection edit, including an edit restored to its old value,
         * requires a fresh warning. LVGL also emits this for set_text. */
        if (i < 3) lv_obj_add_event_cb(areas[i], textarea_event, LV_EVENT_VALUE_CHANGED, NULL);
    }
    update_controls();
    ui_timer = lv_timer_create(mqtt_tick, 200, NULL);
}
