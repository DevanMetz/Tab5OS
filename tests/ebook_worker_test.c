/* Actual worker/guards, native C atomics; controlled clock, task and I/O boundaries. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdatomic.h>

typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef struct { int unused; } lv_timer_t;
enum { pdPASS = 1, pdFAIL = 0, MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2 };
static unsigned tick_rate;
#define pdMS_TO_TICKS(ms) ((TickType_t)((uint64_t)(ms) * tick_rate / 1000))
#include "ebook_worker_types.inc"
#include "ebook_worker_config.inc"
#define ebook_download_busy busy_storage
#define ebook_download_done done_storage
#include "ebook_worker_signals.inc"
#undef ebook_download_busy
#undef ebook_download_done
#if EBOOK_SIGNALS_ATOMIC
typedef atomic_bool signal_t;
#else
typedef volatile bool signal_t;
#endif

static bool sd_ready, wifi_connected;
static bool ride_recording, voice_recording, voice_mic_open, scope_sampling, servo_running, weather_busy;
static bool chat_busy, browser_busy, wifi_scan_busy, alarm_active;
static bool govee_enabled, ring_enabled, ring_connecting, ring_connected, ring_stopping, ring_hr_active;
static bool ring_sync_active, ring_sync_pending, kickr_enabled, kickr_connecting, kickr_connected, kickr_stopping;
static void *ride_file, *i2c_capture_file, *ring_hr_samples;
static int govee_lock, ring_lock, kickr_lock;
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static bool i2c_register_busy(void) { return false; }
static bool signal_tool_busy(void) { return false; }
static bool spi_tool_busy(void) { return false; }
static bool uart_tool_busy(void) { return false; }
static bool ender3_tool_busy(void) { return false; }
static bool network_tool_busy(void) { return false; }
static bool http_tool_busy(void) { return false; }
static bool mqtt_tool_busy(void) { return false; }
static bool modbus_tool_busy(void) { return false; }
static bool ntp_tool_busy(void) { return false; }
static bool wol_tool_busy(void) { return false; }
static bool udp_tool_busy(void) { return false; }
static bool ble_tool_busy(void) { return false; }
static unsigned uxQueueMessagesWaiting(void *queue) { (void)queue; return 0; }

typedef struct { void *stack, *tcb; TaskFunction_t entry; bool deleted; } task_t;
static task_t tasks[4], *current_task;
static unsigned task_count, allocations, creates, caps_deletes, plain_deletes, downloads, attempted, probes;
static unsigned delays, renders, cases, failures, admission_accesses;
static bool worker_active, inside_timer, admission_active, nested_admission, race_at_claim, run_inline, create_fail;
static bool publication_observed, publication_busy, publication_handle_clear, release_checked, premature_render, cutoff, wrong_download;
static unsigned installed_mask, failed_books, lose_sd_after_book, lose_wifi_after_book;
static uint32_t elapsed_ms, wifi_at_ms, sd_loss_ms;
static TickType_t clock_tick;
static jmp_buf worker_exit;
static void ebook_download_task(void *argument);
static void ebook_download_tick(lv_timer_t *timer);
static void ebook_start_default_downloads(void);
static const char *restart_blocker(void);

static bool handle_clear(void)
{
#if EBOOK_LEGACY_HANDLE
    return ebook_download_task_handle == NULL;
#else
    return true;
#endif
}
static signal_t *busy_access(void)
{
    if (admission_active && !nested_admission) {
        admission_accesses++;
        /* Both versions reach this access immediately before claiming ownership. */
        if (race_at_claim && admission_accesses == 2) {
            race_at_claim = false; nested_admission = true;
            ebook_start_default_downloads(); nested_admission = false;
        }
    }
    if (worker_active && done_storage && !inside_timer && !release_checked) {
        release_checked = true; inside_timer = true;
        ebook_download_tick(NULL); inside_timer = false;
    }
    return &busy_storage;
}
static signal_t *done_access(void)
{
    if (worker_active && !inside_timer && !publication_observed) {
        publication_observed = true; publication_busy = busy_storage;
        publication_handle_clear = handle_clear();
    }
    return &done_storage;
}
#define ebook_download_busy (*busy_access())
#define ebook_download_done (*done_access())
static void show_ebooks(void) { renders++; premature_render |= busy_storage; }
static unsigned book_index(const ebook_default_t *book)
{
    for (unsigned i = 0; i < sizeof(ebook_defaults) / sizeof(ebook_defaults[0]); i++) if (book == &ebook_defaults[i]) return i;
    assert(false); return 0;
}
static bool ebook_default_installed(const ebook_default_t *book)
{ probes++; return (installed_mask & (1u << book_index(book))) != 0; }
static bool ebook_download_default(const ebook_default_t *book)
{
    unsigned index = book_index(book); downloads++; attempted |= 1u << index;
    wrong_download |= !sd_ready || !wifi_connected;
    if (downloads == lose_sd_after_book) sd_ready = false;
    if (downloads == lose_wifi_after_book) wifi_connected = false;
    return (failed_books & (1u << index)) == 0;
}
TickType_t xTaskGetTickCount(void) { return clock_tick; }
void vTaskDelay(TickType_t delay)
{
    assert(worker_active && delay == pdMS_TO_TICKS(500)); delays++;
    clock_tick += delay; elapsed_ms += (uint32_t)((uint64_t)delay * 1000 / tick_rate);
    if (elapsed_ms >= wifi_at_ms) wifi_connected = true;
    if (elapsed_ms >= sd_loss_ms) sd_ready = false;
    if (elapsed_ms > 30000 && !wifi_connected) {
        /* Bound the unmodified infinite baseline without pretending it finished. */
        cutoff = true; longjmp(worker_exit, 2);
    }
}
static void execute(task_t *task)
{
    assert(task && task->entry == ebook_download_task && task->stack && task->tcb && !worker_active);
    current_task = task; worker_active = true;
    if (setjmp(worker_exit) == 0) { task->entry(NULL); assert(false); }
    worker_active = false; current_task = NULL;
}
int xTaskCreateWithCaps(TaskFunction_t entry, const char *name, unsigned depth, void *argument, unsigned priority, TaskHandle_t *handle, unsigned caps)
{
    assert(entry == ebook_download_task && !strcmp(name, "ebooks") && depth == 10240 && !argument && priority == 4 && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    creates++; if (create_fail) return pdFAIL;
    assert(task_count < 4); task_t *task = &tasks[task_count++];
    task->entry = entry; task->stack = malloc(depth); task->tcb = malloc(64); assert(task->stack && task->tcb); allocations += 2;
    if (run_inline) execute(task);
    /* The real SDK can schedule the new task before writing its output handle. */
    if (handle) *handle = task;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t handle)
{
    assert(worker_active && current_task && !handle); plain_deletes++; current_task->deleted = true;
    /* WithCaps uses static kernel buffers; the ordinary delete does not own them. */
    longjmp(worker_exit, 1);
}
void vTaskDeleteWithCaps(TaskHandle_t handle)
{
    assert(worker_active && current_task && !handle && current_task->stack && current_task->tcb);
    caps_deletes++; current_task->deleted = true;
    free(current_task->stack); free(current_task->tcb); current_task->stack = current_task->tcb = NULL; allocations -= 2;
    longjmp(worker_exit, 1);
}
#include "ebook_worker.inc"
#undef ebook_download_busy
#undef ebook_download_done

static void prepare(unsigned rate)
{
    assert(!worker_active);
    for (unsigned i = 0; i < task_count; i++) { free(tasks[i].stack); free(tasks[i].tcb); }
    memset(tasks, 0, sizeof(tasks)); task_count = allocations = creates = caps_deletes = plain_deletes = downloads = attempted = probes = 0;
    delays = renders = admission_accesses = 0; tick_rate = rate; clock_tick = 0; elapsed_ms = 0;
    busy_storage = done_storage = false;
#if EBOOK_LEGACY_HANDLE
    ebook_download_task_handle = NULL;
#endif
    sd_ready = wifi_connected = true;
    installed_mask = failed_books = lose_sd_after_book = lose_wifi_after_book = 0;
    wifi_at_ms = sd_loss_ms = UINT32_MAX;
    inside_timer = admission_active = nested_admission = race_at_claim = run_inline = create_fail = false;
    publication_observed = publication_busy = publication_handle_clear = release_checked = premature_render = cutoff = wrong_download = false;
    ride_recording = voice_recording = voice_mic_open = scope_sampling = servo_running = weather_busy = false;
    chat_busy = browser_busy = wifi_scan_busy = alarm_active = false;
    govee_enabled = ring_enabled = ring_connecting = ring_connected = ring_stopping = ring_hr_active = false;
    ring_sync_active = ring_sync_pending = kickr_enabled = kickr_connecting = kickr_connected = kickr_stopping = false;
    ride_file = i2c_capture_file = ring_hr_samples = NULL;
}
static void start(void)
{ admission_active = true; ebook_start_default_downloads(); admission_active = false; }
static bool finished(void)
{
    return !cutoff && !wrong_download && !busy_storage && done_storage && handle_clear() && !allocations &&
           caps_deletes == 1 && !plain_deletes && publication_observed && publication_busy && publication_handle_clear && !premature_render && !restart_blocker();
}
static void report(const char *name, bool pass)
{
    cases++; failures += !pass;
    printf("%s %s hz=%u elapsed_ms=%u creates=%u downloads=%u mask=%u delays=%u busy=%u done=%u caps_deletes=%u plain_deletes=%u controlled_allocations=%u published_busy=%u handle_clear=%u renders=%u cutoff=%u\n",
           pass ? "PASS" : "FAIL", name, tick_rate, elapsed_ms, creates, downloads, attempted, delays,
           (unsigned)(bool)busy_storage, (unsigned)(bool)done_storage, caps_deletes, plain_deletes, allocations,
           (unsigned)publication_busy, (unsigned)handle_clear(), renders, (unsigned)cutoff);
}
static void admission_cases(void)
{
    prepare(100); sd_ready = false; start(); report("no-sd-admission", !creates && !probes && !busy_storage && !done_storage && !allocations);
    prepare(100); installed_mask = 7; start(); report("installed-admission", !creates && probes == 3 && !busy_storage && !done_storage && !allocations);
    prepare(100); busy_storage = true; start(); report("busy-admission", !creates && !probes && busy_storage && !done_storage && !allocations);
    prepare(100); create_fail = true; start();
    report("creation-failed", creates == 1 && !task_count && !busy_storage && done_storage && handle_clear() && !allocations && !restart_blocker());
    prepare(100); start(); start(); bool pass = creates == 1 && probes == 1 && busy_storage && task_count == 1;
    execute(&tasks[0]); report("duplicate-admission", pass && finished() && downloads == 3 && attempted == 7);
    prepare(100); race_at_claim = true; start(); pass = creates == 1 && task_count == 1 && busy_storage;
    execute(&tasks[0]); report("interleaved-admission", pass && finished() && downloads == 3);
    prepare(100); run_inline = true; start(); report("worker-before-create-return", creates == 1 && finished() && downloads == 3);
}
static void worker_case(const char *label, unsigned rate, unsigned scenario)
{
    prepare(rate); start(); assert(task_count == 1 && busy_storage && restart_blocker());
    unsigned expected_downloads = 3, expected_delay = 0;
    switch (scenario) {
    case 0: break;
    case 1: wifi_connected = false; wifi_at_ms = 500; expected_delay = 500; break;
    case 2: wifi_connected = false; wifi_at_ms = 29500; expected_delay = 29500; break;
    case 3: wifi_connected = false; wifi_at_ms = 30000; expected_delay = 30000; break;
    case 4: wifi_connected = false; expected_delay = 30000; expected_downloads = 0; break;
    case 5: wifi_connected = false; wifi_at_ms = 30500; expected_delay = 30000; expected_downloads = 0; break;
    case 6: sd_ready = false; wifi_connected = false; expected_downloads = 0; break;
    case 7: wifi_connected = false; sd_loss_ms = 500; expected_delay = 500; expected_downloads = 0; break;
    case 8: lose_sd_after_book = 1; expected_downloads = 1; break;
    case 9: lose_sd_after_book = 2; expected_downloads = 2; break;
    case 10: lose_wifi_after_book = 1; expected_downloads = 1; break;
    case 11: lose_wifi_after_book = 2; expected_downloads = 2; break;
    case 12: failed_books = 1; break;
    case 13: clock_tick = UINT32_MAX - 200; wifi_connected = false; expected_delay = 30000; expected_downloads = 0; break;
    case 14: clock_tick = UINT32_MAX - 200; wifi_connected = false; wifi_at_ms = 29500; expected_delay = 29500; break;
    default: assert(false);
    }
    execute(&tasks[0]); char name[80]; snprintf(name, sizeof(name), "%s-%uHz", label, rate);
    report(name, finished() && creates == 1 && downloads == expected_downloads && attempted == ((1u << expected_downloads) - 1) &&
           elapsed_ms == expected_delay && delays == expected_delay / 500);
}
static void timer_tick(void) { inside_timer = true; ebook_download_tick(NULL); inside_timer = false; }
static void timer_cases(void)
{
    prepare(100); timer_tick(); report("timer-idle", !renders && !done_storage && !busy_storage);
    prepare(100); busy_storage = true; done_storage = true; timer_tick();
    report("timer-before-release", !renders && done_storage && busy_storage && !premature_render);
    prepare(100); done_storage = true; timer_tick(); report("timer-after-release", renders == 1 && !done_storage && !busy_storage && !premature_render);
    timer_tick(); report("timer-consumes-once", renders == 1 && !done_storage && !premature_render);
}
static void cycle_case(void)
{
    bool pass = true;
    for (unsigned i = 0; i < 25; i++) {
        prepare(100); start(); wifi_connected = false; execute(&tasks[0]);
        pass = pass && finished() && !downloads && elapsed_ms == 30000;
        timer_tick(); pass = pass && renders == 1 && !done_storage && !busy_storage && !allocations;
        prepare(1000); run_inline = true; start();
        pass = pass && finished() && downloads == 3 && !elapsed_ms;
        timer_tick(); pass = pass && renders == 1 && !done_storage && !busy_storage && !allocations && !restart_blocker();
    }
    report("timeout-retry-consume-25", pass);
}
int main(void)
{
    admission_cases();
    const char *labels[] = {"online", "wifi-arrives", "wifi-before-deadline", "wifi-at-deadline", "wifi-timeout", "wifi-after-deadline",
                            "sd-missing-before-worker", "sd-lost-during-wait", "sd-lost-after-first", "sd-lost-after-second",
                            "wifi-lost-after-first", "wifi-lost-after-second", "isolated-book-failure", "clock-wrap-timeout", "clock-wrap-wifi"};
    for (unsigned rate = 100; rate <= 1000; rate *= 10) for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) worker_case(labels[i], rate, i);
    timer_cases(); cycle_case(); prepare(100);
    printf("%s %u ebook worker cases failures=%u controlled_allocations=%u (controlled tasks/clock/Wi-Fi/SD/timer/downloads; no SDK scheduler)\n",
           failures ? "FAIL" : "PASS", cases, failures, allocations);
    return failures ? 1 : 0;
}
