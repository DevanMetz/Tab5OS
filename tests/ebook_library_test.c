/* Actual library/extension callbacks; existing native BOOKS directory only. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <process.h>
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
static void sd_record_error(int error) { reports++; reported_error = error; }
static int fixture_mkdir(const char *path, int mode)
{
    assert(owns_directory && !strcmp(path, SD_PATH "/BOOKS") && mode == 0775); mkdirs++;
    if (fault == MKDIR_FAIL || fault == MKDIR_NO_ERRNO) { errno = fault == MKDIR_FAIL ? EROFS : 0; return -1; }
    int result = make_directory(physical); assert(result != 0 && errno == EEXIST); return result;
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
/* The unchanged welcome-writing branch must not run for an existing directory. */
static FILE *fixture_fopen(const char *path, const char *mode) { (void)path; (void)mode; assert(false); return NULL; }
static int fixture_fputs(const char *text, FILE *file) { (void)text; (void)file; assert(false); return EOF; }
static int fixture_fclose(FILE *file) { (void)file; assert(false); return EOF; }
static int fixture_remove(const char *path) { (void)path; assert(false); return -1; }
static int storage_commit_new_file(FILE **file, const char *temporary, const char *final)
{ (void)file; (void)temporary; (void)final; assert(false); return -1; }

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
#undef fclose
#undef remove

static void clean_files(void)
{
    assert(owns_directory && !handles); char name[128];
    for (unsigned i = 0; i < files; i++) { snprintf(name, sizeof(name), "%s/%s", physical, names[i]); assert(remove(name) == 0); }
    if (dirs) { snprintf(name, sizeof(name), "%s/FOLDER.TXT", physical); assert(remove_directory(name) == 0); }
    files = dirs = 0;
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
    clear_content(); clean_files(); assert(remove_directory(physical) == 0); owns_directory = false;
    printf("%s %u ebook library cases failures=%u handles=%u timers=%u (native directories/files; controlled UI/timers/mount/faults)\n", failures ? "FAIL" : "PASS", cases, failures, handles, timer_handles);
    return failures ? 1 : 0;
}
