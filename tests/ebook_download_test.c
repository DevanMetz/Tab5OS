/* Actual ebook callbacks/storage; native files; controlled HTTP and I/O faults. */
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#define fixture_pid _getpid
#else
#include <unistd.h>
#define fixture_pid getpid
#endif

typedef int esp_err_t;
enum { ESP_OK = 0, ESP_FAIL = -1, HTTP_EVENT_ON_DATA = 1, HTTP_EVENT_ON_FINISH, HTTP_EVENT_DISCONNECTED };
typedef struct { int event_id; void *user_data; void *data; int data_len; } esp_http_client_event_t;
typedef struct {
    const char *url;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    esp_err_t (*crt_bundle_attach)(void *);
    int timeout_ms, buffer_size;
} esp_http_client_config_t;
typedef struct { esp_http_client_config_t config; } client_t;
typedef client_t *esp_http_client_handle_t;
static client_t client;
static unsigned clients, handles, stat_calls, remove_calls, open_calls, write_calls, flush_calls, sync_calls, close_calls, publications;
static unsigned init_calls, perform_calls, cleanup_calls, callbacks, callback_failures, reports, cases, failures;
static int reported_error, status;
static FILE *output_file;
static bool output_error, no_errno, stale_success, full_count, flag_error, close_cleanup_error, remove_cleanup_error;
static int stale_errno;
static char paths[2][128];
static unsigned char body[2051], installed[1025];
static const unsigned char retained[] = "old unpublished ebook\n";
enum point { NONE, PROBE, REMOVE, OPEN, INIT, PERFORM, WRITE, FLUSH, SYNC, CLOSE, PUBLISH_PROBE, PUBLISH };
static enum point fault;
static unsigned fault_at, active_book;
static void supply_errno(int error) { if (!no_errno) errno = error; }
static void leave_stale_errno(void) { if (stale_success) errno = stale_errno; }
#include "ebook_download_types.inc"
#include "ebook_download_config.inc"

static unsigned path_index(const char *path)
{
    static const char *const final[] = {SD_PATH "/BOOKS/ALICE.TXT", SD_PATH "/BOOKS/FRANK.TXT", SD_PATH "/BOOKS/HOLMES.TXT"};
    static const char *const temporary[] = {SD_PATH "/BOOKS/ALICE.TMP", SD_PATH "/BOOKS/FRANK.TMP", SD_PATH "/BOOKS/HOLMES.TMP"};
    assert(active_book < sizeof(final) / sizeof(final[0]));
    if (!strcmp(path, final[active_book])) return 0;
    assert(!strcmp(path, temporary[active_book])); return 1;
}
static int fixture_stat(const char *path, struct stat *info)
{
    unsigned index = path_index(path); stat_calls++;
    if ((!index && fault == PROBE && stat_calls == 1) || (!index && fault == PUBLISH_PROBE && stat_calls == 2)) {
        supply_errno(EACCES); return -1;
    }
    int result = stat(paths[index], info);
    if (!result) leave_stale_errno();
    return result;
}
static int fixture_remove(const char *path)
{
    assert(path_index(path) == 1); remove_calls++;
    if (fault == REMOVE || (remove_cleanup_error && remove_calls > 1)) { supply_errno(EACCES); return -1; }
    int result = remove(paths[1]);
    if (!result) leave_stale_errno();
    return result;
}
static FILE *fixture_open(const char *path, const char *mode)
{
    assert(path_index(path) == 1 && !strcmp(mode, "wb")); open_calls++;
    if (fault == OPEN) { supply_errno(EACCES); return NULL; }
    FILE *file = fopen(paths[1], mode);
    if (file) { handles++; output_file = file; output_error = false; leave_stale_errno(); }
    return file;
}
static size_t fixture_write(const void *data, size_t size, size_t count, FILE *file)
{
    assert(file == output_file && size == 1); write_calls++;
    if (fault == WRITE && write_calls == fault_at) {
        size_t written = fwrite(data, size, full_count ? count : count / 2, file);
        output_error = flag_error; supply_errno(EACCES); return written;
    }
    size_t written = fwrite(data, size, count, file);
    if (!ferror(file)) leave_stale_errno();
    return written;
}
static int fixture_error(FILE *file) { assert(file == output_file); return output_error || ferror(file); }
static int fixture_close(FILE *file)
{
    assert(file == output_file && handles == 1); handles--; close_calls++;
    int result = fclose(file); output_file = NULL;
    if (fault == CLOSE) { supply_errno(EPERM); return EOF; }
    if (close_cleanup_error) { errno = EPERM; return EOF; }
    if (!result) leave_stale_errno();
    return result;
}
static int fixture_flush(FILE *file)
{
    assert(file == output_file); flush_calls++;
    if (fault == FLUSH) { supply_errno(EACCES); return EOF; }
    return fflush(file);
}
static int fixture_sync(int descriptor)
{
    sync_calls++;
    if (fault == SYNC) { supply_errno(EACCES); return -1; }
#ifdef _WIN32
    return _commit(descriptor);
#else
    return fsync(descriptor);
#endif
}
static int fixture_rename(const char *from, const char *to)
{
    assert(path_index(from) == 1 && path_index(to) == 0);
    if (fault == PUBLISH) { supply_errno(EACCES); return -1; }
    int result = rename(paths[1], paths[0]);
    if (!result) { publications++; leave_stale_errno(); }
    return result;
}
static void sd_record_error(int error) { reports++; reported_error = error; errno = EPERM; }
static void fixture_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
#define ESP_LOGE fixture_log
#define ESP_LOGI fixture_log
static esp_err_t esp_crt_bundle_attach(void *config) { (void)config; return ESP_OK; }
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    init_calls++; assert(!clients && config && !strcmp(config->url, ebook_defaults[active_book].url));
    assert(config->timeout_ms == 30000 && config->buffer_size == 1024 && config->crt_bundle_attach == esp_crt_bundle_attach);
    if (fault == INIT) { errno = EBUSY; return NULL; }
    client.config = *config; clients++; errno = EBUSY; return &client;
}
static esp_err_t esp_http_client_set_header(esp_http_client_handle_t handle, const char *key, const char *value)
{
    assert(handle == &client && clients == 1 && !strcmp(key, "User-Agent") && !strcmp(value, "Tab5OS/1.0"));
    errno = EBUSY; return ESP_OK;
}
static esp_err_t send_event(int id, void *data, int length)
{
    esp_http_client_event_t event = {id, client.config.user_data, data, length}; callbacks++;
    esp_err_t result = client.config.event_handler(&event); callback_failures += result != ESP_OK; return result;
}
static esp_err_t esp_http_client_perform(esp_http_client_handle_t handle)
{
    assert(handle == &client && clients == 1); perform_calls++;
    /* The pinned SDK's body parser discards callback return codes. Deliver all chunks. */
    send_event(HTTP_EVENT_ON_DATA, body, 700);
    send_event(HTTP_EVENT_ON_DATA, body + 700, 700);
    send_event(HTTP_EVENT_ON_DATA, body + 1400, 651);
    send_event(HTTP_EVENT_ON_FINISH, NULL, 0);
    errno = EBUSY; return fault == PERFORM ? ESP_FAIL : ESP_OK;
}
static int esp_http_client_get_status_code(esp_http_client_handle_t handle)
{ assert(handle == &client && clients == 1); errno = EBUSY; return status; }
static esp_err_t esp_http_client_cleanup(esp_http_client_handle_t handle)
{
    assert(handle == &client && clients == 1); cleanup_calls++; send_event(HTTP_EVENT_DISCONNECTED, NULL, 0);
    clients--; errno = EBUSY; return ESP_OK;
}
#define stat(path, info) fixture_stat(path, info)
#define remove fixture_remove
#define fopen fixture_open
#define fwrite fixture_write
#define ferror fixture_error
#define fclose fixture_close
#define fflush fixture_flush
#define rename fixture_rename
#ifdef _WIN32
#define _commit fixture_sync
#else
#define fsync fixture_sync
#endif
#include "storage_source.inc"
#include "ebook_download.inc"
#undef stat
#undef remove
#undef fopen
#undef fwrite
#undef ferror
#undef fclose
#undef fflush
#undef rename
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif

static void seed(unsigned index, const unsigned char *data, size_t length)
{
    if (!data) return;
    FILE *file = fopen(paths[index], "wb"); assert(file);
    assert(fwrite(data, 1, length, file) == length && fclose(file) == 0);
}
static bool matches(unsigned index, const unsigned char *data, size_t length)
{
    FILE *file = fopen(paths[index], "rb");
    if (!data) { if (file) fclose(file); return !file && errno == ENOENT; }
    if (!file) return false;
    unsigned char bytes[4096]; size_t count = fread(bytes, 1, sizeof(bytes), file);
    bool ok = !ferror(file) && count == length && !memcmp(bytes, data, length);
    assert(fclose(file) == 0); return ok;
}
static void prepare(const unsigned char *original, size_t length)
{
    assert(!clients && !handles);
    for (unsigned i = 0; i < 2; i++) if (remove(paths[i]) != 0) assert(errno == ENOENT);
    seed(0, original, length); seed(1, retained, sizeof(retained) - 1);
    clients = handles = stat_calls = remove_calls = open_calls = write_calls = flush_calls = sync_calls = close_calls = publications = 0;
    init_calls = perform_calls = cleanup_calls = callbacks = callback_failures = reports = 0;
    output_file = NULL; output_error = no_errno = full_count = flag_error = close_cleanup_error = remove_cleanup_error = false;
    stale_success = true; stale_errno = ENOENT; fault = NONE; fault_at = 1; active_book = 0; reported_error = 0; status = 200; errno = ENOENT;
}
static void report(const char *name, bool pass, bool result)
{
    pass = pass && !clients && !handles; cases++; failures += !pass;
    printf("%s %s saved=%u error=%d reports=%u stat=%u removes=%u opens=%u writes=%u flushes=%u syncs=%u closes=%u published=%u init=%u perform=%u cleanup=%u events=%u callback_failures=%u clients=%u handles=%u\n",
        pass ? "PASS" : "FAIL", name, result, reported_error, reports, stat_calls, remove_calls, open_calls, write_calls,
        flush_calls, sync_calls, close_calls, publications, init_calls, perform_calls, cleanup_calls, callbacks, callback_failures, clients, handles);
}
static void healthy(const char *name, const unsigned char *original, size_t length, int response, unsigned book)
{
    prepare(original, length); status = response; active_book = book; bool result = ebook_download_default(&ebook_defaults[book]);
    bool present = original && length > 1024;
    bool pass = result && !reports && matches(0, present ? original : body, present ? length : sizeof(body));
    pass = pass && matches(1, present ? retained : NULL, present ? sizeof(retained) - 1 : 0);
    pass = pass && (present ? !init_calls && !open_calls : publications == 1 && write_calls == 3 && callbacks == 5 && cleanup_calls == 1);
    report(name, pass, result);
}
static void failed_download(const char *name, enum point point, bool missing, bool cleanup, int response)
{
    prepare(NULL, 0); fault = point; no_errno = missing; close_cleanup_error = cleanup; status = response;
    bool result = ebook_download_default(&ebook_defaults[0]);
    bool network_failed = point == INIT || point == PERFORM || response < 200 || response >= 300;
    int expected = point == CLOSE ? (missing ? EIO : EPERM) : network_failed ? (cleanup ? EPERM : 0) : missing ? EIO : EACCES;
    bool pass = !result && !publications && reports == (unsigned)(expected != 0) && reported_error == expected && matches(0, NULL, 0);
    bool prepared = point == PROBE || point == REMOVE;
    bool keep = point == FLUSH || point == SYNC || (point == CLOSE && !network_failed) || point == PUBLISH_PROBE || point == PUBLISH;
    pass = pass && matches(1, prepared ? retained : keep ? body : NULL, prepared ? sizeof(retained) - 1 : keep ? sizeof(body) : 0);
    if (prepared) pass = pass && !open_calls && !init_calls && !write_calls;
    if (point == PROBE) pass = pass && !remove_calls;
    if (network_failed) pass = pass && !flush_calls && !sync_calls;
    report(name, pass, result);
}
static void failed_write(const char *name, unsigned at, bool missing, bool full, bool flagged, bool cleanup)
{
    prepare(NULL, 0); fault = WRITE; fault_at = at; no_errno = missing; full_count = full; flag_error = flagged;
    close_cleanup_error = remove_cleanup_error = cleanup; stale_errno = EBUSY;
    bool result = ebook_download_default(&ebook_defaults[0]);
    size_t prefix = (at - 1) * 700 + (full ? 700 : 350);
    bool pass = !result && reports == 1 && reported_error == (missing ? EIO : EACCES) && !publications;
    pass = pass && write_calls == at && callbacks == 5 && callback_failures == 4 - at && close_calls == 1 && !flush_calls && !sync_calls;
    pass = pass && matches(0, NULL, 0) && matches(1, cleanup ? body : NULL, cleanup ? prefix : 0);
    report(name, pass, result);
}
static void failed_write_retry(void)
{
    bool pass = true;
    for (unsigned i = 0; i < 25; i++) {
        prepare(NULL, 0); fault = WRITE; no_errno = full_count = flag_error = close_cleanup_error = remove_cleanup_error = true;
        stale_errno = EBUSY; bool rejected = ebook_download_default(&ebook_defaults[0]);
        pass = pass && !rejected && reports == 1 && reported_error == EIO && !clients && !handles && write_calls == 1 && !publications;
        pass = pass && matches(0, NULL, 0) && matches(1, body, 700);
        fault = NONE; no_errno = full_count = flag_error = close_cleanup_error = remove_cleanup_error = false;
        bool saved = ebook_download_default(&ebook_defaults[0]);
        pass = pass && saved && !clients && !handles && publications == 1 && reports == 1 && matches(0, body, sizeof(body)) && matches(1, NULL, 0);
        unsigned previous_inits = init_calls; saved = ebook_download_default(&ebook_defaults[0]);
        pass = pass && saved && init_calls == previous_inits && matches(0, body, sizeof(body)) && !clients && !handles;
    }
    report("write-failure-retry-25", pass, true);
}
static void failed_existing_probe(bool missing)
{
    prepare(installed, sizeof(installed)); fault = PROBE; no_errno = missing;
    bool saved = ebook_download_default(&ebook_defaults[0]);
    report(missing ? "existing-probe-no-errno" : "existing-probe-error",
           !saved && reports == 1 && reported_error == (missing ? EIO : EACCES) && !remove_calls && !open_calls && !init_calls &&
           matches(0, installed, sizeof(installed)) && matches(1, retained, sizeof(retained) - 1), saved);
}
static void failed_network_cleanup(const char *name, enum point point, int response, bool missing)
{
    prepare(NULL, 0); fault = point; status = response; no_errno = missing; remove_cleanup_error = true;
    bool saved = ebook_download_default(&ebook_defaults[0]);
    int expected = missing ? EIO : point == CLOSE ? EPERM : EACCES;
    report(name, !saved && reports == 1 && reported_error == expected && !publications && !flush_calls && !sync_calls && close_calls == 1 &&
           matches(0, NULL, 0) && matches(1, point == INIT ? (const unsigned char *)"" : body, point == INIT ? 0 : sizeof(body)), saved);
}
int main(void)
{
    for (unsigned i = 0; i < 2; i++) {
        assert(snprintf(paths[i], sizeof(paths[i]), ".ebook_download_%ld_%u", (long)fixture_pid(), i) > 0);
        struct stat info; assert(stat(paths[i], &info) != 0 && errno == ENOENT);
    }
    for (size_t i = 0; i < sizeof(body); i++) body[i] = (unsigned char)(i % 251);
    memset(installed, 'x', sizeof(installed));
    healthy("alice-download-200", NULL, 0, 200, 0); healthy("download-299", NULL, 0, 299, 0);
    healthy("frank-download", NULL, 0, 200, 1); healthy("holmes-download", NULL, 0, 200, 2);
    healthy("installed-1025", installed, sizeof(installed), 200, 0);
    failed_existing_probe(false); failed_existing_probe(true);
    prepare(installed, 1024); bool result = ebook_download_default(&ebook_defaults[0]);
    report("existing-1024-not-replaced", !result && reports == 1 && reported_error == EEXIST && !publications && matches(0, installed, 1024) && matches(1, body, sizeof(body)), result);
    const struct { const char *name; enum point point; } points[] = {
        {"initial-probe", PROBE}, {"temporary-removal", REMOVE}, {"writer-open", OPEN},
        {"flush", FLUSH}, {"sync", SYNC}, {"close", CLOSE}, {"publish-probe", PUBLISH_PROBE}, {"publish", PUBLISH}};
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) for (unsigned missing = 0; missing < 2; missing++) {
        char name[96]; assert(snprintf(name, sizeof(name), "%s-%s", points[i].name, missing ? "no-errno" : "error") > 0);
        failed_download(name, points[i].point, missing != 0, false, 200);
    }
    failed_download("client-init-failure", INIT, false, false, 200);
    failed_download("client-init-close-failure", INIT, false, true, 200);
    failed_download("transport-failure", PERFORM, false, false, 200);
    failed_download("transport-close-failure", PERFORM, false, true, 200);
    failed_download("status-404", NONE, false, false, 404);
    failed_download("status-199", NONE, false, false, 199);
    failed_download("status-300", NONE, false, false, 300);
    failed_download("status-404-close-failure", NONE, false, true, 404);
    failed_download("status-404-close-no-errno", CLOSE, true, false, 404);
    failed_network_cleanup("init-remove-cleanup-error", INIT, 200, false);
    failed_network_cleanup("init-remove-cleanup-no-errno", INIT, 200, true);
    failed_network_cleanup("transport-remove-cleanup-error", PERFORM, 200, false);
    failed_network_cleanup("transport-remove-cleanup-no-errno", PERFORM, 200, true);
    failed_network_cleanup("status-remove-cleanup-error", NONE, 404, false);
    failed_network_cleanup("status-remove-cleanup-no-errno", NONE, 404, true);
    failed_network_cleanup("first-close-remove-cleanup-error", CLOSE, 404, false);
    failed_network_cleanup("first-close-remove-cleanup-no-errno", CLOSE, 404, true);
    for (unsigned at = 1; at < 3; at++) for (unsigned missing = 0; missing < 2; missing++) {
        for (unsigned style = 0; style < 3; style++) {
            char name[96]; assert(snprintf(name, sizeof(name), "write-%u-%s-%s", at,
                style == 0 ? "short-no-flag" : style == 1 ? "short-flag" : "full-flag", missing ? "no-errno" : "error") > 0);
            failed_write(name, at, missing != 0, style == 2, style != 0, false);
        }
    }
    failed_write("first-short-write-cleanup", 1, false, false, true, true);
    failed_write("first-short-write-no-errno-cleanup", 1, true, false, true, true);
    failed_write("first-full-write-cleanup", 2, false, true, true, true);
    failed_write("first-full-write-no-errno-cleanup", 2, true, true, true, true);
    failed_write_retry();
    for (unsigned i = 0; i < 2; i++) if (remove(paths[i]) != 0) assert(errno == ENOENT);
    printf("%s %u ebook download cases failures=%u clients=%u handles=%u (native files; controlled HTTP/I/O)\n", failures ? "FAIL" : "PASS", cases, failures, clients, handles);
    return failures ? 1 : 0;
}
