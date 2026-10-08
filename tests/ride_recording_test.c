/* Actual recording/history/storage code; native files; controlled UI/BLE/time/faults. */
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
#include <direct.h>
#include <io.h>
#include <process.h>
#define fixture_pid _getpid
#define native_open _open
#define native_fdopen _fdopen
#define native_close _close
#define native_rmdir _rmdir
#else
#include <unistd.h>
#define fixture_pid getpid
#define native_open open
#define native_fdopen fdopen
#define native_close close
#define native_rmdir rmdir
#endif
#include "ride_recording_config.inc"
enum { INDEX_TMP = 200, INDEX_CSV, INDEX_BAK, PATHS };
enum point { NONE, ROOT, DATE, STAT, OPEN, FDOPEN, HEADER, ROW, FLUSH, SYNC, CLOSE, PUBLISH, INDEX_OPEN, POINTS };
static enum point failed_point;
static unsigned failed_call, calls[POINTS], handles, descriptors, publications, index_publications, reports, cases, failures, ring_requests;
static int failed_errno, reported_error;
static bool close_error, tick_clobber, stale_success, sd_ready, ride_recording;
static FILE *ride_file;
static char ride_temporary_path[128], ride_final_path[128], ride_notice[96];
static char directories[2][96], paths[PATHS][256], logical[PATHS][128];
static bool owned[PATHS], owned_directories[2];
typedef struct { FILE *file; unsigned slot; bool flagged; } stream_t;
static stream_t streams[4];
static int descriptor_slot = -1;
typedef uint32_t TickType_t;
static TickType_t tick, ride_started_tick, ride_last_log_tick, ride_last_flush_tick, ride_next_hr_measure;
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTICKS_TO_MS(ticks) (ticks)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static int kickr_lock, ring_lock;
typedef struct { float speed_kmh, cadence_rpm; int power_w, resistance; bool has_speed, has_cadence, has_power, has_resistance; } kickr_data_t;
static kickr_data_t kickr_data;
static bool kickr_enabled, kickr_found, kickr_connecting, kickr_connected, kickr_subscribed;
static time_t wall, kickr_updated_at, ring_hr_updated_at, ride_started_at;
static int ring_heart_rate;
static float ride_distance_km, ride_work_kj;
static int64_t ride_power_sum, ride_hr_sum;
static uint32_t ride_power_samples, ride_hr_samples_count;
static int ride_max_power, ride_max_hr;
typedef struct { char text[160]; } lv_obj_t;
typedef struct { int unused; } lv_event_t;
typedef struct { int unused; } lv_timer_t;
typedef struct { int unused; } lv_chart_series_t;
static lv_obj_t labels[8];
static lv_obj_t *ride_status = &labels[0], *ride_history = &labels[1], *ride_power_label = &labels[2], *ride_cadence_label = &labels[3];
static lv_obj_t *ride_hr_label = &labels[4], *ride_stats = &labels[5], *ride_button_label = &labels[6], *ride_toggle_label = &labels[7], *ride_chart;
static lv_chart_series_t *ride_power_series, *ride_hr_series;
#define LV_CHART_POINT_NONE (-1)
static void lv_chart_set_next_value(lv_obj_t *chart, lv_chart_series_t *series, int value) { (void)chart; (void)series; (void)value; abort(); }
static void lv_label_set_text(lv_obj_t *label, const char *text) { assert(label && strlen(text) < sizeof(label->text)); strcpy(label->text, text); }
static void lv_label_set_text_fmt(lv_obj_t *label, const char *format, ...)
{
    assert(label); va_list args; va_start(args, format); int n = vsnprintf(label->text, sizeof(label->text), format, args); va_end(args);
    assert(n >= 0 && (size_t)n < sizeof(label->text));
}
static bool ring_hr_begin(void) { ring_requests++; errno = EBUSY; return false; }
static TickType_t xTaskGetTickCount(void) { if (tick_clobber) errno = EBUSY; return tick; }
static void sd_record_error(int error) { reports++; reported_error = error; errno = EPERM; }
static int sd_error_snapshot(void) { return sd_ready ? 0 : ENODEV; }
static time_t fixture_time(time_t *out) { if (out) *out = wall; return wall; }
static struct tm *fixture_localtime(const time_t *value, struct tm *out)
{
#ifdef _WIN32
    return gmtime_s(out, value) ? NULL : out;
#else
    return gmtime_r(value, out);
#endif
}
static bool inject(enum point point)
{ calls[point]++; if (failed_point != point || calls[point] != failed_call) return false; if (failed_errno) errno = failed_errno; return true; }
static unsigned path_slot(const char *path)
{ for (unsigned i = 0; i < PATHS; i++) if (!strcmp(path, logical[i])) return i; fprintf(stderr, "Unknown path: %s\n", path); abort(); }
static stream_t *stream(FILE *file)
{ for (unsigned i = 0; i < 4; i++) if (streams[i].file == file) return &streams[i]; abort(); }
static void track(FILE *file, unsigned slot)
{ for (unsigned i = 0; i < 4; i++) if (!streams[i].file) { streams[i] = (stream_t){file, slot, false}; handles++; return; } abort(); }
static int fixture_stat(const char *path, struct stat *info)
{ unsigned slot = path_slot(path); if (slot < 200 && inject(STAT)) return -1; int result = stat(paths[slot], info); if (!result && stale_success) errno = ENOENT; return result; }
static int fixture_mkdir(const char *path, int mode)
{
    unsigned d = !strcmp(path, SD_PATH "/RIDES/231114"); assert(mode == 0775 && !strcmp(path, d ? SD_PATH "/RIDES/231114" : SD_PATH "/RIDES"));
    if (inject(d ? DATE : ROOT)) return -1;
#ifdef _WIN32
    int result = _mkdir(directories[d]);
#else
    int result = mkdir(directories[d], (mode_t)mode);
#endif
    if (!result) owned_directories[d] = true;
    return result;
}
static int fixture_open(const char *path, int flags, int mode)
{
    unsigned slot = path_slot(path); assert(slot < 100 && flags == (O_WRONLY | O_CREAT | O_EXCL) && mode == 0664);
    if (inject(OPEN)) return -1;
#ifdef _WIN32
    int fd = native_open(paths[slot], flags | _O_BINARY, mode);
#else
    int fd = native_open(paths[slot], flags, mode);
#endif
    if (fd >= 0) { descriptors++; descriptor_slot = (int)slot; owned[slot] = true; } return fd;
}
static FILE *fixture_fdopen(int fd, const char *mode)
{
    assert(descriptors == 1 && descriptor_slot >= 0 && !strcmp(mode, "wb")); if (inject(FDOPEN)) return NULL;
    FILE *file = native_fdopen(fd, mode); if (file) { track(file, (unsigned)descriptor_slot); descriptors--; descriptor_slot = -1; } return file;
}
static int fixture_close_fd(int fd)
{ assert(descriptors == 1); descriptors--; descriptor_slot = -1; int result = native_close(fd); if (close_error) { errno = EPERM; return -1; } return result; }
static FILE *fixture_fopen(const char *path, const char *mode)
{
    unsigned slot = path_slot(path); assert(slot >= 200);
    if (slot == INDEX_TMP && inject(INDEX_OPEN)) return NULL;
    FILE *file = fopen(paths[slot], mode); if (file) { track(file, slot); if (strchr(mode, 'w')) owned[slot] = true; } return file;
}
static int fixture_remove(const char *path)
{ unsigned slot = path_slot(path); if (owned[slot]) { int result = remove(paths[slot]); if (!result) owned[slot] = false; return result; } errno = ENOENT; return -1; }
static int fixture_rename(const char *from, const char *to)
{
    unsigned a = path_slot(from), b = path_slot(to); assert(owned[a] && !owned[b]); if (a < 100 && inject(PUBLISH)) return -1;
    int result = rename(paths[a], paths[b]); if (!result) { owned[a] = false; owned[b] = true; if (a < 100) publications++; else if (a == INDEX_TMP) index_publications++; } return result;
}
static int fixture_error(FILE *file) { return stream(file)->flagged || ferror(file); }
static int fixture_puts(const char *text, FILE *file)
{
    if (stream(file)->slot < 100 && inject(HEADER)) { assert(fwrite("PART", 1, 4, file) == 4); return EOF; }
    int result = fputs(text, file); if (stale_success) errno = ENOSPC; return result;
}
static int fixture_printf(FILE *file, const char *format, ...)
{
    if (stream(file)->slot < 100 && inject(ROW)) { assert(fwrite("PART", 1, 4, file) == 4); return -1; }
    va_list args; va_start(args, format); int result = vfprintf(file, format, args); va_end(args); if (stale_success) errno = ENOSPC; return result;
}
static int fixture_flush(FILE *file)
{ int result = fflush(file); return stream(file)->slot < 100 && inject(FLUSH) ? EOF : result; }
static int fixture_sync(int fd)
{
    bool ride = false; for (unsigned i = 0; i < 4; i++) if (streams[i].file) {
#ifdef _WIN32
        if (_fileno(streams[i].file) == fd) ride = streams[i].slot < 100;
#else
        if (fileno(streams[i].file) == fd) ride = streams[i].slot < 100;
#endif
    }
#ifdef _WIN32
    int result = _commit(fd);
#else
    int result = fsync(fd);
#endif
    return ride && inject(SYNC) ? -1 : result;
}
static int fixture_close(FILE *file)
{
    stream_t *s = stream(file); bool ride = s->slot < 100; int result = fclose(file); s->file = NULL; handles--;
    if (ride && (inject(CLOSE) || close_error)) { errno = failed_point == CLOSE && failed_errno ? failed_errno : EPERM; return EOF; }
    return result;
}
#define stat(path, info) fixture_stat(path, info)
#define mkdir fixture_mkdir
#define open fixture_open
#define fdopen fixture_fdopen
#define close fixture_close_fd
#define fopen fixture_fopen
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
#include "ride_recording.inc"
#undef stat
#undef mkdir
#undef open
#undef fdopen
#undef close
#undef fopen
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
static const char *header = "unix_time,elapsed_s,power_w,cadence_rpm,speed_kmh,heart_rate,resistance,distance_km,work_kj\n";
static const char *row1 = "1700000001,1,200,80.0,36.00,120,4,0.010,0.2\n";
static const char *row2 = "1700000002,2,200,80.0,36.00,120,4,0.020,0.4\n";
static const char *original_index = "start_unix,duration_s,distance_km,work_kj,avg_power_w,max_power_w,avg_hr,max_hr\n1600000000,60,1.000,10.0,100,100,100,100\n";
static const char *summary_row = "1700000000,2,0.020,0.4,200,200,120,120\n";
static char complete[4096], index_complete[4096];
static bool matches(unsigned slot, const char *text)
{
    FILE *file = fopen(paths[slot], "rb"); if (!text) { if (file) fclose(file); return !file && errno == ENOENT; } if (!file) return false;
    char bytes[4096]; size_t length = fread(bytes, 1, sizeof(bytes), file); bool ok = !ferror(file) && length == strlen(text) && !memcmp(bytes, text, length);
    return fclose(file) == 0 && ok;
}
static void seed(unsigned slot, const char *text)
{ assert(!owned[slot]); FILE *file = fopen(paths[slot], "wb"); assert(file); owned[slot] = true; assert(fwrite(text, 1, strlen(text), file) == strlen(text)); assert(fclose(file) == 0); }
static void clean(void)
{ assert(!handles && !descriptors && !ride_file); for (unsigned i = 0; i < PATHS; i++) if (owned[i]) { assert(remove(paths[i]) == 0); owned[i] = false; } }
static void prepare(enum point point, int error)
{
    clean(); failed_point = point; failed_errno = error; failed_call = 1; memset(calls, 0, sizeof(calls));
    close_error = tick_clobber = stale_success = false; sd_ready = true; ride_recording = false;
    publications = index_publications = reports = ring_requests = 0; reported_error = 0; wall = 1700000000; tick = 0;
    kickr_enabled = kickr_found = kickr_connected = kickr_subscribed = true; kickr_connecting = false;
    kickr_data = (kickr_data_t){36, 80, 200, 4, true, true, true, true}; kickr_updated_at = ring_hr_updated_at = wall; ring_heart_rate = 120;
    memset(labels, 0, sizeof(labels)); ride_notice[0] = ride_temporary_path[0] = ride_final_path[0] = '\0';
    seed(INDEX_CSV, original_index); errno = EBUSY;
}
static void step(unsigned seconds)
{ tick = ride_started_tick + seconds * 1000; wall = 1700000000 + seconds; kickr_updated_at = ring_hr_updated_at = wall; cycling_tick(NULL); }
static void close_unexpected(void)
{ if (ride_file) { fixture_close(ride_file); ride_file = NULL; } ride_recording = false; ride_temporary_path[0] = ride_final_path[0] = '\0'; }
static bool stopped(void) { return !ride_recording && !ride_file && !ride_temporary_path[0] && !ride_final_path[0] && !handles && !descriptors; }
static bool failed(int error)
{ return stopped() && !publications && !index_publications && reports == 1 && reported_error == error && !strstr(ride_notice, "saved") && strstr(ride_notice, strerror(error)); }
static bool index_untouched(void) { return matches(INDEX_CSV, original_index) && matches(INDEX_TMP, NULL) && matches(INDEX_BAK, NULL); }
static void result(const char *name, bool ok, bool bytes_ok)
{
    ok = ok && bytes_ok && !handles && !descriptors;
    printf("%s %s bytes_verified=%u published=%u index_published=%u reports=%u error=%d roots=%u dates=%u stats=%u opens=%u fdopens=%u rows=%u flushes=%u syncs=%u closes=%u ring=%u handles=%u descriptors=%u\n",
           ok ? "PASS" : "FAIL", name, bytes_ok, publications, index_publications, reports, reported_error, calls[ROOT], calls[DATE], calls[STAT], calls[OPEN], calls[FDOPEN], calls[ROW], calls[FLUSH], calls[SYNC], calls[CLOSE], ring_requests, handles, descriptors);
    cases++; failures += !ok;
}
static void creation_case(const char *name, enum point point, int error, bool cleanup_error)
{
    prepare(point, error); close_error = cleanup_error; bool started = ride_start(); close_unexpected();
    bool ok = !started && failed(error ? error : EIO); if (point == ROOT) ok = ok && !calls[DATE] && !calls[OPEN];
    if (point == STAT) ok = ok && calls[STAT] == 1 && !calls[OPEN];
    if (point == OPEN) ok = ok && calls[OPEN] == 1 && !calls[FDOPEN];
    if (point == FDOPEN) ok = ok && calls[FDOPEN] == 1 && !calls[HEADER];
    bool bytes_ok = index_untouched(); for (unsigned i = 0; i < 200; i++) bytes_ok = matches(i, NULL) && bytes_ok;
    result(name, ok, bytes_ok);
}
static void header_case(const char *name, int error, bool cleanup_error)
{
    prepare(HEADER, error); close_error = cleanup_error; bool started = ride_start(); close_unexpected();
    result(name, !started && failed(error ? error : EIO), matches(0, "PART") && matches(100, NULL) && index_untouched());
}
static void success_case(const char *name, bool wrap, bool stale)
{
    prepare(NONE, 0); if (wrap) tick = UINT32_MAX - 999; stale_success = stale; ride_button_clicked(NULL);
    bool ok = ride_recording; step(1); step(2); ride_button_clicked(NULL);
    result(name, ok && stopped() && !reports && publications == 1 && index_publications == 1 && strstr(ride_notice, "Ride saved safely"),
           matches(0, NULL) && matches(100, complete) && matches(INDEX_CSV, index_complete) && matches(INDEX_BAK, original_index) && matches(INDEX_TMP, NULL));
}
static void write_case(const char *name, enum point point, int error, bool cleanup_error)
{
    prepare(NONE, 0); assert(ride_start()); failed_point = point; failed_errno = error; close_error = cleanup_error; stale_success = !error;
    tick_clobber = !error; step(point == ROW ? 1 : 10); bool ended = !ride_recording; close_unexpected();
    char expected[4096]; snprintf(expected, sizeof(expected), "%s%s", header, point == ROW ? "PART" : "1700000010,10,200,80.0,36.00,120,4,0.100,2.0\n");
    bool ok = ended && failed(error ? error : EIO) && !ride_stop();
    if (point == ROW) ok = ok && !calls[FLUSH] && !calls[SYNC];
    if (point == FLUSH) ok = ok && calls[FLUSH] == 1 && !calls[SYNC];
    if (point == SYNC) ok = ok && calls[SYNC] == 1;
    result(name, ok, matches(0, expected) && matches(100, NULL) && index_untouched());
}
static void stop_case(const char *name, enum point point, int error)
{
    prepare(NONE, 0); assert(ride_start()); step(1); step(2); failed_point = point; failed_errno = error; errno = EBUSY;
    bool saved = ride_stop(); close_unexpected(); result(name, !saved && failed(error ? error : EIO), matches(0, complete) && matches(100, NULL) && index_untouched());
}
static void no_sd_case(bool on_stop)
{
    prepare(NONE, 0); assert(ride_start()); step(1); if (on_stop) step(2); sd_ready = false;
    unsigned previous_ring = ring_requests; bool saved = false; if (on_stop) saved = ride_stop(); else { step(2); if (ride_recording) saved = ride_stop(); }
    char expected[4096]; snprintf(expected, sizeof(expected), "%s%s%s", header, row1, on_stop ? row2 : "");
    result(on_stop ? "known-sd-loss-stop" : "known-sd-loss-tick", !saved && failed(ENODEV) && ring_requests == previous_ring,
           matches(0, expected) && matches(100, NULL) && index_untouched());
}
static void collisions(void)
{
    prepare(NONE, 0); seed(100, "KEEP"); seed(1, "OLD-PART"); bool started = ride_start(); step(1); step(2); bool saved = ride_stop();
    result("collision-recovery", started && saved && stopped() && !reports, matches(100, "KEEP") && matches(1, "OLD-PART") && matches(102, complete) && matches(INDEX_CSV, index_complete));
    for (unsigned final = 0; final < 2; final++) {
        prepare(NONE, 0); stale_success = true; for (unsigned i = 0; i < 100; i++) seed(i + (final ? 100 : 0), "KEEP");
        started = ride_start(); close_unexpected(); bool bytes_ok = index_untouched(); for (unsigned i = 0; i < 100; i++) bytes_ok = matches(i + (final ? 100 : 0), "KEEP") && matches(i + (final ? 0 : 100), NULL) && bytes_ok;
        result(final ? "final-names-full" : "temporary-names-full", !started && failed(EEXIST) && calls[STAT] == 100 && calls[OPEN] == (final ? 0 : 100), bytes_ok);
    }
}
static void recovery_case(unsigned count)
{
    bool ok = true, bytes_ok = true;
    for (unsigned i = 0; i < count; i++) {
        prepare(HEADER, ENOSPC); bool started = ride_start(); close_unexpected(); ok = !started && failed(ENOSPC) && ok;
        bytes_ok = matches(0, "PART") && matches(100, NULL) && index_untouched() && bytes_ok;
        failed_point = NONE; started = ride_start(); step(1); step(2); bool saved = ride_stop();
        ok = started && saved && stopped() && reports == 1 && publications == 1 && index_publications == 1 && ok;
        bytes_ok = matches(0, "PART") && matches(101, complete) && matches(INDEX_CSV, index_complete) && matches(INDEX_BAK, original_index) && bytes_ok;
    }
    result(count == 1 ? "header-restart" : "header-restart-25", ok, bytes_ok);
}
int main(void)
{
    snprintf(directories[0], sizeof(directories[0]), "ride_recording_%d", fixture_pid());
    snprintf(directories[1], sizeof(directories[1]), "ride_recording_%d/231114", fixture_pid());
    for (unsigned d = 0; d < 2; d++) { struct stat info; if (stat(directories[d], &info) == 0 || errno != ENOENT) { fputs("Refusing existing fixture directory\n", stderr); return 2; } }
    for (unsigned i = 0; i < 100; i++) for (unsigned final = 0; final < 2; final++) {
        unsigned slot = i + final * 100;
        snprintf(logical[slot], sizeof(logical[slot]), SD_PATH "/RIDES/231114/221320%02u.%s", i, final ? "CSV" : "TMP");
        snprintf(paths[slot], sizeof(paths[slot]), "%s/221320%02u.%s", directories[1], i, final ? "CSV" : "TMP");
    }
    const char *extensions[] = {"TMP", "CSV", "BAK"};
    for (unsigned i = 0; i < 3; i++) { snprintf(logical[200 + i], sizeof(logical[200 + i]), SD_PATH "/RIDES/SUMMARY.%s", extensions[i]); snprintf(paths[200 + i], sizeof(paths[200 + i]), "%s/SUMMARY.%s", directories[0], extensions[i]); }
#ifdef _WIN32
    assert(_mkdir(directories[0]) == 0);
#else
    assert(mkdir(directories[0], 0775) == 0);
#endif
    owned_directories[0] = true;
    snprintf(complete, sizeof(complete), "%s%s%s", header, row1, row2); snprintf(index_complete, sizeof(index_complete), "%s%s", original_index, summary_row);
    success_case("success", false, false); success_case("tick-wrap", true, false); success_case("stale-success", false, true);
    creation_case("root-error", ROOT, EACCES, false); creation_case("root-no-errno", ROOT, 0, false);
    creation_case("date-error", DATE, EACCES, false); creation_case("date-no-errno", DATE, 0, false);
    creation_case("stat-error", STAT, EACCES, false); creation_case("stat-no-errno", STAT, 0, false);
    creation_case("open-error", OPEN, ENOSPC, false); creation_case("open-no-errno", OPEN, 0, false);
    creation_case("fdopen-error", FDOPEN, ENOMEM, false); creation_case("fdopen-no-errno", FDOPEN, 0, false); creation_case("first-fdopen-error", FDOPEN, ENOMEM, true);
    header_case("header-error", ENOSPC, false); header_case("header-no-errno", 0, false); header_case("first-header-error", ENOSPC, true);
    write_case("row-error", ROW, ENOSPC, false); write_case("row-no-errno", ROW, 0, false); write_case("first-row-error", ROW, ENOSPC, true);
    write_case("flush-once", FLUSH, EIO, false); write_case("flush-no-errno", FLUSH, 0, false);
    write_case("sync-once", SYNC, EIO, false); write_case("sync-no-errno", SYNC, 0, false); write_case("first-sync-error", SYNC, EACCES, true);
    stop_case("stop-flush", FLUSH, EIO); stop_case("stop-sync", SYNC, EIO); stop_case("stop-close", CLOSE, EACCES); stop_case("stop-publish", PUBLISH, EACCES);
    no_sd_case(false); no_sd_case(true); collisions(); recovery_case(1); recovery_case(25);
    prepare(NONE, 0); assert(ride_start()); step(1); step(2); failed_point = INDEX_OPEN; failed_errno = ENOSPC; bool saved = ride_stop();
    result("summary-failure", saved && stopped() && publications == 1 && !index_publications && reports == 1 && reported_error == ENOSPC && strstr(ride_notice, "history index update failed"), matches(100, complete) && index_untouched());
    prepare(NONE, 0); sd_ready = false; ride_button_clicked(NULL); result("no-sd-admission", stopped() && !reports && !calls[ROOT] && strstr(labels[0].text, "unavailable"), index_untouched());
    prepare(NONE, 0); kickr_subscribed = false; ride_button_clicked(NULL); result("no-subscription-admission", stopped() && !reports && !calls[ROOT] && strstr(labels[0].text, "connect"), index_untouched());
    clean(); for (int d = 1; d >= 0; d--) if (owned_directories[d]) { assert(native_rmdir(directories[d]) == 0); owned_directories[d] = false; }
    printf("%s %u ride recording cases failures=%u handles=%u descriptors=%u (native recording/history/storage; controlled UI/BLE/time/faults)\n", failures ? "FAIL" : "PASS", cases, failures, handles, descriptors);
    return failures ? 1 : 0;
}
