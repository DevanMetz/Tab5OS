/* Actual HTTP/MQTT appenders and storage; native bytes, controlled clock/faults. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
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
#define native_mkdir(path) _mkdir(path)
#define native_rmdir _rmdir
#else
#include <unistd.h>
#define fixture_pid getpid
#define native_mkdir(path) mkdir(path, 0775)
#define native_rmdir rmdir
#endif

#if META_HTTP
/* SDK enum values and error naming are controlled; no HTTP client is linked. */
typedef enum { HTTP_METHOD_GET = 0, HTTP_METHOD_POST = 1, HTTP_METHOD_PUT = 2, HTTP_METHOD_DELETE = 4 } esp_http_client_method_t;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#include "http_log_config.inc"
#include "http_log_types.inc"
#define APP "http"
#define LOG_PATH HTTP_LOG_PATH
#define LOG_DIR "/sdcard/HTTP"
static const char *esp_err_to_name(int error) { assert(error == ESP_FAIL); return "ESP_FAIL"; }
static time_t fixture_time(time_t *value) { if (value) *value = 1700000000; return 1700000000; }
#else
#include "mqtt_log_config.inc"
#include "mqtt_log_types.inc"
#define APP "mqtt"
#define LOG_PATH MQTT_LOG_PATH
#define LOG_DIR "/sdcard/MQTT"
#endif

enum point { NONE, DIR, REPAIR, OPEN, SEEK, TELL, HEADER, PREFIX, QUOTE, SUFFIX, FLUSH, SYNC, CLOSE, POINTS };
static enum point fault;
static unsigned at, calls[POINTS], cases, failures, handles, repair_closes, repair_syncs;
static int fault_errno;
static bool flag_fault, flagged, close_error, file_owned, directory_owned;
static FILE *append_file;
static char directory[128], path[160];
static bool inject(enum point point)
{
    calls[point]++;
    if (fault != point || calls[point] != at) return false;
    if (fault_errno) errno = fault_errno;
    return true;
}
static void stale(void) { errno = EBUSY; }
static int fixture_mkdir(const char *name, int mode)
{
    assert(!strcmp(name, LOG_DIR) && mode == 0775);
    if (inject(DIR)) return -1;
    int result = native_mkdir(directory);
    if (!result) { directory_owned = true; stale(); }
    return result;
}
static FILE *fixture_open(const char *name, const char *mode)
{
    assert(!strcmp(name, LOG_PATH));
    bool append = !strcmp(mode, "a+"); assert(append || !strcmp(mode, "r+b"));
    if (inject(append ? OPEN : REPAIR)) return NULL;
    /* Binary host streams keep ESP/POSIX LF bytes on Windows too. */
    FILE *file = fopen(path, append ? "a+b" : mode);
    if (file) {
        handles++;
        if (append) { assert(!append_file); append_file = file; file_owned = true; }
        stale();
    }
    return file;
}
static int fixture_seek(FILE *file, long offset, int origin)
{
    if (file == append_file && inject(SEEK)) return -1;
    int result = fseek(file, offset, origin);
    if (!result) stale();
    return result;
}
static long fixture_tell(FILE *file)
{
    if (file == append_file && inject(TELL)) return -1;
    long result = ftell(file);
    if (result >= 0) stale();
    return result;
}
static int fixture_error(FILE *file) { return (file == append_file && flagged) || ferror(file); }
static int write_piece(FILE *file, const char *bytes, size_t length, enum point point)
{
    assert(file == append_file);
    bool rejected = inject(point);
    size_t count = rejected && !flag_fault ? (length < 3 ? 0 : 3) : length;
    assert(fwrite(bytes, 1, count, file) == count);
    if (rejected) {
        if (fault_errno) errno = fault_errno;
        if (flag_fault) { flagged = true; return (int)length; }
        return -1;
    }
    stale();
    return (int)length;
}
static int fixture_puts(const char *bytes, FILE *file) { return write_piece(file, bytes, strlen(bytes), HEADER); }
static int fixture_putc(int byte, FILE *file)
{
    char text = (char)byte;
    return write_piece(file, &text, 1, QUOTE) < 0 ? EOF : (unsigned char)byte;
}
static int fixture_printf(FILE *file, const char *format, ...)
{
    char bytes[1024]; va_list arguments; va_start(arguments, format);
    int length = vsnprintf(bytes, sizeof(bytes), format, arguments); va_end(arguments);
    assert(length >= 0 && length < (int)sizeof(bytes));
    return write_piece(file, bytes, (size_t)length, calls[PREFIX] ? SUFFIX : PREFIX);
}
static int fixture_flush(FILE *file)
{
    assert(file == append_file); int result = fflush(file);
    if (inject(FLUSH)) return EOF;
    if (!result) stale();
    return result;
}
static int fixture_sync(int descriptor)
{
#ifdef _WIN32
    int result = _commit(descriptor);
    bool append = append_file && descriptor == _fileno(append_file);
#else
    int result = fsync(descriptor);
    bool append = append_file && descriptor == fileno(append_file);
#endif
    if (append) {
        if (inject(SYNC)) return -1;
    } else repair_syncs++;
    if (!result) stale();
    return result;
}
static int fixture_close(FILE *file)
{
    assert(handles); bool append = file == append_file;
    int result = fclose(file); handles--;
    if (append) {
        append_file = NULL;
        if (inject(CLOSE) || close_error) { if (close_error) errno = EPERM; return EOF; }
    } else repair_closes++;
    if (!result) stale();
    return result;
}
#ifdef _WIN32
#define _commit fixture_sync
#else
#define fsync fixture_sync
#endif
#define mkdir fixture_mkdir
#define fopen fixture_open
#define fseek fixture_seek
#define ftell fixture_tell
#define ferror fixture_error
#define fputs fixture_puts
#define fputc fixture_putc
#define fprintf fixture_printf
#define fflush fixture_flush
#define fclose fixture_close
#include "storage_source.inc"
#if META_HTTP
#define time fixture_time
#include "http_log.inc"
#undef time
#else
#include "mqtt_log.inc"
#endif
#undef mkdir
#undef fopen
#undef fseek
#undef ftell
#undef ferror
#undef fputs
#undef fputc
#undef fprintf
#undef fflush
#undef fclose
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif

#if META_HTTP
static const char *header = "unix_time,method,url,status,response_bytes,duration_ms,outcome\n";
static const char *old_row = "1,GET,\"https://old.invalid/\",200,2,1,complete\n";
static const char *prefix = "1700000000,POST,";
static const char *field = "\"https://example.invalid/a\"\"b,c\"";
static const char *suffix = ",201,123,456,complete\n";
static http_job_t job;
static int invoke(void) { return append_log(&job); }
static void default_job(void)
{
    memset(&job, 0, sizeof(job)); job.method = HTTP_METHOD_POST; job.status = 201; job.response_bytes = 123; job.duration_ms = 456;
    strcpy(job.url, "https://example.invalid/a\"b,c?SECRET#PRIVATE"); strcpy(job.headers, "Authorization: SECRET"); strcpy(job.body, "PRIVATE");
}
#else
static const char *header = "unix_time,direction,topic,qos,retained,payload_bytes,outcome\n";
static const char *old_row = "1,RX,\"old/topic\",0,0,2,received\n";
static const char *prefix = "1700000000,TX,";
static const char *field = "\"bench/a\"\"b,c\"";
static const char *suffix = ",1,1,123,queued\n";
static mqtt_log_entry_t job;
static int invoke(void) { return append_log(&job); }
static void default_job(void)
{ memset(&job, 0, sizeof(job)); job.timestamp = 1700000000; job.qos = 1; job.retained = true; job.payload_bytes = 123; strcpy(job.topic, "bench/a\"b,c"); }
#endif
static void reset_controls(void)
{ fault = NONE; at = 1; fault_errno = 0; flag_fault = flagged = close_error = false; memset(calls, 0, sizeof(calls)); repair_closes = repair_syncs = 0; errno = EEXIST; }
static void clean(void)
{
    assert(!handles && !append_file);
    if (file_owned) { assert(remove(path) == 0); file_owned = false; }
    if (directory_owned) { assert(native_rmdir(directory) == 0); directory_owned = false; }
}
static void prepare(const char *seed, bool existing_directory)
{
    clean(); default_job(); reset_controls();
    if (existing_directory || seed) { assert(native_mkdir(directory) == 0); directory_owned = true; }
    if (seed) { FILE *file = fopen(path, "wb"); assert(file); file_owned = true; assert(fputs(seed, file) >= 0 && fclose(file) == 0); }
    errno = EEXIST;
}
static bool matches(const char *expected)
{
    FILE *file = fopen(path, "rb");
    if (!expected) { if (file) fclose(file); return !file && errno == ENOENT; }
    if (!file) return false;
    char bytes[2048]; size_t length = fread(bytes, 1, sizeof(bytes), file);
    bool ok = !ferror(file) && length == strlen(expected) && !memcmp(bytes, expected, length);
    return fclose(file) == 0 && ok;
}
static void result(const char *name, bool ok, const char *expected, int error)
{
    bool bytes_ok = matches(expected); ok = ok && bytes_ok && !handles && !append_file;
    printf("%s %s bytes_verified=%u error=%d header=%u prefix=%u quoted_bytes=%u suffix=%u flush=%u sync=%u close=%u repair_close=%u repair_sync=%u handles=%u\n",
           ok ? "PASS" : "FAIL", name, bytes_ok, error, calls[HEADER], calls[PREFIX], calls[QUOTE], calls[SUFFIX], calls[FLUSH], calls[SYNC], calls[CLOSE], repair_closes, repair_syncs, handles);
    cases++; failures += !ok;
}
static void healthy(const char *name, const char *seed, const char *base, bool existing_directory)
{
    prepare(seed, existing_directory); char expected[2048];
    snprintf(expected, sizeof(expected), "%s%s%s%s", base, prefix, field, suffix);
    int error = invoke(); result(name, !error && calls[SYNC] == 1 && calls[CLOSE] == 1, expected, error);
}
static void fault_case(const char *name, enum point point, unsigned call, int error, bool stream_flag, bool cleanup_error)
{
    char seed[1024]; snprintf(seed, sizeof(seed), "%s%s", header, old_row);
    prepare(point == HEADER ? NULL : seed, true); fault = point; at = call; fault_errno = error; flag_fault = stream_flag; close_error = cleanup_error;
    char expected[2048]; snprintf(expected, sizeof(expected), "%s", point == HEADER ? "" : seed);
    size_t used = strlen(expected);
    if (point == HEADER) {
        snprintf(expected, sizeof(expected), "%.*s", stream_flag ? (int)strlen(header) : 3, header);
    } else if (point == PREFIX) {
        snprintf(expected + used, sizeof(expected) - used, "%.*s", stream_flag ? (int)strlen(prefix) : 3, prefix);
    } else if (point == QUOTE) {
        snprintf(expected + used, sizeof(expected) - used, "%s%.*s", prefix, (int)call - (stream_flag ? 0 : 1), field);
    } else if (point == SUFFIX) {
        snprintf(expected + used, sizeof(expected) - used, "%s%s%.*s", prefix, field, stream_flag ? (int)strlen(suffix) : 3, suffix);
    } else if (point == FLUSH || point == SYNC || point == CLOSE) {
        snprintf(expected + used, sizeof(expected) - used, "%s%s%s", prefix, field, suffix);
    }
    int actual = invoke(); bool ok = actual == (error ? error : EIO);
    if (point == DIR) ok = ok && !calls[REPAIR] && !calls[OPEN];
    if (point == DIR || point == REPAIR || point == OPEN) ok = ok && !calls[CLOSE] && !calls[PREFIX];
    if (point == SEEK || point == TELL || point == HEADER || point == PREFIX || point == QUOTE || point == SUFFIX)
        ok = ok && !calls[FLUSH] && !calls[SYNC] && calls[CLOSE] == 1;
    if (point == HEADER) ok = ok && !calls[PREFIX];
    if (point == PREFIX) ok = ok && !calls[QUOTE];
    if (point == QUOTE) ok = ok && calls[QUOTE] == call && !calls[SUFFIX];
    result(name, ok, expected, actual);
}
static void variants(void)
{
#if META_HTTP
    const esp_http_client_method_t values[] = {HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_METHOD_PUT, HTTP_METHOD_DELETE};
    const char *labels[] = {"GET", "POST", "PUT", "DELETE"};
    for (unsigned i = 0; i < 4; i++) {
        prepare(NULL, true); job.method = values[i]; char expected[2048], name[64];
        snprintf(expected, sizeof(expected), "%s1700000000,%s,%s%s", header, labels[i], field, suffix);
        snprintf(name, sizeof(name), "method-%s", labels[i]); int error = invoke(); result(name, !error, expected, error);
    }
    const char *outcomes[] = {"ESP_FAIL", "preview_truncated", "incomplete_response", "invalid_response_headers", "request_deadline", "cancelled", "cancelled"};
    for (unsigned i = 0; i < 7; i++) {
        prepare(NULL, true); job.error = i == 0 ? ESP_FAIL : ESP_OK; job.truncated = i == 1 || i == 6; job.incomplete = i == 2 || i == 6;
        job.invalid_headers = i == 3 || i == 6; job.deadline_expired = i == 4 || i == 6; job.cancelled = i >= 5;
        char expected[2048], name[64]; snprintf(expected, sizeof(expected), "%s%s%s,201,123,456,%s\n", header, prefix, field, outcomes[i]);
        snprintf(name, sizeof(name), "outcome-%u", i); int error = invoke(); result(name, !error, expected, error);
    }
#else
    for (unsigned i = 0; i < 7; i++) {
        prepare(NULL, true); job.received = i == 0 || i == 3; job.truncated = i == 1 || i == 3; job.topic_preview = i == 2;
        job.qos = i == 4 ? 0 : i == 5 ? 2 : 1; job.payload_bytes = i == 6 ? 0 : 123; job.retained = i != 6;
        char expected[2048], name[64];
        snprintf(expected, sizeof(expected), "%s1700000000,%s,%s,%u,%u,%u,%s\n", header, job.received ? "RX" : "TX", field,
                 i == 4 ? 0 : i == 5 ? 2 : 1, i == 6 ? 0 : 1, i == 6 ? 0 : 123,
                 i == 1 || i == 2 || i == 3 ? "preview_truncated" : i == 0 ? "received" : "queued");
        snprintf(name, sizeof(name), "metadata-%u", i); int error = invoke(); result(name, !error, expected, error);
    }
#endif
    for (unsigned i = 0; i < 2; i++) {
        prepare(NULL, true); char expected[2048], quoted[300];
#if META_HTTP
        size_t length = i ? HTTP_URL_MAX : 0; memset(job.url, 'a', length); job.url[length] = '\0';
#else
        size_t length = i ? MQTT_TOPIC_MAX : 0; memset(job.topic, 'a', length); job.topic[length] = '\0';
#endif
        quoted[0] = '"'; memset(quoted + 1, 'a', length); quoted[length + 1] = '"'; quoted[length + 2] = '\0';
        snprintf(expected, sizeof(expected), "%s%s%s%s", header, prefix, quoted, suffix);
        int error = invoke(); result(i ? "maximum-field" : "empty-field", !error, expected, error);
    }
}
static void repeat_retry(void)
{
    char seed[1024], retained[1200], complete[1600]; bool ok = true;
    snprintf(seed, sizeof(seed), "%s%s", header, old_row); snprintf(retained, sizeof(retained), "%s170", seed);
    snprintf(complete, sizeof(complete), "%s%s%s%s", seed, prefix, field, suffix);
    for (unsigned i = 0; i < 25; i++) {
        prepare(seed, true); fault = PREFIX; fault_errno = 0; close_error = true;
        ok = invoke() == EIO && !handles && matches(retained) && ok;
        reset_controls(); ok = invoke() == 0 && !handles && matches(complete) && repair_syncs == 1 && ok;
    }
    result("failed-row-repair-retry-25", ok, complete, 0);
}
int main(void)
{
    snprintf(directory, sizeof(directory), ".metadata_log_%d_%s", fixture_pid(), APP);
    snprintf(path, sizeof(path), "%s/LOG.CSV", directory);
    struct stat info; if (stat(directory, &info) == 0 || errno != ENOENT) { fputs("Refusing existing fixture directory\n", stderr); return 2; }
    char seed[1024], partial[1200]; snprintf(seed, sizeof(seed), "%s%s", header, old_row); snprintf(partial, sizeof(partial), "%sunfinished", seed);
    healthy("new-file", NULL, header, true); healthy("new-directory", NULL, header, false);
    healthy("existing-file", seed, seed, true); healthy("partial-row-repair", partial, seed, true); healthy("partial-header-repair", "unix", header, true);
    variants();
    const enum point points[] = {DIR, REPAIR, OPEN, SEEK, TELL, HEADER, PREFIX, SUFFIX, FLUSH, SYNC, CLOSE};
    const char *labels[] = {"directory", "repair", "open", "seek", "size", "header", "prefix", "suffix", "flush", "sync", "close"};
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) for (unsigned missing = 0; missing < 2; missing++) {
        char name[64]; snprintf(name, sizeof(name), "%s-%s", labels[i], missing ? "no-errno" : "error");
        fault_case(name, points[i], 1, missing ? 0 : EACCES, false, false);
    }
    for (unsigned i = 1; i <= strlen(field); i++) for (unsigned missing = 0; missing < 2; missing++) {
        char name[64]; snprintf(name, sizeof(name), "quoted-byte-%u-%s", i, missing ? "no-errno" : "error");
        fault_case(name, QUOTE, i, missing ? 0 : EACCES, false, false);
    }
    for (unsigned i = 0; i < 4; i++) for (unsigned missing = 0; missing < 2; missing++) {
        enum point point = i == 0 ? HEADER : i == 1 ? PREFIX : i == 2 ? QUOTE : SUFFIX;
        char name[64]; snprintf(name, sizeof(name), "stream-flag-%u-%s", i, missing ? "no-errno" : "error");
        fault_case(name, point, point == QUOTE ? 3 : 1, missing ? 0 : EACCES, true, false);
    }
    fault_case("first-prefix-error", PREFIX, 1, EACCES, false, true);
    fault_case("first-quote-no-errno", QUOTE, 3, 0, false, true);
    fault_case("first-suffix-stream-error", SUFFIX, 1, EACCES, true, true);
    repeat_retry(); clean();
    printf("%s %u %s metadata log cases failures=%u handles=%u (native files/storage; controlled clock/SDK types/faults)\n", failures ? "FAIL" : "PASS", cases, APP, failures, handles);
    return failures ? 1 : 0;
}
