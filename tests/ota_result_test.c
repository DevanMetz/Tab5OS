/* Actual main.c OTA callbacks; SDK, UI, scheduling and signal access are controlled. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 0x102, ESP_ERR_NOT_FOUND = 0x105,
       ESP_ERR_NOT_SUPPORTED = 0x106, ESP_ERR_NVS_NOT_FOUND = 0x1102, ESP_ERR_NVS_INVALID_LENGTH = 0x110c };
typedef enum { ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID,
               ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED = -1 } esp_ota_img_states_t;
typedef unsigned nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef struct { unsigned id; } esp_partition_t;
typedef struct { char version[32]; } esp_app_desc_t;
typedef struct { unsigned id; } lv_timer_t;
typedef struct { unsigned id; } lv_event_t;
typedef struct { char text[96]; unsigned states; } lv_obj_t;
typedef struct { char version[32]; } ota_manifest_t;
typedef void *TaskHandle_t;
enum { pdTRUE = 1, pdPASS = 1, LV_STATE_DISABLED = 1 };
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define OTA_MANIFEST_URL "controlled manifest URL"

static esp_err_t nvs_init_error;
static char ota_last_result[64];
static char saved_result[64], saved_pending[32];
static bool has_result, has_pending, invalid_present;
static esp_app_desc_t running_description, invalid_description;
static const esp_partition_t running_partition = {1}, invalid_partition = {2};
static unsigned handles, readonly_opens, write_opens, sets, erases, commits, state_reads, warnings;
static esp_err_t open_error, pending_error, state_error, description_error, set_error, erase_error, commit_error;
static esp_ota_img_states_t running_state;
static unsigned state_read_fail_at;
static bool internal_ready, ota_health_window_elapsed;
static esp_err_t storage_init_error, validation_error;
static unsigned validation_calls, timer_deletes, info_logs, error_logs;
static lv_timer_t health_timer = {1};
static atomic_bool ota_busy, done_storage;
static bool ota_ok, wifi_connected;
static TaskHandle_t ota_task_handle;
static char ota_error[96];
static lv_obj_t status_object, button_object;
static lv_obj_t *ota_status = &status_object, *ota_button = &button_object;
static const char *blocker_text;
static unsigned task_token, create_calls, notify_calls, fetch_calls, check_calls, install_calls;
static unsigned delay_calls, restart_calls, wait_calls, activity_calls, screensaver_calls;
static int create_result;
static esp_err_t fetch_error, check_error, install_error;
static bool fetch_error_text, worker_active, publication_observed, publication_busy;
static bool click_at_publication, health_during_fetch, health_during_delay;
static unsigned health_writes_during_worker;
static unsigned health_timer_period, timer_period_changes;
static jmp_buf worker_exit;
static const esp_partition_t installed_partition = {3};
static esp_app_desc_t installed_description;
static bool boot_partition_missing;
static esp_err_t boot_description_error;
static const char *boot_version_override;
static unsigned boot_partition_reads, boot_description_reads;
static void ota_update_task(void *argument);
static void ota_clicked(lv_event_t *event);
static void confirm_running_ota(lv_timer_t *timer);
/* Observe the publication lvalue before its store; source bodies are unchanged. */
static atomic_bool *fixture_done_access(void)
{
    if (worker_active && !publication_observed) {
        publication_observed = true;
        publication_busy = ota_busy;
        if (click_at_publication) ota_clicked(NULL);
    }
    return &done_storage;
}
#define ota_done (*fixture_done_access())
static unsigned cases, failures;

static const char *esp_err_to_name(esp_err_t error) { return error == ESP_OK ? "ESP_OK" : "controlled error"; }
static void fixture_log(unsigned *counter, const char *format, ...)
{
    char message[128];
    va_list args;
    va_start(args, format);
    assert(vsnprintf(message, sizeof(message), format, args) > 0);
    va_end(args);
    (*counter)++;
}
#define ESP_LOGW(tag, ...) ((void)(tag), fixture_log(&warnings, __VA_ARGS__))
#define ESP_LOGI(tag, ...) ((void)(tag), fixture_log(&info_logs, __VA_ARGS__))
#define ESP_LOGE(tag, ...) ((void)(tag), fixture_log(&error_logs, __VA_ARGS__))

static esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "tab5") && !handles);
    if (mode == NVS_READONLY) readonly_opens++; else { assert(mode == NVS_READWRITE); write_opens++; }
    if (open_error) return open_error;
    *handle = 1;
    handles++;
    return ESP_OK;
}
static void nvs_close(nvs_handle_t handle) { assert(handle == 1 && handles == 1); handles--; }
static esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size)
{
    assert(handle == 1 && handles == 1);
    bool pending = !strcmp(key, "ota_pending");
    assert(pending || !strcmp(key, "ota_result"));
    if (pending && pending_error) return pending_error;
    if (!(pending ? has_pending : has_result)) return ESP_ERR_NVS_NOT_FOUND;
    const char *saved = pending ? saved_pending : saved_result;
    size_t needed = strlen(saved) + 1;
    if (*size < needed) { *size = needed; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(value, saved, needed);
    *size = needed;
    return ESP_OK;
}
static esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    assert(handle == 1 && handles == 1);
    sets++;
    if (set_error) return set_error;
    bool pending = !strcmp(key, "ota_pending");
    assert(pending || !strcmp(key, "ota_result"));
    assert(strlen(value) < (pending ? sizeof(saved_pending) : sizeof(saved_result)));
    strcpy(pending ? saved_pending : saved_result, value);
    if (pending) has_pending = true; else has_result = true;
    return ESP_OK;
}
static esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(handle == 1 && handles == 1 && !strcmp(key, "ota_pending"));
    erases++;
    if (erase_error) return erase_error;
    if (!has_pending) return ESP_ERR_NVS_NOT_FOUND;
    has_pending = false;
    saved_pending[0] = '\0';
    return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t handle)
{ assert(handle == 1 && handles == 1); commits++; return commit_error; }
static const esp_app_desc_t *esp_app_get_description(void) { return &running_description; }
static const esp_partition_t *esp_ota_get_running_partition(void) { return &running_partition; }
static esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *state)
{
    assert(partition == &running_partition);
    state_reads++;
    *state = running_state; /* A failed return must take precedence over this value. */
    if (state_reads == state_read_fail_at) return ESP_FAIL;
    return state_error;
}
static const esp_partition_t *esp_ota_get_last_invalid_partition(void)
{ return invalid_present ? &invalid_partition : NULL; }
static esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition, esp_app_desc_t *description)
{
    assert(partition == &invalid_partition || partition == &installed_partition);
    if (partition == &installed_partition) {
        boot_description_reads++;
        if (boot_description_error) return boot_description_error;
        *description = installed_description;
        if (boot_version_override) strcpy(description->version, boot_version_override);
        return ESP_OK;
    }
    if (description_error) return description_error;
    *description = invalid_description;
    return ESP_OK;
}
static esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    assert(running_state == ESP_OTA_IMG_PENDING_VERIFY && ota_health_window_elapsed && !ota_busy);
    validation_calls++;
    if (validation_error == ESP_OK) running_state = ESP_OTA_IMG_VALID;
    return validation_error;
}
static void lv_timer_delete(lv_timer_t *timer)
{
    assert(timer == &health_timer && !timer_deletes && !ota_busy);
    timer_deletes++;
}
static void lv_timer_set_period(lv_timer_t *timer, uint32_t period)
{
    assert(timer == &health_timer && !timer_deletes && period == 250);
    health_timer_period = period;
    timer_period_changes++;
}
const esp_partition_t *esp_ota_get_boot_partition(void)
{
    boot_partition_reads++;
    return boot_partition_missing ? NULL : &installed_partition;
}
static esp_err_t ota_manifest_fetch(const char *url, ota_manifest_t *manifest, char *error, size_t size)
{
    assert(!strcmp(url, OTA_MANIFEST_URL) && ota_busy && !done_storage);
    fetch_calls++;
    strcpy(manifest->version, installed_description.version);
    if (health_during_fetch) {
        unsigned writes_before = write_opens;
        confirm_running_ota(&health_timer);
        health_writes_during_worker += write_opens - writes_before;
    }
    if (fetch_error && fetch_error_text) snprintf(error, size, "Controlled manifest failure");
    return fetch_error;
}
static esp_err_t ota_manifest_check(const ota_manifest_t *manifest, const char *running, char *error, size_t size)
{
    (void)error; (void)size;
    assert(!strcmp(manifest->version, installed_description.version) && !strcmp(running, running_description.version));
    check_calls++;
    return check_error;
}
static esp_err_t ota_manifest_install(const ota_manifest_t *manifest, char *error, size_t size)
{
    (void)error; (void)size;
    assert(!strcmp(manifest->version, installed_description.version));
    install_calls++;
    return install_error;
}
static void vTaskDelay(unsigned ticks)
{
    assert(ticks == 2000 && ota_busy && ota_ok && done_storage);
    delay_calls++;
    if (health_during_delay) {
        unsigned writes_before = write_opens;
        confirm_running_ota(&health_timer);
        health_writes_during_worker += write_opens - writes_before;
    }
}
static void esp_restart(void) { restart_calls++; longjmp(worker_exit, 1); }
static unsigned ulTaskNotifyTake(unsigned clear, uint32_t ticks)
{
    assert(clear == pdTRUE && ticks == portMAX_DELAY);
    wait_calls++;
    longjmp(worker_exit, 1);
}
static const char *restart_blocker(void) { return blocker_text; }
static void lv_label_set_text(lv_obj_t *object, const char *text)
{
    assert(object && strlen(text) < sizeof(object->text));
    strcpy(object->text, text);
}
static void lv_obj_add_state(lv_obj_t *object, unsigned state) { assert(object); object->states |= state; }
static void lv_obj_remove_state(lv_obj_t *object, unsigned state) { assert(object); object->states &= ~state; }
static void lv_display_trigger_activity(void *display) { assert(!display); activity_calls++; }
static void screensaver_close(void) { screensaver_calls++; }
static void xTaskNotifyGive(TaskHandle_t handle) { assert(handle == &task_token); notify_calls++; }
static int xTaskCreate(void (*function)(void *), const char *name, unsigned stack,
                       void *argument, unsigned priority, TaskHandle_t *handle)
{
    assert(function == ota_update_task && !strcmp(name, "ota") && stack == 6144 && !argument && priority == 4);
    create_calls++;
    if (create_result == pdPASS) *handle = &task_token;
    return create_result;
}

#include "ota_result.inc"

static void reset(void)
{
    assert(!handles);
    readonly_opens = write_opens = sets = erases = commits = state_reads = warnings = 0;
    validation_calls = timer_deletes = info_logs = error_logs = 0;
    state_read_fail_at = 0;
    ota_busy = done_storage = ota_ok = worker_active = publication_observed = publication_busy = false;
    click_at_publication = health_during_fetch = health_during_delay = false;
    health_writes_during_worker = 0;
    health_timer_period = 30000; timer_period_changes = 0;
    boot_partition_missing = false; boot_description_error = ESP_OK; boot_version_override = NULL;
    boot_partition_reads = boot_description_reads = 0;
    ota_task_handle = NULL; ota_error[0] = '\0'; blocker_text = NULL;
    wifi_connected = true; create_result = pdPASS; fetch_error = check_error = install_error = ESP_OK;
    fetch_error_text = true;
    create_calls = notify_calls = fetch_calls = check_calls = install_calls = 0;
    delay_calls = restart_calls = wait_calls = activity_calls = screensaver_calls = 0;
    memset(&status_object, 0, sizeof(status_object)); memset(&button_object, 0, sizeof(button_object));
    ota_status = &status_object; ota_button = &button_object;
    strcpy(installed_description.version, "v0.8.0-87654321");
    internal_ready = true;
    ota_health_window_elapsed = false;
    storage_init_error = validation_error = ESP_OK;
    nvs_init_error = open_error = pending_error = state_error = description_error = set_error = erase_error = commit_error = ESP_OK;
    has_result = has_pending = true;
    invalid_present = false;
    running_state = ESP_OTA_IMG_VALID;
    strcpy(ota_last_result, "none recorded");
    strcpy(saved_result, "Previous result");
    strcpy(saved_pending, "v0.7.0-12345678");
    strcpy(running_description.version, saved_pending);
    strcpy(invalid_description.version, saved_pending);
}
static void check(const char *name, const char *message, bool retain_pending, unsigned writes, unsigned reads)
{
    bool pass = !strcmp(ota_last_result, message) && has_pending == retain_pending &&
                !handles && write_opens == writes && state_reads == reads;
    if (!writes) pass = pass && !sets && !erases && !commits && !strcmp(saved_result, "Previous result");
    else if (!set_error && !erase_error && !commit_error)
        pass = pass && sets == writes && erases == writes && commits == writes && !strcmp(saved_result, message);
    cases++;
    failures += !pass;
    printf("%s %s pending=%u writes=%u sets=%u erases=%u commits=%u state_reads=%u handles=%u status=\"%s\"\n",
           pass ? "PASS" : "FAIL", name, (unsigned)has_pending, write_opens, sets, erases, commits, state_reads, handles, ota_last_result);
}
static void check_health(const char *name, const char *message, bool retain_pending,
                         unsigned writes, unsigned reads, unsigned validations)
{
    assert(timer_deletes == 1 && ota_health_window_elapsed && validation_calls == validations);
    printf("HEALTH %s timer_deletes=%u window_elapsed=%u validations=%u warnings=%u errors=%u info=%u\n",
           name, timer_deletes, (unsigned)ota_health_window_elapsed, validation_calls, warnings, error_logs, info_logs);
    check(name, message, retain_pending, writes, reads);
}
static void run_worker(void)
{
    assert(ota_busy && ota_task_handle == &task_token);
    worker_active = true;
    if (!setjmp(worker_exit)) ota_update_task(NULL);
    worker_active = false;
}
static void worker_case(const char *name, bool pass)
{
    assert(!handles);
    cases++; failures += !pass;
    printf("%s %s busy=%u done=%u ok=%u publish_busy=%u notifications=%u fetch=%u check=%u install=%u delay=%u restart=%u wait=%u health_writes=%u pending=%u writes=%u handles=%u state_reads=%u validations=%u timer_deletes=%u timer_period=%u period_changes=%u button_disabled=%u boot_reads=%u boot_desc_reads=%u pending_version=\"%s\" result=\"%s\" status=\"%s\"\n",
           pass ? "PASS" : "FAIL", name, (unsigned)ota_busy, (unsigned)done_storage, (unsigned)ota_ok,
           (unsigned)publication_busy, notify_calls, fetch_calls, check_calls, install_calls, delay_calls,
           restart_calls, wait_calls, health_writes_during_worker, (unsigned)has_pending, write_opens, handles,
           state_reads, validation_calls, timer_deletes, health_timer_period, timer_period_changes,
           (unsigned)(ota_button && (ota_button->states & LV_STATE_DISABLED)),
           boot_partition_reads, boot_description_reads,
           saved_pending, ota_last_result, status_object.text);
}

int main(void)
{
    const char *installed = "Installed v0.7.0-12345678";
    const char *pending = "Installing v0.7.0-12345678; health pending";
    const char *unconfirmed = "Running v0.7.0-12345678; health not confirmed";
    const char *unavailable = "Running v0.7.0-12345678; OTA state unavailable";
    const char *state_names[] = {"new", "pending-verify", "valid", "invalid", "aborted"};
    for (unsigned state = 0; state < 5; state++) {
        reset(); running_state = (esp_ota_img_states_t)state; ota_load_result();
        check(state_names[state], state == 2 ? installed : state < 2 ? pending : unconfirmed, state != 2, state == 2, 1);
    }
    reset(); running_state = ESP_OTA_IMG_UNDEFINED; ota_load_result(); check("undefined", unconfirmed, true, 0, 1);
    reset(); running_state = (esp_ota_img_states_t)99; ota_load_result(); check("unknown-state", unconfirmed, true, 0, 1);
    const esp_err_t errors[] = {ESP_FAIL, ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED, ESP_ERR_INVALID_ARG};
    const char *error_names[] = {"state-read-error", "state-not-found", "state-not-supported", "state-invalid-arg"};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        reset(); state_error = errors[i]; ota_load_result(); check(error_names[i], unavailable, true, 0, 1);
    }
    reset(); strcpy(running_description.version, "v0.6.0"); invalid_present = true; ota_load_result();
    check("rolled-back", "Rolled back v0.7.0-12345678", false, 1, 0);
    reset(); strcpy(running_description.version, "v0.6.0"); ota_load_result();
    check("did-not-activate", "Update to v0.7.0-12345678 did not activate", false, 1, 0);
    reset(); strcpy(running_description.version, "v0.6.0"); invalid_present = true;
    strcpy(invalid_description.version, "v0.5.1"); ota_load_result();
    check("other-invalid-image", "Update to v0.7.0-12345678 did not activate", false, 1, 0);
    reset(); strcpy(running_description.version, "v0.6.0"); invalid_present = true; description_error = ESP_FAIL; ota_load_result();
    check("invalid-description-error", "Update to v0.7.0-12345678 did not activate", false, 1, 0);
    reset(); nvs_init_error = ESP_FAIL; ota_load_result(); check("nvs-unavailable", "none recorded", true, 0, 0);
    reset(); open_error = ESP_FAIL; ota_load_result(); check("nvs-open-error", "none recorded", true, 0, 0);
    reset(); pending_error = ESP_FAIL; ota_load_result(); check("pending-read-error", "Previous result", true, 0, 0);
    reset(); has_pending = false; ota_load_result(); check("pending-absent", "Previous result", false, 0, 0);
    reset(); saved_pending[0] = '\0'; ota_load_result(); check("pending-empty", "Previous result", true, 0, 0);
    reset(); has_result = false; ota_load_result(); check("previous-result-absent", installed, false, 1, 1);
    reset(); state_error = ESP_FAIL; ota_load_result();
    state_error = ESP_OK; running_state = ESP_OTA_IMG_NEW; ota_load_result();
    running_state = ESP_OTA_IMG_PENDING_VERIFY; ota_load_result();
    running_state = ESP_OTA_IMG_VALID; ota_load_result(); check("state-read-recovery", installed, false, 1, 4);
    reset(); set_error = ESP_FAIL; ota_load_result();
    assert(sets == 1 && !erases && !commits && warnings == 1); check("result-set-error", installed, true, 1, 1);
    reset(); erase_error = ESP_FAIL; ota_load_result();
    assert(sets == 1 && erases == 1 && !commits && warnings == 1); check("pending-erase-error", installed, true, 1, 1);
    reset(); commit_error = ESP_FAIL; ota_load_result();
    assert(sets == 1 && erases == 1 && commits == 1 && warnings == 1); check("result-commit-error", installed, false, 1, 1);
    reset(); ota_record_pending("v0.8.0-87654321");
    assert(has_pending && !strcmp(saved_pending, "v0.8.0-87654321") && sets == 1 && commits == 1 && !erases && !handles);
    cases++; printf("PASS record-pending handles=%u\n", handles);
    reset(); strcpy(saved_pending, "v0.7.0-123456789012345678901234"); strcpy(running_description.version, saved_pending);
    state_error = ESP_FAIL; ota_load_result();
    assert(strlen(ota_last_result) == 62);
    check("maximum-version-state-error", "Running v0.7.0-123456789012345678901234; OTA state unavailable", true, 0, 1);
    reset(); strcpy(saved_pending, "v0.7.0-123456789012345678901234"); strcpy(running_description.version, saved_pending);
    running_state = ESP_OTA_IMG_UNDEFINED; ota_load_result();
    check("maximum-version-unconfirmed", "Running v0.7.0-123456789012345678901234; health not confirmed", true, 0, 1);
    for (unsigned state = 0; state < 2; state++) {
        reset(); strcpy(saved_pending, "v0.7.0-123456789012345678901234"); strcpy(running_description.version, saved_pending);
        assert(strlen(saved_pending) == 31);
        running_state = (esp_ota_img_states_t)state; ota_load_result();
        check(state ? "maximum-version-pending-verify" : "maximum-version-new",
              "Installing v0.7.0-123456789012345678901234; health pending", true, 0, 1);
    }
    reset(); state_error = ESP_FAIL; ota_load_result(); state_error = ESP_OK;
    confirm_running_ota(&health_timer);
    check_health("health-state-read-recovery-valid", installed, false, 1, 3, 0);
    const esp_ota_img_states_t before_valid[] = {ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_UNDEFINED};
    const char *recovery_names[] = {"health-new-recovery-valid", "health-pending-recovery-valid", "health-unconfirmed-recovery-valid"};
    for (unsigned i = 0; i < sizeof(before_valid) / sizeof(before_valid[0]); i++) {
        reset(); running_state = before_valid[i]; ota_load_result(); running_state = ESP_OTA_IMG_VALID;
        if (i == 2) { internal_ready = false; storage_init_error = ESP_FAIL; }
        confirm_running_ota(&health_timer);
        check_health(recovery_names[i], installed, false, 1, 3, 0);
    }
    reset(); running_state = ESP_OTA_IMG_PENDING_VERIFY; ota_load_result();
    confirm_running_ota(&health_timer);
    assert(info_logs == 1 && !error_logs);
    check_health("health-validates-pending", installed, false, 1, 2, 1);
    reset(); running_state = ESP_OTA_IMG_PENDING_VERIFY; state_error = ESP_FAIL; ota_load_result();
    state_error = ESP_OK; confirm_running_ota(&health_timer);
    check_health("health-state-read-recovery-pending", installed, false, 1, 2, 1);
    reset(); running_state = ESP_OTA_IMG_PENDING_VERIFY; ota_load_result(); validation_error = ESP_FAIL;
    confirm_running_ota(&health_timer);
    assert(error_logs == 1 && !info_logs);
    check_health("health-validation-error", pending, true, 0, 2, 1);
    reset(); running_state = ESP_OTA_IMG_PENDING_VERIFY; ota_load_result(); nvs_init_error = ESP_FAIL;
    confirm_running_ota(&health_timer);
    assert(error_logs == 1 && !info_logs);
    check_health("health-nvs-unavailable", pending, true, 0, 2, 0);
    reset(); running_state = ESP_OTA_IMG_PENDING_VERIFY; ota_load_result(); internal_ready = false;
    storage_init_error = ESP_FAIL; confirm_running_ota(&health_timer);
    assert(error_logs == 1 && !info_logs);
    check_health("health-storage-unavailable", pending, true, 0, 2, 0);
    reset(); running_state = ESP_OTA_IMG_PENDING_VERIFY; ota_load_result(); state_error = ESP_FAIL;
    confirm_running_ota(&health_timer);
    check_health("health-state-read-error", pending, true, 0, 2, 0);
    for (unsigned i = 0; i < sizeof(before_valid) / sizeof(before_valid[0]); i++) {
        if (before_valid[i] == ESP_OTA_IMG_PENDING_VERIFY) continue;
        reset(); running_state = before_valid[i]; ota_load_result(); confirm_running_ota(&health_timer);
        check_health(i ? "health-unconfirmed" : "health-new", i ? unconfirmed : pending, true, 0, 2, 0);
    }
    reset(); ota_load_result(); confirm_running_ota(&health_timer);
    check_health("health-already-recorded", installed, false, 1, 2, 0);
    reset(); running_state = ESP_OTA_IMG_PENDING_VERIFY; ota_load_result(); set_error = ESP_FAIL;
    confirm_running_ota(&health_timer);
    assert(sets == 1 && !erases && !commits && warnings == 1 && info_logs == 1);
    check_health("health-result-set-error", installed, true, 1, 2, 1);
    reset(); running_state = ESP_OTA_IMG_PENDING_VERIFY; ota_load_result(); erase_error = ESP_FAIL;
    confirm_running_ota(&health_timer);
    assert(sets == 1 && erases == 1 && !commits && warnings == 1 && info_logs == 1);
    check_health("health-pending-erase-error", installed, true, 1, 2, 1);
    reset(); running_state = ESP_OTA_IMG_PENDING_VERIFY; ota_load_result(); commit_error = ESP_FAIL;
    confirm_running_ota(&health_timer);
    assert(sets == 1 && erases == 1 && commits == 1 && warnings == 1 && info_logs == 1);
    check_health("health-result-commit-error", installed, false, 1, 2, 1);
    reset(); state_error = ESP_FAIL; ota_load_result(); state_error = ESP_OK; state_read_fail_at = 3;
    confirm_running_ota(&health_timer);
    assert(warnings == 2);
    check_health("health-valid-recheck-error", unavailable, true, 0, 3, 0);
    reset(); state_error = ESP_FAIL; ota_load_result(); state_error = ESP_OK; open_error = ESP_FAIL;
    confirm_running_ota(&health_timer);
    assert(readonly_opens == 2);
    check_health("health-valid-nvs-open-error", unavailable, true, 0, 2, 0);
    reset(); state_error = ESP_FAIL; ota_load_result(); state_error = ESP_OK; nvs_init_error = ESP_FAIL;
    confirm_running_ota(&health_timer);
    assert(readonly_opens == 1);
    check_health("health-valid-nvs-unavailable", unavailable, true, 0, 2, 0);
    reset(); strcpy(saved_pending, "v0.7.0-123456789012345678901234"); strcpy(running_description.version, saved_pending);
    running_state = ESP_OTA_IMG_PENDING_VERIFY; ota_load_result(); confirm_running_ota(&health_timer);
    check_health("health-maximum-version", "Installed v0.7.0-123456789012345678901234", false, 1, 2, 1);
    const char *worker_error_names[] = {"worker-fetch-failure", "worker-check-failure", "worker-install-failure", "worker-fallback-error"};
    for (unsigned i = 0; i < 4; i++) {
        reset();
        if (i == 1) check_error = ESP_FAIL; else if (i == 2) install_error = ESP_FAIL; else fetch_error = ESP_FAIL;
        if (i == 3) fetch_error_text = false;
        ota_clicked(NULL); run_worker();
        worker_case(worker_error_names[i], publication_busy && !ota_busy && done_storage && !ota_ok &&
            fetch_calls == 1 && check_calls == (unsigned)(i == 1 || i == 2) && install_calls == (unsigned)(i == 2) &&
            wait_calls == 1 && !delay_calls && !restart_calls && !write_opens &&
            !strcmp(ota_error, i == 0 ? "Controlled manifest failure" : "Update failed: controlled error"));
    }
    reset(); fetch_error = ESP_FAIL; click_at_publication = true;
    ota_clicked(NULL); run_worker();
    worker_case("worker-publication-click", publication_busy && !ota_busy && done_storage && !ota_ok &&
        create_calls == 1 && !notify_calls && activity_calls == 1 && wait_calls == 1);
    const char *success_names[] = {"worker-success", "worker-success-health-window", "worker-success-offscreen-health"};
    for (unsigned i = 0; i < 3; i++) {
        reset(); has_pending = false; saved_pending[0] = '\0'; strcpy(ota_last_result, saved_result);
        health_during_delay = i != 0;
        ota_clicked(NULL);
        if (i == 2) { ota_status = NULL; ota_button = NULL; }
        run_worker();
        worker_case(success_names[i], publication_busy && ota_busy && done_storage && ota_ok &&
            fetch_calls == 1 && check_calls == 1 && install_calls == 1 && delay_calls == 1 && restart_calls == 1 && !wait_calls &&
            !health_writes_during_worker && has_pending && !strcmp(saved_pending, installed_description.version) &&
            !strcmp(ota_last_result, "Previous result") && write_opens == 1 && sets == 1 && !erases && commits == 1 &&
            !timer_deletes && !validation_calls && timer_period_changes == (unsigned)(i != 0) &&
            health_timer_period == (i ? 250U : 30000U));
    }
    reset(); ota_busy = done_storage = true; strcpy(ota_error, "Controlled manifest failure");
    strcpy(status_object.text, "Waiting for updater"); button_object.states = LV_STATE_DISABLED;
    ota_tick(NULL);
    worker_case("worker-tick-before-release", ota_busy && done_storage && !strcmp(status_object.text, "Waiting for updater") &&
        button_object.states == LV_STATE_DISABLED && !activity_calls && !state_reads);
    reset(); fetch_error = ESP_FAIL; ota_clicked(NULL); run_worker(); ota_tick(NULL);
    worker_case("worker-tick-failure", !ota_busy && !done_storage && !ota_ok &&
        !strcmp(status_object.text, "Controlled manifest failure") && !button_object.states && activity_calls == 2 && !state_reads);
    reset(); state_error = ESP_FAIL; ota_load_result(); state_error = ESP_OK;
    fetch_error = ESP_FAIL; health_during_fetch = true; ota_clicked(NULL); run_worker();
    bool held_metadata = !health_writes_during_worker && has_pending && !write_opens && !strcmp(ota_last_result, unavailable);
    ota_tick(NULL);
    confirm_running_ota(&health_timer);
    worker_case("worker-failure-after-health-window", held_metadata && !ota_busy && !done_storage && !has_pending &&
        !strcmp(ota_last_result, installed) && write_opens == 1 && state_reads == 3 && !validation_calls &&
        timer_deletes == 1 && timer_period_changes == 1 && health_timer_period == 250 &&
        !strcmp(status_object.text, "Controlled manifest failure"));
    reset(); state_error = ESP_FAIL; ota_load_result(); state_error = ESP_OK;
    fetch_error = ESP_FAIL; health_during_fetch = true; ota_clicked(NULL); ota_status = NULL; ota_button = NULL; run_worker();
    held_metadata = !health_writes_during_worker && has_pending && !write_opens && !timer_deletes;
    confirm_running_ota(&health_timer);
    worker_case("worker-failure-offscreen-health", held_metadata && !ota_busy && done_storage && !has_pending &&
        !strcmp(ota_last_result, installed) && write_opens == 1 && state_reads == 3 && !validation_calls &&
        timer_deletes == 1 && timer_period_changes == 1 && health_timer_period == 250 && !ota_status);
    reset(); running_state = ESP_OTA_IMG_PENDING_VERIFY; ota_load_result();
    fetch_error = ESP_FAIL; health_during_fetch = true; ota_clicked(NULL); run_worker();
    held_metadata = !health_writes_during_worker && !validation_calls && has_pending && !write_opens && !timer_deletes;
    confirm_running_ota(&health_timer);
    worker_case("worker-pending-health-after-failure", held_metadata && !ota_busy && done_storage && !has_pending &&
        !strcmp(ota_last_result, installed) && write_opens == 1 && state_reads == 2 && validation_calls == 1 &&
        timer_deletes == 1 && timer_period_changes == 1 && health_timer_period == 250);
    reset(); fetch_error = ESP_FAIL; ota_clicked(NULL); ota_status = NULL; ota_button = NULL; run_worker(); ota_tick(NULL);
    bool held_completion = !ota_busy && done_storage && activity_calls == 1 && !state_reads;
    ota_status = &status_object; ota_button = &button_object; ota_tick(NULL);
    worker_case("worker-offscreen-failure", held_completion && !ota_busy && !done_storage && !button_object.states &&
        !strcmp(status_object.text, "Controlled manifest failure") && activity_calls == 2 && !state_reads);
    reset(); create_result = 0; ota_clicked(NULL);
    worker_case("worker-task-create-error", !ota_busy && !done_storage && !ota_task_handle && create_calls == 1 &&
        !fetch_calls && !notify_calls && !button_object.states && !strcmp(status_object.text, "Could not start updater"));
    reset(); fetch_error = ESP_FAIL;
    bool retries_pass = true;
    for (unsigned i = 0; i < 25; i++) {
        publication_observed = publication_busy = false;
        ota_clicked(NULL); run_worker();
        retries_pass = retries_pass && publication_busy && !ota_busy && done_storage && !ota_ok;
        ota_tick(NULL);
        retries_pass = retries_pass && !done_storage && !button_object.states && ota_task_handle == &task_token && !handles;
    }
    worker_case("worker-retained-retry", retries_pass && create_calls == 1 && notify_calls == 24 &&
        fetch_calls == 25 && wait_calls == 25 && !check_calls && !install_calls && !write_opens && activity_calls == 50);
    reset(); ota_busy = true; ota_clicked(NULL);
    worker_case("worker-busy-click", ota_busy && !done_storage && !create_calls && !notify_calls && !activity_calls);
    reset(); ota_busy = true; validate_running_ota();
    worker_case("worker-busy-validation", ota_busy && !state_reads && !write_opens && !readonly_opens && !validation_calls);
    reset(); wifi_connected = false; ota_clicked(NULL);
    worker_case("worker-disconnected-click", !ota_busy && !done_storage && !create_calls && !notify_calls && !activity_calls &&
        !strcmp(status_object.text, "Connect to Wi-Fi first"));
    reset(); blocker_text = "Controlled active session"; ota_clicked(NULL);
    worker_case("worker-restart-blocker", !ota_busy && !done_storage && !create_calls && !notify_calls && !activity_calls &&
        !strcmp(status_object.text, blocker_text));
    const char *metadata_names[] = {"worker-missing-boot-metadata", "worker-description-read-error",
        "worker-description-drift", "worker-missing-boot-empty-store"};
    for (unsigned i = 0; i < 4; i++) {
        reset();
        if (i == 1) boot_description_error = ESP_FAIL;
        else if (i == 2) boot_version_override = "v0.8.0-stale";
        else boot_partition_missing = true;
        if (i == 3) { has_pending = false; saved_pending[0] = '\0'; }
        ota_clicked(NULL); run_worker();
        worker_case(metadata_names[i], ota_busy && done_storage && ota_ok && publication_busy &&
            has_pending && !strcmp(saved_pending, installed_description.version) && write_opens == 1 && sets == 1 && commits == 1 &&
            !erases && !warnings && delay_calls == 1 && restart_calls == 1 && !wait_calls);
    }
    reset(); strcpy(installed_description.version, "v0.8.0-123456789012345678901234");
    ota_clicked(NULL); run_worker();
    worker_case("worker-maximum-manifest-version", ota_busy && done_storage && ota_ok && strlen(saved_pending) == 31 &&
        !strcmp(saved_pending, installed_description.version) && write_opens == 1 && sets == 1 && commits == 1 && !erases &&
        delay_calls == 1 && restart_calls == 1);
    const char *pending_error_names[] = {"worker-pending-nvs-unavailable", "worker-pending-open-error",
        "worker-pending-set-error", "worker-pending-commit-error"};
    for (unsigned i = 0; i < 4; i++) {
        reset();
        if (i == 0) nvs_init_error = ESP_FAIL;
        else if (i == 1) open_error = ESP_FAIL;
        else if (i == 2) set_error = ESP_FAIL;
        else commit_error = ESP_FAIL;
        ota_clicked(NULL); run_worker();
        worker_case(pending_error_names[i], ota_busy && done_storage && ota_ok && publication_busy &&
            has_pending && !strcmp(saved_pending, i == 3 ? installed_description.version : "v0.7.0-12345678") &&
            write_opens == (unsigned)(i != 0) && sets == (unsigned)(i >= 2) && commits == (unsigned)(i == 3) &&
            !erases && warnings == (unsigned)(i != 0) && delay_calls == 1 && restart_calls == 1 && !wait_calls);
    }
    reset(); ota_clicked(NULL); run_worker();
    ota_busy = done_storage = false;
    strcpy(running_description.version, installed_description.version); running_state = ESP_OTA_IMG_PENDING_VERIFY;
    ota_load_result(); confirm_running_ota(&health_timer);
    worker_case("worker-pending-reboot-reconciliation", !ota_busy && !done_storage && !has_pending &&
        !strcmp(ota_last_result, "Installed v0.8.0-87654321") && !strcmp(saved_result, ota_last_result) &&
        write_opens == 2 && sets == 2 && erases == 1 && commits == 2 && state_reads == 2 && validation_calls == 1 && timer_deletes == 1);
    printf("%s %u OTA result cases failures=%u handles=%u (controlled NVS/state APIs)\n",
           failures ? "FAIL" : "PASS", cases, failures, handles);
    return failures ? 1 : 0;
}
