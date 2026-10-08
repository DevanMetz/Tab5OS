/* Actual serial log/storage functions; native files, controlled UI/time/faults. */
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
#include "serial_log_config.inc"
enum point { NONE, ROOT_DIR, DATE_DIR, STAT, OPEN, FDOPEN, HEADER, ROW, NEWLINE, FLUSH, SYNC, CLOSE, PUBLISH, POINTS };
static enum point failed_point;
static unsigned failed_call, calls[POINTS];
static int failed_errno, reported_error;
static bool flagged, fail_close, tick_clobber, stale_success;
static bool rs485_interface, sd_available;
static FILE *log_file, *active;
static char log_temporary_path[96], log_final_path[96], status_text[192];
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
static TickType_t tick, log_last_flush_tick;
static unsigned reports, handles, descriptors, publications, controls, cases, failures;
static char directories[2][2][96], logical[2][10][2][96], paths[2][10][2][160];
static bool owned_directories[2][2], owned[2][10][2];
static const char *header = "unix_time,direction,data_hex\n";
static const char *row = "1700000000,RX,00 A5 FF\n";
static const uint8_t bytes[] = {0, 0xA5, 0xFF};

static void report_error(int error) { reports++; reported_error = error; }
static void (*storage_error_cb)(int) = report_error;
static void update_controls(void) { controls++; }
static void set_status(const char *format, ...)
{
    va_list args; va_start(args, format);
    int length = vsnprintf(status_text, sizeof(status_text), format, args); va_end(args);
    assert(length >= 0 && (size_t)length < sizeof(status_text));
}
static time_t fixture_time(time_t *result) { time_t now = 1700000000; if (result) *result = now; return now; }
static struct tm *fixture_localtime(const time_t *value, struct tm *result)
{
#ifdef _WIN32
    return gmtime_s(result, value) ? NULL : result;
#else
    return gmtime_r(value, result);
#endif
}
static TickType_t xTaskGetTickCount(void) { if (tick_clobber) errno = EBUSY; return tick; }
static bool inject(enum point point)
{
    calls[point]++;
    if (failed_point != point || calls[point] != failed_call) return false;
    if (failed_errno) errno = failed_errno;
    return true;
}
static void locate(const char *path, unsigned *link, unsigned *suffix, unsigned *final)
{
    for (unsigned m = 0; m < 2; m++) for (unsigned s = 0; s < 10; s++) for (unsigned f = 0; f < 2; f++)
        if (!strcmp(path, logical[m][s][f])) { *link = m; *suffix = s; *final = f; return; }
    fprintf(stderr, "Unexpected logical path: %s\n", path); abort();
}
static const char *native_path(const char *path)
{ unsigned m, s, f; locate(path, &m, &s, &f); return paths[m][s][f]; }
static int fixture_stat(const char *path, struct stat *info)
{
    if (inject(STAT)) return -1;
    int result = stat(native_path(path), info);
    if (!result && stale_success) errno = ENOENT;
    return result;
}
static int fixture_mkdir(const char *path, int mode)
{
    unsigned m = rs485_interface, d = !strcmp(path, m ? RS485_TOOL_PATH "/231114" : UART_TOOL_PATH "/231114");
    assert(mode == 0775 && !strcmp(path, d ? (m ? RS485_TOOL_PATH "/231114" : UART_TOOL_PATH "/231114") :
                                           (m ? RS485_TOOL_PATH : UART_TOOL_PATH)));
    if (inject(d ? DATE_DIR : ROOT_DIR)) return -1;
#ifdef _WIN32
    int result = _mkdir(directories[m][d]);
#else
    int result = mkdir(directories[m][d], (mode_t)mode);
#endif
    if (!result) owned_directories[m][d] = true;
    return result;
}
static int fixture_open(const char *path, int flags, int mode)
{
    unsigned m, s, f; locate(path, &m, &s, &f);
    assert(!f && flags == (O_WRONLY | O_CREAT | O_EXCL) && mode == 0664);
    if (inject(OPEN)) return -1;
#ifdef _WIN32
    int fd = native_open(paths[m][s][f], flags | _O_BINARY, mode);
#else
    int fd = native_open(paths[m][s][f], flags, mode);
#endif
    if (fd >= 0) { descriptors++; owned[m][s][f] = true; }
    return fd;
}
static FILE *fixture_fdopen(int fd, const char *mode)
{
    assert(descriptors == 1 && !active && !strcmp(mode, "wb"));
    if (inject(FDOPEN)) return NULL;
    FILE *file = native_fdopen(fd, mode);
    if (file) { descriptors--; handles++; active = file; flagged = false; }
    return file;
}
static int fixture_close_fd(int fd) { assert(descriptors == 1); descriptors--; return native_close(fd); }
static int fixture_remove(const char *path)
{
    unsigned m, s, f; locate(path, &m, &s, &f);
    assert(owned[m][s][f]);
    int result = remove(paths[m][s][f]); if (!result) owned[m][s][f] = false; return result;
}
static int fixture_rename(const char *from, const char *to)
{
    unsigned m, s, f; locate(from, &m, &s, &f);
    assert(!f && owned[m][s][0] && !owned[m][s][1] && !strcmp(to, logical[m][s][1]));
    if (inject(PUBLISH)) return -1;
    int result = rename(paths[m][s][0], paths[m][s][1]);
    if (!result) { owned[m][s][0] = false; owned[m][s][1] = true; publications++; }
    return result;
}
static int fixture_error(FILE *file) { assert(file == active); return flagged || ferror(file); }
static int fixture_puts(const char *text, FILE *file)
{
    assert(file == active && !strcmp(text, header));
    if (inject(HEADER)) { assert(fwrite("PART", 1, 4, file) == 4); return EOF; }
    int result = fputs(text, file); if (stale_success) errno = ENOSPC; return result;
}
static int fixture_printf(FILE *file, const char *format, ...)
{
    assert(file == active);
    if (inject(ROW)) { assert(fwrite("PART", 1, 4, file) == 4); return -1; }
    va_list args; va_start(args, format); int result = vfprintf(file, format, args); va_end(args);
    if (stale_success) errno = ENOSPC;
    return result;
}
static int fixture_putc(int value, FILE *file)
{
    assert(file == active && value == '\n');
    if (inject(NEWLINE)) { assert(fwrite("PART", 1, 4, file) == 4); return EOF; }
    int result = fputc(value, file); if (stale_success) errno = ENOSPC; return result;
}
static int fixture_flush(FILE *file)
{
    assert(file == active); int result = fflush(file); return inject(FLUSH) ? EOF : result;
}
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
    assert(file == active && handles == 1); int result = fclose(file); handles--; active = NULL;
    bool failure = inject(CLOSE);
    if (fail_close) { errno = EPERM; return EOF; }
    return failure ? EOF : result;
}
#define stat(path, info) fixture_stat(path, info)
#define mkdir fixture_mkdir
#define open fixture_open
#define fdopen fixture_fdopen
#define close fixture_close_fd
#define unlink fixture_remove
#define remove fixture_remove
#define rename fixture_rename
#define fputs fixture_puts
#define fprintf fixture_printf
#define fputc fixture_putc
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
#include "serial_log_write.inc"
#undef stat
#undef mkdir
#undef open
#undef fdopen
#undef close
#undef unlink
#undef remove
#undef rename
#undef fputs
#undef fprintf
#undef fputc
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

static void clean_files(void)
{
    assert(!handles && !descriptors && !log_file);
    for (unsigned m = 0; m < 2; m++) for (unsigned s = 0; s < 10; s++) for (unsigned f = 0; f < 2; f++)
        if (owned[m][s][f]) { assert(remove(paths[m][s][f]) == 0); owned[m][s][f] = false; }
}
static void prepare(bool rs485, enum point point, int error)
{
    clean_files(); rs485_interface = rs485; sd_available = true;
    failed_point = point; failed_call = 1; failed_errno = error;
    flagged = fail_close = tick_clobber = stale_success = false;
    tick = 2000; log_last_flush_tick = 0;
    reports = publications = controls = 0; reported_error = 0; memset(calls, 0, sizeof(calls));
    status_text[0] = log_temporary_path[0] = log_final_path[0] = '\0'; errno = EBUSY;
}
static bool file_bytes(unsigned s, unsigned f, const char *expected)
{
    const char *path = paths[rs485_interface][s][f];
    FILE *file = fopen(path, "rb");
    if (!expected) { if (file) fclose(file); return !file && errno == ENOENT; }
    if (!file) return false;
    char content[2048]; size_t length = fread(content, 1, sizeof(content), file);
    bool ok = !ferror(file) && length == strlen(expected) && !memcmp(content, expected, length);
    return fclose(file) == 0 && ok;
}
static void seed(unsigned s, unsigned f, const char *content)
{
    assert(!owned[rs485_interface][s][f]);
    FILE *file = fopen(paths[rs485_interface][s][f], "wb"); assert(file);
    owned[rs485_interface][s][f] = true;
    assert(fwrite(content, 1, strlen(content), file) == strlen(content)); assert(fclose(file) == 0);
}
static void result(const char *name, bool ok, bool bytes_ok)
{
    ok = ok && bytes_ok && !handles && !descriptors && !log_file;
    printf("%s %s-%s bytes_verified=%u reports=%u error=%d stat=%u open=%u fdopen=%u row=%u flushes=%u syncs=%u closes=%u published=%u handles=%u descriptors=%u\n",
           ok ? "PASS" : "FAIL", rs485_interface ? "rs485" : "uart", name, bytes_ok, reports, reported_error,
           calls[STAT], calls[OPEN], calls[FDOPEN], calls[ROW], calls[FLUSH], calls[SYNC], calls[CLOSE], publications, handles, descriptors);
    cases++; if (!ok) failures++;
}
static void close_unexpected(void)
{ if (log_file) { fixture_close(log_file); log_file = NULL; } }
static void complete_text(char *text, size_t capacity)
{ int n = snprintf(text, capacity, "%s%s", header, row); assert(n > 0 && (size_t)n < capacity); }
static void creation_case(bool rs485, const char *name, enum point point, int error)
{
    prepare(rs485, point, error); bool started = start_log(); close_unexpected();
    bool ok = !started && reports == 1 && reported_error == (error ? error : EIO) && !publications;
    ok = ok && strstr(status_text, strerror(error ? error : EIO));
    if (point == STAT) ok = ok && calls[STAT] == 1 && !calls[OPEN];
    if (point == OPEN) ok = ok && calls[OPEN] == 1 && !calls[FDOPEN];
    if (point == FDOPEN) ok = ok && calls[FDOPEN] == 1 && !calls[HEADER];
    if (point == ROOT_DIR) ok = ok && !calls[DATE_DIR] && !calls[STAT];
    if (point == DATE_DIR) ok = ok && !calls[STAT];
    bool bytes_ok = true;
    for (unsigned s = 0; s < 10; s++) bytes_ok = file_bytes(s, 0, NULL) && file_bytes(s, 1, NULL) && bytes_ok;
    result(name, ok, bytes_ok);
}
static void header_case(bool rs485, const char *name, int error, bool close_error)
{
    prepare(rs485, HEADER, error); fail_close = close_error;
    bool started = start_log(); close_unexpected();
    result(name, !started && reports == 1 && reported_error == (error ? error : EIO) && !publications && !calls[FLUSH] && !calls[SYNC] &&
           strstr(status_text, "retained") && strstr(status_text, strrchr(log_temporary_path, '/') + 1),
           file_bytes(0, 0, "PART") && file_bytes(0, 1, NULL));
}
static void row_case(bool rs485, const char *name, enum point point, unsigned call, int error, bool close_error, bool tick_error)
{
    prepare(rs485, NONE, 0); assert(start_log());
    failed_point = point; failed_call = call; failed_errno = error; fail_close = close_error; tick_clobber = tick_error;
    stale_success = !error; tick = 12000; errno = EBUSY;
    log_bytes("RX", bytes, sizeof(bytes)); bool stopped = !log_file; close_unexpected();
    char expected[256]; const char *tail = "";
    if (point == ROW && call == 2) tail = "1700000000,RX,";
    if (point == ROW && call == 3) tail = "1700000000,RX,00";
    if (point == NEWLINE) tail = "1700000000,RX,00 A5 FF";
    if (point == FLUSH || point == SYNC) tail = row;
    int n = snprintf(expected, sizeof(expected), "%s%s%s", header, tail, point == FLUSH || point == SYNC ? "" : "PART");
    assert(n > 0 && (size_t)n < sizeof(expected));
    bool ok = stopped && reports == 1 && reported_error == (error ? error : EIO) && !publications;
    ok = ok && controls == 2 && strstr(status_text, strerror(error ? error : EIO)) && strstr(status_text, "retained");
    if (point == ROW || point == NEWLINE) ok = ok && !calls[FLUSH] && !calls[SYNC];
    if (point == FLUSH) ok = ok && calls[FLUSH] == 1 && !calls[SYNC];
    if (point == SYNC) ok = ok && calls[FLUSH] == 1 && calls[SYNC] == 1;
    unsigned previous_reports = reports; log_bytes("RX", bytes, sizeof(bytes)); bool idle_stop = stop_log();
    result(name, ok && idle_stop && reports == previous_reports, file_bytes(0, 0, expected) && file_bytes(0, 1, NULL));
}
static void stop_case(bool rs485, const char *name, enum point point, int error)
{
    prepare(rs485, NONE, 0); assert(start_log()); log_bytes("RX", bytes, sizeof(bytes));
    failed_point = point; failed_errno = error; errno = EBUSY;
    bool stopped = stop_log(); char expected[256]; complete_text(expected, sizeof(expected));
    result(name, !stopped && reports == 1 && reported_error == (error ? error : EIO) && !publications,
           file_bytes(0, 0, expected) && file_bytes(0, 1, NULL));
}
static void stream_error_case(bool rs485)
{
    prepare(rs485, NONE, 0); assert(start_log()); log_bytes("RX", bytes, sizeof(bytes));
    flagged = true; errno = EBUSY; bool stopped = stop_log();
    char expected[256]; complete_text(expected, sizeof(expected));
    result("stream-error", !stopped && reports == 1 && reported_error == EIO && !publications && !calls[FLUSH] && !calls[SYNC],
           file_bytes(0, 0, expected) && file_bytes(0, 1, NULL));
}
static void success_case(bool rs485, const char *name, TickType_t started, TickType_t now, bool stale)
{
    prepare(rs485, NONE, 0); tick = started; stale_success = stale; assert(start_log()); tick = now;
    log_bytes("RX", bytes, sizeof(bytes));
    unsigned periodic = (TickType_t)(now - started) >= UART_TOOL_FLUSH_MS;
    bool ok = log_file && calls[FLUSH] == periodic && calls[SYNC] == periodic &&
              log_last_flush_tick == (periodic ? now : started);
    ok = stop_log() && ok; char expected[256]; complete_text(expected, sizeof(expected));
    result(name, ok && !reports && publications == 1 && controls == 2 && strstr(status_text, "Saved") && calls[FLUSH] == periodic + 1 && calls[SYNC] == periodic + 1,
           file_bytes(0, 0, NULL) && file_bytes(0, 1, expected));
}
static void recovery_case(bool rs485, const char *name, enum point point, unsigned count)
{
    prepare(rs485, NONE, 0); bool ok = true, bytes_ok = true;
    for (unsigned i = 0; i < count; i++) {
        clean_files(); failed_point = point; failed_errno = ENOSPC; failed_call = calls[point] + 1;
        if (point == HEADER) { bool started = start_log(); ok = !started && ok; }
        else { assert(start_log()); log_bytes("RX", bytes, sizeof(bytes)); }
        ok = !log_file && ok; close_unexpected();
        char partial[256]; int n = snprintf(partial, sizeof(partial), "%sPART", point == HEADER ? "" : header); assert(n > 0 && (size_t)n < sizeof(partial));
        bytes_ok = file_bytes(0, 0, partial) && file_bytes(0, 1, NULL) && bytes_ok;
        failed_point = NONE; bool started = start_log(); ok = started && !strcmp(log_temporary_path, logical[rs485][1][0]) && ok;
        if (log_file) { log_bytes("RX", bytes, sizeof(bytes)); ok = stop_log() && ok; }
        char expected[256]; complete_text(expected, sizeof(expected));
        bytes_ok = file_bytes(0, 0, partial) && file_bytes(0, 1, NULL) && file_bytes(1, 0, NULL) && file_bytes(1, 1, expected) && bytes_ok;
        ok = reports == i + 1 && publications == i + 1 && !handles && !descriptors && ok;
    }
    result(name, ok, bytes_ok);
}
static void collisions(bool rs485)
{
    prepare(rs485, NONE, 0); seed(0, 1, "KEEP"); seed(1, 0, "OLD-PART"); assert(start_log());
    bool ok = !strcmp(log_temporary_path, logical[rs485][2][0]); log_bytes("RX", bytes, sizeof(bytes)); ok = stop_log() && ok;
    char expected[256]; complete_text(expected, sizeof(expected));
    result("collision-recovery", ok && !reports && publications == 1,
           file_bytes(0, 1, "KEEP") && file_bytes(1, 0, "OLD-PART") && file_bytes(2, 1, expected));
    prepare(rs485, NONE, 0); stale_success = true;
    for (unsigned s = 0; s < 10; s++) seed(s, 1, "KEEP");
    bool started = start_log(); close_unexpected(); bool bytes_ok = true;
    for (unsigned s = 0; s < 10; s++) bytes_ok = file_bytes(s, 0, NULL) && file_bytes(s, 1, "KEEP") && bytes_ok;
    result("final-names-full", !started && reports == 1 && reported_error == EEXIST && calls[STAT] == 10 && !calls[OPEN], bytes_ok);
    prepare(rs485, NONE, 0); for (unsigned s = 0; s < 10; s++) seed(s, 0, "OLD-PART");
    started = start_log(); close_unexpected(); bytes_ok = true;
    for (unsigned s = 0; s < 10; s++) bytes_ok = file_bytes(s, 0, "OLD-PART") && file_bytes(s, 1, NULL) && bytes_ok;
    result("temporary-names-full", !started && reports == 1 && reported_error == EEXIST && calls[OPEN] == 10 && !calls[FDOPEN], bytes_ok);
}
static void binary_case(bool rs485)
{
    prepare(rs485, NONE, 0); assert(start_log()); uint8_t all[256]; char expected[1200];
    int n = snprintf(expected, sizeof(expected), "%s1700000000,TX,", header); assert(n > 0); size_t used = (size_t)n;
    for (unsigned i = 0; i < 256; i++) { all[i] = (uint8_t)i; n = snprintf(expected + used, sizeof(expected) - used, "%s%02X", i ? " " : "", i); assert(n > 0); used += (size_t)n; }
    expected[used++] = '\n'; expected[used] = '\0'; log_bytes("TX", all, sizeof(all));
    bool ok = stop_log(); result("all-256-bytes", ok && !reports && publications == 1, file_bytes(0, 0, NULL) && file_bytes(0, 1, expected));
}
static void run_link(bool rs485)
{
    success_case(rs485, "success", 2000, 2000, false);
    success_case(rs485, "before-flush", 2000, 11999, false);
    success_case(rs485, "flush-threshold", 2000, 12000, false);
    success_case(rs485, "tick-wrap", UINT32_MAX - 4999, 5000, false);
    success_case(rs485, "stale-success", 2000, 12000, true);
    creation_case(rs485, "root-error", ROOT_DIR, EACCES);
    creation_case(rs485, "root-no-errno", ROOT_DIR, 0);
    creation_case(rs485, "date-error", DATE_DIR, EACCES);
    creation_case(rs485, "date-no-errno", DATE_DIR, 0);
    creation_case(rs485, "stat-error", STAT, EACCES);
    creation_case(rs485, "stat-no-errno", STAT, 0);
    creation_case(rs485, "open-error", OPEN, ENOSPC);
    creation_case(rs485, "open-no-errno", OPEN, 0);
    creation_case(rs485, "fdopen-error", FDOPEN, ENOMEM);
    creation_case(rs485, "fdopen-no-errno", FDOPEN, 0);
    header_case(rs485, "header-error", ENOSPC, false);
    header_case(rs485, "header-no-errno", 0, false);
    header_case(rs485, "first-header-error", ENOSPC, true);
    row_case(rs485, "prefix-error", ROW, 1, ENOSPC, false, false);
    row_case(rs485, "prefix-no-errno", ROW, 1, 0, false, false);
    row_case(rs485, "first-row-error", ROW, 1, ENOSPC, true, true);
    row_case(rs485, "byte-error", ROW, 3, ENOSPC, false, false);
    row_case(rs485, "byte-no-errno", ROW, 3, 0, false, false);
    row_case(rs485, "newline-error", NEWLINE, 1, ENOSPC, false, false);
    row_case(rs485, "newline-no-errno", NEWLINE, 1, 0, false, false);
    row_case(rs485, "flush-once", FLUSH, 1, EIO, false, false);
    row_case(rs485, "flush-no-errno", FLUSH, 1, 0, false, false);
    row_case(rs485, "sync-once", SYNC, 1, EIO, false, false);
    row_case(rs485, "first-sync-error", SYNC, 1, EACCES, true, false);
    row_case(rs485, "sync-no-errno", SYNC, 1, 0, false, false);
    stop_case(rs485, "close-error", CLOSE, EACCES);
    stop_case(rs485, "close-no-errno", CLOSE, 0);
    stop_case(rs485, "publish-error", PUBLISH, EACCES);
    stream_error_case(rs485);
    recovery_case(rs485, "header-restart", HEADER, 1);
    recovery_case(rs485, "row-restart-25", ROW, 25);
    collisions(rs485); binary_case(rs485);
    prepare(rs485, NONE, 0); sd_available = false; bool started = start_log();
    result("no-sd", !started && !reports && !calls[ROOT_DIR] && !calls[OPEN], file_bytes(0, 0, NULL) && file_bytes(0, 1, NULL));
}
int main(void)
{
    for (unsigned m = 0; m < 2; m++) {
        snprintf(directories[m][0], sizeof(directories[m][0]), "serial_log_write_%d_%u", fixture_pid(), m);
        snprintf(directories[m][1], sizeof(directories[m][1]), "serial_log_write_%d_%u/231114", fixture_pid(), m);
        for (unsigned d = 0; d < 2; d++) { struct stat info; if (stat(directories[m][d], &info) == 0 || errno != ENOENT) { fputs("Refusing existing fixture directory\n", stderr); return 2; } }
        for (unsigned s = 0; s < 10; s++) for (unsigned f = 0; f < 2; f++) {
            snprintf(logical[m][s][f], sizeof(logical[m][s][f]), "%s/231114/%c221320%u.%s", m ? RS485_TOOL_PATH : UART_TOOL_PATH, m ? 'R' : 'U', s, f ? "CSV" : "TMP");
            snprintf(paths[m][s][f], sizeof(paths[m][s][f]), "%s/%c221320%u.%s", directories[m][1], m ? 'R' : 'U', s, f ? "CSV" : "TMP");
        }
    }
    run_link(false); run_link(true); clean_files();
    for (unsigned m = 0; m < 2; m++) for (int d = 1; d >= 0; d--)
        if (owned_directories[m][d]) { assert(native_rmdir(directories[m][d]) == 0); owned_directories[m][d] = false; }
    printf("%s %u serial log writer cases failures=%u handles=%u descriptors=%u (native files/storage; controlled UI/time/faults)\n",
           failures ? "FAIL" : "PASS", cases, failures, handles, descriptors);
    return failures ? 1 : 0;
}
