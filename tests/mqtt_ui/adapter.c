/* Native task notifications and deliberately stalled SDK/file boundaries.
 * The app, LVGL and storage_io.c run their actual code; no broker is contacted. */
#include "adapter.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "nvs.h"
#include "http_parser.h"
#include <assert.h>
#include <direct.h>
#include <errno.h>
#include <io.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>

volatile LONG mqtt_host_clients, mqtt_host_allocations, mqtt_host_stop_entered;
volatile LONG mqtt_host_destroy_entered, mqtt_host_sync_entered, mqtt_host_close_entered;
volatile LONG mqtt_host_open_files, mqtt_host_appends, mqtt_host_subscribes, mqtt_host_publishes;
volatile LONG mqtt_host_initializations, mqtt_host_disconnects, mqtt_host_nvs_writes;
volatile LONG mqtt_host_fail_allocation;
volatile LONG mqtt_host_actions_entered, mqtt_host_actions_active, mqtt_host_dispatch_entered;
volatile LONG mqtt_host_log_dispatch_entered;
volatile LONG mqtt_host_nvs_handles, mqtt_host_nvs_reads, mqtt_host_nvs_commits, mqtt_host_nvs_erases;
DWORD mqtt_host_ui_thread;
mqtt_host_connection_record_t mqtt_host_connection;
mqtt_host_profile_record_t mqtt_host_profile;
mqtt_host_action_record_t mqtt_host_actions[256];
HANDLE mqtt_host_stop_release, mqtt_host_destroy_release, mqtt_host_file_release;
HANDLE mqtt_host_action_release, mqtt_host_dispatch_release;
HANDLE mqtt_host_log_dispatch_release;
bool mqtt_host_hold_stop, mqtt_host_hold_destroy, mqtt_host_hold_sync, mqtt_host_hold_close;
bool mqtt_host_hold_action, mqtt_host_hold_dispatch;
bool mqtt_host_hold_log_dispatch;
static const char *test_mode;
static char directory[1024], csv_path[1100];
static volatile LONG file_fault;
static FILE *append_file;
static unsigned char nvs_blob[1024];
static size_t nvs_blob_length;
static bool nvs_namespace_exists, nvs_key_exists, nvs_wrong_type;
static mqtt_nvs_fault_t nvs_fault;
static nvs_handle_t nvs_live_handle, nvs_next_handle;
static int nvs_access;
struct mqtt_host_client { mqtt_event_cb_t callback; void *argument; bool started; };
static struct mqtt_host_client *live_client;
struct mqtt_host_task { HANDLE thread, notification; volatile LONG count, shutdown; void (*function)(void *); void *argument; };
static struct mqtt_host_task *cleanup_task;
static _Thread_local struct mqtt_host_task *current_task;

static void gate(HANDLE release) { assert(WaitForSingleObject(release, 5000) == WAIT_OBJECT_0); }
static unsigned __stdcall task_entry(void *argument)
{
    current_task = argument; current_task->function(current_task->argument);
    InterlockedDecrement(&host_active_tasks); return 0;
}
int mqtt_host_task_create(void (*function)(void *), const char *name, unsigned stack,
                          void *argument, unsigned priority, TaskHandle_t *handle, unsigned caps)
{
    assert(!cleanup_task && !strcmp(name, "mqtt-cleanup") && stack == 4096 && priority == 4);
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (InterlockedExchange(&host_fail_next_task, 0)) return 0;
    struct mqtt_host_task *task = calloc(1, sizeof(*task)); assert(task);
    task->notification = CreateEvent(NULL, FALSE, FALSE, NULL); assert(task->notification);
    task->function = function; task->argument = argument; cleanup_task = *handle = task;
    InterlockedIncrement(&host_active_tasks);
    task->thread = (HANDLE)_beginthreadex(NULL, 0, task_entry, task, 0, NULL); assert(task->thread);
    InterlockedIncrement(&host_tasks_started); return 1;
}
uint32_t mqtt_host_task_take(int clear, uint32_t wait)
{
    assert(current_task && clear == pdTRUE && wait == portMAX_DELAY);
    assert(WaitForSingleObject(current_task->notification, INFINITE) == WAIT_OBJECT_0);
    if (current_task->shutdown) { InterlockedDecrement(&host_active_tasks); _endthreadex(0); }
    uint32_t count = (uint32_t)InterlockedExchange(&current_task->count, 0);
    if (mqtt_host_hold_dispatch) {
        InterlockedIncrement(&mqtt_host_dispatch_entered); gate(mqtt_host_dispatch_release);
    }
    return count;
}
void mqtt_host_task_notify(TaskHandle_t task)
{
    assert(task == cleanup_task && !task->shutdown);
    InterlockedIncrement(&task->count); assert(SetEvent(task->notification));
}
void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (InterlockedExchange(&mqtt_host_fail_allocation, 0)) return NULL;
    void *memory = calloc(count, size); if (memory) InterlockedIncrement(&mqtt_host_allocations);
    return memory;
}
void heap_caps_free(void *memory)
{ if (memory) { free(memory); InterlockedDecrement(&mqtt_host_allocations); } }
esp_err_t esp_crt_bundle_attach(void *configuration) { (void)configuration; return ESP_OK; }
void esp_efuse_mac_get_default(uint8_t *mac)
{ const uint8_t fixed[] = {2, 0, 0, 1, 2, 3}; memcpy(mac, fixed, sizeof(fixed)); }
struct tm *mqtt_host_localtime_r(const time_t *time, struct tm *result)
{ return localtime_s(result, time) == 0 ? result : NULL; }
void mqtt_host_nvs_fault(mqtt_nvs_fault_t fault) { nvs_fault = fault; }
static bool nvs_fail(mqtt_nvs_fault_t fault)
{ if (nvs_fault != fault) return false; nvs_fault = MQTT_NVS_OK; return true; }
void mqtt_host_nvs_seed(const void *data, size_t length, bool wrong_type)
{
    assert(!mqtt_host_nvs_handles && length <= sizeof(nvs_blob));
    nvs_namespace_exists = true; nvs_key_exists = data != NULL; nvs_wrong_type = wrong_type;
    nvs_blob_length = data ? length : 0;
    if (data) memcpy(nvs_blob, data, length);
}
bool mqtt_host_nvs_present(void) { return nvs_key_exists; }
static void nvs_key(nvs_handle_t handle, const char *key, bool write)
{
    assert(handle == nvs_live_handle && mqtt_host_nvs_handles == 1 && !strcmp(key, "profile"));
    assert(!write || nvs_access == NVS_READWRITE);
}
esp_err_t nvs_open(const char *space, int access, nvs_handle_t *handle)
{
    assert(!strcmp(space, "mqtt") && !mqtt_host_nvs_handles &&
           (access == NVS_READONLY || access == NVS_READWRITE));
    if (nvs_fail(access == NVS_READONLY ? MQTT_NVS_OPEN_READ : MQTT_NVS_OPEN_WRITE)) return ESP_FAIL;
    if (access == NVS_READONLY && !nvs_namespace_exists) return ESP_ERR_NVS_NOT_FOUND;
    if (access == NVS_READWRITE) nvs_namespace_exists = true;
    nvs_access = access; nvs_live_handle = ++nvs_next_handle; *handle = nvs_live_handle;
    InterlockedIncrement(&mqtt_host_nvs_handles); return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *length)
{
    nvs_key(handle, key, false); assert(length); InterlockedIncrement(&mqtt_host_nvs_reads);
    if (nvs_fail(data ? MQTT_NVS_READ : MQTT_NVS_SIZE)) return ESP_FAIL;
    if (data && nvs_fail(MQTT_NVS_READ_GONE)) nvs_key_exists = false;
    if (!nvs_key_exists) return ESP_ERR_NVS_NOT_FOUND;
    if (nvs_wrong_type) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (data && nvs_fail(MQTT_NVS_READ_SHORT)) nvs_blob_length = 500;
    if (!data) { *length = nvs_blob_length; return ESP_OK; }
    if (*length < nvs_blob_length) { *length = nvs_blob_length; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(data, nvs_blob, nvs_blob_length); *length = nvs_blob_length; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length)
{
    nvs_key(handle, key, true); assert(length == sizeof(mqtt_host_profile));
    InterlockedIncrement(&mqtt_host_nvs_writes);
    if (nvs_fail(MQTT_NVS_SET_BEFORE)) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    memcpy(&mqtt_host_profile, data, length); memcpy(nvs_blob, data, length);
    nvs_blob_length = length; nvs_key_exists = true; nvs_wrong_type = false;
    /* The pinned SimpleHandle writes/erases immediately; commit is still required
     * by the public API contract. A reported failure cannot promise no effect. */
    return nvs_fail(MQTT_NVS_SET_AFTER) ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    nvs_key(handle, key, true); InterlockedIncrement(&mqtt_host_nvs_erases); InterlockedIncrement(&mqtt_host_nvs_writes);
    if (nvs_fail(MQTT_NVS_ERASE_BEFORE)) return ESP_FAIL;
    if (!nvs_key_exists) return ESP_ERR_NVS_NOT_FOUND;
    nvs_key_exists = false; nvs_wrong_type = false; memset(nvs_blob, 0, sizeof(nvs_blob));
    return nvs_fail(MQTT_NVS_ERASE_AFTER) ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{ nvs_key(handle, "profile", true); InterlockedIncrement(&mqtt_host_nvs_commits); return nvs_fail(MQTT_NVS_COMMIT) ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t handle)
{ nvs_key(handle, "profile", false); InterlockedDecrement(&mqtt_host_nvs_handles); nvs_live_handle = 0; }

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config)
{
    assert(!live_client && (!config->broker.verification.crt_bundle_attach ||
                           config->broker.verification.crt_bundle_attach == esp_crt_bundle_attach));
    assert(config->network.disable_auto_reconnect && config->network.timeout_ms == 10000);
    assert(config->session.protocol_ver == MQTT_PROTOCOL_V_3_1_1);
    assert(strlen(config->broker.address.uri) < sizeof(mqtt_host_connection.uri));
    const char *username = config->credentials.username ? config->credentials.username : "";
    const char *password = config->credentials.authentication.password ? config->credentials.authentication.password : "";
    assert(strlen(username) < sizeof(mqtt_host_connection.username) && strlen(password) < sizeof(mqtt_host_connection.password));
    strcpy(mqtt_host_connection.uri, config->broker.address.uri);
    strcpy(mqtt_host_connection.username, username); strcpy(mqtt_host_connection.password, password);
    mqtt_host_connection.certificate_bundle = config->broker.verification.crt_bundle_attach != NULL;
    InterlockedIncrement(&mqtt_host_initializations);
    struct http_parser_url parsed; http_parser_url_init(&parsed);
    if (http_parser_parse_url(config->broker.address.uri, strlen(config->broker.address.uri), 0, &parsed)) return NULL;
    if (!strcmp(test_mode, "init-failure")) return NULL;
    live_client = calloc(1, sizeof(*live_client)); assert(live_client);
    InterlockedIncrement(&mqtt_host_clients); return live_client;
}
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, int event, mqtt_event_cb_t callback, void *argument)
{
    assert(client == live_client && event == MQTT_EVENT_ANY);
    client->callback = callback; client->argument = argument;
    return !strcmp(test_mode, "register-failure") ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client)
{ assert(client == live_client); client->started = strcmp(test_mode, "start-failure") != 0; return client->started ? ESP_OK : ESP_FAIL; }
int platform_random(int max)
{ assert(max == 65535); return 0x1234; }
void mqtt_host_event(esp_mqtt_event_id_t event)
{
    assert(live_client && live_client->callback);
    esp_mqtt_event_t message = {.msg_id = 7};
    mqtt_host_event_data(event, &message);
}
void mqtt_host_event_data(esp_mqtt_event_id_t event_id, esp_mqtt_event_t *event)
{
    assert(live_client && live_client->callback);
    live_client->callback(live_client->argument, "MQTT", event_id, event);
}
void mqtt_host_receive(const char *topic, const char *data)
{
    esp_mqtt_event_t message = {.topic = (char *)topic, .topic_len = (int)strlen(topic),
        .data = (char *)data, .data_len = (int)strlen(data), .total_data_len = (int)strlen(data),
        .qos = 1, .retain = true};
    mqtt_host_data(&message);
}
void mqtt_host_data(esp_mqtt_event_t *event)
{
    mqtt_host_event_data(MQTT_EVENT_DATA, event);
}
esp_err_t esp_mqtt_client_disconnect(esp_mqtt_client_handle_t client)
{ assert(client == live_client && client->started); InterlockedIncrement(&mqtt_host_disconnects); return ESP_OK; }
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client)
{
    assert(client == live_client && client->started && !mqtt_host_actions_active); InterlockedIncrement(&mqtt_host_stop_entered);
    if (mqtt_host_hold_stop) gate(mqtt_host_stop_release);
    client->started = false; return ESP_OK;
}
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client)
{
    assert(client == live_client && !client->started && !mqtt_host_actions_active); InterlockedIncrement(&mqtt_host_destroy_entered);
    if (mqtt_host_hold_destroy) gate(mqtt_host_destroy_release);
    live_client = NULL; free(client); InterlockedDecrement(&mqtt_host_clients); return ESP_OK;
}
static void enter_action(bool publish, const char *topic, const char *payload, int length, int qos, int retained, bool store)
{
    if (GetCurrentThreadId() == mqtt_host_ui_thread) {
        fprintf(stderr, "MQTT SDK action ran on the UI thread\n"); fflush(stderr); ExitProcess(3);
    }
    assert(!mqtt_host_actions_active && mqtt_host_actions_entered < 256);
    mqtt_host_action_record_t *record = &mqtt_host_actions[mqtt_host_actions_entered];
    record->publish = publish; record->store = store; record->qos = qos; record->retained = retained; record->length = length;
    assert(strlen(topic) < sizeof(record->topic) && length >= 0 && length < (int)sizeof(record->payload));
    strcpy(record->topic, topic); memcpy(record->payload, payload, (size_t)length);
    record->payload[length] = '\0';
    InterlockedIncrement(&mqtt_host_actions_active); InterlockedIncrement(&mqtt_host_actions_entered);
    if (mqtt_host_hold_action) gate(mqtt_host_action_release);
}
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char *topic, int qos)
{
    assert(client == live_client && client && client->started && topic[0] && qos >= 0 && qos <= 2);
    enter_action(false, topic, "", 0, qos, 0, false);
    int result = !strcmp(test_mode, "subscribe-error") ? -1 : (int)mqtt_host_actions_entered;
    mqtt_host_actions[mqtt_host_actions_entered - 1].result = result;
    InterlockedIncrement(&mqtt_host_subscribes); InterlockedDecrement(&mqtt_host_actions_active);
    return result;
}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t client, const char *topic, const char *payload,
                             int length, int qos, int retained, bool store)
{
    assert(client == live_client && client && client->started && topic[0] && length >= 0 && length <= 512);
    assert(qos >= 0 && qos <= 2 && retained >= 0 && retained <= 1);
    enter_action(true, topic, payload, length, qos, retained, store);
    int result = !strcmp(test_mode, "publish-full") || !strcmp(test_mode, "paste-error") ? -2 :
                 !strcmp(test_mode, "publish-error") || (!qos && !store) ? -1 :
                 qos ? (int)mqtt_host_actions_entered : 0;
    mqtt_host_actions[mqtt_host_actions_entered - 1].result = result;
    InterlockedIncrement(&mqtt_host_publishes); InterlockedDecrement(&mqtt_host_actions_active);
    return result;
}

void mqtt_host_setup(const char *output, const char *mode)
{
    test_mode = mode;
    mqtt_host_ui_thread = GetCurrentThreadId();
    assert(snprintf(directory, sizeof(directory), "%s/storage-%s", output, mode) < (int)sizeof(directory));
    assert(_mkdir(directory) == 0 || errno == EEXIST);
    assert(snprintf(csv_path, sizeof(csv_path), "%s/MQTTLOG.CSV", directory) < (int)sizeof(csv_path));
    assert(remove(csv_path) == 0 || errno == ENOENT);
    mqtt_host_stop_release = CreateEvent(NULL, TRUE, FALSE, NULL);
    mqtt_host_destroy_release = CreateEvent(NULL, TRUE, FALSE, NULL);
    mqtt_host_file_release = CreateEvent(NULL, TRUE, FALSE, NULL);
    mqtt_host_action_release = CreateEvent(NULL, TRUE, FALSE, NULL);
    mqtt_host_dispatch_release = CreateEvent(NULL, TRUE, FALSE, NULL);
    mqtt_host_log_dispatch_release = CreateEvent(NULL, TRUE, FALSE, NULL);
    assert(mqtt_host_stop_release && mqtt_host_destroy_release && mqtt_host_file_release && mqtt_host_action_release && mqtt_host_dispatch_release && mqtt_host_log_dispatch_release);
}
void mqtt_host_shutdown(void)
{
    assert(!live_client && !mqtt_host_open_files && !mqtt_host_allocations && !mqtt_host_nvs_handles);
    if (cleanup_task) {
        InterlockedExchange(&cleanup_task->shutdown, 1); SetEvent(cleanup_task->notification);
        assert(WaitForSingleObject(cleanup_task->thread, 1000) == WAIT_OBJECT_0);
        CloseHandle(cleanup_task->thread); CloseHandle(cleanup_task->notification); free(cleanup_task); cleanup_task = NULL;
    }
    CloseHandle(mqtt_host_stop_release); CloseHandle(mqtt_host_destroy_release); CloseHandle(mqtt_host_file_release);
    CloseHandle(mqtt_host_action_release); CloseHandle(mqtt_host_dispatch_release);
    CloseHandle(mqtt_host_log_dispatch_release);
    assert(!host_active_tasks);
}
void mqtt_host_file_fault(mqtt_file_fault_t fault) { InterlockedExchange(&file_fault, fault); }
const char *mqtt_host_csv_path(void) { return csv_path; }
static bool fail(mqtt_file_fault_t fault)
{ if (InterlockedCompareExchange(&file_fault, MQTT_FILE_OK, fault) != fault) return false; errno = EIO; return true; }
static void worker_file_call(void)
{
    if (GetCurrentThreadId() == mqtt_host_ui_thread) {
        fprintf(stderr, "MQTT filesystem call ran on the UI thread\n"); fflush(stderr); ExitProcess(3);
    }
}
int mqtt_host_mkdir(const char *path)
{
    worker_file_call(); assert(!strcmp(path, "/sdcard/MQTT"));
    if (mqtt_host_hold_log_dispatch) {
        InterlockedIncrement(&mqtt_host_log_dispatch_entered); gate(mqtt_host_log_dispatch_release);
    }
    return _mkdir(directory);
}
FILE *mqtt_host_fopen(const char *path, const char *mode)
{
    worker_file_call();
    assert(!strcmp(path, "/sdcard/MQTT/MQTTLOG.CSV"));
    bool append = !strcmp(mode, "a+"); assert(append || !strcmp(mode, "r+b"));
    if (append) InterlockedIncrement(&mqtt_host_appends);
    if (fail(append ? MQTT_FILE_OPEN : MQTT_FILE_REPAIR)) return NULL;
    FILE *file = fopen(csv_path, mode);
    if (file) { InterlockedIncrement(&mqtt_host_open_files); if (append) { assert(!append_file); append_file = file; } }
    return file;
}
int mqtt_host_fclose(FILE *file)
{
    worker_file_call();
    bool append = file == append_file;
    if (append) { InterlockedIncrement(&mqtt_host_close_entered); if (mqtt_host_hold_close) gate(mqtt_host_file_release); }
    int result = fclose(file); InterlockedDecrement(&mqtt_host_open_files);
    if (append) { append_file = NULL; if (fail(MQTT_FILE_CLOSE)) return EOF; }
    return result;
}
int mqtt_host_fflush(FILE *file) { worker_file_call(); return fail(MQTT_FILE_FLUSH) ? EOF : fflush(file); }
int mqtt_host_commit(int descriptor)
{
    worker_file_call();
    if (append_file && descriptor == _fileno(append_file)) {
        InterlockedIncrement(&mqtt_host_sync_entered); if (mqtt_host_hold_sync) gate(mqtt_host_file_release);
    }
    return fail(MQTT_FILE_SYNC) ? -1 : _commit(descriptor);
}
