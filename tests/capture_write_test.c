/* Actual writers/storage, real files/descriptors; controlled UI/data/faults. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#define fixture_pid _getpid
#define native_open _open
#define native_fdopen _fdopen
#define native_close _close
#else
#include <unistd.h>
#define fixture_pid getpid
#define native_open open
#define native_fdopen fdopen
#define native_close close
#endif
#include "capture_write_config.inc"
typedef enum { NONE, HEADER, HEADER_ZERO, HEADER_FLAG, ROW, ROW_ZERO, ROW_FLAG, ROW_CLOSE, ROW_TICK,
    FLUSH_ONCE, SYNC_ONCE, SYNC_CLOSE, COMMIT_CLOSE, PUBLISH, STALE, NO_SD, NO_CHART } fault_t;
static fault_t fault;
static bool scope_mode, flagged, sd_ready, scope_chart_ready;
static char paths[3][2][128];
static bool owned[3][2];
static FILE *active, *i2c_capture_file;
static unsigned handles, descriptors, writes, flushes, syncs, closes, publications, reports, cases, failures;
static int reported_error, scope_lock;
static char i2c_capture_temporary_path[96], i2c_capture_final_path[96], i2c_capture_notice[128], scope_capture_notice[128];
typedef struct { char text[160]; } lv_obj_t;
typedef struct { int unused; } lv_event_t;
static lv_obj_t label;
static lv_obj_t *scope_capture_status = &label, *i2c_capture_status = &label;
static uint8_t scope_channel_index, scope_rate_index, i2c_selected_address = 0x40, i2c_selected_register = 0x20;
static uint16_t points[SCOPE_CHART_POINTS], *scope_chart_points = points, scope_gains_permille[] = {1050};
static int16_t scope_offsets_mv[] = {-10};
static const struct { int pin; } scope_channels[] = {{54}};
static const uint32_t scope_sample_rates[] = {1000};
static uint32_t i2c_bus_speed_hz = 100000;
typedef uint32_t TickType_t;
static TickType_t i2c_capture_last_flush_tick, tick;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define pdMS_TO_TICKS(ms) (ms)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static void i2c_update_controls(void) { }
static const char *esp_err_to_name(esp_err_t error) { return error == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }
static TickType_t xTaskGetTickCount(void) { if (fault == ROW_TICK) errno = EBUSY; return tick; }
static void sd_record_error(int error) { reports++; reported_error = error; }
static int sd_error_snapshot(void) { return fault == NO_SD ? ENODEV : 0; }
static time_t fixture_time(time_t *out) { if (out) *out = 1700000000; return 1700000000; }
static struct tm *fixture_localtime(const time_t *value, struct tm *local)
{
#ifdef _WIN32
    return gmtime_s(local, value) == 0 ? local : NULL;
#else
    return gmtime_r(value, local);
#endif
}
static void lv_label_set_text(lv_obj_t *object, const char *text) { assert(object == &label && strlen(text) < sizeof(label.text)); strcpy(object->text, text); }
static void lv_label_set_text_fmt(lv_obj_t *object, const char *format, ...)
{
    assert(object == &label); va_list args; va_start(args, format); int count = vsnprintf(object->text, sizeof(object->text), format, args); va_end(args);
    assert(count >= 0 && (size_t)count < sizeof(object->text));
}
static unsigned path_index(const char *path)
{
    if (!strcmp(path, SCOPE_PATH "/231114/22132000.TMP") || !strcmp(path, SCOPE_PATH "/231114/22132000.CSV")) return 0;
    if (!strcmp(path, SCOPE_PATH "/231114/22132001.TMP") || !strcmp(path, SCOPE_PATH "/231114/22132001.CSV")) return 1;
    assert(!strcmp(path, SD_PATH "/I2C/231114/22132000.TMP") || !strcmp(path, SD_PATH "/I2C/231114/22132000.CSV")); return 2;
}
static unsigned path_final(const char *path) { return strstr(path, ".CSV") != NULL; }
static const char *native_path(const char *path) { return paths[path_index(path)][path_final(path)]; }
static int fixture_stat(const char *path, struct stat *info) { return stat(native_path(path), info); }
static int fixture_mkdir(const char *path, int mode)
{ assert(mode == 0775 && (!strcmp(path, SCOPE_PATH) || !strcmp(path, SCOPE_PATH "/231114"))); errno = EEXIST; return -1; }
static int fixture_open(const char *path, int flags, int mode)
{
    assert(flags == (O_WRONLY | O_CREAT | O_EXCL) && mode == 0664 && !path_final(path));
#ifdef _WIN32
    int fd = native_open(native_path(path), flags | _O_BINARY, mode);
#else
    int fd = native_open(native_path(path), flags, mode);
#endif
    if (fd >= 0) { descriptors++; owned[path_index(path)][0] = true; }
    return fd;
}
static FILE *fixture_fdopen(int fd, const char *mode)
{
    assert(descriptors == 1 && !active); FILE *file = native_fdopen(fd, mode);
    if (file) { descriptors--; handles++; active = file; }
    return file;
}
static int fixture_close_fd(int fd) { assert(descriptors == 1); descriptors--; return native_close(fd); }
static int fixture_remove(const char *path)
{
    unsigned i = path_index(path), j = path_final(path); int result = remove(paths[i][j]); if (!result) owned[i][j] = false; return result;
}
static int fixture_rename(const char *from, const char *to)
{
    assert(path_index(from) == path_index(to) && !path_final(from) && path_final(to));
    if (fault == PUBLISH) { errno = EACCES; return -1; }
    int result = rename(native_path(from), native_path(to));
    if (!result) { owned[path_index(from)][0] = false; owned[path_index(to)][1] = true; publications++; }
    return result;
}
static int fixture_error(FILE *file) { assert(file == active); return flagged || ferror(file); }
static int fixture_puts(const char *text, FILE *file)
{
    assert(file == active); writes++;
    if (fault == HEADER || fault == HEADER_ZERO || fault == HEADER_FLAG) {
        assert(fwrite("PART", 1, 4, file) == 4); flagged = fault == HEADER_FLAG; errno = fault == HEADER_ZERO ? 0 : ENOSPC; return EOF;
    }
    int result = fputs(text, file); if (fault == STALE) errno = ENOSPC; return result;
}
static int fixture_printf(FILE *file, const char *format, ...)
{
    assert(file == active); writes++;
    if (fault == ROW || fault == ROW_ZERO || fault == ROW_FLAG || fault == ROW_CLOSE || fault == ROW_TICK) {
        assert(fwrite("PART", 1, 4, file) == 4); flagged = fault == ROW_FLAG; errno = fault == ROW_ZERO ? 0 : ENOSPC; return -1;
    }
    va_list args; va_start(args, format); int result = vfprintf(file, format, args); va_end(args);
    if (fault == STALE) errno = ENOSPC;
    return result;
}
static int fixture_flush(FILE *file)
{
    assert(file == active); flushes++; int result = fflush(file);
    if (fault == FLUSH_ONCE && flushes == 1) { errno = EIO; return EOF; }
    return result;
}
static int fixture_sync(int fd)
{
    syncs++;
#ifdef _WIN32
    int result = _commit(fd);
#else
    int result = fsync(fd);
#endif
    if ((fault == SYNC_ONCE || fault == SYNC_CLOSE) && syncs == 1) { errno = fault == SYNC_CLOSE ? EACCES : EIO; return -1; }
    return result;
}
static int fixture_close(FILE *file)
{
    assert(file == active && handles == 1); int result = fclose(file); handles--; closes++; active = NULL;
    if (fault == ROW_CLOSE || fault == SYNC_CLOSE || fault == COMMIT_CLOSE) { errno = EPERM; return EOF; }
    return result;
}
#define stat(path, info) fixture_stat(path, info)
#define mkdir fixture_mkdir
#define open fixture_open
#define fdopen fixture_fdopen
#define close fixture_close_fd
#define remove fixture_remove
#define rename fixture_rename
#define fputs fixture_puts
#define fprintf fixture_printf
#define ferror fixture_error
#define fflush fixture_flush
#define fclose fixture_close
#ifdef _WIN32
#define _commit fixture_sync
#else
#define fsync fixture_sync
#endif
#include "storage_source.inc"
#define time fixture_time
#define localtime_r fixture_localtime
#include "capture_write.inc"
#undef stat
#undef mkdir
#undef open
#undef fdopen
#undef close
#undef remove
#undef rename
#undef fputs
#undef fprintf
#undef ferror
#undef fflush
#undef fclose
#undef time
#undef localtime_r
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif
static const char *scope_header = "unix_time,elapsed_us,gpio,millivolts,sample_rate_hz,offset_mv,scale_permille\n";
static const char *i2c_header = "unix_time,address,register,value,status,speed_khz\n";
static char expected[24000];
static void clean(void)
{ assert(!handles && !descriptors); for (unsigned i = 0; i < 3; i++) for (unsigned j = 0; j < 2; j++) if (owned[i][j]) { assert(remove(paths[i][j]) == 0); owned[i][j] = false; } }
static void prepare(bool scope, fault_t injected)
{
    clean(); scope_mode = scope; fault = injected; flagged = false; active = i2c_capture_file = NULL;
    sd_ready = injected != NO_SD; scope_chart_ready = injected != NO_CHART; tick = 10000; i2c_capture_last_flush_tick = 0;
    handles = descriptors = writes = flushes = syncs = closes = publications = reports = 0; reported_error = 0;
    label.text[0] = i2c_capture_notice[0] = scope_capture_notice[0] = '\0';
    if (!scope) {
        strcpy(i2c_capture_temporary_path, SD_PATH "/I2C/231114/22132000.TMP"); strcpy(i2c_capture_final_path, SD_PATH "/I2C/231114/22132000.CSV");
        active = i2c_capture_file = fopen(paths[2][0], "wb"); assert(active); handles = 1; owned[2][0] = true;
        assert(fputs(i2c_header, active) >= 0);
    }
}
static bool matches(const char *path, const char *text)
{
    FILE *file = fopen(path, "rb"); if (!text) { if (file) assert(fclose(file) == 0); return !file && errno == ENOENT; }
    if (!file) return false;
    char bytes[24000]; size_t count = fread(bytes, 1, sizeof(bytes), file); bool pass = !ferror(file) && count == strlen(text) && !memcmp(bytes, text, count);
    assert(fclose(file) == 0); return pass;
}
static void full_csv(bool transaction_failed)
{
    if (!scope_mode) { snprintf(expected, sizeof(expected), "%s1700000000,0x40,0x20,%s,%s,100\n", i2c_header, transaction_failed ? "" : "0xA5", transaction_failed ? "ESP_FAIL" : "ESP_OK"); return; }
    strcpy(expected, scope_header); size_t length = strlen(expected);
    for (unsigned i = 0; i < 300; i++) {
        int count = snprintf(expected + length, sizeof(expected) - length, "1700000000,%u,54,%u,1000,-10,1050\n", i * 1000, i + 1);
        assert(count > 0 && (size_t)count < sizeof(expected) - length); length += (size_t)count;
    }
}
static void report(const char *name, bool pass)
{
    cases++; failures += !pass;
    printf("%s %s published=%u reports=%u error=%d writes=%u flushes=%u syncs=%u closes=%u handles=%u descriptors=%u bytes_verified=%d\n",
        pass ? "PASS" : "FAIL", name, publications, reports, reported_error, writes, flushes, syncs, closes, handles, descriptors, pass);
}
static bool failure_expected(int error)
{ return !handles && !descriptors && !i2c_capture_file && publications == 0 && reports == 1 && reported_error == error && !strstr(label.text, "Saved ") && strstr(label.text, strerror(error)); }
static void run_case(const char *name, bool scope, fault_t injected)
{
    prepare(scope, injected); if (scope) scope_capture_clicked(NULL); else { i2c_capture_log(ESP_OK, 0xA5); if (i2c_capture_file) i2c_capture_stop(); }
    unsigned index = scope ? 0 : 2; full_csv(false);
    bool header_fault = injected == HEADER || injected == HEADER_ZERO || injected == HEADER_FLAG;
    bool row_fault = injected == ROW || injected == ROW_ZERO || injected == ROW_FLAG || injected == ROW_CLOSE || injected == ROW_TICK;
    int error = header_fault || row_fault ? injected == HEADER_ZERO || injected == ROW_ZERO ? EIO : ENOSPC : injected == SYNC_CLOSE || injected == PUBLISH ? EACCES : injected == COMMIT_CLOSE ? EPERM : injected == NO_SD ? ENODEV : EIO;
    if (header_fault) strcpy(expected, "PART");
    if (row_fault) snprintf(expected, sizeof(expected), "%sPART", scope ? scope_header : i2c_header);
    if (!scope && injected == NO_SD) strcpy(expected, i2c_header);
    bool success = injected == NONE || injected == STALE;
    bool pass = matches(paths[index][0], success || (scope && (injected == NO_SD || injected == NO_CHART)) ? NULL : expected) &&
                matches(paths[index][1], success ? expected : NULL) && !handles && !descriptors && !i2c_capture_file;
    if (success) pass = pass && publications == 1 && !reports && strstr(label.text, "Saved ");
    else if (scope && (injected == NO_SD || injected == NO_CHART)) pass = pass && !publications && !reports && !writes;
    else pass = pass && failure_expected(error);
    report(name, pass);
}
int main(void)
{
    for (unsigned i = 0; i < 3; i++) for (unsigned j = 0; j < 2; j++) {
        snprintf(paths[i][j], sizeof(paths[i][j]), ".capture_write_%d_%u.%s", fixture_pid(), i, j ? "CSV" : "TMP");
        struct stat info; assert(stat(paths[i][j], &info) != 0 && errno == ENOENT);
    }
    for (unsigned i = 0; i < SCOPE_CHART_POINTS; i++) points[i] = (uint16_t)(i + 1);
    const fault_t common[] = {NONE, ROW, ROW_ZERO, ROW_FLAG, ROW_CLOSE, FLUSH_ONCE, SYNC_ONCE, SYNC_CLOSE, COMMIT_CLOSE, PUBLISH, STALE};
    const char *names[] = {"success", "row", "row-no-errno", "row-flagged", "first-row-error", "flush-once", "sync-once", "first-sync-error", "close", "publish", "stale-success"};
    for (unsigned i = 0; i < sizeof(common) / sizeof(common[0]); i++) for (unsigned kind = 0; kind < 2; kind++) {
        char name[96]; snprintf(name, sizeof(name), "%s-%s", kind ? "i2c" : "scope", names[i]); run_case(name, kind == 0, common[i]);
    }
    run_case("scope-header", true, HEADER); run_case("scope-header-no-errno", true, HEADER_ZERO); run_case("scope-header-flagged", true, HEADER_FLAG);
    run_case("i2c-row-tick-error", false, ROW_TICK); run_case("scope-no-chart", true, NO_CHART); run_case("scope-no-sd", true, NO_SD); run_case("i2c-no-sd", false, NO_SD);
    prepare(false, NO_SD); bool stopped = i2c_capture_stop();
    report("i2c-stop-no-sd", !stopped && failure_expected(ENODEV) && matches(paths[2][0], i2c_header) && matches(paths[2][1], NULL));
    prepare(false, NONE); tick = 9999; i2c_capture_log(ESP_OK, 0xA5); bool pass = i2c_capture_file && !flushes && i2c_capture_last_flush_tick == 0; i2c_capture_stop(); full_csv(false);
    report("i2c-before-flush", pass && matches(paths[2][1], expected) && !handles && publications == 1);
    prepare(false, NONE); i2c_capture_log(ESP_FAIL, 0); i2c_capture_stop(); full_csv(true);
    report("i2c-transaction-error", matches(paths[2][1], expected) && !handles && publications == 1 && !reports);
    prepare(true, NONE); FILE *existing = fopen(paths[0][1], "wb"); assert(existing && fputs("KEEP", existing) >= 0 && fclose(existing) == 0); owned[0][1] = true;
    scope_capture_clicked(NULL); full_csv(false); report("scope-collision", matches(paths[0][1], "KEEP") && matches(paths[1][1], expected) && !handles && publications == 1);
    for (unsigned kind = 0; kind < 2; kind++) {
        pass = true;
        for (unsigned i = 0; i < 25; i++) {
            prepare(kind == 0, SYNC_ONCE); if (kind == 0) scope_capture_clicked(NULL); else i2c_capture_log(ESP_OK, 0xA5);
            unsigned index = kind == 0 ? 0 : 2; full_csv(false);
            pass = pass && failure_expected(EIO) && matches(paths[index][0], expected) && matches(paths[index][1], NULL);
            if (i2c_capture_file) i2c_capture_stop();
            prepare(kind == 0, NONE); if (kind == 0) scope_capture_clicked(NULL); else { i2c_capture_log(ESP_OK, 0xA5); i2c_capture_stop(); }
            full_csv(false); pass = pass && matches(paths[index][1], expected) && !handles && publications == 1 && !reports;
        }
        report(kind ? "i2c-cycles-25" : "scope-cycles-25", pass);
    }
    clean(); printf("%s %u capture writer cases failures=%u handles=%u descriptors=%u (native files/storage; controlled UI/data/faults)\n", failures ? "FAIL" : "PASS", cases, failures, handles, descriptors);
    return failures ? 1 : 0;
}
