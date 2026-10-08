/* Actual ebook callbacks with native files and controlled UI/heap/I/O. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
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
#include "ebook_config.inc"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define LV_STATE_DISABLED 1
#define LV_ANIM_OFF 0
#define ESP_LOGI(...) ((void)0)
typedef struct { char text[EBOOK_PAGE_BYTES + 80]; bool disabled; } lv_obj_t;
typedef struct { int unused; } lv_event_t;
static lv_obj_t text, status, prev, next;
static lv_obj_t *ebook_text = &text, *ebook_status = &status, *ebook_prev = &prev, *ebook_next = &next;
static char *ebook_buffer, ebook_path[256];
static long ebook_offset, ebook_next_offset;
static bool sd_ready;
typedef enum { NONE, NO_SD, ALLOC, OPEN, SEEK, SEEK_CLOSE, READ, READ_ZERO, EMPTY_READ, FULL_READ, READ_CLOSE, CLOSE, STALE, OFFSET_OVERFLOW } fault_t;
static fault_t fault;
static unsigned handles, opens, seeks, reads, closes, allocations, renders, scrolls;
static FILE *active;
static bool flagged;
static int recorded_error;
void sd_record_error(int error) { recorded_error = error; if (error == EIO || error == ENODEV) sd_ready = false; }
int sd_error_snapshot(void) { return recorded_error; }
static void *heap_caps_malloc(size_t size, int caps)
{
    assert(size == EBOOK_PAGE_BYTES + 1 && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fault == ALLOC) return NULL;
    void *buffer = malloc(size); assert(buffer); allocations++; return buffer;
}
static void lv_label_set_text(lv_obj_t *object, const char *value)
{ assert(object && strlen(value) < sizeof(object->text)); strcpy(object->text, value); }
static void lv_label_set_text_fmt(lv_obj_t *object, const char *format, ...)
{
    assert(object); va_list args; va_start(args, format); int count = vsnprintf(object->text, sizeof(object->text), format, args); va_end(args);
    assert(count >= 0 && (size_t)count < sizeof(object->text));
}
static void lv_textarea_set_text(lv_obj_t *object, const char *value) { assert(object == &text); renders++; lv_label_set_text(object, value); }
static void lv_obj_scroll_to_y(lv_obj_t *object, int position, int animation) { assert(object == &text && !position && animation == LV_ANIM_OFF); scrolls++; }
static void lv_obj_add_state(lv_obj_t *object, int state) { assert(object && state == LV_STATE_DISABLED); object->disabled = true; }
static void lv_obj_remove_state(lv_obj_t *object, int state) { assert(object && state == LV_STATE_DISABLED); object->disabled = false; }
#if EBOOK_TRANSACTIONAL
static bool lv_obj_has_state(lv_obj_t *object, int state) { assert(object && state == LV_STATE_DISABLED); return object->disabled; }
#endif
FILE *fixture_open(const char *path, const char *mode)
{
    assert(!handles && !strcmp(path, ebook_path) && !strcmp(mode, "rb")); opens++;
    if (fault == OPEN) { errno = EACCES; return NULL; }
    FILE *file = fopen(path, mode); active = file; flagged = false;
    if (file) handles++;
    return file;
}
int fixture_seek(FILE *file, long offset, int origin)
{
    assert(file == active && origin == SEEK_SET); seeks++;
    if (fault == SEEK || fault == SEEK_CLOSE) { errno = EACCES; return -1; }
    int result = fseek(file, fault == OFFSET_OVERFLOW ? 0 : offset, origin);
    if (fault == STALE) errno = ENOSPC;
    return result;
}
size_t fixture_read(void *buffer, size_t size, size_t count, FILE *file)
{
    assert(file == active && size == 1 && count == EBOOK_PAGE_BYTES); reads++;
    size_t length = fread(buffer, size, count, file);
    if (fault == READ || fault == READ_ZERO || fault == EMPTY_READ || fault == FULL_READ || fault == READ_CLOSE || fault == STALE) {
        if (fault != FULL_READ) length /= 2;
        if (fault == EMPTY_READ) length = 0;
        flagged = true;
        if (fault != STALE) errno = fault == READ_ZERO ? 0 : EIO;
    }
    return length;
}
int fixture_error(FILE *file) { assert(file == active); return flagged || ferror(file); }
int fixture_close(FILE *file)
{
    assert(file == active && handles == 1); closes++; handles--; active = NULL;
    int result = fclose(file);
    if (fault == CLOSE || fault == READ_CLOSE || fault == SEEK_CLOSE) { errno = EPERM; return EOF; }
    return result;
}
#define fopen fixture_open
#define fseek fixture_seek
#define fread fixture_read
#define ferror fixture_error
#define fclose fixture_close
#include "ebook_pages.inc"
#undef fopen
#undef fseek
#undef fread
#undef ferror
#undef fclose

static unsigned cases, failures;
static char *book;
static size_t book_length;
static void prepare(const void *bytes, size_t length, bool existing)
{
    assert(!handles);
    if (ebook_buffer) { free(ebook_buffer); ebook_buffer = NULL; assert(allocations == 1); allocations--; }
    if (remove(ebook_path) != 0) assert(errno == ENOENT);
    if (existing) { FILE *file = fopen(ebook_path, "wb"); assert(file); assert(fwrite(bytes, 1, length, file) == length); assert(fclose(file) == 0); }
    ebook_text = &text; ebook_status = &status; ebook_prev = &prev; ebook_next = &next;
    strcpy(text.text, "unchanged"); strcpy(status.text, "unchanged"); prev.disabled = next.disabled = true;
    ebook_offset = ebook_next_offset = 0; fault = NONE; flagged = false; sd_ready = true; recorded_error = 0;
    opens = seeks = reads = closes = renders = scrolls = 0;
}
static bool file_matches(const void *bytes, size_t length, bool existing)
{
    FILE *file = fopen(ebook_path, "rb");
    if (!existing) { if (file) fclose(file); return !file && errno == ENOENT; }
    if (!file) return false;
    unsigned char buffer[1024]; size_t offset = 0; bool pass = true;
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (offset + count > length || (count && memcmp(buffer, (const char *)bytes + offset, count))) pass = false;
        offset += count;
        if (count < sizeof(buffer)) { assert(!ferror(file)); break; }
    }
    assert(fclose(file) == 0); return pass && offset == length;
}
static void load(long offset)
{
#if EBOOK_TRANSACTIONAL
    ebook_load_page(offset);
#else
    ebook_offset = offset; ebook_load_page();
#endif
}
static void report(const char *name, bool pass, int error, int expected)
{
    cases++; failures += !pass;
    const char *view = !strncmp(status.text, "Page ", 5) ? "page" : !strcmp(status.text, "unchanged") ? "unchanged" : "error";
    printf("%s %s error=%d expected_error=%d sd_ready=%d offset=%ld next_offset=%ld prev_disabled=%d next_disabled=%d view=%s opens=%u seeks=%u reads=%u closes=%u renders=%u scrolls=%u handles=%u allocations=%u\n",
        pass ? "PASS" : "FAIL", name, error, expected, sd_ready, ebook_offset, ebook_next_offset, prev.disabled, next.disabled, view,
        opens, seeks, reads, closes, renders, scrolls, handles, allocations);
}
static void page_case(const char *name, const void *bytes, size_t length, long offset, const char *expected, bool next_enabled)
{
    prepare(bytes, length, true); load(offset);
    long read_length = length > (size_t)offset ? (long)(length - (size_t)offset) : 0;
    if (read_length > EBOOK_PAGE_BYTES) read_length = EBOOK_PAGE_BYTES;
    bool pass = !handles && !strcmp(text.text, expected) && ebook_offset == offset && ebook_next_offset == offset + read_length &&
        prev.disabled == (offset == 0) && next.disabled == !next_enabled && renders == 1 && scrolls == 1 && file_matches(bytes, length, true);
    report(name, pass, recorded_error, 0);
}
static void failure_case(const char *name, fault_t injected, int expected, bool existing, bool cached)
{
    prepare(book, book_length, existing);
    if (cached) { load(0); assert(renders == 1); }
    long original_offset = ebook_offset, original_next = ebook_next_offset;
    char previous[EBOOK_PAGE_BYTES + 80]; strcpy(previous, text.text);
    bool previous_prev = prev.disabled, previous_next = next.disabled;
    unsigned original_renders = renders, original_scrolls = scrolls, original_opens = opens;
    fault = injected; if (fault == NO_SD) sd_ready = false;
    load(cached ? EBOOK_PAGE_BYTES : 0);
    int actual = recorded_error;
    bool pass = !handles && ebook_offset == original_offset && ebook_next_offset == original_next && !strcmp(previous, text.text) &&
        prev.disabled == previous_prev && next.disabled == previous_next && renders == original_renders && scrolls == original_scrolls;
    if (fault == ALLOC) pass = pass && !strcmp(status.text, "Out of memory") && opens == original_opens;
    else pass = pass && actual == expected && !strncmp(status.text, "Could not read book:", strlen("Could not read book:"));
    if (fault == NO_SD) pass = pass && opens == original_opens;
    pass = pass && file_matches(book, book_length, existing);
    report(name, pass, actual, expected);
}
static void retry_case(const char *name, fault_t injected, bool backwards)
{
    prepare(book, book_length, true); load(backwards ? EBOOK_PAGE_BYTES : 0);
    long old_offset = ebook_offset, old_next = ebook_next_offset;
    char old_text[EBOOK_PAGE_BYTES + 80]; strcpy(old_text, text.text);
    fault = injected;
    if (backwards) ebook_prev_clicked(NULL); else ebook_next_clicked(NULL);
    bool pass = !handles && ebook_offset == old_offset && ebook_next_offset == old_next && !strcmp(old_text, text.text) && renders == 1 && scrolls == 1 && recorded_error == (injected == OPEN ? EACCES : EIO);
    int first_error = recorded_error;
    fault = NONE; sd_ready = true;
    if (backwards) ebook_prev_clicked(NULL); else ebook_next_clicked(NULL);
    long expected_offset = backwards ? 0 : EBOOK_PAGE_BYTES;
    const char *expected = backwards ? "A" : "B";
    pass = pass && ebook_offset == expected_offset && text.text[0] == expected[0] && strlen(text.text) == EBOOK_PAGE_BYTES && renders == 2 && scrolls == 2 && file_matches(book, book_length, true);
    report(name, pass, first_error, injected == OPEN ? EACCES : EIO);
}
int main(void)
{
    assert(snprintf(ebook_path, sizeof(ebook_path), ".ebook_pages_%ld.TXT", (long)fixture_pid()) > 0);
    struct stat info; assert(stat(ebook_path, &info) != 0 && errno == ENOENT);
    book_length = 2 * EBOOK_PAGE_BYTES + 7; book = malloc(book_length); assert(book);
    memset(book, 'A', EBOOK_PAGE_BYTES); memset(book + EBOOK_PAGE_BYTES, 'B', EBOOK_PAGE_BYTES); memcpy(book + 2 * EBOOK_PAGE_BYTES, "last123", 7);
    char full[EBOOK_PAGE_BYTES + 1]; memset(full, 'A', EBOOK_PAGE_BYTES); full[EBOOK_PAGE_BYTES] = '\0';
    page_case("page-empty", "", 0, 0, "End of book", false);
    page_case("page-small", "short book\n", 11, 0, "short book\n", false);
    page_case("page-exact", full, EBOOK_PAGE_BYTES, 0, full, true);
    page_case("page-first", book, book_length, 0, full, true);
    memset(full, 'B', EBOOK_PAGE_BYTES);
    page_case("page-second", book, book_length, EBOOK_PAGE_BYTES, full, true);
    page_case("page-last", book, book_length, 2 * EBOOK_PAGE_BYTES, "last123", false);
    page_case("page-eof", book, book_length, (long)book_length, "End of book", false);
    const unsigned char encoding[] = {'a', 0, 'b', 0xc3, 0xa9, 'c', 0xf0, 0x9f, 0x99, 0x82, 'd'};
    page_case("page-existing-text-conversion", encoding, sizeof(encoding), 0, "a b?c?d", false);
    failure_case("load-missing", NONE, ENOENT, false, false);
    failure_case("load-allocation", ALLOC, 0, true, false);
    failure_case("load-no-sd", NO_SD, ENODEV, true, false);
    prepare(book, book_length, true); sd_ready = false; recorded_error = EIO; load(0);
    report("load-stored-sd-error", !opens && !allocations && !renders && !handles && ebook_offset == 0 && ebook_next_offset == 0 && recorded_error == EIO && !strncmp(status.text, "Could not read book:", 20) && file_matches(book, book_length, true), recorded_error, EIO);
    const struct { const char *name; fault_t fault; int error; } faults[] = {
        {"open", OPEN, EACCES}, {"seek", SEEK, EACCES}, {"first-seek-error", SEEK_CLOSE, EACCES},
        {"read", READ, EIO}, {"read-zero-errno", READ_ZERO, EIO}, {"empty-read-error", EMPTY_READ, EIO}, {"full-count-read-error", FULL_READ, EIO},
        {"first-read-error", READ_CLOSE, EIO}, {"close", CLOSE, EPERM}, {"stale-read-errno", STALE, EIO}};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        char name[64]; snprintf(name, sizeof(name), "load-%s", faults[i].name);
        failure_case(name, faults[i].fault, faults[i].error, true, false);
        snprintf(name, sizeof(name), "retain-page-%s", faults[i].name);
        failure_case(name, faults[i].fault, faults[i].error, true, true);
    }
    failure_case("retain-page-no-sd", NO_SD, ENODEV, true, true);
    retry_case("next-retry-open", OPEN, false);
    retry_case("next-retry-read", READ, false);
    retry_case("prev-retry-open", OPEN, true);
    retry_case("prev-retry-read", READ, true);
    prepare(book, book_length, true); load(0); unsigned old_opens = opens; prev.disabled = true; ebook_prev_clicked(NULL);
    report("disabled-prev", opens == old_opens && ebook_offset == 0 && renders == 1 && file_matches(book, book_length, true), recorded_error, 0);
    prepare(book, book_length, true); load(0); old_opens = opens; next.disabled = true; ebook_next_clicked(NULL);
    report("disabled-next", opens == old_opens && ebook_offset == 0 && renders == 1 && file_matches(book, book_length, true), recorded_error, 0);
    prepare(book, book_length, true); load(0); old_opens = opens; ebook_next_offset = ebook_offset; ebook_next_clicked(NULL);
    report("invalid-next-offset", opens == old_opens && ebook_offset == 0 && renders == 1 && file_matches(book, book_length, true), recorded_error, 0);
#if EBOOK_TRANSACTIONAL
    prepare(book, book_length, true); ebook_text = ebook_status = ebook_prev = ebook_next = NULL;
    load(0); ebook_prev_clicked(NULL); ebook_next_clicked(NULL);
    report("offscreen-actions", !opens && !allocations && !renders && !handles && file_matches(book, book_length, true), recorded_error, 0);
    prepare(book, book_length, true); load(-1);
    report("negative-offset", !opens && !renders && ebook_offset == 0 && ebook_next_offset == 0 && recorded_error == EINVAL && file_matches(book, book_length, true), recorded_error, EINVAL);
    prepare("overflow", 8, true); fault = OFFSET_OVERFLOW; load(LONG_MAX - 3);
    report("offset-overflow", !handles && !renders && ebook_offset == 0 && ebook_next_offset == 0 && recorded_error == EOVERFLOW && file_matches("overflow", 8, true), recorded_error, EOVERFLOW);
#else
    /* Original callbacks cannot safely run these newly guarded boundaries. */
    prepare(book, book_length, true);
    report("offscreen-actions", false, 0, 0);
    report("negative-offset", false, 0, EINVAL);
    report("offset-overflow", false, 0, EOVERFLOW);
#endif
    prepare(book, book_length, true); bool stable = true;
    for (unsigned i = 0; i < 25; i++) {
        load(0); ebook_next_clicked(NULL); ebook_prev_clicked(NULL);
        stable = stable && !handles && allocations == 1 && ebook_offset == 0 && ebook_next_offset == EBOOK_PAGE_BYTES && text.text[0] == 'A';
    }
    report("navigation-25", stable && opens == 75 && closes == 75 && renders == 75 && file_matches(book, book_length, true), recorded_error, 0);
    if (ebook_buffer) { free(ebook_buffer); ebook_buffer = NULL; assert(allocations == 1); allocations--; }
    assert(remove(ebook_path) == 0); free(book);
    printf("%s %u ebook page cases failures=%u handles=%u allocations=%u (native files; controlled UI/heap/I/O)\n", failures ? "FAIL" : "PASS", cases, failures, handles, allocations);
    return failures ? 1 : 0;
}
