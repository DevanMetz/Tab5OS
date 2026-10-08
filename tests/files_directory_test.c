/* Actual show_files callback; native enumeration/stat and controlled errors/UI. */
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
#endif
#include "files_directory_config.inc"

typedef enum { NORMAL, OPEN_FAIL, OPEN_NO_ERRNO, READ_FIRST, READ_LATER, READ_CLOSE,
               CLOSE_FAIL, CLOSE_NO_ERRNO, STALE_OPEN, STALE_STAT, STALE_CLOSE, WITH_DOTS } fault_t;
static fault_t fault;
static char physical[96], target[FILES_PATH_BYTES], current_directory[FILES_PATH_BYTES];
static char file_paths[FILES_CAPACITY][FILES_PATH_BYTES];
static size_t file_path_count;
static unsigned files, dirs, handles, opens, reads, closes, stats, reports, returned, dot_index, cases, failures;
static int reported_error;
static bool owns_directory, internal_ready, sd_ready;
struct fixture_dirent { char d_name[FILES_PATH_BYTES]; };
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
void sd_record_error(int error) { reports++; reported_error = error; }
static fixture_dir_t *fixture_open(const char *path)
{
    assert(owns_directory && !handles && !strcmp(path, target)); opens++; returned = dot_index = 0;
    if (fault == OPEN_FAIL || fault == OPEN_NO_ERRNO) { errno = fault == OPEN_FAIL ? EACCES : 0; return NULL; }
#ifdef _WIN32
    char pattern[128]; snprintf(pattern, sizeof(pattern), "%s\\*", physical);
    directory.handle = FindFirstFileA(pattern, &directory.data); directory.first = true;
    if (directory.handle == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND) { errno = EIO; return NULL; }
#else
    directory.handle = opendir(physical); if (!directory.handle) return NULL;
#endif
    handles++;
    if (fault == STALE_OPEN) errno = ENOSPC;
    return &directory;
}
static struct fixture_dirent *fixture_read(fixture_dir_t *dir)
{
    assert(dir == &directory && handles == 1); reads++;
    if (fault == WITH_DOTS && dot_index < 2) { strcpy(entry.d_name, dot_index++ ? ".." : "."); return &entry; }
    if ((fault == READ_FIRST && returned == 0) || ((fault == READ_LATER || fault == READ_CLOSE) && returned == 1)) {
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
    assert(directory.handle == INVALID_HANDLE_VALUE || FindClose(directory.handle));
    directory.handle = INVALID_HANDLE_VALUE;
#else
    assert(closedir(directory.handle) == 0); directory.handle = NULL;
#endif
    if (fault == CLOSE_FAIL || fault == CLOSE_NO_ERRNO || fault == READ_CLOSE) {
        errno = fault == CLOSE_NO_ERRNO ? 0 : fault == READ_CLOSE ? EPERM : EIO; return -1;
    }
    if (fault == STALE_CLOSE) errno = ENOSPC;
    return 0;
}
static int fixture_stat(const char *path, struct stat *info)
{
    stats++; size_t prefix = strlen(target); assert(!strncmp(path, target, prefix) && path[prefix] == '/');
    char actual[128]; snprintf(actual, sizeof(actual), "%s/%s", physical, path + prefix + 1);
    int result = stat(actual, info);
    if (fault == STALE_STAT) { errno = EACCES; return -1; }
    return result;
}

#define LV_SYMBOL_LEFT "LEFT"
#define LV_SYMBOL_DIRECTORY "DIR"
#define LV_SYMBOL_SD_CARD "SD"
#define LV_SYMBOL_UP "UP"
#define LV_SYMBOL_FILE "FILE"
#define LV_EVENT_CLICKED 1
typedef struct { int unused; } lv_event_t;
typedef void (*lv_event_cb_t)(lv_event_t *);
typedef struct { char text[400]; const char *symbol; lv_event_cb_t callback; const char *path; } lv_obj_t;
static lv_obj_t root, title, list, rows[96];
static lv_obj_t *content = &root;
static unsigned row_count, clears;
static const int lv_font_montserrat_28 = 28;
static void clear_content(void) { clears++; row_count = 0; memset(rows, 0, sizeof(rows)); }
static void home_clicked(lv_event_t *event) { (void)event; }
static void file_clicked(lv_event_t *event) { (void)event; }
static lv_obj_t *lv_label_create(lv_obj_t *parent) { assert(parent == content); return &title; }
static void lv_label_set_text_fmt(lv_obj_t *object, const char *format, ...)
{
    assert(object == &title); va_list args; va_start(args, format); int count = vsnprintf(object->text, sizeof(object->text), format, args); va_end(args);
    assert(count >= 0 && (size_t)count < sizeof(object->text));
}
static void lv_obj_set_style_text_font(lv_obj_t *object, const int *font, int part) { assert(object == &title && font == &lv_font_montserrat_28 && !part); }
static lv_obj_t *lv_list_create(lv_obj_t *parent) { assert(parent == content); return &list; }
static void lv_obj_set_size(lv_obj_t *object, int width, int height) { assert(object == &list && width == 640 && height == 940); }
static lv_obj_t *lv_list_add_button(lv_obj_t *parent, const char *symbol, const char *text)
{
    assert(parent == &list && row_count < 96); lv_obj_t *row = &rows[row_count++];
    assert(strlen(text) < sizeof(row->text)); strcpy(row->text, text); row->symbol = symbol; return row;
}
static void lv_list_add_text(lv_obj_t *parent, const char *text) { (void)lv_list_add_button(parent, NULL, text); }
static void lv_obj_add_event_cb(lv_obj_t *object, lv_event_cb_t callback, int event, void *data)
{ assert(object >= rows && object < rows + row_count && event == LV_EVENT_CLICKED); object->callback = callback; object->path = data; }
#define DIR fixture_dir_t
#define dirent fixture_dirent
#define opendir fixture_open
#define readdir fixture_read
#define closedir fixture_close
#define stat(path, info) fixture_stat(path, info)
#include "files_directory.inc"
#undef DIR
#undef dirent
#undef opendir
#undef readdir
#undef closedir
#undef stat

static void clean_files(void)
{
    assert(owns_directory && !handles); char name[128];
    for (unsigned i = 0; i < files; i++) { snprintf(name, sizeof(name), "%s/F%02u.TXT", physical, i); assert(remove(name) == 0); }
    if (dirs) { snprintf(name, sizeof(name), "%s/CHILD", physical); assert(remove_directory(name) == 0); }
    files = dirs = 0;
}
static void prepare(unsigned count, bool child, const char *logical)
{
    clean_files(); char name[128];
    for (unsigned i = 0; i < count; i++) {
        snprintf(name, sizeof(name), "%s/F%02u.TXT", physical, i); FILE *file = fopen(name, "wb"); assert(file);
        assert(fputc((int)(i % 251 + 1), file) != EOF && fclose(file) == 0); files++;
    }
    if (child) { snprintf(name, sizeof(name), "%s/CHILD", physical); assert(make_directory(name) == 0); dirs = 1; }
    assert(strlen(logical) < sizeof(target)); strcpy(target, logical);
    fault = NORMAL; internal_ready = sd_ready = true;
    opens = reads = closes = stats = reports = reported_error = clears = 0;
}
static bool unchanged(void)
{
    char name[128]; struct stat info;
    for (unsigned i = 0; i < files; i++) {
        snprintf(name, sizeof(name), "%s/F%02u.TXT", physical, i); FILE *file = fopen(name, "rb");
        if (!file) return false;
        bool pass = fgetc(file) == (int)(i % 251 + 1) && fgetc(file) == EOF && !ferror(file);
        assert(fclose(file) == 0); if (!pass) return false;
    }
    if (dirs) { snprintf(name, sizeof(name), "%s/CHILD", physical); if (stat(name, &info) != 0 || !S_ISDIR(info.st_mode)) return false; }
    return true;
}
static bool text_contains(const char *text)
{ for (unsigned i = 0; i < row_count; i++) if (strstr(rows[i].text, text)) return true; return false; }
static unsigned entries(void)
{ unsigned count = 0; for (unsigned i = 0; i < row_count; i++) if (rows[i].symbol && strcmp(rows[i].text, "..")) count++; return count; }
static bool paths_valid(void)
{
    for (unsigned i = 0; i < row_count; i++) {
        lv_obj_t *row = &rows[i];
        if (row->callback != file_clicked || !strcmp(row->text, "Internal storage") || !strcmp(row->text, "SD card")) continue;
        bool owned = false;
        for (size_t j = 0; j < file_path_count; j++) if (row->path == file_paths[j]) owned = true;
        if (!owned || strlen(row->path) >= FILES_PATH_BYTES) return false;
        if (strcmp(row->text, "..")) {
            const char *name = strrchr(row->path, '/'); if (!name || strcmp(name + 1, row->text)) return false;
        }
    }
    return file_path_count <= FILES_CAPACITY;
}
static void report(const char *name, bool pass)
{
    bool same = unchanged(); pass = pass && same && !handles && paths_valid(); cases++; failures += !pass;
    printf("%s %s entries=%u path_slots=%zu incomplete=%d limited=%d skipped=%d reported_error=%d sd_reports=%u opens=%u reads=%u closes=%u handles=%u file_unchanged=%d\n",
        pass ? "PASS" : "FAIL", name, entries(), file_path_count, text_contains("Directory listing incomplete:"),
        text_contains("Directory entry limit reached"), text_contains("paths that exceed the path limit"),
        reported_error, reports, opens, reads, closes, handles, same);
}
static bool no_warning(void)
{ return !text_contains("Directory listing incomplete:") && !text_contains("Directory entry limit reached") && !text_contains("paths that exceed the path limit") && !reports; }
static bool directory_error(bool sd, int error, bool opening)
{
    char message[128]; snprintf(message, sizeof(message), opening ? "Could not open directory: %s" : "Directory listing incomplete: %s", strerror(error));
    return text_contains(message) && reports == (unsigned)sd && reported_error == (sd ? error : 0);
}
static void ordinary_case(const char *name, unsigned count, bool child, fault_t injected)
{
    prepare(count, child, "/internal/FILES"); fault = injected; show_files(target);
    report(name, entries() == count + (unsigned)child && file_path_count == count + (unsigned)child + 1 && no_warning() && opens == 1 && closes == 1);
}
static void failure_case(const char *name, fault_t injected, int error, const char *logical, bool sd)
{
    prepare(2, true, logical); fault = injected; show_files(target);
    bool opening = injected == OPEN_FAIL || injected == OPEN_NO_ERRNO;
    unsigned expected_entries = opening || injected == READ_FIRST ? 0 : injected == READ_LATER || injected == READ_CLOSE ? 1 : 3;
    report(name, entries() == expected_entries && directory_error(sd, error, opening) && opens == 1 && closes == (unsigned)!opening);
}
static void mount_case(const char *name, bool internal, bool sd)
{
    prepare(0, false, "/internal/FILES"); internal_ready = internal; sd_ready = sd; show_files(NULL);
    report(name, text_contains("Back to apps") && text_contains("Internal storage") == internal && text_contains("SD card") == sd &&
        text_contains("No storage mounted") == (!internal && !sd) && !opens && !closes && !file_path_count);
}
int main(void)
{
    snprintf(physical, sizeof(physical), ".files_directory_%d", fixture_pid());
    struct stat existing; assert(stat(physical, &existing) != 0 && errno == ENOENT);
    assert(make_directory(physical) == 0); owns_directory = true;
    ordinary_case("empty", 0, false, NORMAL); ordinary_case("files-and-directory", 2, true, NORMAL);
    bool found_child = false;
    for (unsigned i = 0; i < row_count; i++) if (!strcmp(rows[i].text, "CHILD") && rows[i].symbol && !strcmp(rows[i].symbol, LV_SYMBOL_DIRECTORY)) found_child = true;
    assert(found_child);
    ordinary_case("dot-entries", 2, true, WITH_DOTS);
    prepare(66, false, "/internal/FILES"); show_files(target);
    report("entry-limit-with-parent", entries() == 63 && file_path_count == 64 && text_contains("Directory entry limit reached") && !reports && closes == 1);
    prepare(66, false, SD_PATH); show_files(target);
    report("entry-limit-at-root", entries() == 64 && file_path_count == 64 && text_contains("Directory entry limit reached") && !reports && closes == 1);
    char long_path[FILES_PATH_BYTES]; memset(long_path, 'x', 252); memcpy(long_path, "/internal/", 10); long_path[252] = '\0';
    prepare(2, true, long_path); show_files(target);
    report("path-limit", !entries() && file_path_count == 1 && text_contains("Skipped 3 paths that exceed the path limit") && !reports && closes == 1);
    mount_case("mounts-both", true, true); mount_case("mounts-internal", true, false);
    mount_case("mounts-sd", false, true); mount_case("mounts-none", false, false);
    const fault_t faults[] = {OPEN_FAIL, OPEN_NO_ERRNO, READ_FIRST, READ_LATER, READ_CLOSE, CLOSE_FAIL, CLOSE_NO_ERRNO};
    const int errors[] = {EACCES, EIO, EIO, EIO, EACCES, EIO, EIO};
    const char *names[] = {"open", "open-no-errno", "read-first", "read-later", "first-read-error", "close", "close-no-errno"};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        char name[96]; snprintf(name, sizeof(name), "internal-%s", names[i]);
        failure_case(name, faults[i], errors[i], "/internal/FILES", false);
        snprintf(name, sizeof(name), "sd-%s", names[i]);
        failure_case(name, faults[i], errors[i], SD_PATH "/FILES", true);
    }
    failure_case("sd-root-close", CLOSE_FAIL, EIO, SD_PATH, true);
    failure_case("sd-prefix-boundary", CLOSE_FAIL, EIO, "/sdcard-other/FILES", false);
    ordinary_case("stale-open-errno", 2, true, STALE_OPEN);
    ordinary_case("stale-stat-errno", 2, true, STALE_STAT);
    ordinary_case("stale-close-errno", 2, true, STALE_CLOSE);
    prepare(2, true, "/internal/FILES"); fault = READ_LATER; show_files(target);
    bool pass = directory_error(false, EIO, false) && entries() == 1;
    fault = NORMAL; show_files(target); report("reopen-after-error", pass && no_warning() && entries() == 3 && opens == 2 && closes == 2);
    prepare(2, true, "/internal/FILES"); pass = true;
    for (unsigned i = 0; i < 25; i++) {
        fault = NORMAL; show_files(target); pass = pass && no_warning() && entries() == 3;
        fault = CLOSE_FAIL; show_files(target); pass = pass && directory_error(false, EIO, false) && entries() == 3;
        fault = NORMAL; show_files(target); pass = pass && no_warning() && entries() == 3;
    }
    report("cycles-25", pass && opens == 75 && closes == 75 && clears == 75);
    clean_files(); assert(remove_directory(physical) == 0); owns_directory = false;
    printf("%s %u Files directory cases failures=%u handles=%u (native directories/files; controlled paths/UI/errors)\n", failures ? "FAIL" : "PASS", cases, failures, handles);
    return failures ? 1 : 0;
}
