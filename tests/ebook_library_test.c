/* Actual library/extension callbacks; native BOOKS listing and welcome files. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <process.h>
#include <io.h>
#define fixture_pid _getpid
#define make_directory(path) _mkdir(path)
#define remove_directory(path) _rmdir(path)
#else
#include <dirent.h>
#include <unistd.h>
#define fixture_pid getpid
#define make_directory(path) mkdir(path, 0775)
#define remove_directory(path) rmdir(path)
#endif
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
#include "ebook_library_config.inc"
#include "storage_io.h"

typedef enum { NORMAL, MKDIR_FAIL, MKDIR_NO_ERRNO, OPEN_FAIL, OPEN_NO_ERRNO,
    READ_FIRST, READ_LATER, READ_CLOSE, CLOSE_FAIL, CLOSE_NO_ERRNO,
    STAT_FAIL, STAT_NO_ERRNO, STAT_READ_CLOSE, STALE_OPEN, STALE_STAT, STALE_CLOSE, LONG_NAME } fault_t;
static fault_t fault;
static char physical[96], names[80][32], file_paths[BOOKS_CAPACITY][BOOKS_PATH_BYTES];
static size_t file_path_count;
static unsigned files, dirs, handles, mkdirs, opens, reads, closes, stats, reports, returned, cases, failures;
static int reported_error;
static bool owns_directory, sd_ready, ebook_download_busy, defaults_missing, long_returned;
struct fixture_dirent { char d_name[BOOKS_PATH_BYTES]; };
typedef struct {
#ifdef _WIN32
    HANDLE handle;
    WIN32_FIND_DATAA data;
    bool first;
#else
    DIR *handle;
#endif
} fixture_dir_t;
static fixture_dir_t directory;
static struct fixture_dirent entry;
typedef enum { W_OPEN, W_WRITE, W_FLUSH, W_SYNC, W_CLOSE, W_PROBE, W_RENAME, W_REMOVE, W_OPERATIONS } welcome_operation_t;
typedef struct { bool enabled; int error; } welcome_fault_t;
static welcome_fault_t welcome_faults[W_OPERATIONS];
static bool welcome_run, welcome_create, welcome_collision, welcome_write_flag, welcome_flush_flag, welcome_stream_error;
static FILE *welcome_stream;
static unsigned file_handles, welcome_writes, welcome_flushes, welcome_syncs, welcome_closes, welcome_probes, welcome_renames, welcome_removes;
static const char welcome_text[] = "Welcome to Tab5 Books!\n\nCopy .txt ebooks into the BOOKS folder on the SD card. Use Next and Prev to move through the book, and Text to change the reading size.\n";
static const char previous_welcome[] = "previous complete welcome\n";
static void welcome_path(const char *logical, char *actual, size_t capacity)
{
    const char *prefix = SD_PATH "/BOOKS/"; size_t length = strlen(prefix);
    assert(welcome_run && owns_directory && !strncmp(logical, prefix, length));
    const char *leaf = logical + length;
    assert(!strcmp(leaf, "WELCOME.TMP") || !strcmp(leaf, "WELCOME.TXT"));
    int size = snprintf(actual, capacity, "%s/%s", physical, leaf); assert(size > 0 && (size_t)size < capacity);
}
static bool welcome_fault(welcome_operation_t operation, int incoming)
{
    if (!welcome_faults[operation].enabled) return false;
    errno = welcome_faults[operation].error ? welcome_faults[operation].error : incoming;
    return true;
}
static void sd_record_error(int error) { reports++; reported_error = error; errno = EPERM; }
static int fixture_mkdir(const char *path, int mode)
{
    assert(owns_directory && !strcmp(path, SD_PATH "/BOOKS") && mode == 0775); mkdirs++;
    if (fault == MKDIR_FAIL || fault == MKDIR_NO_ERRNO) { errno = fault == MKDIR_FAIL ? EROFS : 0; return -1; }
    int result = make_directory(physical);
    if (welcome_run && welcome_create) {
        assert(result == 0); welcome_create = false;
        if (welcome_collision) {
            char path[128]; welcome_path(SD_PATH "/BOOKS/WELCOME.TXT", path, sizeof(path));
            FILE *file = fopen(path, "wb"); assert(file);
            assert(fwrite(previous_welcome, 1, strlen(previous_welcome), file) == strlen(previous_welcome) && fclose(file) == 0);
        }
        errno = EROFS;
    } else assert(result != 0 && errno == EEXIST);
    return result;
}
static fixture_dir_t *fixture_open(const char *path)
{
    assert(owns_directory && !handles && !strcmp(path, SD_PATH "/BOOKS")); opens++; returned = 0; long_returned = false;
    if (fault == OPEN_FAIL || fault == OPEN_NO_ERRNO) { errno = fault == OPEN_FAIL ? EACCES : 0; return NULL; }
#ifdef _WIN32
    char pattern[128]; snprintf(pattern, sizeof(pattern), "%s\\*", physical);
    directory.handle = FindFirstFileA(pattern, &directory.data); directory.first = true;
    if (directory.handle == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND) { errno = EIO; return NULL; }
#else
    directory.handle = opendir(physical); if (!directory.handle) return NULL;
#endif
    handles++; if (fault == STALE_OPEN) errno = ENOSPC; return &directory;
}
static struct fixture_dirent *fixture_read(fixture_dir_t *dir)
{
    assert(dir == &directory && handles == 1); reads++;
    if (fault == LONG_NAME && !long_returned) {
        long_returned = true; memset(entry.d_name, 'X', 238); strcpy(entry.d_name + 238, ".TXT"); return &entry;
    }
    if ((fault == READ_FIRST && returned == 0) || ((fault == READ_LATER || fault == READ_CLOSE || fault == STAT_READ_CLOSE) && returned == 1)) {
        errno = fault == READ_CLOSE ? EACCES : EIO; return NULL;
    }
    for (;;) {
        const char *name;
#ifdef _WIN32
        if (directory.handle == INVALID_HANDLE_VALUE) return NULL;
        if (!directory.first && !FindNextFileA(directory.handle, &directory.data)) {
            if (GetLastError() != ERROR_NO_MORE_FILES) errno = EIO;
            return NULL;
        }
        directory.first = false; name = directory.data.cFileName;
#else
        struct dirent *native = readdir(directory.handle); if (!native) return NULL; name = native->d_name;
#endif
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        assert(strlen(name) < sizeof(entry.d_name)); strcpy(entry.d_name, name); returned++; return &entry;
    }
}
static int fixture_close(fixture_dir_t *dir)
{
    assert(dir == &directory && handles == 1); handles--; closes++;
#ifdef _WIN32
    assert(directory.handle == INVALID_HANDLE_VALUE || FindClose(directory.handle)); directory.handle = INVALID_HANDLE_VALUE;
#else
    assert(closedir(directory.handle) == 0); directory.handle = NULL;
#endif
    if (fault == CLOSE_FAIL || fault == CLOSE_NO_ERRNO || fault == READ_CLOSE || fault == STAT_READ_CLOSE) {
        errno = fault == CLOSE_NO_ERRNO ? 0 : fault == READ_CLOSE || fault == STAT_READ_CLOSE ? EPERM : EIO; return -1;
    }
    if (fault == STALE_CLOSE) errno = ENOSPC;
    return 0;
}
int fixture_stat(const char *path, struct stat *info)
{
    stats++; const char *prefix = SD_PATH "/BOOKS/"; size_t length = strlen(prefix); assert(!strncmp(path, prefix, length));
    char actual[128]; snprintf(actual, sizeof(actual), "%s/%s", physical, path + length);
    int result = stat(actual, info);
    if ((fault == STAT_FAIL || fault == STAT_NO_ERRNO || fault == STAT_READ_CLOSE) && stats == 1) {
        errno = fault == STAT_NO_ERRNO ? 0 : EACCES; return -1;
    }
    if (fault == STALE_STAT) errno = ENOSPC;
    return result;
}
static FILE *fixture_fopen(const char *path, const char *mode)
{
    char actual[128]; welcome_path(path, actual, sizeof(actual));
    assert(!strcmp(path, SD_PATH "/BOOKS/WELCOME.TMP") && !strcmp(mode, "wb") && !file_handles && !welcome_stream);
    int incoming = errno; if (welcome_fault(W_OPEN, incoming)) return NULL;
    welcome_stream = fopen(actual, mode); assert(welcome_stream); file_handles++; welcome_stream_error = false;
    errno = EACCES; return welcome_stream;
}
static int fixture_fputs(const char *text, FILE *file)
{
    assert(file == welcome_stream && file_handles == 1 && !strcmp(text, welcome_text)); welcome_writes++;
    int incoming = errno;
    if (welcome_faults[W_WRITE].enabled) {
        assert(fwrite(text, 1, 9, file) == 9); (void)welcome_fault(W_WRITE, incoming); return EOF;
    }
    int result = fputs(text, file); assert(result >= 0);
    if (welcome_write_flag) {
        welcome_stream_error = true;
        errno = welcome_faults[W_WRITE].error ? welcome_faults[W_WRITE].error : incoming;
    } else errno = ERANGE;
    return result;
}
static int fixture_ferror(FILE *file)
{ assert(file == welcome_stream && file_handles == 1); return welcome_stream_error || ferror(file); }
static int fixture_fflush(FILE *file)
{
    assert(file == welcome_stream && file_handles == 1); welcome_flushes++; int incoming = errno;
    if (welcome_fault(W_FLUSH, incoming)) return EOF;
    int result = fflush(file); assert(result == 0);
    if (welcome_flush_flag) welcome_stream_error = true;
    errno = ERANGE; return result;
}
static int fixture_sync(int descriptor)
{
#ifdef _WIN32
    assert(welcome_stream && descriptor == _fileno(welcome_stream));
#else
    assert(welcome_stream && descriptor == fileno(welcome_stream));
#endif
    assert(file_handles == 1); welcome_syncs++; int incoming = errno;
    if (welcome_fault(W_SYNC, incoming)) return -1;
#ifdef _WIN32
    int result = _commit(descriptor);
#else
    int result = fsync(descriptor);
#endif
    assert(result == 0); errno = ERANGE; return result;
}
static int fixture_fclose(FILE *file)
{
    assert(file == welcome_stream && file_handles == 1); welcome_closes++; int incoming = errno;
    assert(fclose(file) == 0); welcome_stream = NULL; file_handles--; welcome_stream_error = false;
    if (welcome_fault(W_CLOSE, incoming)) return EOF;
    errno = ERANGE; return 0;
}
static int fixture_remove(const char *path)
{
    char actual[128]; welcome_path(path, actual, sizeof(actual)); welcome_removes++; int incoming = errno;
    if (welcome_fault(W_REMOVE, incoming)) return -1;
    int result = remove(actual); if (result == 0) errno = ERANGE; return result;
}
static int fixture_welcome_stat(const char *path, struct stat *info)
{
    char actual[128]; welcome_path(path, actual, sizeof(actual)); welcome_probes++; int incoming = errno;
    if (welcome_fault(W_PROBE, incoming)) return -1;
    int result = stat(actual, info); if (result == 0) errno = ERANGE; return result;
}
static int fixture_rename(const char *from, const char *to)
{
    char source[128], target[128]; welcome_path(from, source, sizeof(source)); welcome_path(to, target, sizeof(target));
    welcome_renames++; int incoming = errno; if (welcome_fault(W_RENAME, incoming)) return -1;
    int result = rename(source, target); assert(result == 0); errno = ERANGE; return result;
}
#define fopen fixture_fopen
#define ferror fixture_ferror
#define fflush fixture_fflush
#define fclose fixture_fclose
#define remove fixture_remove
#define stat(path, info) fixture_welcome_stat(path, info)
#define rename fixture_rename
#ifdef _WIN32
#define _commit fixture_sync
#else
#define fsync fixture_sync
#endif
#include "storage_source.inc"
#undef fopen
#undef ferror
#undef fflush
#undef fclose
#undef remove
#undef stat
#undef rename
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif

#define LV_SYMBOL_LEFT "LEFT"
#define LV_SYMBOL_FILE "FILE"
#define LV_EVENT_CLICKED 1
typedef struct { int unused; } lv_event_t;
typedef void (*lv_event_cb_t)(lv_event_t *);
typedef struct { char text[400]; lv_event_cb_t callback; const char *path; } lv_obj_t;
typedef struct { int unused; } lv_timer_t;
static lv_obj_t root, title, list, rows[80];
static lv_obj_t *content = &root;
static lv_timer_t timer, *ebook_timer;
static unsigned row_count, timer_handles, timer_creates, clears, default_probes;
static const int lv_font_montserrat_28 = 28;
static void clear_content(void) { clears++; row_count = timer_handles = 0; ebook_timer = NULL; memset(rows, 0, sizeof(rows)); }
static void home_clicked(lv_event_t *event) { (void)event; }
static void ebook_open_clicked(lv_event_t *event) { (void)event; }
static void ebook_download_tick(lv_timer_t *active) { (void)active; assert(false); }
static bool ebook_defaults_missing(void) { default_probes++; return defaults_missing; }
static lv_timer_t *lv_timer_create(void (*callback)(lv_timer_t *), unsigned period, void *data)
{ assert(callback == ebook_download_tick && period == 500 && !data && !timer_handles); timer_handles++; timer_creates++; return &timer; }
static lv_obj_t *lv_label_create(lv_obj_t *parent) { assert(parent == content); return &title; }
static void lv_label_set_text(lv_obj_t *object, const char *text) { assert(object == &title && !strcmp(text, "Ebooks")); strcpy(object->text, text); }
static void lv_obj_set_style_text_font(lv_obj_t *object, const int *font, int part) { assert(object == &title && font == &lv_font_montserrat_28 && !part); }
static lv_obj_t *lv_list_create(lv_obj_t *parent) { assert(parent == content); return &list; }
static void lv_obj_set_size(lv_obj_t *object, int width, int height) { assert(object == &list && width == 640 && height == 940); }
static lv_obj_t *lv_list_add_button(lv_obj_t *parent, const char *symbol, const char *text)
{
    assert(parent == &list && row_count < 80 && (!symbol || !strcmp(symbol, LV_SYMBOL_LEFT) || !strcmp(symbol, LV_SYMBOL_FILE)));
    lv_obj_t *row = &rows[row_count++]; assert(strlen(text) < sizeof(row->text)); strcpy(row->text, text); return row;
}
static void lv_list_add_text(lv_obj_t *parent, const char *text) { (void)lv_list_add_button(parent, NULL, text); }
static void lv_obj_add_event_cb(lv_obj_t *object, lv_event_cb_t callback, int event, void *data)
{ assert(object >= rows && object < rows + row_count && event == LV_EVENT_CLICKED); object->callback = callback; object->path = data; }
#define DIR fixture_dir_t
#define dirent fixture_dirent
#define mkdir fixture_mkdir
#define opendir fixture_open
#define readdir fixture_read
#define closedir fixture_close
#define stat(path, info) fixture_stat(path, info)
#define fopen fixture_fopen
#define fputs fixture_fputs
#define ferror fixture_ferror
#define fclose fixture_fclose
#define remove fixture_remove
#include "ebook_library.inc"
#undef DIR
#undef dirent
#undef mkdir
#undef opendir
#undef readdir
#undef closedir
#undef stat
#undef fopen
#undef fputs
#undef ferror
#undef fclose
#undef remove

static void clean_files(void)
{
    assert(owns_directory && !handles && !file_handles && !welcome_stream); char name[128];
    for (unsigned i = 0; i < files; i++) { snprintf(name, sizeof(name), "%s/%s", physical, names[i]); assert(remove(name) == 0); }
    if (dirs) { snprintf(name, sizeof(name), "%s/FOLDER.TXT", physical); assert(remove_directory(name) == 0); }
    files = dirs = 0;
    if (welcome_run) {
        const char *leaves[] = {"WELCOME.TMP", "WELCOME.TXT"};
        for (unsigned i = 0; i < 2; i++) {
            snprintf(name, sizeof(name), "%s/%s", physical, leaves[i]); struct stat info;
            if (stat(name, &info) == 0) { assert(S_ISREG(info.st_mode)); assert(remove(name) == 0); }
            else assert(errno == ENOENT);
        }
    }
}
static void add_file(const char *leaf)
{
    assert(files < 80 && strlen(leaf) < sizeof(names[0])); strcpy(names[files], leaf);
    char name[128]; snprintf(name, sizeof(name), "%s/%s", physical, leaf); FILE *file = fopen(name, "wb"); assert(file);
    assert(fputc((int)(files + 1), file) != EOF && fclose(file) == 0); files++;
}
static void prepare(unsigned count, bool child)
{
    clean_files();
    welcome_run = welcome_create = welcome_collision = welcome_write_flag = welcome_flush_flag = welcome_stream_error = false;
    memset(welcome_faults, 0, sizeof(welcome_faults));
    welcome_writes = welcome_flushes = welcome_syncs = welcome_closes = welcome_probes = welcome_renames = welcome_removes = 0;
    for (unsigned i = 0; i < count; i++) { char leaf[32]; snprintf(leaf, sizeof(leaf), "B%02u.TXT", i); add_file(leaf); }
    if (child) { char path[128]; snprintf(path, sizeof(path), "%s/FOLDER.TXT", physical); assert(make_directory(path) == 0); dirs = 1; }
    fault = NORMAL; sd_ready = true; ebook_download_busy = defaults_missing = false;
    mkdirs = opens = reads = closes = stats = reports = returned = timer_creates = clears = default_probes = 0; reported_error = 0;
}
static bool unchanged(void)
{
    char name[128]; struct stat info;
    for (unsigned i = 0; i < files; i++) {
        snprintf(name, sizeof(name), "%s/%s", physical, names[i]); FILE *file = fopen(name, "rb"); if (!file) return false;
        bool pass = fgetc(file) == (int)(i + 1) && fgetc(file) == EOF && !ferror(file); assert(fclose(file) == 0); if (!pass) return false;
    }
    if (dirs) { snprintf(name, sizeof(name), "%s/FOLDER.TXT", physical); if (stat(name, &info) != 0 || !S_ISDIR(info.st_mode)) return false; }
    return true;
}
static bool text_contains(const char *text)
{ for (unsigned i = 0; i < row_count; i++) if (strstr(rows[i].text, text)) return true; return false; }
static unsigned books(void)
{ unsigned count = 0; for (unsigned i = 0; i < row_count; i++) count += rows[i].callback == ebook_open_clicked; return count; }
static bool paths_valid(void)
{
    for (unsigned i = 0; i < row_count; i++) {
        lv_obj_t *row = &rows[i]; if (row->callback != ebook_open_clicked) continue;
        bool owned = false; for (size_t j = 0; j < file_path_count; j++) if (row->path == file_paths[j]) owned = true;
        if (!owned || strlen(row->path) >= BOOKS_PATH_BYTES || strncmp(row->path, SD_PATH "/BOOKS/", strlen(SD_PATH "/BOOKS/"))) return false;
        const char *name = strrchr(row->path, '/'); if (!name || strcmp(name + 1, row->text)) return false;
    }
    return file_path_count <= BOOKS_CAPACITY && books() == file_path_count;
}
static bool no_listing_warning(void)
{ return !text_contains("Book listing incomplete:") && !text_contains("Book entry limit reached") && !text_contains("book paths that exceed the path limit"); }
static bool no_warning(void) { return no_listing_warning() && !reports; }
static bool error_message(const char *prefix, int error)
{ char message[128]; snprintf(message, sizeof(message), "%s: %s", prefix, strerror(error)); return text_contains(message) && reports > 0 && reported_error == error; }
static void report(const char *name, bool pass)
{
    bool same = unchanged(); pass = pass && same && !handles && paths_valid(); cases++; failures += !pass;
    printf("%s %s books=%u path_slots=%zu incomplete=%d limited=%d skipped=%d empty=%d reported_error=%d sd_reports=%u mkdirs=%u opens=%u stats=%u closes=%u handles=%u timer=%u file_unchanged=%d\n",
        pass ? "PASS" : "FAIL", name, books(), file_path_count, text_contains("Book listing incomplete:"), text_contains("Book entry limit reached"),
        text_contains("book paths that exceed the path limit"), text_contains("Copy .txt books into /sdcard/BOOKS"), reported_error, reports,
        mkdirs, opens, stats, closes, handles, timer_handles, same);
}
static void failure_case(const char *name, fault_t injected, int error, unsigned expected_books)
{
    prepare(3, false); fault = injected; show_ebooks();
    bool setup = injected == MKDIR_FAIL || injected == MKDIR_NO_ERRNO;
    bool opening = injected == OPEN_FAIL || injected == OPEN_NO_ERRNO;
    const char *prefix = setup ? "Could not prepare /sdcard/BOOKS" : opening ? "Could not open /sdcard/BOOKS" : "Book listing incomplete";
    report(name, books() == expected_books && error_message(prefix, error) && reports == 1 && !text_contains("Copy .txt books into /sdcard/BOOKS") &&
        opens == (unsigned)!setup && closes == (unsigned)(!setup && !opening) && timer_handles == (unsigned)(!setup && !opening));
}
static void prepare_welcome(void)
{
    prepare(0, false); assert(remove_directory(physical) == 0); welcome_run = welcome_create = true;
}
static bool welcome_bytes(const char *leaf, const char *expected, size_t size)
{
    char path[128]; snprintf(path, sizeof(path), "%s/%s", physical, leaf); FILE *file = fopen(path, "rb");
    if (!expected) { if (file) { assert(fclose(file) == 0); return false; } return errno == ENOENT; }
    if (!file) return false;
    char buffer[sizeof(welcome_text)]; size_t length = fread(buffer, 1, sizeof(buffer), file);
    bool pass = length == size && !ferror(file) && !memcmp(buffer, expected, size); assert(fclose(file) == 0); return pass;
}
static void report_welcome(const char *name, bool pass, int expected_error, const char *final, size_t final_size, const char *temporary, size_t temporary_size)
{
    bool same = welcome_bytes("WELCOME.TXT", final, final_size) && welcome_bytes("WELCOME.TMP", temporary, temporary_size);
    bool warning = text_contains("Could not create WELCOME.TXT:");
    bool cause = !expected_error ? !reports && !warning : reports == 1 && error_message("Could not create WELCOME.TXT", expected_error);
    pass = pass && same && cause && !file_handles && !welcome_stream && !handles && paths_valid() &&
           !text_contains("Copy .txt books into /sdcard/BOOKS");
    cases++; failures += !pass;
    printf("%s %s books=%u error=%d reports=%u writes=%u flushes=%u syncs=%u file_closes=%u probes=%u renames=%u removes=%u handles=%u file_handles=%u bytes=%d\n",
        pass ? "PASS" : "FAIL", name, books(), reported_error, reports, welcome_writes, welcome_flushes, welcome_syncs,
        welcome_closes, welcome_probes, welcome_renames, welcome_removes, handles, file_handles, same);
}
static void welcome_failure_case(const char *name, welcome_operation_t operation, int cause)
{
    prepare_welcome(); welcome_faults[operation] = (welcome_fault_t){true, cause}; show_ebooks();
    bool wrote = operation != W_OPEN, commit = wrote && operation != W_WRITE;
    bool pass = !books() && welcome_writes == (unsigned)wrote && welcome_closes == (unsigned)wrote &&
                welcome_flushes == (unsigned)commit && welcome_syncs == (unsigned)(commit && operation != W_FLUSH) &&
                welcome_probes == (unsigned)(operation == W_PROBE || operation == W_RENAME) &&
                welcome_renames == (unsigned)(operation == W_RENAME) && welcome_removes == (unsigned)(operation == W_WRITE);
    report_welcome(name, pass, cause ? cause : EIO, NULL, 0, commit ? welcome_text : NULL, commit ? strlen(welcome_text) : 0);
}
static void welcome_cases(void)
{
    prepare_welcome(); show_ebooks();
    report_welcome("welcome-created", books() == 1 && welcome_writes == 1 && welcome_flushes == 1 && welcome_syncs == 1 && welcome_closes == 1 && welcome_probes == 1 && welcome_renames == 1 && !welcome_removes,
                   0, welcome_text, strlen(welcome_text), NULL, 0);
    show_ebooks();
    report_welcome("welcome-existing-reopen", books() == 1 && welcome_writes == 1 && welcome_closes == 1 && !welcome_removes,
                   0, welcome_text, strlen(welcome_text), NULL, 0);
    const welcome_operation_t operations[] = {W_OPEN, W_WRITE, W_FLUSH, W_SYNC, W_CLOSE, W_PROBE, W_RENAME};
    const char *labels[] = {"open", "write", "flush", "sync", "close", "probe", "publish"};
    const int errors[] = {EACCES, ENOSPC, ENOSPC, EIO, EACCES, EACCES, EACCES};
    for (unsigned i = 0; i < sizeof(operations) / sizeof(operations[0]); i++) {
        char name[64]; snprintf(name, sizeof(name), "welcome-%s", labels[i]); welcome_failure_case(name, operations[i], errors[i]);
        snprintf(name, sizeof(name), "welcome-%s-no-errno", labels[i]); welcome_failure_case(name, operations[i], 0);
    }
    for (unsigned missing = 0; missing < 2; missing++) {
        prepare_welcome(); welcome_write_flag = true; welcome_faults[W_WRITE].error = missing ? 0 : ENOSPC; show_ebooks();
        report_welcome(missing ? "welcome-positive-write-flag-no-errno" : "welcome-positive-write-flag",
                       !books() && !welcome_flushes && !welcome_syncs && !welcome_probes && !welcome_renames && welcome_removes == 1 && welcome_closes == 1,
                       missing ? EIO : ENOSPC, NULL, 0, NULL, 0);
    }
    prepare_welcome(); welcome_flush_flag = true; show_ebooks();
    report_welcome("welcome-positive-flush-flag", !books() && welcome_flushes == 1 && !welcome_syncs && !welcome_probes && !welcome_renames && welcome_closes == 1,
                   ERANGE, NULL, 0, welcome_text, strlen(welcome_text));
    prepare_welcome(); welcome_collision = true; show_ebooks();
    report_welcome("welcome-final-collision", books() == 1 && welcome_probes == 1 && !welcome_renames && !welcome_removes,
                   EEXIST, previous_welcome, strlen(previous_welcome), welcome_text, strlen(welcome_text));
    for (unsigned flagged = 0; flagged < 2; flagged++) for (unsigned missing = 0; missing < 2; missing++) {
        prepare_welcome(); welcome_write_flag = flagged != 0;
        welcome_faults[W_WRITE] = (welcome_fault_t){!flagged, missing ? 0 : ENOSPC};
        welcome_faults[W_CLOSE] = (welcome_fault_t){true, EPERM}; welcome_faults[W_REMOVE] = (welcome_fault_t){true, EACCES}; show_ebooks();
        char name[64]; snprintf(name, sizeof(name), "welcome-first-%s-error%s", flagged ? "flag" : "write", missing ? "-no-errno" : "");
        report_welcome(name, !books() && !welcome_flushes && !welcome_syncs && !welcome_probes && !welcome_renames && welcome_removes == 1 && welcome_closes == 1,
                       missing ? EIO : ENOSPC, NULL, 0, welcome_text, flagged ? strlen(welcome_text) : 9);
    }
    prepare_welcome(); welcome_faults[W_WRITE] = (welcome_fault_t){true, ENOSPC}; welcome_faults[W_CLOSE] = (welcome_fault_t){true, EPERM};
    show_ebooks();
    report_welcome("welcome-write-failed-close", !books() && !welcome_flushes && !welcome_syncs && welcome_removes == 1 && welcome_closes == 1,
                   ENOSPC, NULL, 0, NULL, 0);
    prepare_welcome(); welcome_faults[W_SYNC] = (welcome_fault_t){true, EIO}; welcome_faults[W_CLOSE] = (welcome_fault_t){true, EACCES}; show_ebooks();
    report_welcome("welcome-sync-before-close", !books() && welcome_flushes == 1 && welcome_syncs == 1 && welcome_closes == 1 && !welcome_probes && !welcome_renames,
                   EIO, NULL, 0, welcome_text, strlen(welcome_text));
    bool pass = true;
    for (unsigned i = 0; i < 25; i++) {
        prepare_welcome(); welcome_write_flag = true; welcome_faults[W_WRITE].error = ENOSPC;
        welcome_faults[W_CLOSE] = (welcome_fault_t){true, EPERM}; welcome_faults[W_REMOVE] = (welcome_fault_t){true, EACCES}; show_ebooks();
        pass = pass && !books() && reports == 1 && reported_error == ENOSPC && !file_handles && !handles && !welcome_stream &&
               !welcome_flushes && !welcome_syncs && !welcome_renames && error_message("Could not create WELCOME.TXT", ENOSPC) &&
               welcome_bytes("WELCOME.TXT", NULL, 0) && welcome_bytes("WELCOME.TMP", welcome_text, strlen(welcome_text));
        memset(welcome_faults, 0, sizeof(welcome_faults)); welcome_write_flag = false; show_ebooks();
        pass = pass && !books() && reports == 1 && !text_contains("Could not create WELCOME.TXT:") && welcome_writes == 1 &&
               welcome_closes == 1 && welcome_removes == 1 && !file_handles && !handles && !welcome_stream &&
               welcome_bytes("WELCOME.TMP", welcome_text, strlen(welcome_text));
        prepare_welcome(); show_ebooks(); show_ebooks();
        pass = pass && books() == 1 && !reports && welcome_writes == 1 && welcome_closes == 1 && welcome_flushes == 1 && welcome_syncs == 1 &&
               !file_handles && !handles && !welcome_stream && welcome_bytes("WELCOME.TXT", welcome_text, strlen(welcome_text)) && welcome_bytes("WELCOME.TMP", NULL, 0);
    }
    report_welcome("welcome-failure-recreation-reopen-25", pass, 0, welcome_text, strlen(welcome_text), NULL, 0);
}
int main(void)
{
    snprintf(physical, sizeof(physical), ".ebook_library_%d", fixture_pid()); struct stat existing;
    assert(stat(physical, &existing) != 0 && errno == ENOENT); assert(make_directory(physical) == 0); owns_directory = true;
    prepare(0, false); show_ebooks(); report("empty", !books() && no_warning() && text_contains("Copy .txt books into /sdcard/BOOKS") && closes == 1 && timer_handles == 1);
    prepare(3, false); show_ebooks(); report("books", books() == 3 && no_warning() && closes == 1 && timer_handles == 1);
    prepare(2, true); show_ebooks(); report("txt-directory", books() == 2 && no_warning() && !text_contains("FOLDER.TXT"));
    prepare(0, false); add_file("lower.txt"); add_file("mixed.tXt"); add_file("BOOK.TXT.BAK"); add_file("NOEXT"); show_ebooks();
    report("extensions", books() == 2 && no_warning() && text_contains("lower.txt") && text_contains("mixed.tXt"));
    prepare(0, false); add_file("BOOK.TMP"); add_file("PHOTO.PNG"); show_ebooks(); report("unsupported", !books() && no_warning() && text_contains("Copy .txt books into /sdcard/BOOKS"));
    prepare(66, false); show_ebooks(); report("entry-limit", books() == 64 && file_path_count == 64 && text_contains("Book entry limit reached") && !reports && closes == 1);
    prepare(66, false); add_file("UNUSED.TMP"); show_ebooks(); report("filtered-entry-limit", books() == 64 && file_path_count == 64 && text_contains("Book entry limit reached") && !reports && closes == 1);
    prepare(1, false); fault = LONG_NAME; show_ebooks(); report("path-limit", books() == 1 && text_contains("Skipped 1 book paths that exceed the path limit") && !reports && closes == 1);
    prepare(3, false); sd_ready = false; show_ebooks(); report("no-sd", !books() && !mkdirs && !opens && !timer_handles && text_contains("Insert an SD card to read books"));
    failure_case("mkdir", MKDIR_FAIL, EROFS, 0); failure_case("mkdir-no-errno", MKDIR_NO_ERRNO, EIO, 0);
    failure_case("open", OPEN_FAIL, EACCES, 0); failure_case("open-no-errno", OPEN_NO_ERRNO, EIO, 0);
    failure_case("read-first", READ_FIRST, EIO, 0); failure_case("read-later", READ_LATER, EIO, 1);
    failure_case("first-read-error", READ_CLOSE, EACCES, 1);
    failure_case("close", CLOSE_FAIL, EIO, 3); failure_case("close-no-errno", CLOSE_NO_ERRNO, EIO, 3);
    failure_case("stat", STAT_FAIL, EACCES, 2); failure_case("stat-no-errno", STAT_NO_ERRNO, EIO, 2);
    failure_case("first-stat-error", STAT_READ_CLOSE, EACCES, 0);
    const fault_t stale[] = {STALE_OPEN, STALE_STAT, STALE_CLOSE}; const char *stale_names[] = {"stale-open-errno", "stale-stat-errno", "stale-close-errno"};
    for (unsigned i = 0; i < 3; i++) { prepare(3, false); fault = stale[i]; show_ebooks(); report(stale_names[i], books() == 3 && no_warning() && closes == 1); }
    prepare(3, false); ebook_download_busy = true; show_ebooks(); report("downloads-busy", books() == 3 && no_warning() && text_contains("Downloading free classics...") && !default_probes && timer_handles == 1);
    prepare(3, false); defaults_missing = true; show_ebooks(); report("downloads-missing", books() == 3 && no_warning() && text_contains("Classics download failed; restart to retry") && default_probes == 1 && timer_handles == 1);
    prepare(3, false); fault = READ_FIRST; show_ebooks(); bool pass = error_message("Book listing incomplete", EIO) && !books();
    fault = NORMAL; sd_ready = true; show_ebooks(); report("reopen-after-error", pass && books() == 3 && no_listing_warning() && reports == 1 && opens == 2 && closes == 2 && timer_handles == 1);
    prepare(3, false); pass = true;
    for (unsigned i = 0; i < 25; i++) {
        fault = NORMAL; sd_ready = true; show_ebooks(); pass = pass && books() == 3 && no_listing_warning() && reports == i;
        fault = CLOSE_FAIL; show_ebooks(); pass = pass && books() == 3 && error_message("Book listing incomplete", EIO) && reports == i + 1;
        fault = NORMAL; sd_ready = true; show_ebooks(); pass = pass && books() == 3 && no_listing_warning() && reports == i + 1;
    }
    report("cycles-25", pass && mkdirs == 75 && opens == 75 && closes == 75 && reads == 300 && clears == 75 && timer_creates == 75 && timer_handles == 1);
    welcome_cases();
    clear_content(); clean_files(); assert(remove_directory(physical) == 0); owns_directory = false;
    printf("%s %u ebook library cases failures=%u handles=%u file_handles=%u timers=%u (native directories/files; controlled UI/timers/mount/faults)\n", failures ? "FAIL" : "PASS", cases, failures, handles, file_handles, timer_handles);
    return failures ? 1 : 0;
}
