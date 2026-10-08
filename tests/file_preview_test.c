/* Actual open_file callback; native files and controlled UI/probe/SD/I/O APIs. */
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
#include <process.h>
#define fixture_pid _getpid
#else
#include <unistd.h>
#define fixture_pid getpid
#endif
#include "file_preview_config.inc"
#define LV_LABEL_LONG_DOT 1
#define LV_ANIM_OFF 0
typedef struct { char text[FILE_PREVIEW_BYTES + 128]; } lv_obj_t;
typedef struct { int unused; } lv_event_t;
typedef void (*lv_event_cb_t)(lv_event_t *);
static lv_obj_t root, back, title, preview;
static lv_obj_t *content = &root;
typedef enum { NONE, OPEN_ERROR, OPEN_NO_ERRNO, READ_ERROR, ZERO_READ_ERROR, FULL_READ_ERROR,
               READ_NO_ERRNO, READ_CLOSE_ERROR, CLOSE_ERROR, CLOSE_NO_ERRNO, STALE_OPEN, STALE_CLOSE } fault_t;
static fault_t fault;
static FILE *active;
static char real_path[96], logical_path[160], expected[FILE_PREVIEW_BYTES + 128];
static unsigned handles, opens, reads, closes, clears, labels, renders, capture_probes, serial_probes, reports;
static bool flagged, exists, capture_claim, serial_claim;
static int reported_error;
static const unsigned char *source_bytes;
static size_t source_length;
static unsigned cases, failures;
static void clear_content(void)
{
    clears++; labels = 0; strcpy(preview.text, "unrendered"); strcpy(title.text, "untitled");
}
static void file_back_clicked(lv_event_t *event) { (void)event; }
static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback)
{ assert(parent == content && !strcmp(text, "Back to files") && callback == file_back_clicked); strcpy(back.text, text); return &back; }
static bool capture_viewer_show(lv_obj_t *parent, const char *path)
{ assert(parent == content && !strcmp(path, logical_path)); capture_probes++; return capture_claim; }
static bool serial_log_viewer_show(lv_obj_t *parent, const char *path)
{ assert(parent == content && !strcmp(path, logical_path) && !capture_claim); serial_probes++; return serial_claim; }
static lv_obj_t *lv_label_create(lv_obj_t *parent) { assert(parent == content); labels++; return &title; }
static void lv_label_set_text(lv_obj_t *object, const char *text)
{ assert(object == &title && strlen(text) < sizeof(object->text)); strcpy(object->text, text); }
static void lv_obj_set_width(lv_obj_t *object, int width) { assert(object == &title && width == 620); }
static void lv_label_set_long_mode(lv_obj_t *object, int mode) { assert(object == &title && mode == LV_LABEL_LONG_DOT); }
static void lv_obj_set_size(lv_obj_t *object, int width, int height)
{ assert(width == 640 && ((object == &back && height == 68) || (object == &preview && height == 900))); }
static lv_obj_t *lv_textarea_create(lv_obj_t *parent) { assert(parent == content); return &preview; }
static void lv_textarea_set_text(lv_obj_t *object, const char *text)
{ assert(object == &preview && strlen(text) < sizeof(object->text)); renders++; strcpy(object->text, text); }
static void lv_textarea_set_one_line(lv_obj_t *object, bool one_line) { assert(object == &preview && !one_line); }
static void lv_textarea_set_cursor_pos(lv_obj_t *object, int position) { assert(object == &preview && position == 0); }
static void lv_obj_scroll_to_y(lv_obj_t *object, int position, int animation) { assert(object == &preview && !position && animation == LV_ANIM_OFF); }
void sd_record_error(int error) { reports++; reported_error = error; }
FILE *fixture_open(const char *path, const char *mode)
{
    assert(!active && !handles && !strcmp(path, logical_path) && !strcmp(mode, "rb")); opens++;
    if (fault == OPEN_ERROR || fault == OPEN_NO_ERRNO) { errno = fault == OPEN_ERROR ? EACCES : 0; return NULL; }
    active = fopen(real_path, mode); flagged = false;
    if (active) handles++;
    if (fault == STALE_OPEN && active) errno = ENOSPC;
    return active;
}
size_t fixture_read(void *buffer, size_t size, size_t count, FILE *file)
{
    assert(file == active && size == 1 && count == FILE_PREVIEW_BYTES - 1); reads++;
    size_t length = fread(buffer, size, count, file);
    if (fault == READ_ERROR || fault == ZERO_READ_ERROR || fault == FULL_READ_ERROR ||
        fault == READ_NO_ERRNO || fault == READ_CLOSE_ERROR || fault == STALE_OPEN) {
        if (fault != FULL_READ_ERROR) length /= 2;
        if (fault == ZERO_READ_ERROR) length = 0;
        flagged = true;
        if (fault != STALE_OPEN) errno = fault == READ_NO_ERRNO ? 0 : fault == READ_CLOSE_ERROR ? EACCES : EIO;
    }
    return length;
}
int fixture_error(FILE *file) { assert(file == active); return flagged || ferror(file); }
int fixture_close(FILE *file)
{
    assert(file == active && handles == 1); active = NULL; handles--; closes++;
    int result = fclose(file);
    if (fault == CLOSE_ERROR || fault == CLOSE_NO_ERRNO || fault == READ_CLOSE_ERROR) {
        errno = fault == CLOSE_NO_ERRNO ? 0 : fault == READ_CLOSE_ERROR ? EPERM : EIO; return EOF;
    }
    if (fault == STALE_CLOSE) errno = ENOSPC;
    return result;
}
#define fopen fixture_open
#define fread fixture_read
#define ferror fixture_error
#define fclose fixture_close
#include "file_preview.inc"
#undef fopen
#undef fread
#undef ferror
#undef fclose

static bool unchanged(void)
{
    FILE *file = fopen(real_path, "rb");
    if (!exists) { if (file) fclose(file); return !file && errno == ENOENT; }
    if (!file) return false;
    unsigned char bytes[512]; size_t offset = 0; bool pass = true;
    for (;;) {
        size_t count = fread(bytes, 1, sizeof(bytes), file);
        if (offset + count > source_length || (count && memcmp(bytes, source_bytes + offset, count))) pass = false;
        offset += count;
        if (count < sizeof(bytes)) { assert(!ferror(file)); break; }
    }
    assert(fclose(file) == 0); return pass && offset == source_length;
}
static void prepare(const void *bytes, size_t length, bool present, const char *prefix)
{
    assert(!handles && !active);
    if (remove(real_path) != 0) assert(errno == ENOENT);
    if (present) { FILE *file = fopen(real_path, "wb"); assert(file); assert(fwrite(bytes, 1, length, file) == length); assert(fclose(file) == 0); }
    source_bytes = bytes; source_length = length; exists = present;
    snprintf(logical_path, sizeof(logical_path), "%s/PREVIEW.TXT", prefix);
    fault = NONE; flagged = capture_claim = serial_claim = false;
    handles = opens = reads = closes = clears = labels = renders = capture_probes = serial_probes = reports = 0; reported_error = 0;
    strcpy(preview.text, "unrendered"); strcpy(title.text, "untitled");
}
static void expected_text(void)
{
    size_t length = source_length < FILE_PREVIEW_BYTES - 1 ? source_length : FILE_PREVIEW_BYTES - 1;
    if (length) memcpy(expected, source_bytes, length);
    expected[length] = '\0';
}
static void expected_error(int error) { snprintf(expected, sizeof(expected), "Could not read this file: %s", strerror(error)); }
static bool correct_view(bool sd, int error)
{
    return !strcmp(preview.text, expected) && !strcmp(title.text, logical_path) && labels == 1 &&
        reports == (unsigned)(sd && error != 0) && reported_error == (sd ? error : 0);
}
static void report(const char *name, bool pass)
{
    bool bytes_match = unchanged(); pass = pass && bytes_match && !handles; cases++; failures += !pass;
    const char *view = !strncmp(preview.text, "Could not read this file: ", 25) || !strcmp(preview.text, "Could not open this file.") ?
        "error" : !strcmp(preview.text, "unrendered") ? "routed" : "text";
    printf("%s %s view=%s reported_error=%d sd_reports=%u opens=%u reads=%u closes=%u clears=%u renders=%u capture_probes=%u serial_probes=%u handles=%u file_unchanged=%d\n",
        pass ? "PASS" : "FAIL", name, view, reported_error, reports, opens, reads, closes, clears, renders,
        capture_probes, serial_probes, handles, bytes_match);
}
static void success_case(const char *name, const void *bytes, size_t length)
{
    prepare(bytes, length, true, "/internal"); expected_text(); open_file(logical_path);
    report(name, correct_view(false, 0) && opens == 1 && reads == 1 && closes == 1 && clears == 1 && renders == 1);
}
static void fault_case(const char *name, fault_t injected, int error, const char *prefix, bool sd, const void *bytes, size_t length)
{
    prepare(bytes, length, true, prefix); fault = injected; expected_error(error); open_file(logical_path);
    bool open_failed = injected == OPEN_ERROR || injected == OPEN_NO_ERRNO;
    report(name, correct_view(sd, error) && opens == 1 && reads == (unsigned)!open_failed && closes == (unsigned)!open_failed && renders == 1);
}
static void retry_case(const char *name, const char *prefix, bool sd)
{
    static const char text[] = "complete text\n";
    prepare(text, sizeof(text) - 1, true, prefix); fault = READ_ERROR; expected_error(EIO); open_file(logical_path);
    bool pass = correct_view(sd, EIO); fault = NONE; expected_text(); open_file(logical_path);
    pass = pass && !strcmp(preview.text, expected) && clears == 2 && renders == 2 && opens == 2 && closes == 2;
    report(name, pass);
}
int main(void)
{
    snprintf(real_path, sizeof(real_path), ".file_preview_%d.TXT", fixture_pid());
    struct stat existing; assert(stat(real_path, &existing) != 0 && errno == ENOENT);
    char *large = malloc(8193); assert(large); memset(large, 'A', 8192); large[8192] = '\0';
    success_case("text-empty", "", 0); success_case("text-small", "hello\n", 6);
    success_case("text-exact", large, 4095); success_case("text-limited", large, 8192);
    prepare("", 0, false, "/internal"); expected_error(ENOENT); open_file(logical_path);
    report("missing-internal", correct_view(false, ENOENT) && opens == 1 && !reads && !closes);
    prepare("", 0, false, SD_PATH); expected_error(ENOENT); open_file(logical_path);
    report("missing-sd", correct_view(true, ENOENT) && opens == 1 && !reads && !closes);
    const fault_t faults[] = {OPEN_ERROR, OPEN_NO_ERRNO, READ_ERROR, ZERO_READ_ERROR, FULL_READ_ERROR, READ_NO_ERRNO,
                             READ_CLOSE_ERROR, CLOSE_ERROR, CLOSE_NO_ERRNO, STALE_OPEN};
    const int errors[] = {EACCES, EIO, EIO, EIO, EIO, EIO, EACCES, EIO, EIO, EIO};
    const char *names[] = {"open", "open-no-errno", "read", "zero-read", "full-read", "read-no-errno", "first-read-error", "close", "close-no-errno", "stale-open-errno"};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        char name[96]; snprintf(name, sizeof(name), "internal-%s", names[i]);
        fault_case(name, faults[i], errors[i], "/internal", false, large, 4095);
        snprintf(name, sizeof(name), "sd-%s", names[i]);
        fault_case(name, faults[i], errors[i], SD_PATH, true, large, 4095);
    }
    fault_case("sd-prefix-boundary", CLOSE_ERROR, EIO, "/sdcard-other", false, large, 4095);
    prepare("hello", 5, true, "/internal"); fault = STALE_CLOSE; expected_text(); open_file(logical_path);
    report("success-stale-close-errno", correct_view(false, 0));
    fault_case("empty-read-error", ZERO_READ_ERROR, EIO, "/internal", false, "", 0);
    fault_case("empty-close-error", CLOSE_ERROR, EIO, "/internal", false, "", 0);
    retry_case("internal-retry", "/internal", false); retry_case("sd-retry", SD_PATH, true);
    prepare(large, 4095, true, "/internal"); expected_text(); open_file(logical_path);
    bool pass = correct_view(false, 0); fault = READ_ERROR; expected_error(EIO); open_file(logical_path);
    report("error-after-full-preview", pass && correct_view(false, EIO) && renders == 2 && opens == 2 && closes == 2);
    static const char capture_csv[] = "unix_time,elapsed_us,gpio,millivolts,sample_rate_hz,offset_mv,scale_permille\n100,0,16,1200,5000,0,1000\n";
    prepare(capture_csv, sizeof(capture_csv) - 1, true, "/internal"); strcpy(logical_path, "/internal/PREVIEW.CSV"); capture_claim = true; open_file(logical_path);
    report("capture-routing", capture_probes == 1 && !serial_probes && !opens && !renders && !labels && clears == 1);
    static const char serial_csv[] = "unix_time,direction,data_hex\n100,RX,01 02\n";
    prepare(serial_csv, sizeof(serial_csv) - 1, true, "/internal"); strcpy(logical_path, "/internal/PREVIEW.CSV"); serial_claim = true; open_file(logical_path);
    report("serial-routing", capture_probes == 1 && serial_probes == 1 && !opens && !renders && !labels && clears == 1);
    prepare("hello", 5, true, "/internal"); pass = true;
    for (unsigned i = 0; i < 25; i++) {
        fault = NONE; expected_text(); open_file(logical_path); pass = pass && correct_view(false, 0);
        fault = CLOSE_ERROR; expected_error(EIO); open_file(logical_path); pass = pass && correct_view(false, EIO);
        fault = NONE; expected_text(); open_file(logical_path); pass = pass && correct_view(false, 0);
    }
    report("cycles-25", pass && opens == 75 && reads == 75 && closes == 75 && clears == 75 && renders == 75);
    assert(remove(real_path) == 0); free(large);
    printf("%s %u file preview cases failures=%u handles=%u (native files; controlled UI/probes/SD/I/O)\n", failures ? "FAIL" : "PASS", cases, failures, handles);
    return failures ? 1 : 0;
}
