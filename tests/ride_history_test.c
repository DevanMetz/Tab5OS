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
static bool read_error;
static int recorded_error;
typedef enum { NO_FAULT, NO_SD, FOLDER_ERROR, FINAL_STAT_ERROR, BACKUP_STAT_ERROR,
    RECOVERY_ERROR, INITIAL_REMOVE_ERROR, OPEN_READ_ERROR, OPEN_WRITE_ERROR,
    READ_ERROR, NEXT_READ_ERROR, READ_ZERO_ERRNO, FULL_READ_ERROR, SOURCE_CLOSE_ERROR,
    COPY_ERROR, COPY_ZERO_ERRNO, NEWLINE_ERROR, HEADER_ERROR, ROW_ERROR,
    FLUSH_ERROR, SYNC_ERROR, OUTPUT_CLOSE_ERROR, PUBLISH_ERROR, ROLLBACK_ERROR,
    READ_CLEANUP_ERROR, COPY_CLEANUP_ERROR, OPEN_CLEANUP_ERROR, STALE_READ_ERROR } fault_t;
static fault_t fault;

static const char *native_path(const char *path)
{
    const char *source[] = {SD_PATH "/RIDES/SUMMARY.CSV", SD_PATH "/RIDES/SUMMARY.TMP", SD_PATH "/RIDES/SUMMARY.BAK"};
    for (unsigned i = 0; i < 3; i++) if (!strcmp(path, source[i])) return paths[i];
    assert(false); return NULL;
}
FILE *fixture_open(const char *path, const char *mode)
{
    fs_calls++; bool input = !strcmp(mode, "rb");
    if (!input) mutations++;
    if (input && fault == OPEN_READ_ERROR) { errno = EACCES; return NULL; }
    if (!input && (fault == OPEN_WRITE_ERROR || fault == OPEN_CLEANUP_ERROR)) { errno = ENOSPC; return NULL; }
    FILE *file = fopen(native_path(path), mode);
    if (file) { handles++; if (input) source_file = file; else output_file = file; }
    return file;
}
size_t fixture_read(void *buffer, size_t size, size_t count, FILE *file)
{
    assert(file == source_file && size == 1); fs_calls++; reads++;
    if (((fault == READ_ERROR || fault == READ_ZERO_ERRNO || fault == FULL_READ_ERROR || fault == READ_CLEANUP_ERROR) && reads == 1) ||
        (fault == NEXT_READ_ERROR && reads == 2)) {
        size_t read = fread(buffer, size, fault == FULL_READ_ERROR ? count : count / 2, file);
        read_error = true; errno = fault == READ_ZERO_ERRNO ? 0 : EIO; return read;
    }
    return fread(buffer, size, count, file);
}
char *fixture_gets(char *buffer, int size, FILE *file)
{
    assert(file == source_file); fs_calls++; reads++;
    if (fault == STALE_READ_ERROR && reads == 3) { read_error = true; return NULL; }
    if (((fault == READ_ERROR || fault == READ_ZERO_ERRNO || fault == READ_CLEANUP_ERROR) && reads == 3) ||
        (fault == NEXT_READ_ERROR && reads == 4)) { read_error = true; errno = fault == READ_ZERO_ERRNO ? 0 : EIO; return NULL; }
    char *result = fgets(buffer, size, file);
    if (result && fault == STALE_READ_ERROR) errno = ENOSPC;
    return result;
}
int fixture_error(FILE *file) { return (file == source_file && read_error) || ferror(file); }
size_t fixture_write(const void *buffer, size_t size, size_t count, FILE *file)
{
    assert(file == output_file && size == 1); fs_calls++; writes++; mutations++;
    if (fault == COPY_ERROR || fault == COPY_ZERO_ERRNO || fault == COPY_CLEANUP_ERROR) {
        assert(count); size_t written = fwrite(buffer, size, count - 1, file); copied += (unsigned)written;
        errno = fault == COPY_ZERO_ERRNO ? 0 : ENOSPC; return written;
    }
    size_t written = fwrite(buffer, size, count, file); copied += (unsigned)written; return written;
}
int fixture_close(FILE *file)
{
    assert(handles); handles--; fs_calls++;
    bool input = file == source_file;
    if (input) { source_closes++; source_file = NULL; } else { assert(file == output_file); output_closes++; output_file = NULL; }
    int result = fclose(file);
    if ((input && (fault == SOURCE_CLOSE_ERROR || fault == READ_CLEANUP_ERROR || fault == COPY_CLEANUP_ERROR || fault == OPEN_CLEANUP_ERROR)) ||
        (!input && (fault == OUTPUT_CLOSE_ERROR || fault == READ_CLEANUP_ERROR || fault == COPY_CLEANUP_ERROR))) { errno = EPERM; return EOF; }
    return result;
}
int fixture_stat(const char *path, struct stat *info)
{
    fs_calls++; const char *mapped = native_path(path);
    if ((mapped == paths[0] && fault == FINAL_STAT_ERROR) || (mapped == paths[2] && fault == BACKUP_STAT_ERROR)) { errno = EACCES; return -1; }
    return stat(mapped, info);
}
int fixture_remove(const char *path)
{
    fs_calls++; mutations++; const char *mapped = native_path(path);
    if (mapped == paths[1]) {
        temporary_removes++;
        if (fault == INITIAL_REMOVE_ERROR || ((fault == READ_CLEANUP_ERROR || fault == COPY_CLEANUP_ERROR) && temporary_removes > 1)) { errno = EACCES; return -1; }
    }
    return remove(mapped);
}
int fixture_rename(const char *from, const char *to)
{
    fs_calls++; mutations++; const char *a = native_path(from), *b = native_path(to);
    if ((a == paths[2] && b == paths[0] && (fault == RECOVERY_ERROR || fault == ROLLBACK_ERROR)) ||
        (a == paths[1] && b == paths[0] && (fault == PUBLISH_ERROR || fault == ROLLBACK_ERROR))) { errno = EACCES; return -1; }
    return rename(a, b);
}
int fixture_mkdir(const char *path, int mode)
{
    assert(!strcmp(path, SD_PATH "/RIDES") && mode == 0775); fs_calls++; mutations++;
    if (fault == FOLDER_ERROR) { errno = ENOSPC; return -1; } errno = EEXIST; return -1;
}
int fixture_putc(int byte, FILE *file)
{
    assert(file == output_file); fs_calls++; mutations++;
    if (fault == NEWLINE_ERROR) { errno = ENOSPC; return EOF; } return fputc(byte, file);
}
int fixture_puts(const char *text, FILE *file)
{
    assert(file == output_file); fs_calls++; mutations++;
    if (fault == HEADER_ERROR) { errno = ENOSPC; return EOF; } return fputs(text, file);
}
int fixture_printf(FILE *file, const char *format, ...)
{
    assert(file == output_file); fs_calls++; mutations++;
    if (fault == ROW_ERROR) { errno = ENOSPC; return -1; }
    va_list args; va_start(args, format); int result = vfprintf(file, format, args); va_end(args); return result;
}
int fixture_flush(FILE *file)
{
    fs_calls++; if (fault == FLUSH_ERROR) { errno = EIO; return EOF; } return fflush(file);
}
#ifdef _WIN32
int fixture_sync(int descriptor) { fs_calls++; if (fault == SYNC_ERROR) { errno = EIO; return -1; } return _commit(descriptor); }
#define _commit fixture_sync
#else
int fixture_sync(int descriptor) { fs_calls++; if (fault == SYNC_ERROR) { errno = EIO; return -1; } return fsync(descriptor); }
#define fsync fixture_sync
#endif
void lv_label_set_text(lv_obj_t *object, const char *text)
{ assert(object && strlen(text) < sizeof(object->text)); strcpy(object->text, text); }
void lv_label_set_text_fmt(lv_obj_t *object, const char *format, ...)
{
    assert(object); va_list args; va_start(args, format); int result = vsnprintf(object->text, sizeof(object->text), format, args); va_end(args);
    assert(result >= 0 && (size_t)result < sizeof(object->text));
}
void sd_record_error(int error) { recorded_error = error; }
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
    source_file = output_file = NULL; read_error = false; fault = NO_FAULT; sd_ready = true; recorded_error = 0;
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
    for (unsigned i = 0; i < 3; i++) if (remove(paths[i]) != 0) assert(errno == ENOENT);
    printf("%s %u ride history cases failures=%u handles=%u (native files; controlled UI/I/O)\n", failures ? "FAIL" : "PASS", cases, failures, handles);
    return failures ? 1 : 0;
}
