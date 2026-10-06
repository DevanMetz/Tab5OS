/* Actual native CSV files; an absent-profile NVS boundary, not a flash engine. */
#include "app_services.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "mqtt_tool.h"
#include "nvs.h"
#include <direct.h>
#include <errno.h>
#include <io.h>

volatile LONG mqtt_app_open_files, mqtt_app_closed_files;
volatile LONG mqtt_app_append_attempts, mqtt_app_file_waiting, mqtt_app_file_faults;
volatile LONG mqtt_app_storage_reports, mqtt_app_report_waiting;
static char directory[1024], csv_path[1100];
static const char *test_mode;
static FILE *append_file;
static HANDLE file_release, report_release;
enum boundary { NO_BOUNDARY, OPEN_BOUNDARY, FLUSH_BOUNDARY, SYNC_BOUNDARY, CLOSE_BOUNDARY };
static enum boundary fault_boundary, hold_boundary;
void mqtt_app_storage_setup(const char *output, const char *mode)
{
    test_mode = mode;
    if (!strcmp(mode, "app-log-open-error")) fault_boundary = OPEN_BOUNDARY;
    else if (!strcmp(mode, "app-log-flush-error")) fault_boundary = FLUSH_BOUNDARY;
    else if (!strcmp(mode, "app-log-close-error")) fault_boundary = CLOSE_BOUNDARY;
    else if (!strcmp(mode, "app-log-sync-error") || !strcmp(mode, "app-log-active-error") ||
             !strcmp(mode, "app-log-home-error")) fault_boundary = SYNC_BOUNDARY;
    if (!strcmp(mode, "app-log-sync-home") || !strcmp(mode, "app-log-overflow") ||
        !strcmp(mode, "app-log-home-error")) hold_boundary = SYNC_BOUNDARY;
    else if (!strcmp(mode, "app-log-close-home")) hold_boundary = CLOSE_BOUNDARY;
    if (hold_boundary) { file_release = CreateEvent(NULL, TRUE, FALSE, NULL); assert(file_release); }
    if (!strcmp(mode, "app-log-active-error") || !strcmp(mode, "app-log-home-error")) {
        report_release = CreateEvent(NULL, TRUE, FALSE, NULL); assert(report_release);
    }
    assert(snprintf(directory, sizeof(directory), "%s/storage-%s", output, mode) < (int)sizeof(directory));
    assert(_mkdir(directory) == 0 || errno == EEXIST);
    assert(snprintf(csv_path, sizeof(csv_path), "%s/MQTTLOG.CSV", directory) < (int)sizeof(csv_path));
    assert(remove(csv_path) == 0 || errno == ENOENT);
}
bool mqtt_app_fault_mode(void) { return fault_boundary != NO_BOUNDARY; }
static void hold_file(enum boundary boundary)
{
    if (boundary != hold_boundary || mqtt_app_append_attempts != 1) return;
    assert(file_release && append_file && mqtt_app_open_files == 1);
    InterlockedExchange(&mqtt_app_file_waiting, 1);
    printf("FILE_WAIT boundary=%d files=%ld busy=%d\n", boundary, mqtt_app_open_files, mqtt_tool_busy());
    assert(WaitForSingleObject(file_release, 15000) == WAIT_OBJECT_0);
    InterlockedExchange(&mqtt_app_file_waiting, 0);
}
static bool fail_file(enum boundary boundary)
{
    if (boundary != fault_boundary || mqtt_app_append_attempts != 1 || mqtt_app_file_faults) return false;
    InterlockedIncrement(&mqtt_app_file_faults); errno = EIO;
    printf("FILE_ERROR boundary=%d append_attempts=%ld\n", boundary, mqtt_app_append_attempts);
    return true;
}
void mqtt_app_release_file(void)
{
    assert(GetCurrentThreadId() == mqtt_net_ui_thread && file_release && mqtt_app_file_waiting);
    assert(SetEvent(file_release));
}
void mqtt_app_release_report(void)
{
    assert(GetCurrentThreadId() == mqtt_net_ui_thread && report_release && mqtt_app_report_waiting);
    assert(SetEvent(report_release));
}
void mqtt_app_storage_error(int error)
{
    assert(mqtt_net_on_worker() && error == EIO && mqtt_app_fault_mode() && mqtt_app_file_faults == 1);
    assert(mqtt_tool_busy() && !mqtt_app_open_files);
    if (!strcmp(test_mode, "app-log-home-error"))
        assert(!mqtt_net_transports && mqtt_net_allocations == 1 && mqtt_net_threads == 1);
    else assert(mqtt_net_transports && mqtt_net_allocations > 1);
    assert(InterlockedIncrement(&mqtt_app_storage_reports) == 1);
    printf("STORAGE_REPORT error=%d busy=%d allocations=%ld transports=%ld files=%ld\n",
           error, mqtt_tool_busy(), mqtt_net_allocations, mqtt_net_transports, mqtt_app_open_files);
    if (report_release) {
        InterlockedExchange(&mqtt_app_report_waiting, 1);
        assert(WaitForSingleObject(report_release, 15000) == WAIT_OBJECT_0);
        InterlockedExchange(&mqtt_app_report_waiting, 0);
    }
}
void mqtt_app_storage_shutdown(void)
{
    assert(!mqtt_app_open_files && !append_file && !mqtt_app_file_waiting && !mqtt_app_report_waiting);
    if (file_release) assert(CloseHandle(file_release));
    if (report_release) assert(CloseHandle(report_release));
}
struct tm *mqtt_app_localtime_r(const time_t *time, struct tm *result)
{
    return localtime_s(result, time) == 0 ? result : NULL;
}
int mqtt_app_mkdir(const char *path)
{
    assert(mqtt_net_on_worker() && !strcmp(path, "/sdcard/MQTT")); return _mkdir(directory);
}
FILE *mqtt_app_fopen(const char *path, const char *mode)
{
    assert(mqtt_net_on_worker() && !strcmp(path, "/sdcard/MQTT/MQTTLOG.CSV"));
    assert(!strcmp(mode, "r+b") || !strcmp(mode, "a+"));
    bool append = !strcmp(mode, "a+");
    if (append) { InterlockedIncrement(&mqtt_app_append_attempts); if (fail_file(OPEN_BOUNDARY)) return NULL; }
    FILE *file = fopen(csv_path, mode);
    if (file) {
        InterlockedIncrement(&mqtt_app_open_files);
        if (append) { assert(!append_file); append_file = file; }
    }
    return file;
}
int mqtt_app_fclose(FILE *file)
{
    assert(mqtt_net_on_worker());
    bool append = file == append_file;
    if (append) hold_file(CLOSE_BOUNDARY);
    int result = fclose(file); InterlockedDecrement(&mqtt_app_open_files);
    InterlockedIncrement(&mqtt_app_closed_files);
    if (append) { append_file = NULL; if (fail_file(CLOSE_BOUNDARY)) return EOF; }
    return result;
}
/* Real writes may take effect even when persistence cannot be confirmed. Flush,
 * sync and close faults deliberately perform the operation before reporting EIO. */
int mqtt_app_fflush(FILE *file)
{
    assert(mqtt_net_on_worker()); int result = fflush(file);
    return file == append_file && fail_file(FLUSH_BOUNDARY) ? EOF : result;
}
int mqtt_app_commit(int descriptor)
{
    assert(mqtt_net_on_worker()); bool append = append_file && descriptor == _fileno(append_file);
    if (append) hold_file(SYNC_BOUNDARY);
    int result = _commit(descriptor);
    return append && fail_file(SYNC_BOUNDARY) ? -1 : result;
}
void esp_efuse_mac_get_default(uint8_t *mac)
{
    const uint8_t fixed[] = {2, 0, 0, 1, 2, 3}; memcpy(mac, fixed, sizeof(fixed));
}
esp_err_t esp_crt_bundle_attach(void *configuration)
{
    (void)configuration; assert(0); return ESP_FAIL; /* TLS is absent and fails closed. */
}
esp_err_t nvs_open(const char *space, int access, nvs_handle_t *handle)
{
    (void)handle;
    assert(GetCurrentThreadId() == mqtt_net_ui_thread && !strcmp(space, "mqtt") && access == NVS_READONLY);
    return ESP_ERR_NVS_NOT_FOUND;
}
#define UNUSED_HANDLE() do { (void)handle; assert(0); } while (0)
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *length)
{
    (void)key; (void)data; (void)length; UNUSED_HANDLE(); return ESP_FAIL;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length)
{
    (void)key; (void)data; (void)length; UNUSED_HANDLE(); return ESP_FAIL;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)key; UNUSED_HANDLE(); return ESP_FAIL;
}
esp_err_t nvs_commit(nvs_handle_t handle) { UNUSED_HANDLE(); return ESP_FAIL; }
void nvs_close(nvs_handle_t handle) { UNUSED_HANDLE(); }
