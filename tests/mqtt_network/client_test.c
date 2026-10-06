#include "adapter.h"
#include "mqtt_client.h"
#include <errno.h>
ESP_EVENT_DECLARE_BASE(MQTT_EVENTS);

typedef struct {
    const char *mode;
    volatile LONG connected, disconnected, subscribed, published, errors, complete, done;
    int publish_id, subscribe_id, subscribe_error, refusal, transport_error, error_type;
    unsigned data_events, received, expected_length;
    bool zero_first;
} result_t;

static void wait_for(volatile LONG *value, LONG expected)
{
    uint64_t deadline = GetTickCount64() + 5000;
    while (InterlockedCompareExchange(value, 0, 0) < expected && GetTickCount64() < deadline) Sleep(1);
    assert(InterlockedCompareExchange(value, 0, 0) == expected);
}
static bool is_receive(const char *mode)
{
    return !strncmp(mode, "receive-", 8) || !strcmp(mode, "lifecycle");
}
static void on_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    result_t *result = argument;
    esp_mqtt_event_t *event = data;
    assert(base == MQTT_EVENTS && id == event->event_id && event->protocol_ver == MQTT_PROTOCOL_V_3_1_1);
    if (id == MQTT_EVENT_CONNECTED) InterlockedIncrement(&result->connected);
    else if (id == MQTT_EVENT_DISCONNECTED) InterlockedIncrement(&result->disconnected);
    else if (id == MQTT_EVENT_SUBSCRIBED) {
        assert(event->data_len == 1 && event->data);
        result->subscribe_id = event->msg_id;
        result->subscribe_error = event->error_handle->error_type;
        assert((unsigned char)event->data[0] == (!strcmp(result->mode, "subscribe-rejected") ? 0x80 : 2));
        InterlockedIncrement(&result->subscribed);
    } else if (id == MQTT_EVENT_PUBLISHED) {
        result->publish_id = event->msg_id; InterlockedIncrement(&result->published);
    } else if (id == MQTT_EVENT_ERROR) {
        result->error_type = event->error_handle->error_type;
        result->refusal = event->error_handle->connect_return_code;
        result->transport_error = event->error_handle->esp_tls_last_esp_err;
        InterlockedIncrement(&result->errors);
    } else if (id == MQTT_EVENT_DATA) {
        printf("DATA offset=%d length=%d total=%d topic=%d qos=%d retain=%d dup=%d id=%d\n",
               event->current_data_offset, event->data_len, event->total_data_len, event->topic_len,
               event->qos, event->retain, event->dup, event->msg_id);
        assert(event->data_len >= 0 && event->topic_len >= 0 && event->current_data_offset >= 0);
        if (event->topic_len == 12 && !memcmp(event->topic, "fixture/done", 12)) {
            assert((result->complete == 1 || !strncmp(result->mode, "publish-", 8)) &&
                   event->qos == 0 && !event->retain && !event->dup);
            assert(event->current_data_offset == 0 && event->total_data_len == 2 && event->data_len == 2);
            assert(!memcmp(event->data, "ok", 2)); InterlockedIncrement(&result->done); return;
        }
        assert(is_receive(result->mode) || !strcmp(result->mode, "truncated-publish"));
        bool large_topic = !strcmp(result->mode, "receive-zero-start");
        bool empty = !strcmp(result->mode, "receive-empty");
        bool fragment = !strcmp(result->mode, "receive-fragments") || !strcmp(result->mode, "lifecycle") ||
                        !strcmp(result->mode, "truncated-publish");
        int qos = large_topic || fragment || !strcmp(result->mode, "receive-qos2") ? 2 :
                  !strcmp(result->mode, "receive-qos1") ? 1 : 0;
        assert(event->qos == qos && event->retain == 1 && event->dup == (qos == 2));
        assert(event->msg_id == (qos ? 0x1234 : 0));
        assert(event->total_data_len == (int)result->expected_length);
        assert(event->current_data_offset == (int)result->received);
        if (!result->data_events) {
            assert(event->topic && event->topic_len == (large_topic ? 1017 : 10));
            if (large_topic) {
                for (int i = 0; i < event->topic_len; ++i) assert(event->topic[i] == 't');
                assert(event->data_len == 0 && !event->data); result->zero_first = true;
            } else assert(!memcmp(event->topic, "site/state", 10));
        } else assert(event->topic_len == 0 && !event->topic);
        assert(result->received + (unsigned)event->data_len <= result->expected_length);
        for (int i = 0; i < event->data_len; ++i) {
            unsigned char expected = large_topic ? 'k' : fragment ? (unsigned char)(result->received + (unsigned)i) :
                                     (const unsigned char[]){0, 0x80, 'Z'}[result->received + (unsigned)i];
            assert((unsigned char)event->data[i] == expected);
        }
        result->received += (unsigned)event->data_len; result->data_events++;
        if (result->received == result->expected_length) InterlockedIncrement(&result->complete);
        if (empty) assert(result->received == 0 && result->data_events == 1);
    }
}

static void one_session(int port, const char *mode)
{
    result_t result = {.mode = mode};
    result.expected_length = !strcmp(mode, "receive-empty") ? 0 : !strcmp(mode, "receive-zero-start") ? 17 :
        !strcmp(mode, "truncated-publish") ? 2048 :
        !strcmp(mode, "receive-fragments") || !strcmp(mode, "lifecycle") ? 1024 : 3;
    char uri[96]; snprintf(uri, sizeof(uri), "mqtt://127.0.0.1:%d", port);
    const esp_mqtt_client_config_t config = {
        .broker.address.uri = uri,
        .credentials.client_id = "tab5-native",
        .credentials.username = "fixture-u", .credentials.authentication.password = "fixture-p",
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1, .session.keepalive = 60,
        .session.message_retransmit_timeout = 10000,
        .network.disable_auto_reconnect = true, .network.timeout_ms = 1000,
        .buffer.size = 1024, .buffer.out_size = 1024,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config); assert(client);
    assert(esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, on_event, &result) == ESP_OK);
    assert(esp_mqtt_client_start(client) == ESP_OK);
    if (!strcmp(mode, "connect-rejected")) {
        wait_for(&result.disconnected, 1); assert(result.connected == 0 && result.errors == 1 && result.refusal == 5 &&
                                                result.error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED);
    } else {
        wait_for(&result.connected, 1);
        if (!strcmp(mode, "subscribe-rejected") || !strcmp(mode, "lifecycle")) {
            int id = esp_mqtt_client_subscribe(client, "site/+/state", 2); assert(id > 0);
            wait_for(&result.subscribed, 1); assert(result.subscribe_id == id);
            assert(result.subscribe_error == (!strcmp(mode, "subscribe-rejected") ? MQTT_ERROR_TYPE_SUBSCRIBE_FAILED : MQTT_ERROR_TYPE_NONE));
        }
        if (!strncmp(mode, "publish-", 8) || !strcmp(mode, "lifecycle")) {
            int qos = !strcmp(mode, "publish-qos0") || !strcmp(mode, "publish-empty") ? 0 :
                      !strcmp(mode, "publish-qos2") ? 2 : 1;
            const char bytes[] = {(char)0xC2, (char)0xB5, 'Z', 0};
            bool empty = !strcmp(mode, "publish-empty");
            int id = esp_mqtt_client_enqueue(client, "site/state", empty ? "" : bytes, empty ? 0 : 3, qos, 1, true);
            assert(qos ? id > 0 : id == 0);
            if (qos) { wait_for(&result.published, 1); assert(result.publish_id == id); }
            else {
                /* The fixture sends a custom completion event only after reading
                 * the QoS 0 packet, so enqueue success alone is not the assertion. */
                wait_for(&result.done, 1);
                assert(result.published == 0);
            }
        }
        if (is_receive(mode)) {
            wait_for(&result.done, 1); assert(result.complete == 1 && result.received == result.expected_length);
            if (!strcmp(mode, "receive-zero-start")) assert(result.zero_first && result.data_events >= 2);
            if (!strcmp(mode, "receive-fragments") || !strcmp(mode, "lifecycle")) assert(result.data_events >= 2);
        } else if (!strcmp(mode, "invalid-header") || !strcmp(mode, "truncated-publish")) {
            wait_for(&result.disconnected, 1); assert(result.complete == 0 && result.done == 0);
            if (!strcmp(mode, "invalid-header")) assert(result.data_events == 0);
            else assert(result.errors == 1 && result.error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT &&
                        result.transport_error == ESP_ERR_ESP_TLS_TCP_CLOSED_FIN &&
                        result.data_events >= 1 && result.received > 0 && result.received < result.expected_length);
        }
        if (!result.disconnected) assert(result.errors == 0);
        assert(esp_mqtt_client_get_outbox_size(client) == 0);
    }
    assert(esp_mqtt_client_stop(client) == ESP_OK);
    mqtt_net_join_task();
    assert(esp_mqtt_client_destroy(client) == ESP_OK);
    assert(!mqtt_net_allocations && !mqtt_net_transports && !mqtt_net_threads && !host_open_sockets);
    printf("SESSION mode=%s events=%u received=%u zero_first=%d errors=%ld allocations=%ld sockets=%ld\n",
           mode, result.data_events, result.received, result.zero_first, result.errors, mqtt_net_allocations, host_open_sockets);
}

int main(int argc, char **argv)
{
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    assert(argc == 3); setvbuf(stdout, NULL, _IONBF, 0);
    int port = atoi(argv[1]); assert(port > 0 && port <= 65535);
    const char *mode = argv[2];
    if (!strcmp(mode, "receive-fragments") || !strcmp(mode, "lifecycle")) mqtt_net_read_cap = 17;
    WSADATA sockets; assert(WSAStartup(MAKEWORD(2, 2), &sockets) == 0);
    DWORD before, warm = 0, after; assert(GetProcessHandleCount(GetCurrentProcess(), &before));
    int cycles = !strcmp(mode, "lifecycle") ? 25 : 1;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        one_session(port, mode);
        if (!cycle) assert(GetProcessHandleCount(GetCurrentProcess(), &warm));
    }
    /* WinSock/CRT lazily open process-level resources on their first use. The
     * soak compares subsequent sessions with that first fully closed session. */
    assert(GetProcessHandleCount(GetCurrentProcess(), &after) && warm == after);
    printf("PASS %s cycles=%d cold_handles=%lu handles=%lu->%lu SDK_allocations=%ld transports=%ld tasks=%ld sockets=%ld\n",
           mode, cycles, before, warm, after, mqtt_net_allocations, mqtt_net_transports, mqtt_net_threads, host_open_sockets);
    assert(WSACleanup() == 0); return 0;
}
