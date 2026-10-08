/* Actual Ring callbacks/storage helpers with native files and controlled faults. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <assert.h>
#include <errno.h>
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
#else
#include <unistd.h>
#define fixture_pid getpid
#endif

#define HEALTH_PATH "/sdcard/HEALTH"
#define HEART_RATE_LOG HEALTH_PATH "/HR.CSV"
#define RING_HR_HISTORY_POINTS 60
#define pdTRUE 1
typedef uint32_t TickType_t;
typedef struct { int unused; } lv_timer_t;
typedef struct { time_t timestamp; uint8_t bpm; } ring_hr_sample_t;
static bool sd_ready, ring_hr_last_saved_loaded, ring_hr_active, ring_sync_active, ring_sync_pending;
static time_t ring_hr_last_saved, ring_hr_updated_at;
static TickType_t ring_hr_deadline, ring_sync_deadline;
static int ring_heart_rate, ring_lock;
static uint8_t ring_hr_history[RING_HR_HISTORY_POINTS], ring_hr_history_head, ring_hr_history_count;
static char ring_storage_error[96];
static struct { ring_hr_sample_t values[3]; unsigned count; } queue;
static void *ring_hr_samples = &queue;
static unsigned peeks, receives, requests;
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static TickType_t xTaskGetTickCount(void) { return 1000; }
static int xQueuePeek(void *handle, ring_hr_sample_t *sample, unsigned wait)
{
    assert(handle == &queue && !wait); peeks++;
    if (!queue.count) return 0;
    *sample = queue.values[0]; return pdTRUE;
}
static int xQueueReceive(void *handle, ring_hr_sample_t *sample, unsigned wait)
{
    assert(handle == &queue && !wait && queue.count); receives++;
    *sample = queue.values[0]; queue.count--;
    memmove(queue.values, queue.values + 1, queue.count * sizeof(queue.values[0])); return pdTRUE;
}
static unsigned uxQueueMessagesWaiting(void *handle) { assert(handle == &queue); return queue.count; }
static int ring_request_history(void) { requests++; return 0; }
static struct tm *fixture_localtime(const time_t *timestamp, struct tm *local)
{
#ifdef _WIN32
    return gmtime_s(local, timestamp) == 0 ? local : NULL;
#else
    return gmtime_r(timestamp, local);
#endif
}
#define localtime_r fixture_localtime

typedef enum { REPAIR, LOAD, APPEND } stream_t;
typedef enum { NONE, NO_SD, REPAIR_OPEN, REPAIR_SYNC, LOAD_OPEN, LOAD_END, LOAD_TELL,
    LOAD_START, LOAD_READ, LOAD_SHORT, LOAD_FULL_ERROR, LOAD_CLOSE, LOAD_READ_CLOSE,
    LOAD_SEEK_CLOSE, LOAD_STALE, FOLDER, APPEND_OPEN, APPEND_SEEK, APPEND_TELL,
    HEADER, ROW, HEADER_ZERO, ROW_ZERO, FLUSH, SYNC, APPEND_CLOSE,
    SEEK_CLOSE, TELL_CLOSE, HEADER_CLOSE, ROW_CLOSE, ROW_ZERO_CLOSE,
    ROW_CLEANUP, SYNC_CLEANUP } fault_t;
static fault_t fault;
static stream_t stream;
static FILE *active;
static char path[128];
static unsigned handles, opens[3], closes[3], seeks[3], reads, writes, repairs, syncs, truncates, calls;
static int recorded_error;
static bool flagged;
static const char *mapped(const char *name) { assert(!strcmp(name, HEART_RATE_LOG)); return path; }
FILE *fixture_open(const char *name, const char *mode)
{
    assert(!handles); calls++;
    stream = !strcmp(mode, "r+b") ? REPAIR : !strcmp(mode, "rb") ? LOAD : APPEND;
    assert(stream != APPEND || !strcmp(mode, "ab+")); opens[stream]++;
    if (stream == REPAIR) repairs++;
    if ((stream == REPAIR && (fault == REPAIR_OPEN || (repairs > 1 && (fault == ROW_CLEANUP || fault == SYNC_CLEANUP)))) ||
        (stream == LOAD && fault == LOAD_OPEN) || (stream == APPEND && fault == APPEND_OPEN)) {
        errno = EACCES; return NULL;
    }
    FILE *file = fopen(mapped(name), mode);
    active = file; flagged = false;
    if (file) handles++;
    return file;
}
int fixture_seek(FILE *file, long offset, int origin)
{
    assert(file == active); calls++; seeks[stream]++;
    if ((stream == LOAD && ((seeks[LOAD] == 1 && (fault == LOAD_END || fault == LOAD_SEEK_CLOSE)) || (seeks[LOAD] == 2 && fault == LOAD_START))) ||
        (stream == APPEND && (fault == APPEND_SEEK || fault == SEEK_CLOSE))) { errno = EACCES; return -1; }
    int result = fseek(file, offset, origin);
    if (stream == LOAD && fault == LOAD_STALE) errno = ENOSPC;
    return result;
}
long fixture_tell(FILE *file)
{
    assert(file == active); calls++;
    if ((stream == LOAD && fault == LOAD_TELL) || (stream == APPEND && (fault == APPEND_TELL || fault == TELL_CLOSE))) { errno = EIO; return -1; }
    return ftell(file);
}
size_t fixture_read(void *buffer, size_t size, size_t count, FILE *file)
{
    assert(file == active && size == 1); calls++;
    if (stream == LOAD) {
        reads++;
        if (fault == LOAD_READ || fault == LOAD_SHORT || fault == LOAD_FULL_ERROR || fault == LOAD_READ_CLOSE || fault == LOAD_STALE) {
            size_t result = fread(buffer, size, count, file);
            if (fault != LOAD_FULL_ERROR) result /= 2;
            flagged = fault != LOAD_SHORT;
            if (fault != LOAD_STALE) errno = fault == LOAD_SHORT ? 0 : EIO;
            return result;
        }
    }
    return fread(buffer, size, count, file);
}
int fixture_error(FILE *file) { assert(file == active); return flagged || ferror(file); }
int fixture_getc(FILE *file) { assert(file == active); calls++; return fgetc(file); }
int fixture_close(FILE *file)
{
    assert(file == active && handles == 1); calls++; handles--; closes[stream]++; active = NULL;
    int result = fclose(file);
    if ((stream == LOAD && (fault == LOAD_CLOSE || fault == LOAD_READ_CLOSE || fault == LOAD_SEEK_CLOSE)) ||
        (stream == APPEND && (fault == APPEND_CLOSE || fault == SEEK_CLOSE || fault == TELL_CLOSE || fault == HEADER_CLOSE ||
                              fault == ROW_CLOSE || fault == ROW_ZERO_CLOSE || fault == ROW_CLEANUP || fault == SYNC_CLEANUP))) {
        errno = EPERM; return EOF;
    }
    return result;
}
int fixture_mkdir(const char *name, int mode)
{
    assert(!strcmp(name, HEALTH_PATH) && mode == 0775); calls++;
    if (fault == FOLDER) { errno = ENOSPC; return -1; } errno = EEXIST; return -1;
}
int fixture_puts(const char *text, FILE *file)
{
    assert(file == active && stream == APPEND); calls++; writes++;
    if (fault == HEADER || fault == HEADER_ZERO || fault == HEADER_CLOSE) {
        assert(fwrite(text, 1, 7, file) == 7); errno = fault == HEADER_ZERO ? 0 : ENOSPC; return EOF;
    }
    return fputs(text, file);
}
int fixture_printf(FILE *file, const char *format, ...)
{
    assert(file == active && stream == APPEND); calls++; writes++;
    if (fault == ROW || fault == ROW_ZERO || fault == ROW_CLOSE || fault == ROW_ZERO_CLOSE || fault == ROW_CLEANUP) {
        assert(fwrite("1700000300,part", 1, 15, file) == 15); errno = fault == ROW_ZERO || fault == ROW_ZERO_CLOSE ? 0 : ENOSPC; return -1;
    }
    va_list args; va_start(args, format); int result = vfprintf(file, format, args); va_end(args); return result;
}
int fixture_flush(FILE *file)
{
    assert(file == active); calls++; int result = fflush(file);
    if (stream == APPEND && fault == FLUSH) { errno = EIO; return EOF; } return result;
}
#ifdef _WIN32
int fixture_sync(int descriptor)
{
    calls++; syncs++; int result = _commit(descriptor);
    if ((stream == REPAIR && fault == REPAIR_SYNC) || (stream == APPEND && (fault == SYNC || fault == SYNC_CLEANUP))) { errno = EIO; return -1; }
    return result;
}
int fixture_truncate(int descriptor, int64_t length) { calls++; truncates++; return _chsize_s(descriptor, length); }
#define _commit fixture_sync
#define _chsize_s fixture_truncate
#else
int fixture_sync(int descriptor)
{
    calls++; syncs++; int result = fsync(descriptor);
    if ((stream == REPAIR && fault == REPAIR_SYNC) || (stream == APPEND && (fault == SYNC || fault == SYNC_CLEANUP))) { errno = EIO; return -1; }
    return result;
}
int fixture_truncate(int descriptor, off_t length) { calls++; truncates++; return ftruncate(descriptor, length); }
#define fsync fixture_sync
#define ftruncate fixture_truncate
#endif
static void sd_record_error(int error) { recorded_error = error; }
static int sd_error_snapshot(void) { return recorded_error; }
#define fopen fixture_open
#define fseek fixture_seek
#define ftell fixture_tell
#define fread fixture_read
#define ferror fixture_error
#define fgetc fixture_getc
#define fclose fixture_close
#define mkdir fixture_mkdir
#define fputs fixture_puts
#define fprintf fixture_printf
#define fflush fixture_flush
#include "storage_source.inc"
#include "ring_history.inc"
#undef fopen
#undef fseek
#undef ftell
#undef fread
#undef ferror
#undef fgetc
#undef fclose
#undef mkdir
#undef fputs
#undef fprintf
#undef fflush
#ifdef _WIN32
#undef _commit
#undef _chsize_s
#else
#undef fsync
#undef ftruncate
#endif

static const char *header = "unix_time,local_time,bpm\n";
static const char *data = "unix_time,local_time,bpm\n1700000100,2023-11-14 22:15:00,60\n1700000200,2023-11-14 22:16:40,70\n";
static const char *row = "1700000300,2023-11-14 22:18:20,80\n";
static unsigned cases, failures;
static bool matches(const char *expected)
{
    FILE *file = fopen(path, "rb");
    if (!expected) { if (file) fclose(file); return !file && errno == ENOENT; }
    if (!file) return false;
    char buffer[512]; size_t offset = 0, length = strlen(expected); bool pass = true;
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (offset + count > length || (count && memcmp(buffer, expected + offset, count))) pass = false;
        offset += count;
        if (count < sizeof(buffer)) { assert(!ferror(file)); break; }
    }
    assert(fclose(file) == 0); return pass && offset == length;
}
static char *joined(const char *first, const char *second)
{
    char *value = malloc(strlen(first) + strlen(second) + 1); assert(value); strcpy(value, first); strcat(value, second); return value;
}
static void prepare(const char *initial)
{
    assert(!handles); if (remove(path) != 0) assert(errno == ENOENT);
    if (initial) { FILE *file = fopen(path, "wb"); assert(file); assert(fwrite(initial, 1, strlen(initial), file) == strlen(initial)); assert(fclose(file) == 0); }
    memset(opens, 0, sizeof(opens)); memset(closes, 0, sizeof(closes)); memset(seeks, 0, sizeof(seeks));
    reads = writes = repairs = syncs = truncates = calls = peeks = receives = requests = 0;
    fault = NONE; flagged = false; sd_ready = true; recorded_error = 0; active = NULL;
    ring_hr_last_saved_loaded = false; ring_hr_last_saved = 1700000000;
    ring_storage_error[0] = '\0'; ring_hr_active = ring_sync_active = ring_sync_pending = false;
    queue.count = 0; ring_hr_samples = &queue; ring_hr_history_head = ring_hr_history_count = 0;
}
static void report(const char *name, bool pass, int result, int error, int expected)
{
    cases++; failures += !pass;
    printf("%s %s result=%d error=%d expected_error=%d reported=%d loaded=%d checkpoint=%lld queued=%u peeks=%u receives=%u requests=%u reads=%u writes=%u repairs=%u calls=%u handles=%u\n",
        pass ? "PASS" : "FAIL", name, result, error, expected, recorded_error, ring_hr_last_saved_loaded, (long long)ring_hr_last_saved,
        queue.count, peeks, receives, requests, reads, writes, repairs, calls, handles);
}
static void load_case(const char *name, const char *initial, fault_t injected, int error, time_t checkpoint, const char *expected_bytes)
{
    prepare(initial); fault = injected; sd_ready = fault != NO_SD; errno = 0;
    ring_hr_load_last_saved(); int actual_error = error ? errno : 0;
    bool pass = ring_hr_last_saved_loaded == !error && ring_hr_last_saved == checkpoint && !handles;
    if (error) pass = pass && actual_error == error;
    if (fault == NO_SD) pass = pass && !calls;
    pass = pass && matches(expected_bytes); report(name, pass, ring_hr_last_saved_loaded, actual_error, error);
}
static void append_case(const char *name, const char *initial, fault_t injected, int error, const char *expected_bytes)
{
    prepare(initial); fault = injected; sd_ready = fault != NO_SD; errno = 0;
    bool result = ring_hr_append_log(1700000300, 80); int actual_error = result ? 0 : errno;
    bool pass = result == !error && actual_error == error && !handles && matches(expected_bytes);
    if (fault == NO_SD) pass = pass && !calls;
    if (error && fault != NO_SD) pass = pass && recorded_error == error;
    report(name, pass, result, actual_error, error);
}
static int tick_error;
static bool blocked_tick(int expected)
{
    errno = 0;
    ring_health_tick(NULL);
    tick_error = errno;
    char message[96]; snprintf(message, sizeof(message), "Heart-rate log not saved: %s", strerror(expected));
    return !handles && !ring_hr_last_saved_loaded && ring_hr_last_saved == 1700000000 && queue.count == 2 &&
        !peeks && !receives && !requests && !writes && !strcmp(message, ring_storage_error) && matches(data);
}
static void tick_case(const char *name, fault_t injected, int error, bool retry)
{
    prepare(data); fault = injected; sd_ready = fault != NO_SD;
    queue.count = 2; queue.values[0] = (ring_hr_sample_t){1700000100, 60}; queue.values[1] = (ring_hr_sample_t){1700000300, 80};
    bool pass = blocked_tick(error); int first_error = tick_error;
    if (retry) {
        unsigned old_reads = reads; fault = NONE; sd_ready = true; ring_health_tick(NULL);
        char *expected = joined(data, row);
        pass = pass && ring_hr_last_saved_loaded && ring_hr_last_saved == 1700000300 && !queue.count && receives == 2 && requests == 1 &&
            !ring_storage_error[0] && reads == old_reads + 1 && matches(expected); free(expected);
    }
    report(name, pass, ring_hr_last_saved_loaded, first_error, error);
}
int main(void)
{
    assert(snprintf(path, sizeof(path), ".ring_history_%ld.CSV", (long)fixture_pid()) > 0);
    struct stat info; assert(stat(path, &info) != 0 && errno == ENOENT);
    load_case("load-missing", NULL, NONE, 0, 1700000000, NULL);
    load_case("load-empty", "", NONE, 0, 1700000000, "");
    load_case("load-complete", data, NONE, 0, 1700000200, data);
    load_case("load-no-sd", data, NO_SD, ENODEV, 1700000000, data);
    prepare(data); sd_ready = false; recorded_error = EIO; errno = 0; ring_hr_load_last_saved();
    int stored_error = errno;
    report("load-stored-sd-error", !calls && !handles && !ring_hr_last_saved_loaded && ring_hr_last_saved == 1700000000 && stored_error == EIO && matches(data), ring_hr_last_saved_loaded, stored_error, EIO);
    const struct { const char *name; fault_t fault; int error; } load_faults[] = {
        {"load-repair-open", REPAIR_OPEN, EACCES}, {"load-open", LOAD_OPEN, EACCES}, {"load-seek-end", LOAD_END, EACCES},
        {"load-tell", LOAD_TELL, EIO}, {"load-seek-start", LOAD_START, EACCES}, {"load-read", LOAD_READ, EIO},
        {"load-short-without-error", LOAD_SHORT, EIO}, {"load-full-count-error", LOAD_FULL_ERROR, EIO}, {"load-close", LOAD_CLOSE, EPERM},
        {"load-first-read-error", LOAD_READ_CLOSE, EIO}, {"load-first-seek-error", LOAD_SEEK_CLOSE, EACCES}, {"load-stale-errno", LOAD_STALE, EIO}};
    for (unsigned i = 0; i < sizeof(load_faults) / sizeof(load_faults[0]); i++)
        load_case(load_faults[i].name, data, load_faults[i].fault, load_faults[i].error, 1700000000, data);
    char *partial = joined(data, "1700000300,unfinished");
    load_case("load-repaired-tail", partial, NONE, 0, 1700000200, data);
    load_case("load-repair-sync", partial, REPAIR_SYNC, EIO, 1700000000, data); free(partial);
    char large[2048]; strcpy(large, header);
    for (unsigned i = 0; i < 40; i++) { char line[80]; snprintf(line, sizeof(line), "%lld,2023-11-14 22:15:00,60\n", (long long)1700000000 + i * 20); strcat(large, line); }
    load_case("load-long-tail", large, NONE, 0, 1700000780, large);
    prepare(data); ring_hr_load_last_saved(); unsigned old_calls = calls; fault = LOAD_OPEN; ring_hr_load_last_saved();
    report("load-cached", calls == old_calls && ring_hr_last_saved_loaded && ring_hr_last_saved == 1700000200 && matches(data), ring_hr_last_saved_loaded, 0, 0);
    char *first = joined(header, row), *appended = joined(data, row), *unfinished = joined(data, "1700000300,part");
    append_case("append-first", NULL, NONE, 0, first);
    append_case("append-empty", "", NONE, 0, first);
    append_case("append-complete", data, NONE, 0, appended);
    append_case("append-no-sd", data, NO_SD, ENODEV, data);
    prepare(data); sd_ready = false; recorded_error = EIO; errno = 0;
    bool stored_result = ring_hr_append_log(1700000300, 80); stored_error = errno;
    report("append-stored-sd-error", !stored_result && !calls && !handles && stored_error == EIO && matches(data), stored_result, stored_error, EIO);
    const struct { const char *name; fault_t fault; int error; bool complete; } append_faults[] = {
        {"append-folder", FOLDER, ENOSPC, false}, {"append-repair-open", REPAIR_OPEN, EACCES, false}, {"append-open", APPEND_OPEN, EACCES, false},
        {"append-seek", APPEND_SEEK, EACCES, false}, {"append-tell", APPEND_TELL, EIO, false}, {"append-row", ROW, ENOSPC, false},
        {"append-row-zero-errno", ROW_ZERO, EIO, false}, {"append-flush", FLUSH, EIO, true}, {"append-sync", SYNC, EIO, true},
        {"append-close", APPEND_CLOSE, EPERM, true}, {"append-first-seek-error", SEEK_CLOSE, EACCES, false}, {"append-first-tell-error", TELL_CLOSE, EIO, false},
        {"append-first-row-error", ROW_CLOSE, ENOSPC, false}, {"append-zero-first-row-error", ROW_ZERO_CLOSE, EIO, false},
        {"append-sync-cleanup-error", SYNC_CLEANUP, EIO, true}};
    for (unsigned i = 0; i < sizeof(append_faults) / sizeof(append_faults[0]); i++)
        append_case(append_faults[i].name, data, append_faults[i].fault, append_faults[i].error, append_faults[i].complete ? appended : data);
    append_case("append-header", NULL, HEADER, ENOSPC, "");
    append_case("append-header-zero-errno", NULL, HEADER_ZERO, EIO, "");
    append_case("append-first-header-error", NULL, HEADER_CLOSE, ENOSPC, "");
    append_case("append-first-row-cleanup-error", data, ROW_CLEANUP, ENOSPC, unfinished);
    free(first); free(unfinished);
    tick_case("tick-block-open", LOAD_OPEN, EACCES, false);
    tick_case("tick-block-read", LOAD_READ, EIO, false);
    tick_case("tick-block-close", LOAD_CLOSE, EPERM, false);
    tick_case("tick-block-repair", REPAIR_OPEN, EACCES, false);
    tick_case("tick-block-no-sd", NO_SD, ENODEV, false);
    tick_case("tick-retry-open", LOAD_OPEN, EACCES, true);
    tick_case("tick-retry-read", LOAD_READ, EIO, true);
    prepare(data); fault = LOAD_OPEN; errno = 0; ring_health_tick(NULL); stored_error = errno;
    report("tick-empty-load-error", !handles && !ring_hr_last_saved_loaded && !queue.count && !peeks && !requests && !writes && ring_storage_error[0] && stored_error == EACCES && matches(data), ring_hr_last_saved_loaded, stored_error, EACCES);
    prepare(data); ring_hr_load_last_saved(); old_calls = calls; sd_ready = false;
    queue.count = 2; queue.values[0] = (ring_hr_sample_t){1700000100, 60}; queue.values[1] = (ring_hr_sample_t){1700000300, 80};
    errno = 0; ring_health_tick(NULL); stored_error = errno;
    report("tick-cached-no-sd", !handles && ring_hr_last_saved_loaded && ring_hr_last_saved == 1700000200 && queue.count == 2 && !peeks && !receives && !requests && calls == old_calls && ring_storage_error[0] && stored_error == ENODEV && matches(data), ring_hr_last_saved_loaded, stored_error, ENODEV);
    prepare(data); queue.count = 2; queue.values[0] = (ring_hr_sample_t){1700000100, 60}; queue.values[1] = (ring_hr_sample_t){1700000300, 80}; ring_health_tick(NULL);
    report("tick-deduplicate", ring_hr_last_saved_loaded && ring_hr_last_saved == 1700000300 && !queue.count && receives == 2 && requests == 1 && ring_hr_history_count == 2 && !ring_storage_error[0] && matches(appended), ring_hr_last_saved_loaded, 0, 0);
    prepare(data); strcpy(ring_storage_error, "previous load error"); queue.count = 1; queue.values[0] = (ring_hr_sample_t){1700000100, 60}; ring_health_tick(NULL);
    report("tick-clear-recovered-error", !queue.count && receives == 1 && requests == 1 && ring_hr_last_saved == 1700000200 && !ring_storage_error[0] && matches(data), ring_hr_last_saved_loaded, 0, 0);
    prepare(data); queue.count = 1; queue.values[0] = (ring_hr_sample_t){1700000300, 80}; fault = ROW_CLOSE; ring_health_tick(NULL);
    char message[96]; snprintf(message, sizeof(message), "Heart-rate log not saved: %s", strerror(ENOSPC));
    bool pass = !handles && queue.count == 1 && !receives && !requests && ring_hr_last_saved == 1700000200 && !strcmp(message, ring_storage_error) && matches(data);
    fault = NONE; ring_health_tick(NULL);
    report("tick-retry-row", pass && !queue.count && receives == 1 && requests == 1 && ring_hr_last_saved == 1700000300 && !ring_storage_error[0] && matches(appended), ring_hr_last_saved_loaded, recorded_error, ENOSPC);
    prepare(data); fault = NO_SD; sd_ready = false; queue.count = 2; queue.values[0] = (ring_hr_sample_t){1700000100, 60}; queue.values[1] = (ring_hr_sample_t){1700000300, 80};
    ring_hr_active = ring_sync_active = true; ring_hr_deadline = ring_sync_deadline = 500;
    pass = blocked_tick(ENODEV);
    report("tick-expire-while-blocked", pass && !ring_hr_active && !ring_sync_active && ring_sync_pending && !calls, ring_hr_last_saved_loaded, tick_error, ENODEV);
    prepare(data); pass = true;
    for (unsigned i = 0; i < 25; i++) { ring_hr_last_saved_loaded = false; ring_hr_last_saved = 1700000000; ring_hr_load_last_saved(); pass = pass && ring_hr_last_saved_loaded && ring_hr_last_saved == 1700000200 && !handles; }
    report("load-reopen-25", pass && reads == 25 && closes[LOAD] == 25 && matches(data), ring_hr_last_saved_loaded, 0, 0);
    free(appended); assert(remove(path) == 0);
    printf("%s %u Ring history cases failures=%u handles=%u (native files; controlled queues/timers/I/O)\n", failures ? "FAIL" : "PASS", cases, failures, handles);
    return failures ? 1 : 0;
}
