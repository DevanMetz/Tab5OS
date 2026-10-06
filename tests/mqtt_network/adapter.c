/* OS/event services and a loopback-only TCP endpoint for unmodified ESP-MQTT.
 * The SDK client, receive/dispatch loop, outbox and transport dispatch are real.
 * This port does not implement TLS, WebSocket or ESP-IDF task scheduling. */
#include "adapter.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_transport_internal.h"
#include "esp_transport_tcp.h"
#include "mqtt_client.h"
#include <errno.h>
#include <process.h>

volatile LONG mqtt_net_allocations, mqtt_net_transports, mqtt_net_threads;
unsigned mqtt_net_read_cap;
DWORD mqtt_net_ui_thread;
volatile LONG mqtt_net_data_events, mqtt_net_published_events, mqtt_net_subscribed_events;
volatile LONG mqtt_net_held_api_returns;
static volatile LONG api_return_event;
static LONG api_return_target;
static __declspec(thread) unsigned api_lock_depth;
mqtt_net_data_record_t mqtt_net_data_records[128];
void *mqtt_net_malloc(size_t size)
{
    void *memory = malloc(size);
    if (memory) InterlockedIncrement(&mqtt_net_allocations);
    return memory;
}
void *mqtt_net_calloc(size_t count, size_t size)
{
    void *memory = calloc(count, size);
    if (memory) InterlockedIncrement(&mqtt_net_allocations);
    return memory;
}
void *mqtt_net_realloc(void *memory, size_t size)
{
    assert(size);
    void *replacement = realloc(memory, size);
    if (!memory && replacement) InterlockedIncrement(&mqtt_net_allocations);
    return replacement;
}
void mqtt_net_free(void *memory)
{
    if (memory) { InterlockedDecrement(&mqtt_net_allocations); free(memory); }
}
char *mqtt_net_strdup(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = mqtt_net_malloc(length);
    if (copy) memcpy(copy, text, length);
    return copy;
}
int asprintf(char **output, const char *format, ...)
{
    va_list args; va_start(args, format);
    va_list copy; va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy); va_end(copy);
    *output = length < 0 ? NULL : mqtt_net_malloc((size_t)length + 1);
    int result = *output ? vsnprintf(*output, (size_t)length + 1, format, args) : -1;
    va_end(args); return result;
}
void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(caps == MALLOC_CAP_DEFAULT); return mqtt_net_malloc(size);
}
void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    assert(caps == MALLOC_CAP_DEFAULT || caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return mqtt_net_calloc(count, size);
}
void heap_caps_free(void *memory) { mqtt_net_free(memory); }
char *platform_create_id_string(void) { return mqtt_net_strdup("tab5-native"); }
int platform_random(int max) { assert(max > 0); return rand() % max; }
uint64_t platform_tick_get_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

struct mqtt_net_mutex { CRITICAL_SECTION lock; };
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    SemaphoreHandle_t mutex = mqtt_net_calloc(1, sizeof(*mutex)); assert(mutex);
    InitializeCriticalSection(&mutex->lock); return mutex;
}
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t timeout)
{
    assert(timeout == portMAX_DELAY); EnterCriticalSection(&mutex->lock); ++api_lock_depth; return pdTRUE;
}
static volatile LONG *ack_counter(int event_id)
{
    assert(event_id == MQTT_EVENT_SUBSCRIBED || event_id == MQTT_EVENT_PUBLISHED);
    return event_id == MQTT_EVENT_SUBSCRIBED ? &mqtt_net_subscribed_events : &mqtt_net_published_events;
}
void mqtt_net_hold_api_return(int event_id)
{
    assert(GetCurrentThreadId() == mqtt_net_ui_thread && !api_return_event);
    api_return_target = InterlockedCompareExchange(ack_counter(event_id), 0, 0) + 1;
    InterlockedExchange(&api_return_event, event_id);
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex)
{
    assert(api_lock_depth); --api_lock_depth; LeaveCriticalSection(&mutex->lock);
    if (!api_lock_depth && mqtt_net_on_worker()) {
        LONG event_id = InterlockedExchange(&api_return_event, 0);
        if (event_id) {
            /* Yield after the real SDK releases its API mutex, before that API
             * returns to the app. Only actual broker bytes dispatch the ACK. */
            uint64_t until = GetTickCount64() + 3000;
            while (InterlockedCompareExchange(ack_counter(event_id), 0, 0) < api_return_target) {
                assert(GetTickCount64() < until); Sleep(1);
            }
            InterlockedIncrement(&mqtt_net_held_api_returns);
            printf("SDK_API acknowledgment=%ld dispatched before worker API return\n", event_id);
        }
    }
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    DeleteCriticalSection(&mutex->lock); mqtt_net_free(mutex);
}
struct mqtt_net_group { SRWLOCK lock; CONDITION_VARIABLE changed; EventBits_t bits; };
EventGroupHandle_t xEventGroupCreate(void)
{
    EventGroupHandle_t group = mqtt_net_calloc(1, sizeof(*group)); assert(group);
    InitializeSRWLock(&group->lock); InitializeConditionVariable(&group->changed); return group;
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
    AcquireSRWLockExclusive(&group->lock); group->bits |= bits;
    EventBits_t result = group->bits; WakeAllConditionVariable(&group->changed);
    ReleaseSRWLockExclusive(&group->lock); return result;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{
    AcquireSRWLockExclusive(&group->lock);
    EventBits_t result = group->bits; group->bits &= ~bits;
    ReleaseSRWLockExclusive(&group->lock); return result;
}
EventBits_t xEventGroupGetBits(EventGroupHandle_t group)
{
    AcquireSRWLockShared(&group->lock); EventBits_t result = group->bits;
    ReleaseSRWLockShared(&group->lock); return result;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                              BaseType_t clear, BaseType_t all, TickType_t timeout)
{
    uint64_t start = GetTickCount64();
    AcquireSRWLockExclusive(&group->lock);
    while (all ? (group->bits & bits) != bits : !(group->bits & bits)) {
        uint64_t elapsed = GetTickCount64() - start;
        if (timeout != portMAX_DELAY && elapsed >= timeout) break;
        DWORD wait = timeout == portMAX_DELAY ? INFINITE : (DWORD)(timeout - elapsed);
        if (!SleepConditionVariableSRW(&group->changed, &group->lock, wait, 0)) {
            assert(GetLastError() == ERROR_TIMEOUT); break;
        }
    }
    EventBits_t result = group->bits;
    if ((all ? (result & bits) == bits : (result & bits) != 0) && clear) group->bits &= ~bits;
    ReleaseSRWLockExclusive(&group->lock);
    /* ESP-MQTT signals STOPPED just before its final task statements. Reclaim
     * the native thread here so a real app stop/destroy can safely follow. This
     * explicit host join strengthens shutdown timing and is not a board test. */
    if (bits == 1 && timeout == portMAX_DELAY && (result & bits)) mqtt_net_join_task();
    return result;
}
void vEventGroupDelete(EventGroupHandle_t group) { mqtt_net_free(group); }

struct mqtt_net_task {
    HANDLE thread, notification; volatile LONG notifications, shutdown;
    void (*function)(void *); void *argument;
};
static _Thread_local TaskHandle_t current_task;
static TaskHandle_t started_task;
static TaskHandle_t cleanup_task;
static unsigned __stdcall task_entry(void *argument)
{
    current_task = argument; current_task->function(current_task->argument);
    InterlockedDecrement(&mqtt_net_threads); return 0;
}
BaseType_t xTaskCreate(void (*function)(void *), const char *name, unsigned stack,
                       void *argument, unsigned priority, TaskHandle_t *handle)
{
    assert(!started_task && !strcmp(name, "mqtt_task") && stack && priority);
    TaskHandle_t task = mqtt_net_calloc(1, sizeof(*task)); assert(task);
    task->function = function; task->argument = argument; *handle = task;
    InterlockedIncrement(&mqtt_net_threads);
    task->thread = (HANDLE)_beginthreadex(NULL, 0, task_entry, task, CREATE_SUSPENDED, NULL); assert(task->thread);
    started_task = task; assert(ResumeThread(task->thread) != (DWORD)-1); return pdTRUE;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task; }
void vTaskDelete(TaskHandle_t task) { assert(!task && current_task); }
void mqtt_net_join_task(void)
{
    /* Native handle reclamation is explicit. Keep the client alive until the
     * SDK's final task statements finish; this is not a device timing test. */
    if (!started_task) return;
    assert(started_task != current_task && WaitForSingleObject(started_task->thread, 5000) == WAIT_OBJECT_0);
    CloseHandle(started_task->thread); mqtt_net_free(started_task); started_task = NULL;
}
BaseType_t xTaskCreateWithCaps(void (*function)(void *), const char *name, unsigned stack,
                              void *argument, unsigned priority, TaskHandle_t *handle, unsigned caps)
{
    assert(!cleanup_task && !strcmp(name, "mqtt-cleanup") && stack == 4096 && priority == 4);
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    TaskHandle_t task = mqtt_net_calloc(1, sizeof(*task)); assert(task);
    task->notification = CreateEvent(NULL, FALSE, FALSE, NULL); assert(task->notification);
    task->function = function; task->argument = argument; cleanup_task = *handle = task;
    InterlockedIncrement(&mqtt_net_threads);
    task->thread = (HANDLE)_beginthreadex(NULL, 0, task_entry, task, CREATE_SUSPENDED, NULL); assert(task->thread);
    assert(ResumeThread(task->thread) != (DWORD)-1); return pdTRUE;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    assert(current_task == cleanup_task && clear == pdTRUE && wait == portMAX_DELAY);
    assert(WaitForSingleObject(current_task->notification, INFINITE) == WAIT_OBJECT_0);
    if (current_task->shutdown) { InterlockedDecrement(&mqtt_net_threads); _endthreadex(0); }
    return (uint32_t)InterlockedExchange(&current_task->notifications, 0);
}
void xTaskNotifyGive(TaskHandle_t task)
{
    assert(task == cleanup_task && !task->shutdown);
    InterlockedIncrement(&task->notifications); assert(SetEvent(task->notification));
}
void mqtt_net_shutdown_worker(void)
{
    assert(!started_task);
    if (!cleanup_task) return;
    InterlockedExchange(&cleanup_task->shutdown, 1); assert(SetEvent(cleanup_task->notification));
    assert(WaitForSingleObject(cleanup_task->thread, 1000) == WAIT_OBJECT_0);
    CloseHandle(cleanup_task->thread); CloseHandle(cleanup_task->notification);
    mqtt_net_free(cleanup_task); cleanup_task = NULL;
}
bool mqtt_net_on_worker(void) { return cleanup_task && current_task == cleanup_task; }

struct mqtt_net_loop {
    esp_event_handler_t handler; void *argument;
    esp_event_base_t base; int32_t id, registered_id;
    bool pending; esp_mqtt_event_t event;
};
esp_err_t esp_event_loop_create(const esp_event_loop_args_t *args, esp_event_loop_handle_t *loop)
{
    assert(args->queue_size == 1 && !args->task_name);
    *loop = mqtt_net_calloc(1, sizeof(**loop)); return *loop ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t esp_event_loop_delete(esp_event_loop_handle_t loop) { mqtt_net_free(loop); return ESP_OK; }
esp_err_t esp_event_handler_register_with(esp_event_loop_handle_t loop, esp_event_base_t base,
                                        int32_t id, esp_event_handler_t handler, void *argument)
{
    assert(loop && !loop->handler); loop->handler = handler; loop->argument = argument;
    loop->base = base; loop->registered_id = id; return ESP_OK;
}
esp_err_t esp_event_handler_unregister_with(esp_event_loop_handle_t loop, esp_event_base_t base,
                                          int32_t id, esp_event_handler_t handler)
{
    assert(loop->base == base && loop->registered_id == id && loop->handler == handler);
    loop->handler = NULL; return ESP_OK;
}
esp_err_t esp_event_post_to(esp_event_loop_handle_t loop, esp_event_base_t base, int32_t id,
                          const void *data, size_t size, TickType_t timeout)
{
    (void)timeout; assert(loop && !loop->pending && size == sizeof(loop->event));
    assert(base == loop->base); memcpy(&loop->event, data, size);
    loop->id = id; loop->pending = true; return ESP_OK;
}
esp_err_t esp_event_loop_run(esp_event_loop_handle_t loop, TickType_t timeout)
{
    assert(!timeout);
    if (loop->pending) {
        esp_mqtt_event_t event = loop->event; loop->pending = false;
        if (loop->handler && (loop->registered_id == ESP_EVENT_ANY_ID || loop->registered_id == loop->id))
            loop->handler(loop->argument, loop->base, loop->id, &event);
        if (mqtt_net_ui_thread) {
            if (loop->id == MQTT_EVENT_DATA) {
                LONG index = InterlockedCompareExchange(&mqtt_net_data_events, 0, 0);
                assert(index >= 0 && index < 128);
                mqtt_net_data_records[index] = (mqtt_net_data_record_t){event.current_data_offset, event.data_len,
                    event.total_data_len, event.topic_len, event.qos, event.retain, event.dup};
                printf("SDK_DATA offset=%d length=%d total=%d topic=%d qos=%d retain=%d dup=%d\n",
                       event.current_data_offset, event.data_len, event.total_data_len, event.topic_len,
                       event.qos, event.retain, event.dup);
                InterlockedIncrement(&mqtt_net_data_events);
            } else if (loop->id == MQTT_EVENT_PUBLISHED) InterlockedIncrement(&mqtt_net_published_events);
            else if (loop->id == MQTT_EVENT_SUBSCRIBED) InterlockedIncrement(&mqtt_net_subscribed_events);
        }
    }
    return ESP_OK;
}

typedef struct { int fd; } native_transport_t;
static int native_poll(esp_transport_handle_t t, int timeout_ms, bool reading)
{
    native_transport_t *state = esp_transport_get_context_data(t);
    assert(state->fd >= 0 && timeout_ms >= 0 && timeout_ms <= 10000);
    fd_set ready; FD_ZERO(&ready); FD_SET((SOCKET)state->fd, &ready);
    struct timeval timeout; esp_transport_utils_ms_to_timeval(timeout_ms, &timeout);
    int result = host_select(state->fd + 1, reading ? &ready : NULL, reading ? NULL : &ready, NULL, &timeout);
    if (result < 0) esp_transport_capture_errno(t, errno);
    return result;
}
static int native_poll_read(esp_transport_handle_t t, int timeout) { return native_poll(t, timeout, true); }
static int native_poll_write(esp_transport_handle_t t, int timeout) { return native_poll(t, timeout, false); }
static int native_connect(esp_transport_handle_t t, const char *host, int port, int timeout)
{
    assert(!mqtt_net_ui_thread || GetCurrentThreadId() != mqtt_net_ui_thread);
    native_transport_t *state = esp_transport_get_context_data(t);
    assert(state->fd < 0 && port > 0 && port <= 65535 && timeout > 0);
    if (strcmp(host, "127.0.0.1")) { errno = EINVAL; return -1; }
    state->fd = host_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (state->fd < 0 || host_fcntl(state->fd, 0, 0) < 0) return -1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((u_short)port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int result = host_connect(state->fd, (const struct sockaddr *)&address, sizeof(address));
    if (result == 0) return 0;
    if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
        result = native_poll_write(t, timeout);
        int error = 0; socklen_t length = sizeof(error);
        if (result > 0 && !host_getsockopt(state->fd, SOL_SOCKET, SO_ERROR, &error, &length) && !error) return 0;
        errno = error ? ECONNREFUSED : ETIMEDOUT;
    }
    esp_transport_capture_errno(t, errno); capture_tcp_transport_error(t, ERR_TCP_TRANSPORT_CONNECTION_FAILED);
    return -1;
}
static int native_read(esp_transport_handle_t t, char *buffer, int length, int timeout)
{
    int result = native_poll_read(t, timeout);
    if (result > 0) {
        native_transport_t *state = esp_transport_get_context_data(t);
        if (mqtt_net_read_cap && length > (int)mqtt_net_read_cap) length = (int)mqtt_net_read_cap;
        result = (int)host_recv(state->fd, buffer, (size_t)length, 0);
        if (result > 0) return result;
        if (result < 0) esp_transport_capture_errno(t, errno);
        result = result == 0 ? ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN : ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    } else result = result == 0 ? ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT : ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    capture_tcp_transport_error(t, result); return result;
}
static int native_write(esp_transport_handle_t t, const char *buffer, int length, int timeout)
{
    assert(!mqtt_net_ui_thread || GetCurrentThreadId() != mqtt_net_ui_thread);
    int result = native_poll_write(t, timeout);
    if (result <= 0) return -1;
    native_transport_t *state = esp_transport_get_context_data(t);
    result = (int)host_send(state->fd, buffer, (size_t)length, 0);
    if (result < 0) esp_transport_capture_errno(t, errno);
    return result;
}
static int native_close(esp_transport_handle_t t)
{
    native_transport_t *state = esp_transport_get_context_data(t);
    if (state->fd < 0) return 0;
    int result = host_close(state->fd); state->fd = -1; return result;
}
static int native_destroy(esp_transport_handle_t t)
{
    native_close(t); mqtt_net_free(esp_transport_get_context_data(t));
    esp_transport_destroy_foundation_transport(t->foundation);
    InterlockedDecrement(&mqtt_net_transports); return 0;
}
esp_transport_handle_t esp_transport_tcp_init(void)
{
    esp_transport_handle_t t = esp_transport_init(); assert(t);
    native_transport_t *state = mqtt_net_calloc(1, sizeof(*state)); assert(state); state->fd = -1;
    t->foundation = esp_transport_init_foundation_transport(); assert(t->foundation);
    esp_transport_set_context_data(t, state);
    esp_transport_set_func(t, native_connect, native_read, native_write, native_close,
                           native_poll_read, native_poll_write, native_destroy);
    InterlockedIncrement(&mqtt_net_transports); return t;
}
void esp_transport_tcp_set_interface_name(esp_transport_handle_t t, struct ifreq *name)
{
    (void)t; (void)name; assert(0);
}
