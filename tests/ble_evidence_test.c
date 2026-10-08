/* Actual snapshot/status/formatters/storage; native files; controlled BLE boundaries. */
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
#define native_access _access
#define native_open _open
#define native_fdopen _fdopen
#define native_close _close
#define native_rmdir _rmdir
#ifndef F_OK
#define F_OK 0
#endif
#else
#include <unistd.h>
#define fixture_pid getpid
#define native_access access
#define native_open open
#define native_fdopen fdopen
#define native_close close
#define native_rmdir rmdir
#endif
#include "ble_evidence_config.inc"
/* Only the cache shapes and 16-bit UUID conversion used here are modeled. */
typedef struct { uint8_t type, val[6]; } ble_addr_t;
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
typedef union { ble_uuid_t u; ble_uuid16_t u16; uint8_t opaque[20]; } ble_uuid_any_t;
#define BLE_UUID_STR_LEN 37
#define BLE_GATT_CHR_PROP_READ 0x02
#define BLE_GATT_CHR_PROP_WRITE 0x08
#define BLE_GATT_CHR_PROP_WRITE_NO_RSP 0x04
#define BLE_GATT_CHR_PROP_NOTIFY 0x10
#define BLE_GATT_CHR_PROP_INDICATE 0x20
#include "ble_evidence_types.inc"
typedef struct { int unused; } lv_event_t;
static ble_tool_device_t devices[BLE_TOOL_DEVICE_MAX];
static ble_tool_service_t services[BLE_TOOL_SERVICE_MAX];
static ble_tool_characteristic_t characteristics[BLE_TOOL_CHARACTERISTIC_MAX];
static size_t device_count, service_count, characteristic_count, last_value_length, last_value_total;
static uint8_t last_value[BLE_TOOL_VALUE_MAX];
static uint16_t last_value_handle;
static bool last_value_seen, last_value_was_notification, last_value_was_indication, last_value_copy_failed;
static int selected_device, tool_lock;
static bool connected, stopping, discovering, screen_sd_available, status_dirty;
static char status_text[192];
static unsigned lock_depth;
static bool mutate_after_snapshot;
static void fixture_enter(const void *lock) { assert(lock == &tool_lock && !lock_depth); lock_depth++; }
static void fixture_exit(const void *lock)
{
    assert(lock == &tool_lock && lock_depth == 1); lock_depth--;
    if (mutate_after_snapshot) {
        mutate_after_snapshot = false; memset(devices, 0, sizeof(devices)); memset(services, 0, sizeof(services));
        memset(characteristics, 0, sizeof(characteristics)); memset(last_value, 0, sizeof(last_value));
        device_count = service_count = characteristic_count = last_value_length = 0; selected_device = -1;
    }
}
#define portENTER_CRITICAL(lock) fixture_enter(lock)
#define portEXIT_CRITICAL(lock) fixture_exit(lock)
static char *ble_uuid_to_str(const ble_uuid_t *uuid, char *out)
{ assert(uuid->type == 16); snprintf(out, BLE_UUID_STR_LEN, "0x%04x", ((const ble_uuid16_t *)uuid)->value); errno = EBUSY; return out; }
enum point { NONE, ROOT, DATE, PROBE, OPEN, FDOPEN, WRITE, FLUSH, SYNC, CLOSE, PUBLISH, POINTS };
static enum point fault;
static unsigned failed_call, calls[POINTS], handles, descriptors, publications, reports, cases, failures;
static int fault_errno, reported_error, active_slot = -1;
static bool flagged, close_error, unlink_error, stale_success;
static FILE *active;
static char logical[20][48], paths[20][256], directories[2][96];
static bool owned[20], owned_directories[2];
static void report_error(int error) { assert(!lock_depth); reports++; reported_error = error; errno = EPERM; }
static void (*report_storage_error)(int) = report_error;
static bool inject(enum point point)
{
    calls[point]++;
    if (fault != point || calls[point] != failed_call) return false;
    if (fault_errno) errno = fault_errno;
    return true;
}
static unsigned slot(const char *path)
{ for (unsigned i = 0; i < 20; i++) if (!strcmp(path, logical[i])) return i; fprintf(stderr, "Unknown path: %s\n", path); abort(); }
static int fixture_mkdir(const char *path, int mode)
{
    unsigned d = !strcmp(path, "/sdcard/BLE/231114"); assert(mode == 0775 && !strcmp(path, d ? "/sdcard/BLE/231114" : "/sdcard/BLE"));
    if (inject(d ? DATE : ROOT)) return -1;
#ifdef _WIN32
    int result = _mkdir(directories[d]);
#else
    int result = mkdir(directories[d], (mode_t)mode);
#endif
    if (!result) owned_directories[d] = true;
    return result;
}
static int fixture_access(const char *path, int mode)
{
    assert(mode == F_OK); unsigned i = slot(path); assert(i >= 10);
    if (inject(PROBE)) return -1;
    int result = native_access(paths[i], mode);
    if (!result && stale_success) errno = ENOENT;
    return result;
}
static int fixture_stat(const char *path, struct stat *info) { return stat(paths[slot(path)], info); }
static int fixture_open(const char *path, int flags, int mode)
{
    unsigned i = slot(path); assert(i < 10 && flags == (O_WRONLY | O_CREAT | O_EXCL) && mode == 0664 && !handles && !descriptors);
    if (inject(OPEN)) return -1;
#ifdef _WIN32
    int fd = native_open(paths[i], flags | _O_BINARY, mode);
#else
    int fd = native_open(paths[i], flags, mode);
#endif
    if (fd >= 0) { descriptors++; active_slot = (int)i; owned[i] = true; }
    return fd;
}
static FILE *fixture_fdopen(int fd, const char *mode)
{
    assert(descriptors == 1 && active_slot >= 0 && !strcmp(mode, "wb"));
    if (inject(FDOPEN)) return NULL;
    FILE *file = native_fdopen(fd, mode);
    if (file) { descriptors--; handles++; active = file; }
    return file;
}
static int fixture_close_fd(int fd)
{
    assert(descriptors == 1); descriptors--; int result = native_close(fd); active_slot = -1;
    if (close_error) { errno = EPERM; return -1; }
    return result;
}
static int fixture_unlink(const char *path)
{
    unsigned i = slot(path); assert(owned[i]);
    if (unlink_error) { errno = EACCES; return -1; }
    int result = remove(paths[i]); if (!result) owned[i] = false;
    return result;
}
static int fixture_rename(const char *from, const char *to)
{
    unsigned a = slot(from), b = slot(to); assert(a < 10 && b == a + 10 && owned[a] && !owned[b]);
    if (inject(PUBLISH)) return -1;
    int result = rename(paths[a], paths[b]);
    if (!result) { owned[a] = false; owned[b] = true; publications++; }
    return result;
}
static int fixture_printf(FILE *file, const char *format, ...)
{
    assert(file == active);
    if (inject(WRITE)) { assert(fwrite("PART", 1, 4, file) == 4); return -1; }
    va_list args; va_start(args, format); int result = vfprintf(file, format, args); va_end(args);
    if (stale_success) errno = ENOSPC;
    return result;
}
static int fixture_ferror(FILE *file) { assert(file == active); return flagged || ferror(file); }
static int fixture_flush(FILE *file)
{ assert(file == active); int result = fflush(file); return inject(FLUSH) ? EOF : result; }
static int fixture_sync(int fd)
{
#ifdef _WIN32
    int result = _commit(fd);
#else
    int result = fsync(fd);
#endif
    return inject(SYNC) ? -1 : result;
}
static int fixture_close(FILE *file)
{
    assert(file == active && handles == 1); int result = fclose(file); handles--; active = NULL; active_slot = -1;
    if (inject(CLOSE) || close_error) { errno = fault == CLOSE && fault_errno ? fault_errno : EPERM; return EOF; }
    return result;
}
static time_t fixture_time(time_t *out) { if (out) *out = 1700000000; return 1700000000; }
static struct tm *fixture_localtime(const time_t *value, struct tm *out)
{
#ifdef _WIN32
    return gmtime_s(out, value) ? NULL : out;
#else
    return gmtime_r(value, out);
#endif
}
#define stat(path, info) fixture_stat(path, info)
#define mkdir fixture_mkdir
#define access fixture_access
#define open fixture_open
#define fdopen fixture_fdopen
#define close fixture_close_fd
#define unlink fixture_unlink
#define rename fixture_rename
#define fprintf fixture_printf
#define ferror fixture_ferror
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
#include "ble_evidence.inc"
#undef stat
#undef mkdir
#undef access
#undef open
#undef fdopen
#undef close
#undef unlink
#undef rename
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
static const char *lines[] = {
    "unix_time,event,address,name,rssi,service_uuid,characteristic_uuid,handle,properties,value_hex\n",
    "1700000000,advertisement,06:05:04:03:02:01,Sensor____,-40,,,,,\n",
    "1700000000,characteristic,06:05:04:03:02:01,Sensor____,,0x180d,0x2a37,0x002A,RWwNI,\n",
    "1700000000,read,06:05:04:03:02:01,Sensor____,,,,0x002A,,00 7F FF\n",
};
static char complete[8192];
static bool matches(unsigned i, const char *expected)
{
    FILE *file = fopen(paths[i], "rb");
    if (!expected) { if (file) fclose(file); return !file && errno == ENOENT; }
    if (!file) return false;
    char bytes[8192]; size_t length = fread(bytes, 1, sizeof(bytes), file);
    bool ok = !ferror(file) && length == strlen(expected) && !memcmp(bytes, expected, length);
    return fclose(file) == 0 && ok;
}
static void seed(unsigned i, const char *bytes)
{ assert(!owned[i]); FILE *file = fopen(paths[i], "wb"); assert(file); owned[i] = true; assert(fwrite(bytes, 1, strlen(bytes), file) == strlen(bytes)); assert(fclose(file) == 0); }
static void clean(void)
{ assert(!handles && !descriptors && !active); for (unsigned i = 0; i < 20; i++) if (owned[i]) { assert(remove(paths[i]) == 0); owned[i] = false; } }
static void prepare(enum point point, int error)
{
    clean(); fault = point; fault_errno = error; failed_call = 1; memset(calls, 0, sizeof(calls));
    publications = reports = 0; reported_error = 0; lock_depth = 0; close_error = unlink_error = stale_success = flagged = mutate_after_snapshot = false;
    connected = screen_sd_available = true; stopping = discovering = status_dirty = false; status_text[0] = '\0';
    memset(devices, 0, sizeof(devices)); memset(services, 0, sizeof(services)); memset(characteristics, 0, sizeof(characteristics));
    device_count = service_count = characteristic_count = 1; selected_device = 0;
    devices[0].address = (ble_addr_t){0, {1, 2, 3, 4, 5, 6}}; devices[0].rssi = -40; strcpy(devices[0].name, "Sensor,\"\r\n");
    services[0].uuid.u16 = (ble_uuid16_t){{16}, 0x180d}; characteristics[0].uuid.u16 = (ble_uuid16_t){{16}, 0x2a37};
    characteristics[0].val_handle = 42; characteristics[0].properties = 0x3e;
    last_value_length = last_value_total = 3; last_value_handle = 42; last_value_seen = true;
    last_value_was_notification = last_value_was_indication = last_value_copy_failed = false;
    last_value[0] = 0; last_value[1] = 0x7f; last_value[2] = 0xff; errno = EBUSY;
}
static bool failed(int error)
{ return !handles && !descriptors && !active && !lock_depth && !publications && reports == 1 && reported_error == error && status_dirty && !strstr(status_text, "saved to") && strstr(status_text, strerror(error)); }
static void result(const char *name, bool ok, bool bytes_ok)
{
    ok = ok && bytes_ok && !handles && !descriptors && !active && !lock_depth;
    printf("%s %s bytes_verified=%u published=%u reports=%u error=%d roots=%u dates=%u probes=%u opens=%u fdopens=%u writes=%u flushes=%u syncs=%u closes=%u handles=%u descriptors=%u\n",
           ok ? "PASS" : "FAIL", name, bytes_ok, publications, reports, reported_error, calls[ROOT], calls[DATE], calls[PROBE], calls[OPEN], calls[FDOPEN], calls[WRITE], calls[FLUSH], calls[SYNC], calls[CLOSE], handles, descriptors);
    cases++; failures += !ok;
}
static bool absent_except(unsigned kept, const char *bytes)
{ bool ok = true; for (unsigned i = 0; i < 20; i++) ok = matches(i, i == kept ? bytes : NULL) && ok; return ok; }
static void creation_case(const char *name, enum point point, int error, bool cleanup_error)
{
    prepare(point, error); close_error = unlink_error = cleanup_error; save_evidence_clicked(NULL);
    bool ok = failed(error ? error : EIO) && !calls[WRITE];
    if (point == ROOT) ok = ok && !calls[DATE] && !calls[OPEN];
    if (point == PROBE) ok = ok && calls[PROBE] == 1 && !calls[OPEN];
    if (point == OPEN) ok = ok && calls[OPEN] == 1 && !calls[FDOPEN];
    result(name, ok, absent_except(cleanup_error ? 0 : 20, cleanup_error ? "" : NULL));
}
static void write_case(const char *name, unsigned stage, int error, bool cleanup_error, bool stream_flag)
{
    prepare(WRITE, error); failed_call = stage; close_error = cleanup_error; flagged = stream_flag; stale_success = !error;
    save_evidence_clicked(NULL); char expected[8192] = "";
    for (unsigned i = 0; i < stage - 1; i++) strcat(expected, lines[i]);
    strcat(expected, "PART");
    result(name, failed(error ? error : EIO) && calls[WRITE] == stage && !calls[FLUSH] && !calls[SYNC] && strstr(status_text, "B2213200.TMP"), absent_except(0, expected));
}
static void commit_case(const char *name, enum point point, int error, bool cleanup_error)
{
    prepare(point, error); close_error = cleanup_error; save_evidence_clicked(NULL);
    result(name, failed(error) && calls[WRITE] == 4 && strstr(status_text, "B2213200.TMP"), absent_except(0, complete));
}
static void success_case(const char *name, unsigned mode)
{
    prepare(NONE, 0); char expected[8192] = "";
    if (mode == 1) last_value_was_notification = true;
    if (mode == 2) last_value_was_indication = last_value_was_notification = true;
    if (mode == 3) last_value_total = 4;
    if (mode == 4) { last_value_copy_failed = true; last_value_length = 0; }
    if (mode == 5) last_value_length = last_value_total = 0;
    if (mode == 6) last_value_seen = false;
    if (mode == 7) selected_device = -1;
    if (mode == 8) characteristics[0].service_index = 1;
    if (mode == 9) mutate_after_snapshot = true;
    strcat(expected, lines[0]); strcat(expected, lines[1]);
    if (mode != 8) strcat(expected, mode == 7 ? "1700000000,characteristic,,,,0x180d,0x2a37,0x002A,RWwNI,\n" : lines[2]);
    if (mode != 6) {
        const char *event = mode == 2 ? "indication" : mode == 1 ? "notification" : "read";
        const char *peer = mode == 7 ? "," : "06:05:04:03:02:01,Sensor____";
        char value[256]; snprintf(value, sizeof(value), "1700000000,%s,%s,,,,0x002A,,%s%s\n", event, peer,
                                  mode == 4 || mode == 5 ? "" : "00 7F FF", mode == 4 ? "[unavailable]" : mode == 3 ? " ..." : "");
        strcat(expected, value);
    }
    save_evidence_clicked(NULL);
    result(name, publications == 1 && !reports && status_dirty && strstr(status_text, "saved to /sdcard/BLE/231114/B2213200.CSV"), absent_except(10, expected));
}
static void maximum_case(void)
{
    prepare(NONE, 0); device_count = 8; service_count = 16; characteristic_count = 32; last_value_length = last_value_total = 64;
    for (unsigned i = 1; i < 8; i++) devices[i] = devices[0];
    for (unsigned i = 1; i < 16; i++) services[i] = services[0];
    for (unsigned i = 1; i < 32; i++) { characteristics[i] = characteristics[0]; characteristics[i].service_index = (uint8_t)(i / 2); }
    char expected[8192] = ""; strcat(expected, lines[0]);
    for (unsigned i = 0; i < 8; i++) strcat(expected, lines[1]);
    for (unsigned i = 0; i < 32; i++) strcat(expected, lines[2]);
    strcat(expected, "1700000000,read,06:05:04:03:02:01,Sensor____,,,,0x002A,,");
    for (unsigned i = 0; i < 64; i++) { last_value[i] = (uint8_t)i; char byte[4]; snprintf(byte, sizeof(byte), "%02X", i); if (i) strcat(expected, " "); strcat(expected, byte); }
    strcat(expected, "\n"); save_evidence_clicked(NULL);
    result("bounded-maximum", publications == 1 && !reports && calls[WRITE] == 42, absent_except(10, expected));
}
static void collisions(void)
{
    prepare(NONE, 0); seed(10, "KEEP"); seed(1, "OLD-PART"); save_evidence_clicked(NULL);
    bool bytes_ok = matches(10, "KEEP") && matches(1, "OLD-PART") && matches(12, complete);
    for (unsigned i = 0; i < 20; i++) if (i != 10 && i != 1 && i != 12) bytes_ok = matches(i, NULL) && bytes_ok;
    result("collision-recovery", publications == 1 && !reports && calls[PROBE] == 3 && calls[OPEN] == 2, bytes_ok);
    for (unsigned final = 0; final < 2; final++) {
        prepare(NONE, 0); stale_success = true;
        for (unsigned i = 0; i < 10; i++) seed(i + final * 10, "KEEP");
        save_evidence_clicked(NULL); bytes_ok = true;
        for (unsigned i = 0; i < 10; i++) bytes_ok = matches(i + final * 10, "KEEP") && matches(i + (1 - final) * 10, NULL) && bytes_ok;
        result(final ? "final-names-full" : "temporary-names-full", failed(EEXIST) && calls[PROBE] == 10 && calls[OPEN] == (final ? 0 : 10), bytes_ok);
    }
}
static void recovery_case(unsigned count)
{
    bool ok = true, bytes_ok = true;
    for (unsigned i = 0; i < count; i++) {
        prepare(WRITE, ENOSPC); save_evidence_clicked(NULL); ok = failed(ENOSPC) && ok;
        bytes_ok = absent_except(0, "PART") && bytes_ok; fault = NONE; save_evidence_clicked(NULL);
        ok = reports == 1 && publications == 1 && !handles && !descriptors && !lock_depth && ok;
        bytes_ok = matches(0, "PART") && matches(11, complete) && matches(10, NULL) && matches(1, NULL) && bytes_ok;
    }
    result(count == 1 ? "header-restart" : "header-restart-25", ok, bytes_ok);
}
int main(void)
{
    snprintf(directories[0], sizeof(directories[0]), "ble_evidence_%d", fixture_pid());
    snprintf(directories[1], sizeof(directories[1]), "ble_evidence_%d/231114", fixture_pid());
    for (unsigned d = 0; d < 2; d++) { struct stat info; if (stat(directories[d], &info) == 0 || errno != ENOENT) { fputs("Refusing existing fixture directory\n", stderr); return 2; } }
    for (unsigned i = 0; i < 10; i++) for (unsigned final = 0; final < 2; final++) {
        unsigned n = i + final * 10;
        snprintf(logical[n], sizeof(logical[n]), "/sdcard/BLE/231114/B221320%u.%s", i, final ? "CSV" : "TMP");
        snprintf(paths[n], sizeof(paths[n]), "%s/B221320%u.%s", directories[1], i, final ? "CSV" : "TMP");
    }
    for (unsigned i = 0; i < 4; i++) strcat(complete, lines[i]);
    const char *names[] = {"success", "notification", "indication", "truncated-value", "unavailable-value", "empty-value", "no-value", "no-peer", "invalid-service", "snapshot-isolation"};
    for (unsigned i = 0; i < 10; i++) success_case(names[i], i);
    maximum_case();
    creation_case("root-error", ROOT, EACCES, false); creation_case("root-no-errno", ROOT, 0, false);
    creation_case("date-error", DATE, EACCES, false); creation_case("date-no-errno", DATE, 0, false);
    creation_case("probe-error", PROBE, EACCES, false); creation_case("probe-no-errno", PROBE, 0, false);
    creation_case("open-error", OPEN, ENOSPC, false); creation_case("open-no-errno", OPEN, 0, false);
    creation_case("fdopen-error", FDOPEN, ENOMEM, false); creation_case("fdopen-no-errno", FDOPEN, 0, false); creation_case("first-fdopen-error", FDOPEN, ENOMEM, true);
    for (unsigned stage = 1; stage <= 4; stage++) {
        for (unsigned mode = 0; mode < 3; mode++) { char name[48]; snprintf(name, sizeof(name), "write-%u-%s", stage, mode == 0 ? "error" : mode == 1 ? "no-errno" : "close-error"); write_case(name, stage, mode == 1 ? 0 : ENOSPC, mode == 2, false); }
    }
    write_case("header-stream-flag", 1, ENOSPC, false, true); write_case("value-stream-flag", 4, ENOSPC, true, true);
    commit_case("commit-flush", FLUSH, EIO, false); commit_case("commit-sync", SYNC, EIO, false);
    commit_case("commit-close", CLOSE, EACCES, false); commit_case("commit-publish", PUBLISH, EACCES, false); commit_case("first-sync-error", SYNC, EACCES, true);
    for (unsigned admission = 0; admission < 4; admission++) {
        prepare(NONE, 0);
        if (admission == 0) screen_sd_available = false;
        if (admission == 1) connected = false;
        if (admission == 2) discovering = true;
        if (admission == 3) stopping = true;
        save_evidence_clicked(NULL); char name[32]; snprintf(name, sizeof(name), "admission-%u", admission);
        result(name, !reports && !publications && !calls[ROOT] && status_dirty, absent_except(20, NULL));
    }
    collisions(); recovery_case(1); recovery_case(25);
    clean(); for (int d = 1; d >= 0; d--) if (owned_directories[d]) { assert(native_rmdir(directories[d]) == 0); owned_directories[d] = false; }
    printf("%s %u BLE evidence cases failures=%u handles=%u descriptors=%u (native files/storage; controlled BLE/time/locks/faults)\n", failures ? "FAIL" : "PASS", cases, failures, handles, descriptors);
    return failures ? 1 : 0;
}
