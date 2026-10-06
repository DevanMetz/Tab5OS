#pragma once
/* Selected pinned SDK declarations; calls are a controlled service model.
 * This does not implement the MQTT wire protocol, TLS or SDK task scheduling. */
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct mqtt_host_client *esp_mqtt_client_handle_t;
typedef const char *esp_event_base_t;
typedef enum {
    MQTT_EVENT_ANY = -1, MQTT_EVENT_CONNECTED, MQTT_EVENT_DISCONNECTED,
    MQTT_EVENT_SUBSCRIBED, MQTT_EVENT_PUBLISHED, MQTT_EVENT_DATA, MQTT_EVENT_ERROR,
} esp_mqtt_event_id_t;
#define MQTT_ERROR_TYPE_TCP_TRANSPORT 1
#define MQTT_ERROR_TYPE_CONNECTION_REFUSED 2
#define MQTT_ERROR_TYPE_SUBSCRIBE_FAILED 3
typedef enum {
    MQTT_PROTOCOL_UNDEFINED = 0, MQTT_PROTOCOL_V_3_1, MQTT_PROTOCOL_V_3_1_1, MQTT_PROTOCOL_V_5,
} esp_mqtt_protocol_ver_t;
typedef struct { const char *filter; int qos; } esp_mqtt_topic_t;
typedef struct {
    esp_err_t esp_tls_last_esp_err;
    int esp_tls_stack_err, esp_tls_cert_verify_flags;
    int error_type, connect_return_code, esp_transport_sock_errno;
} esp_mqtt_error_codes_t;
typedef struct {
    int msg_id, current_data_offset, total_data_len, data_len, topic_len, qos;
    bool retain, dup;
    char *topic, *data;
    esp_mqtt_error_codes_t *error_handle;
} esp_mqtt_event_t, *esp_mqtt_event_handle_t;
typedef void (*mqtt_event_cb_t)(void *, esp_event_base_t, int32_t, void *);
typedef struct {
    struct {
        struct { const char *uri; } address;
        struct { esp_err_t (*crt_bundle_attach)(void *); } verification;
    } broker;
    struct {
        const char *username, *client_id;
        struct { const char *password; } authentication;
    } credentials;
    struct { int keepalive, protocol_ver; } session;
    struct { int timeout_ms; bool disable_auto_reconnect; } network;
    struct { int priority, stack_size; } task;
    struct { int size, out_size; } buffer;
    struct { uint64_t limit; } outbox;
} esp_mqtt_client_config_t;
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t, int, mqtt_event_cb_t, void *);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_disconnect(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t, const char *, int);
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t, const char *, const char *, int, int, int, bool);
