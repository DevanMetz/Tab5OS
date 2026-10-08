/* Actual ride callbacks/storage helpers, native files and controlled faults. */
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

#define SD_PATH "/sdcard"
typedef struct { char text[160]; } lv_obj_t;
static lv_obj_t label;
static lv_obj_t *ride_history = &label;
static bool sd_ready;
static time_t ride_started_at = 1700000000;
static float ride_distance_km = 12.3456f, ride_work_kj = 230.04f;
static int64_t ride_power_sum = 700, ride_hr_sum = 300;
static uint32_t ride_power_samples = 4, ride_hr_samples_count = 2;
static int ride_max_power = 350, ride_max_hr = 170;
static FILE *source_file, *output_file;
static char paths[3][128];
static unsigned handles, fs_calls, mutations, reads, writes, copied, source_closes, output_closes, temporary_removes;
static bool read_error, output_error;
static int recorded_error;
static bool no_errno, stale_success, positive_result, cleanup_errors, cleanup_remove_error;
static int stale_errno, boundary_errno;
static unsigned writer_opens, newline_calls, header_calls, row_calls, flush_calls, sync_calls, publications, error_reports;
typedef enum { NO_FAULT, NO_SD, FOLDER_ERROR, FINAL_STAT_ERROR, BACKUP_STAT_ERROR,
    RECOVERY_ERROR, INITIAL_REMOVE_ERROR, OPEN_READ_ERROR, OPEN_WRITE_ERROR,
    READ_ERROR, NEXT_READ_ERROR, READ_ZERO_ERRNO, FULL_READ_ERROR, SOURCE_CLOSE_ERROR,
    COPY_ERROR, COPY_ZERO_ERRNO, NEWLINE_ERROR, HEADER_ERROR, ROW_ERROR,
    FLUSH_ERROR, SYNC_ERROR, OUTPUT_CLOSE_ERROR, PUBLISH_ERROR, ROLLBACK_ERROR,
    READ_CLEANUP_ERROR, COPY_CLEANUP_ERROR, OPEN_CLEANUP_ERROR, STALE_READ_ERROR } fault_t;
static fault_t fault;

static void supply_errno(int error) { if (!no_errno) errno = boundary_errno ? boundary_errno : error; }
static void leave_stale_errno(void) { if (stale_success) errno = stale_errno; }

static const char *native_path(const char *path)
{
    const char *source[] = {SD_PATH "/RIDES/SUMMARY.CSV", SD_PATH "/RIDES/SUMMARY.TMP", SD_PATH "/RIDES/SUMMARY.BAK"};
    for (unsigned i = 0; i < 3; i++) if (!strcmp(path, source[i])) return paths[i];
    assert(false); return NULL;
}
FILE *fixture_open(const char *path, const char *mode)
{
    fs_calls++; bool input = !strcmp(mode, "rb");
    if (!input) { mutations++; writer_opens++; }
    if (input && fault == OPEN_READ_ERROR) { supply_errno(EACCES); return NULL; }
    if (!input && (fault == OPEN_WRITE_ERROR || fault == OPEN_CLEANUP_ERROR)) { supply_errno(ENOSPC); return NULL; }
    FILE *file = fopen(native_path(path), mode);
    if (file) {
        handles++;
        if (input) { source_file = file; read_error = false; }
        else { output_file = file; output_error = false; }
        leave_stale_errno();
    }
    return file;
}
size_t fixture_read(void *buffer, size_t size, size_t count, FILE *file)
{
    assert(file == source_file && size == 1); fs_calls++; reads++;
    if (((fault == READ_ERROR || fault == READ_ZERO_ERRNO || fault == FULL_READ_ERROR || fault == READ_CLEANUP_ERROR) && reads == 1) ||
        (fault == NEXT_READ_ERROR && reads == 2)) {
        size_t read = fread(buffer, size, fault == FULL_READ_ERROR ? count : count / 2, file);
        read_error = true;
        if (fault == READ_ZERO_ERRNO) errno = 0;
        else supply_errno(EIO);
        return read;
    }
    size_t read = fread(buffer, size, count, file);
    if (!ferror(file)) leave_stale_errno();
    return read;
}
char *fixture_gets(char *buffer, int size, FILE *file)
{
    assert(file == source_file); fs_calls++; reads++;
    if (fault == STALE_READ_ERROR && reads == 3) { read_error = true; return NULL; }
    if (((fault == READ_ERROR || fault == READ_ZERO_ERRNO || fault == READ_CLEANUP_ERROR) && reads == 3) ||
        (fault == NEXT_READ_ERROR && reads == 4)) {
        char *result = positive_result ? fgets(buffer, size, file) : NULL;
        read_error = true;
        if (fault == READ_ZERO_ERRNO) errno = 0;
        else supply_errno(EIO);
        return result;
    }
    char *result = fgets(buffer, size, file);
    if (result && fault == STALE_READ_ERROR) errno = ENOSPC;
    else if (!ferror(file)) leave_stale_errno();
    return result;
}
int fixture_error(FILE *file) { return (file == source_file && read_error) || (file == output_file && output_error) || ferror(file); }
size_t fixture_write(const void *buffer, size_t size, size_t count, FILE *file)
{
    assert(file == output_file && size == 1); fs_calls++; writes++; mutations++;
    if (fault == COPY_ERROR || fault == COPY_ZERO_ERRNO || fault == COPY_CLEANUP_ERROR) {
        assert(count); size_t written = fwrite(buffer, size, positive_result ? count : count - 1, file); copied += (unsigned)written;
        if (positive_result) output_error = true;
        if (fault == COPY_ZERO_ERRNO) errno = 0;
        else supply_errno(ENOSPC);
        return written;
    }
    size_t written = fwrite(buffer, size, count, file); copied += (unsigned)written;
    if (!ferror(file)) leave_stale_errno();
    return written;
}
int fixture_close(FILE *file)
{
    assert(handles); handles--; fs_calls++;
    bool input = file == source_file;
    if (input) { source_closes++; source_file = NULL; } else { assert(file == output_file); output_closes++; output_file = NULL; }
    int result = fclose(file);
    if ((input && (fault == SOURCE_CLOSE_ERROR || fault == READ_CLEANUP_ERROR || fault == COPY_CLEANUP_ERROR || fault == OPEN_CLEANUP_ERROR)) ||
        (!input && (fault == OUTPUT_CLOSE_ERROR || fault == READ_CLEANUP_ERROR || fault == COPY_CLEANUP_ERROR))) { supply_errno(EPERM); return EOF; }
    if (cleanup_errors && (!input || fault == READ_ERROR || fault == COPY_ERROR || fault == OPEN_WRITE_ERROR)) { errno = EPERM; return EOF; }
    if (!result) leave_stale_errno();
    return result;
}
int fixture_stat(const char *path, struct stat *info)
{
    fs_calls++; const char *mapped = native_path(path);
    if ((mapped == paths[0] && fault == FINAL_STAT_ERROR) || (mapped == paths[2] && fault == BACKUP_STAT_ERROR)) { supply_errno(EACCES); return -1; }
    int result = stat(mapped, info);
    if (!result) leave_stale_errno();
    return result;
}
int fixture_remove(const char *path)
{
    fs_calls++; mutations++; const char *mapped = native_path(path);
    if (mapped == paths[1]) {
        temporary_removes++;
        if (fault == INITIAL_REMOVE_ERROR || ((fault == READ_CLEANUP_ERROR || fault == COPY_CLEANUP_ERROR) && temporary_removes > 1)) { supply_errno(EACCES); return -1; }
        if (cleanup_remove_error && temporary_removes > 1) { errno = EPERM; return -1; }
    }
    int result = remove(mapped);
    if (!result) leave_stale_errno();
    return result;
}
int fixture_rename(const char *from, const char *to)
{
    fs_calls++; mutations++; const char *a = native_path(from), *b = native_path(to);
    if ((a == paths[2] && b == paths[0] && (fault == RECOVERY_ERROR || fault == ROLLBACK_ERROR)) ||
        (a == paths[1] && b == paths[0] && (fault == PUBLISH_ERROR || fault == ROLLBACK_ERROR))) { errno = EACCES; return -1; }
    int result = rename(a, b);
    if (!result) { if (a == paths[1] && b == paths[0]) publications++; leave_stale_errno(); }
    return result;
}
int fixture_mkdir(const char *path, int mode)
{
    assert(!strcmp(path, SD_PATH "/RIDES") && mode == 0775); fs_calls++; mutations++;
    if (fault == FOLDER_ERROR) { supply_errno(ENOSPC); return -1; } errno = EEXIST; return -1;
}
int fixture_putc(int byte, FILE *file)
{
    assert(file == output_file); fs_calls++; mutations++; newline_calls++;
    if (fault == NEWLINE_ERROR) {
        int result = positive_result ? fputc(byte, file) : EOF;
        if (positive_result) output_error = true;
        supply_errno(ENOSPC); return result;
    }
    int result = fputc(byte, file);
    if (result != EOF) leave_stale_errno();
    return result;
}
int fixture_puts(const char *text, FILE *file)
{
    assert(file == output_file); fs_calls++; mutations++; header_calls++;
    if (fault == HEADER_ERROR) {
        int result = positive_result ? fputs(text, file) : EOF;
        if (positive_result) output_error = true;
        supply_errno(ENOSPC); return result;
    }
    int result = fputs(text, file);
    if (result >= 0) leave_stale_errno();
    return result;
}
int fixture_printf(FILE *file, const char *format, ...)
{
    assert(file == output_file); fs_calls++; mutations++; row_calls++;
    if (fault == ROW_ERROR && !positive_result) { supply_errno(ENOSPC); return -1; }
    va_list args; va_start(args, format); int result = vfprintf(file, format, args); va_end(args);
    if (fault == ROW_ERROR) { output_error = true; supply_errno(ENOSPC); }
    else if (result >= 0) leave_stale_errno();
    return result;
}
int fixture_flush(FILE *file)
{
    fs_calls++; flush_calls++; if (fault == FLUSH_ERROR) { errno = EIO; return EOF; } return fflush(file);
}
#ifdef _WIN32
int fixture_sync(int descriptor) { fs_calls++; sync_calls++; if (fault == SYNC_ERROR) { errno = EIO; return -1; } return _commit(descriptor); }
#define _commit fixture_sync
#else
int fixture_sync(int descriptor) { fs_calls++; sync_calls++; if (fault == SYNC_ERROR) { errno = EIO; return -1; } return fsync(descriptor); }
#define fsync fixture_sync
#endif
void lv_label_set_text(lv_obj_t *object, const char *text)
{ assert(object && strlen(text) < sizeof(object->text)); strcpy(object->text, text); }
void lv_label_set_text_fmt(lv_obj_t *object, const char *format, ...)
{
    assert(object); va_list args; va_start(args, format); int result = vsnprintf(object->text, sizeof(object->text), format, args); va_end(args);
    assert(result >= 0 && (size_t)result < sizeof(object->text));
}
void sd_record_error(int error) { recorded_error = error; error_reports++; }
int sd_error_snapshot(void) { return recorded_error; }

#define fopen fixture_open
#define fread fixture_read
#define fgets fixture_gets
#define ferror fixture_error
#define fwrite fixture_write
#define fclose fixture_close
#define stat(path, info) fixture_stat(path, info)
#define remove fixture_remove
#define rename fixture_rename
#define mkdir fixture_mkdir
#define fputc fixture_putc
#define fputs fixture_puts
#define fprintf fixture_printf
#define fflush fixture_flush
#include "storage_source.inc"
#include "ride_history.inc"
#undef fopen
#undef fread
#undef fgets
#undef ferror
#undef fwrite
#undef fclose
#undef stat
#undef remove
#undef rename
#undef mkdir
#undef fputc
#undef fputs
#undef fprintf
#undef fflush
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif

static const char *header = "start_unix,duration_s,distance_km,work_kj,avg_power_w,max_power_w,avg_hr,max_hr\n";
static const char *summary = "start_unix,duration_s,distance_km,work_kj,avg_power_w,max_power_w,avg_hr,max_hr\n1700000000,3600,1.000,100.0,200,300,130,140\n1700003600,1800,2.000,200.0,250,400,150,160\n";
static const char *new_row = "1700000000,3600,12.346,230.0,175,350,150,170\n";
static const char *backup = "previous backup\n", *retained = "unpublished summary\n";
static unsigned cases, failures;
static int expected_error;
static void write_bytes(unsigned index, const char *data)
{
    if (!data) return;
    FILE *file = fopen(paths[index], "wb"); assert(file);
    assert(fwrite(data, 1, strlen(data), file) == strlen(data) && fclose(file) == 0);
}
static bool bytes_match(unsigned index, const char *data, size_t length)
{
    FILE *file = fopen(paths[index], "rb");
    if (!data) { if (file) fclose(file); return !file && errno == ENOENT; }
    if (!file) return false;
    size_t offset = 0; bool match = true; char buffer[512];
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (offset + count > length || (count && memcmp(buffer, data + offset, count))) match = false;
        offset += count;
        if (count < sizeof(buffer)) { assert(!ferror(file)); break; }
    }
    assert(fclose(file) == 0); return match && offset == length;
}
static bool text_match(unsigned index, const char *data) { return bytes_match(index, data, data ? strlen(data) : 0); }
static void prepare(const char *data, const char *previous)
{
    assert(!handles); for (unsigned i = 0; i < 3; i++) if (remove(paths[i]) != 0) assert(errno == ENOENT);
    write_bytes(0, data); write_bytes(1, retained); write_bytes(2, previous);
    fs_calls = mutations = reads = writes = copied = source_closes = output_closes = temporary_removes = 0;
    source_file = output_file = NULL; read_error = output_error = false; fault = NO_FAULT; sd_ready = true; recorded_error = 0;
    no_errno = stale_success = positive_result = cleanup_errors = cleanup_remove_error = false;
    stale_errno = EBUSY; boundary_errno = 0;
    writer_opens = newline_calls = header_calls = row_calls = flush_calls = sync_calls = publications = error_reports = 0;
    ride_history = &label; strcpy(label.text, "unchanged");
}
static void report(const char *name, bool pass, int result, int error)
{
    cases++; failures += !pass;
    const char *view = !strncmp(label.text, "History unavailable:", 20) ? "unavailable" : !strncmp(label.text, "History:", 8) ? "totals" : "unchanged";
    printf("%s %s result=%d error=%d expected_error=%d reported=%d view=%s fs_calls=%u mutations=%u reads=%u writes=%u copied=%u input_closes=%u output_closes=%u handles=%u\n",
        pass ? "PASS" : "FAIL", name, result, error, expected_error, recorded_error, view, fs_calls, mutations, reads, writes, copied, source_closes, output_closes, handles);
}
static void history_case(const char *name, const char *data, const char *previous, fault_t injected, int error, bool offscreen)
{
    prepare(data, previous); expected_error = error; fault = injected; if (injected == NO_SD) sd_ready = false;
    if (offscreen) ride_history = NULL;
    ride_load_history();
    bool pass = !handles && recorded_error == error;
    if (offscreen) pass = pass && !fs_calls && !strcmp(label.text, "unchanged");
    else if (error) pass = pass && !strncmp(label.text, "History unavailable:", strlen("History unavailable:"));
    else pass = pass && !strcmp(label.text, data || previous ? "History: 2 rides  |  1.9 mi  |  1.5 hr  |  best 400 W" : "History: 0 rides  |  0.0 mi  |  0.0 hr  |  best 0 W");
    bool recovered = !data && previous && !error && !offscreen;
    pass = pass && text_match(0, recovered ? previous : data) && text_match(2, recovered ? NULL : previous) && text_match(1, retained);
    if (injected == NO_SD) pass = pass && !fs_calls;
    report(name, pass, 0, recorded_error);
}
static void append_case(const char *name, const char *data, const char *previous, fault_t injected, int error)
{
    prepare(data, previous); expected_error = error; fault = injected; if (injected == NO_SD) sd_ready = false;
    errno = 0; bool result = ride_append_summary(3600); int actual_error = errno;
    bool pass = result == !error && !handles && (!error || actual_error == error);
    bool recovered = !data && previous && injected != BACKUP_STAT_ERROR && injected != RECOVERY_ERROR && injected != NO_SD;
    const char *original = data ? data : recovered ? previous : NULL;
    if (!error) {
        size_t prefix = original && *original ? strlen(original) : strlen(header);
        bool newline = original && *original && original[prefix - 1] != '\n';
        char *expected = malloc(prefix + (unsigned)newline + strlen(new_row) + 1); assert(expected);
        memcpy(expected, original && *original ? original : header, prefix);
        if (newline) expected[prefix++] = '\n';
        strcpy(expected + prefix, new_row);
        pass = pass && text_match(0, expected) && text_match(2, original) && text_match(1, NULL); free(expected);
    } else if (injected == PUBLISH_ERROR || injected == ROLLBACK_ERROR) {
        pass = pass && text_match(0, injected == ROLLBACK_ERROR ? NULL : original) && text_match(2, injected == ROLLBACK_ERROR ? original : NULL);
        size_t length = strlen(original) + strlen(new_row); char *expected = malloc(length + 1); assert(expected);
        strcpy(expected, original); strcat(expected, new_row); pass = pass && text_match(1, expected); free(expected);
    } else {
        pass = pass && text_match(0, original) && text_match(2, recovered ? NULL : previous);
        bool untouched = injected == NO_SD || injected == FOLDER_ERROR || injected == FINAL_STAT_ERROR || injected == BACKUP_STAT_ERROR || injected == RECOVERY_ERROR || injected == INITIAL_REMOVE_ERROR;
        if (untouched) pass = pass && text_match(1, retained);
        else if (injected == READ_CLEANUP_ERROR) pass = pass && text_match(1, "");
        else if (injected == COPY_CLEANUP_ERROR) pass = pass && bytes_match(1, original, strlen(original) - 1);
        else pass = pass && text_match(1, NULL);
        if (injected == NO_SD) pass = pass && !fs_calls;
        if (injected == READ_ERROR || injected == READ_ZERO_ERRNO || injected == FULL_READ_ERROR || injected == READ_CLEANUP_ERROR) pass = pass && !writes;
        if (injected == NEXT_READ_ERROR) pass = pass && writes == 1 && copied == 512;
    }
    report(name, pass, result, error ? actual_error : 0);
}
static void history_boundary(const char *name, fault_t point, bool missing_errno, bool positive, bool cleanup, bool absent)
{
    prepare(absent ? NULL : summary, absent ? NULL : backup);
    fault = point; no_errno = missing_errno; stale_success = true; positive_result = positive; cleanup_errors = cleanup;
    boundary_errno = EACCES; stale_errno = point == OPEN_READ_ERROR ? ENOENT : EBUSY;
    expected_error = missing_errno ? EIO : EACCES; errno = ENOENT;
    ride_load_history();
    bool pass = !handles && recorded_error == expected_error && error_reports == 1;
    pass = pass && !strncmp(label.text, "History unavailable:", 20) && !writer_opens && !mutations;
    if (point == READ_ERROR) pass = pass && reads == 3;
    pass = pass && text_match(0, absent ? NULL : summary) && text_match(1, retained) && text_match(2, absent ? NULL : backup);
    report(name, pass, 0, recorded_error);
}
static void append_boundary(const char *name, const char *data, fault_t point, bool missing_errno,
                            bool positive, bool cleanup, const char *kept, size_t kept_length)
{
    prepare(data, data ? backup : NULL);
    fault = point; no_errno = missing_errno; stale_success = true; positive_result = positive; cleanup_errors = cleanup;
    cleanup_remove_error = kept != NULL; boundary_errno = EACCES;
    stale_errno = point == FOLDER_ERROR ? EEXIST : point == OPEN_READ_ERROR || point == INITIAL_REMOVE_ERROR ? ENOENT : EBUSY;
    expected_error = missing_errno ? EIO : EACCES; errno = stale_errno;
    bool result = ride_append_summary(3600); int actual = errno;
    bool pass = !result && actual == expected_error && !handles && !publications && !error_reports && !flush_calls && !sync_calls;
    pass = pass && text_match(0, data) && text_match(2, data ? backup : NULL);
    if (point == FOLDER_ERROR || point == INITIAL_REMOVE_ERROR) {
        pass = pass && text_match(1, retained) && !writer_opens && !reads && !writes;
        if (point == FOLDER_ERROR) pass = pass && !temporary_removes;
    } else if (kept) pass = pass && bytes_match(1, kept, kept_length);
    else pass = pass && text_match(1, NULL);
    if (point == OPEN_READ_ERROR) pass = pass && !writer_opens && !writes && !row_calls;
    if (point == OPEN_WRITE_ERROR) pass = pass && !writes && !row_calls && source_closes == (unsigned)(data != NULL);
    if (point == READ_ERROR) pass = pass && !writes && !row_calls;
    if (point == COPY_ERROR) pass = pass && reads == 1 && writes == 1 && !newline_calls && !header_calls && !row_calls;
    if (point == SOURCE_CLOSE_ERROR || point == NEWLINE_ERROR || point == HEADER_ERROR) pass = pass && !row_calls;
    report(name, pass, result, actual);
}
static void history_failure_retry(const char *name, fault_t point)
{
    bool pass = true;
    for (unsigned i = 0; i < 25; i++) {
        prepare(summary, backup); fault = point; no_errno = stale_success = true; positive_result = point == READ_ERROR;
        stale_errno = point == OPEN_READ_ERROR ? ENOENT : EBUSY;
        ride_load_history();
        pass = pass && recorded_error == EIO && error_reports == 1 && !handles && !strncmp(label.text, "History unavailable:", 20);
        if (point == READ_ERROR) pass = pass && reads == 3;
        pass = pass && text_match(0, summary) && text_match(1, retained) && text_match(2, backup);
        fault = NO_FAULT; no_errno = stale_success = positive_result = false;
        ride_load_history();
        pass = pass && !handles && error_reports == 1 && !strcmp(label.text, "History: 2 rides  |  1.9 mi  |  1.5 hr  |  best 400 W");
        pass = pass && text_match(0, summary) && text_match(1, retained) && text_match(2, backup);
    }
    expected_error = EIO; report(name, pass, 0, recorded_error);
}
static void append_failure_retry(const char *name, fault_t point)
{
    char complete[256]; assert(snprintf(complete, sizeof(complete), "%s%s", summary, new_row) > 0);
    bool pass = true;
    for (unsigned i = 0; i < 25; i++) {
        prepare(summary, backup); fault = point; no_errno = stale_success = true; stale_errno = EEXIST;
        positive_result = cleanup_errors = cleanup_remove_error = point == COPY_ERROR;
        bool rejected = ride_append_summary(3600); int error = errno;
        pass = pass && !rejected && error == EIO && !handles && !publications && !flush_calls && !sync_calls;
        pass = pass && text_match(0, summary) && text_match(2, backup) && text_match(1, point == COPY_ERROR ? summary : retained);
        fault = NO_FAULT; no_errno = stale_success = positive_result = cleanup_errors = cleanup_remove_error = false;
        bool saved = ride_append_summary(3600);
        pass = pass && saved && !handles && publications == 1 && text_match(0, complete) && text_match(2, summary) && text_match(1, NULL);
        ride_load_history();
        pass = pass && !handles && !error_reports && !strcmp(label.text, "History: 3 rides  |  9.5 mi  |  2.5 hr  |  best 400 W");
    }
    expected_error = 0; report(name, pass, 1, 0);
}
int main(void)
{
    for (unsigned i = 0; i < 3; i++) {
        assert(snprintf(paths[i], sizeof(paths[i]), ".ride_history_%ld_%u", (long)fixture_pid(), i) > 0);
        struct stat info; assert(stat(paths[i], &info) != 0 && errno == ENOENT);
    }
    history_case("history-missing", NULL, NULL, NO_FAULT, 0, false);
    history_case("history-complete", summary, backup, NO_FAULT, 0, false);
    history_case("history-recovered", NULL, summary, NO_FAULT, 0, false);
    history_case("history-offscreen", summary, backup, READ_ERROR, 0, true);
    const struct { const char *name; fault_t fault; int error; bool absent; } history_faults[] = {
        {"history-no-sd", NO_SD, ENODEV, false}, {"history-final-stat", FINAL_STAT_ERROR, EACCES, false},
        {"history-backup-stat", BACKUP_STAT_ERROR, EACCES, true}, {"history-recovery", RECOVERY_ERROR, EACCES, true},
        {"history-open", OPEN_READ_ERROR, EACCES, false}, {"history-partial-read", READ_ERROR, EIO, false},
        {"history-late-read", NEXT_READ_ERROR, EIO, false}, {"history-zero-errno", READ_ZERO_ERRNO, EIO, false},
        {"history-close", SOURCE_CLOSE_ERROR, EPERM, false}, {"history-first-read-error", READ_CLEANUP_ERROR, EIO, false},
        {"history-stale-errno", STALE_READ_ERROR, EIO, false}};
    for (unsigned i = 0; i < sizeof(history_faults) / sizeof(history_faults[0]); i++)
        history_case(history_faults[i].name, history_faults[i].absent ? NULL : summary, summary, history_faults[i].fault, history_faults[i].error, false);
    prepare(summary, backup); sd_ready = false; expected_error = recorded_error = EIO; ride_load_history();
    report("history-stored-sd-error", !fs_calls && recorded_error == EIO && !strncmp(label.text, "History unavailable:", 20) && text_match(0, summary) && text_match(1, retained) && text_match(2, backup), 0, recorded_error);
    append_case("append-first", NULL, NULL, NO_FAULT, 0);
    append_case("append-empty", "", backup, NO_FAULT, 0);
    append_case("append-complete", summary, backup, NO_FAULT, 0);
    append_case("append-recovered", NULL, summary, NO_FAULT, 0);
    append_case("append-no-newline", "1700000000,3600,1.000,100.0,200,300,130,140", backup, NO_FAULT, 0);
    const struct { const char *name; fault_t fault; int error; bool absent; } append_faults[] = {
        {"append-no-sd", NO_SD, ENODEV, false}, {"append-folder", FOLDER_ERROR, ENOSPC, false},
        {"append-final-stat", FINAL_STAT_ERROR, EACCES, false}, {"append-backup-stat", BACKUP_STAT_ERROR, EACCES, true},
        {"append-recovery", RECOVERY_ERROR, EACCES, true}, {"append-remove-temp", INITIAL_REMOVE_ERROR, EACCES, false},
        {"append-open-read", OPEN_READ_ERROR, EACCES, false}, {"append-open-write", OPEN_WRITE_ERROR, ENOSPC, false},
        {"append-read", READ_ERROR, EIO, false}, {"append-read-zero-errno", READ_ZERO_ERRNO, EIO, false},
        {"append-source-close", SOURCE_CLOSE_ERROR, EPERM, false}, {"append-copy", COPY_ERROR, ENOSPC, false},
        {"append-copy-zero-errno", COPY_ZERO_ERRNO, EIO, false}, {"append-row", ROW_ERROR, ENOSPC, false},
        {"append-flush", FLUSH_ERROR, EIO, false}, {"append-sync", SYNC_ERROR, EIO, false},
        {"append-close", OUTPUT_CLOSE_ERROR, EPERM, false}, {"append-publish", PUBLISH_ERROR, EACCES, false},
        {"append-rollback", ROLLBACK_ERROR, EACCES, false}, {"append-first-read-error", READ_CLEANUP_ERROR, EIO, false},
        {"append-first-copy-error", COPY_CLEANUP_ERROR, ENOSPC, false}, {"append-first-open-error", OPEN_CLEANUP_ERROR, ENOSPC, false}};
    for (unsigned i = 0; i < sizeof(append_faults) / sizeof(append_faults[0]); i++)
        append_case(append_faults[i].name, append_faults[i].absent ? NULL : summary, summary, append_faults[i].fault, append_faults[i].error);
    prepare(summary, backup); sd_ready = false; expected_error = recorded_error = EIO; errno = 0;
    bool stored_result = ride_append_summary(3600); int stored_error = errno;
    report("append-stored-sd-error", !stored_result && stored_error == EIO && !fs_calls && text_match(0, summary) && text_match(1, retained) && text_match(2, backup), stored_result, stored_error);
    append_case("append-header-error", NULL, NULL, HEADER_ERROR, ENOSPC);
    append_case("append-newline-error", "row without LF", backup, NEWLINE_ERROR, ENOSPC);
    size_t length = strlen(header) + 40 * strlen("1700000000,3600,1.000,100.0,200,300,130,140\n");
    char *large = malloc(length + 1); assert(large); strcpy(large, header);
    for (unsigned i = 0; i < 40; i++) strcat(large, "1700000000,3600,1.000,100.0,200,300,130,140\n");
    append_case("append-multi-block", large, backup, NO_FAULT, 0);
    append_case("append-next-read-error", large, backup, NEXT_READ_ERROR, EIO);
    append_case("append-full-count-read-error", large, backup, FULL_READ_ERROR, EIO); free(large);
    prepare(summary, backup); expected_error = 0; bool stable = true;
    for (unsigned i = 0; i < 25; i++) { ride_load_history(); stable = stable && !handles && !recorded_error; }
    report("history-reopen-25", stable && text_match(0, summary) && text_match(2, backup) && text_match(1, retained), 0, 0);
    history_boundary("history-open-no-errno-existing", OPEN_READ_ERROR, true, false, false, false);
    history_boundary("history-open-no-errno-missing", OPEN_READ_ERROR, true, false, false, true);
    history_boundary("history-close-no-errno", SOURCE_CLOSE_ERROR, true, false, false, false);
    history_boundary("history-positive-read-error", READ_ERROR, false, true, false, false);
    history_boundary("history-positive-read-no-errno", READ_ERROR, true, true, false, false);
    history_boundary("history-first-positive-read-error", READ_ERROR, false, true, true, false);
    history_boundary("history-first-read-no-errno", READ_ERROR, true, false, true, false);
    history_failure_retry("history-open-retry-25", OPEN_READ_ERROR);
    history_failure_retry("history-positive-read-retry-25", READ_ERROR);
    const struct { const char *name; fault_t point; } preparation[] = {
        {"directory", FOLDER_ERROR}, {"temp-removal", INITIAL_REMOVE_ERROR},
        {"open-read", OPEN_READ_ERROR}, {"open-write", OPEN_WRITE_ERROR}, {"source-close", SOURCE_CLOSE_ERROR}};
    for (unsigned i = 0; i < sizeof(preparation) / sizeof(preparation[0]); i++) {
        for (unsigned missing = 0; missing < 2; missing++) {
            char name[96]; assert(snprintf(name, sizeof(name), "append-%s-boundary-%s", preparation[i].name, missing ? "no-errno" : "error") > 0);
            append_boundary(name, summary, preparation[i].point, missing != 0, false, false, NULL, 0);
        }
    }
    append_boundary("append-open-no-errno-missing", NULL, OPEN_READ_ERROR, true, false, false, NULL, 0);
    const struct { const char *name, *data; fault_t point; } stages[] = {
        {"copy", summary, COPY_ERROR}, {"newline", "row without LF", NEWLINE_ERROR},
        {"header", NULL, HEADER_ERROR}, {"row", summary, ROW_ERROR}};
    for (unsigned i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        for (unsigned positive = 0; positive < 2; positive++) {
            for (unsigned missing = 0; missing < 2; missing++) {
                char name[96]; assert(snprintf(name, sizeof(name), "append-%s-%s-%s", stages[i].name,
                    positive ? "positive" : "rejected", missing ? "no-errno" : "error") > 0);
                append_boundary(name, stages[i].data, stages[i].point, missing != 0, positive != 0, false, NULL, 0);
            }
        }
    }
    append_boundary("append-first-open-no-errno", summary, OPEN_WRITE_ERROR, true, false, true, NULL, 0);
    append_boundary("append-first-read-no-errno", summary, READ_ERROR, true, false, true, "", 0);
    append_boundary("append-first-copy-no-errno", summary, COPY_ERROR, true, false, true, summary, strlen(summary) - 1);
    append_boundary("append-first-positive-copy-error", summary, COPY_ERROR, false, true, true, summary, strlen(summary));
    append_boundary("append-first-positive-copy-no-errno", summary, COPY_ERROR, true, true, true, summary, strlen(summary));
    char complete[256]; assert(snprintf(complete, sizeof(complete), "%s%s", summary, new_row) > 0);
    append_boundary("append-first-positive-row-error", summary, ROW_ERROR, false, true, true, complete, strlen(complete));
    append_boundary("append-first-positive-row-no-errno", summary, ROW_ERROR, true, true, true, complete, strlen(complete));
    append_failure_retry("append-preparation-retry-25", FOLDER_ERROR);
    append_failure_retry("append-positive-copy-retry-25", COPY_ERROR);
    for (unsigned i = 0; i < 3; i++) if (remove(paths[i]) != 0) assert(errno == ENOENT);
    printf("%s %u ride history cases failures=%u handles=%u (native files; controlled UI/I/O)\n", failures ? "FAIL" : "PASS", cases, failures, handles);
    return failures ? 1 : 0;
}
