/* Actual Notes callbacks/storage code; native bytes, controlled UI/heap/faults. */
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
#ifdef _WIN32
#include <io.h>
#include <process.h>
#define fixture_pid _getpid
#else
#include <unistd.h>
#define fixture_pid getpid
#endif
#include "notes_config.inc"

#define SD_PATH "/sdcard"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#ifdef NOTES_REAL_LVGL
#include "lvgl.h"
static lv_obj_t *content, *note_area;
#else
#define LV_FLEX_FLOW_COLUMN 1
#define LV_FLEX_FLOW_ROW 2
#define LV_EVENT_CLICKED 1
#define LV_STATE_DISABLED 1

typedef struct { char *text; unsigned state; unsigned max_length; } lv_obj_t;
typedef struct { void *user_data; } lv_event_t;
static lv_obj_t objects[8], root_object;
static unsigned object_count;
static lv_obj_t *content = &root_object, *note_area;
#endif
static bool note_can_save, sd_ready;
static unsigned allocations, handles, fs_calls, mutations, read_calls, close_calls;
static int recorded_error;
static unsigned error_reports, write_opens, directory_calls, remove_calls, write_calls, flush_calls, sync_calls;
static bool no_errno, stale_success, full_read, positive_probe, full_write_flag, close_cleanup_error, remove_cleanup_error;
static bool existing_directory;
static int stale_errno = EBUSY;
static char paths[3][128];
static FILE *read_file, *write_file;
static bool read_error, write_error;
typedef enum { NO_FAULT, NO_SD, ALLOC_ERROR, FINAL_STAT_ERROR, BACKUP_STAT_ERROR,
    RECOVERY_ERROR, OPEN_READ_ERROR, PARTIAL_READ_ERROR, ZERO_ERRNO_READ_ERROR,
    PROBE_READ_ERROR, READ_CLOSE_ERROR, READ_AND_CLOSE_ERROR, OPEN_WRITE_ERROR,
    WRITE_ERROR, FLUSH_ERROR, SYNC_ERROR, WRITE_CLOSE_ERROR, PUBLISH_ERROR,
    ROLLBACK_ERROR, DIRECTORY_ERROR, REMOVE_TEMP_ERROR } fault_t;
static fault_t fault;

static void leave_stale_errno(void) { if (stale_success) errno = stale_errno; }
static void supply_errno(int error) { if (!no_errno) errno = error; }

static const char *native_path(const char *path)
{
    const char *source[] = {SD_PATH "/DOCS/NOTE.TXT", SD_PATH "/DOCS/NOTE.TMP", SD_PATH "/DOCS/NOTE.BAK"};
    for (unsigned i = 0; i < 3; i++) if (!strcmp(path, source[i])) return paths[i];
    assert(false); return NULL;
}
FILE *fixture_open(const char *path, const char *mode)
{
    fs_calls++;
    bool reading = !strcmp(mode, "rb");
    if (!reading) { mutations++; write_opens++; }
    if ((reading && fault == OPEN_READ_ERROR) || (!reading && fault == OPEN_WRITE_ERROR)) {
        supply_errno(EACCES); return NULL;
    }
    FILE *file = fopen(native_path(path), mode);
    if (file) { handles++; if (reading) read_file = file; else write_file = file; leave_stale_errno(); }
    return file;
}
size_t fixture_read(void *buffer, size_t size, size_t count, FILE *file)
{
    assert(file == read_file && size == 1); read_calls++; fs_calls++;
    if (fault == PARTIAL_READ_ERROR || fault == ZERO_ERRNO_READ_ERROR || fault == READ_AND_CLOSE_ERROR) {
        size_t read = fread(buffer, size, full_read ? count : count / 2, file);
        read_error = true;
        if (fault == ZERO_ERRNO_READ_ERROR) errno = 0;
        else supply_errno(EACCES);
        return read;
    }
    size_t read = fread(buffer, size, count, file);
    if (!ferror(file)) leave_stale_errno();
    return read;
}
int fixture_getc(FILE *file)
{
    fs_calls++;
    if (fault == PROBE_READ_ERROR && file == read_file) {
        int result = positive_probe ? fgetc(file) : EOF;
        read_error = true; supply_errno(EIO); return result;
    }
    int result = fgetc(file);
    if (!ferror(file)) leave_stale_errno();
    return result;
}
int fixture_error(FILE *file) { return (file == read_file && read_error) || (file == write_file && write_error) || ferror(file); }
int fixture_close(FILE *file)
{
    assert(handles); handles--; close_calls++; fs_calls++;
    bool reading = file == read_file;
    int result = fclose(file);
    if (reading) read_file = NULL;
    else write_file = NULL;
    if ((reading && (fault == READ_CLOSE_ERROR || fault == READ_AND_CLOSE_ERROR)) ||
        (!reading && fault == WRITE_CLOSE_ERROR)) { supply_errno(EPERM); return EOF; }
    if (close_cleanup_error) { errno = EPERM; return EOF; }
    if (!result) leave_stale_errno();
    return result;
}
int fixture_stat(const char *path, struct stat *info)
{
    fs_calls++;
    const char *mapped = native_path(path);
    if ((mapped == paths[0] && fault == FINAL_STAT_ERROR) || (mapped == paths[2] && fault == BACKUP_STAT_ERROR)) {
        errno = EACCES; return -1;
    }
    int result = stat(mapped, info);
    if (!result) leave_stale_errno();
    return result;
}
int fixture_remove(const char *path)
{
    fs_calls++; mutations++; remove_calls++;
    if (!strcmp(path, SD_PATH "/DOCS/NOTE.TMP") && fault == REMOVE_TEMP_ERROR) { supply_errno(EACCES); return -1; }
    if (remove_cleanup_error && write_calls) { errno = EACCES; return -1; }
    int result = remove(native_path(path));
    if (!result) leave_stale_errno();
    return result;
}
int fixture_rename(const char *from, const char *to)
{
    fs_calls++; mutations++;
    const char *a = native_path(from), *b = native_path(to);
    if ((a == paths[2] && b == paths[0] && (fault == RECOVERY_ERROR || fault == ROLLBACK_ERROR)) ||
        (a == paths[1] && b == paths[0] && (fault == PUBLISH_ERROR || fault == ROLLBACK_ERROR))) {
        errno = EACCES; return -1;
    }
    return rename(a, b);
}
int fixture_mkdir(const char *path, int mode)
{
    assert(!strcmp(path, SD_PATH "/DOCS") && mode == 0775); fs_calls++; mutations++; directory_calls++;
    if (fault == DIRECTORY_ERROR) { supply_errno(EACCES); return -1; }
    if (existing_directory) { errno = EEXIST; return -1; }
    leave_stale_errno(); return 0;
}
int fixture_puts(const char *text, FILE *file)
{
    fs_calls++; mutations++; write_calls++;
    if (fault == WRITE_ERROR) {
        if (full_write_flag) {
            int result = fputs(text, file); assert(result >= 0); write_error = true; supply_errno(EIO); return result;
        }
        size_t length = strlen(text) / 2;
        assert(fwrite(text, 1, length, file) == length); supply_errno(EIO); return EOF;
    }
    int result = fputs(text, file);
    if (result >= 0) leave_stale_errno();
    return result;
}
int fixture_flush(FILE *file)
{
    fs_calls++; flush_calls++;
    if (fault == FLUSH_ERROR) { errno = EIO; return EOF; }
    return fflush(file);
}
#ifdef _WIN32
int fixture_sync(int descriptor)
{
    fs_calls++; sync_calls++;
    if (fault == SYNC_ERROR) { errno = EIO; return -1; }
    return _commit(descriptor);
}
#define _commit fixture_sync
#else
int fixture_sync(int descriptor)
{
    fs_calls++; sync_calls++;
    if (fault == SYNC_ERROR) { errno = EIO; return -1; }
    return fsync(descriptor);
}
#define fsync fixture_sync
#endif

#if NOTES_HAS_LEAVE
static void notes_leave(void);
#endif
static void clear_content(void)
{
#if NOTES_HAS_LEAVE
    notes_leave();
#endif
#ifdef NOTES_REAL_LVGL
    if (content) lv_obj_clean(content);
#else
    for (unsigned i = 0; i < object_count; i++) free(objects[i].text);
    memset(objects, 0, sizeof(objects)); object_count = 0;
#endif
}
#ifndef NOTES_REAL_LVGL
static lv_obj_t *create_object(void)
{
    assert(object_count < sizeof(objects) / sizeof(objects[0]));
    return &objects[object_count++];
}
lv_obj_t *lv_obj_create(lv_obj_t *parent) { assert(parent); return create_object(); }
lv_obj_t *lv_label_create(lv_obj_t *parent) { return lv_obj_create(parent); }
lv_obj_t *lv_button_create(lv_obj_t *parent) { return lv_obj_create(parent); }
lv_obj_t *lv_textarea_create(lv_obj_t *parent) { return lv_obj_create(parent); }
lv_obj_t *lv_keyboard_create(lv_obj_t *parent) { return lv_obj_create(parent); }
void lv_obj_set_size(lv_obj_t *object, int width, int height) { assert(object && width > 0 && height > 0); }
void lv_obj_set_flex_flow(lv_obj_t *object, int flow) { assert(object && flow); }
void lv_obj_set_flex_grow(lv_obj_t *object, int grow) { assert(object && grow == 1); }
void lv_obj_center(lv_obj_t *object) { assert(object); }
void lv_obj_add_state(lv_obj_t *object, unsigned state) { assert(object); object->state |= state; }
void *lv_event_get_user_data(lv_event_t *event) { assert(event); return event->user_data; }
void lv_obj_add_event_cb(lv_obj_t *object, void (*callback)(lv_event_t *), int event, void *data)
{ assert(object && callback && event == LV_EVENT_CLICKED && data); }
void lv_label_set_text(lv_obj_t *object, const char *text)
{
    assert(object && text); free(object->text); object->text = malloc(strlen(text) + 1);
    assert(object->text); strcpy(object->text, text);
}
void lv_label_set_text_fmt(lv_obj_t *object, const char *format, ...)
{
    char text[320]; va_list args; va_start(args, format);
    int result = vsnprintf(text, sizeof(text), format, args); va_end(args);
    assert(result >= 0 && (size_t)result < sizeof(text)); lv_label_set_text(object, text);
}
void lv_textarea_set_text(lv_obj_t *object, const char *text) { lv_label_set_text(object, text); }
const char *lv_textarea_get_text(lv_obj_t *object) { assert(object && object->text); return object->text; }
void lv_textarea_set_max_length(lv_obj_t *object, unsigned length) { assert(object); object->max_length = length; }
void lv_keyboard_set_textarea(lv_obj_t *keyboard, lv_obj_t *area) { assert(keyboard && area); }
#endif
void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(size == NOTE_MAX_BYTES + 1 && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fault == ALLOC_ERROR) return NULL;
    void *memory = malloc(size); assert(memory); allocations++; return memory;
}
void heap_caps_free(void *memory) { if (memory) { assert(allocations); allocations--; free(memory); } }
void sd_record_error(int error) { recorded_error = error; error_reports++; }
int sd_error_snapshot(void) { return recorded_error; }

#define fopen fixture_open
#define fread fixture_read
#define fgetc fixture_getc
#define ferror fixture_error
#define fclose fixture_close
#define stat(path, info) fixture_stat(path, info)
#define remove fixture_remove
#define rename fixture_rename
#define mkdir fixture_mkdir
#define fputs fixture_puts
#define fflush fixture_flush
#include "storage_source.inc"
#include "notes_callbacks.inc"
#undef fopen
#undef fread
#undef fgetc
#undef ferror
#undef fclose
#undef stat
#undef remove
#undef rename
#undef mkdir
#undef fputs
#undef fflush
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif

static unsigned cases, failures;
static const unsigned char old_note[] = "previous complete note\n";
static const unsigned char old_backup[] = "older backup\n";
static const unsigned char retained[] = "retained unpublished note\n";
static void write_bytes(unsigned index, const unsigned char *data, size_t length)
{
    if (!data) return;
    FILE *file = fopen(paths[index], "wb"); assert(file);
    assert(fwrite(data, 1, length, file) == length && fclose(file) == 0);
}
static bool bytes_match(unsigned index, const unsigned char *data, size_t length)
{
    FILE *file = fopen(paths[index], "rb");
    if (!data) { if (file) fclose(file); return !file && errno == ENOENT; }
    if (!file) return false;
    unsigned char buffer[512]; size_t offset = 0; bool match = true;
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (offset + count > length || (count && memcmp(buffer, data + offset, count))) match = false;
        offset += count;
        if (count < sizeof(buffer)) { assert(!ferror(file)); break; }
    }
    assert(fclose(file) == 0); return match && offset == length;
}
static void prepare(const unsigned char *data, size_t length, const unsigned char *backup, size_t backup_length)
{
    assert(!handles && !allocations); clear_content(); note_area = NULL; note_can_save = false;
    for (unsigned i = 0; i < 3; i++) { if (remove(paths[i]) != 0) assert(errno == ENOENT); }
    write_bytes(0, data, length); write_bytes(2, backup, backup_length);
    write_bytes(1, retained, sizeof(retained) - 1);
    recorded_error = 0; fs_calls = mutations = read_calls = close_calls = 0;
    read_file = write_file = NULL; read_error = write_error = false; sd_ready = true; fault = NO_FAULT;
    no_errno = stale_success = full_read = positive_probe = full_write_flag = close_cleanup_error = remove_cleanup_error = false;
    existing_directory = false;
    stale_errno = EBUSY;
    error_reports = write_opens = directory_calls = remove_calls = write_calls = flush_calls = sync_calls = 0;
}
static void report(const char *name, bool pass, size_t length, size_t loaded, unsigned blocked_mutations)
{
    cases++; failures += !pass;
    printf("%s %s input=%zu displayed=%zu fs_calls=%u mutations=%u blocked_mutations=%u reads=%u closes=%u handles=%u allocations=%u error=%d\n",
        pass ? "PASS" : "FAIL", name, length, loaded, fs_calls, mutations, blocked_mutations,
        read_calls, close_calls, handles, allocations, recorded_error);
}
static lv_obj_t *boundary_status(void)
{
#ifdef NOTES_REAL_LVGL
    return lv_obj_get_child(lv_obj_get_child(content, 0), 0);
#else
    return &objects[1];
#endif
}
static bool boundary_disabled(void)
{
#ifdef NOTES_REAL_LVGL
    return lv_obj_has_state(lv_obj_get_child(lv_obj_get_child(content, 0), 1), LV_STATE_DISABLED) &&
           lv_obj_has_state(note_area, LV_STATE_DISABLED);
#else
    return (objects[2].state & LV_STATE_DISABLED) && (note_area->state & LV_STATE_DISABLED);
#endif
}
static void boundary_save(void)
{
#ifdef NOTES_REAL_LVGL
    lv_obj_send_event(lv_obj_get_child(lv_obj_get_child(content, 0), 1), LV_EVENT_CLICKED, NULL);
#else
    lv_event_t event = {.user_data = boundary_status()}; save_note(&event);
#endif
}
static const char *boundary_status_text(void)
{
#ifdef NOTES_REAL_LVGL
    return lv_label_get_text(boundary_status());
#else
    return boundary_status()->text;
#endif
}
static void load_boundary(const char *name, const unsigned char *data, size_t length, fault_t point,
                          bool missing_errno, bool full_count, bool positive, bool cleanup_error, int expected)
{
    prepare(data, length, data ? old_backup : NULL, data ? sizeof(old_backup) - 1 : 0);
    fault = point; no_errno = missing_errno; stale_success = true; full_read = full_count; positive_probe = positive;
    close_cleanup_error = cleanup_error; if (point == OPEN_READ_ERROR) stale_errno = ENOENT;
    notes_clicked(NULL);
    size_t displayed = strlen(lv_textarea_get_text(note_area));
    bool pass = !note_can_save && boundary_disabled() && !displayed && recorded_error == expected && error_reports == 1;
    unsigned previous_mutations = mutations; boundary_save();
    pass = pass && mutations == previous_mutations && !handles && !allocations;
    pass = pass && bytes_match(0, data, length) && bytes_match(1, retained, sizeof(retained) - 1);
    pass = pass && bytes_match(2, data ? old_backup : NULL, data ? sizeof(old_backup) - 1 : 0);
    report(name, pass, length, displayed, mutations - previous_mutations);
}
static void save_boundary(const char *name, fault_t point, bool missing_errno, bool full_flag, bool cleanup_error, bool retain_partial)
{
    const char *draft = "new complete note\n";
    prepare(old_note, sizeof(old_note) - 1, old_backup, sizeof(old_backup) - 1);
    notes_clicked(NULL); lv_textarea_set_text(note_area, draft);
    fault = point; no_errno = missing_errno; stale_success = true; full_write_flag = full_flag;
    close_cleanup_error = cleanup_error; remove_cleanup_error = retain_partial;
    errno = point == DIRECTORY_ERROR ? EEXIST : EBUSY;
    unsigned previous_opens = write_opens, previous_removes = remove_calls, previous_closes = close_calls;
    int expected = missing_errno ? EIO : point == WRITE_ERROR ? EIO : EACCES;
    boundary_save();
    bool pass = strncmp(boundary_status_text(), "Saved", 5) && recorded_error == expected && error_reports == 1;
    pass = pass && !handles && !allocations && !flush_calls && !sync_calls;
    pass = pass && bytes_match(0, old_note, sizeof(old_note) - 1) && bytes_match(2, old_backup, sizeof(old_backup) - 1);
    if (point == DIRECTORY_ERROR || point == REMOVE_TEMP_ERROR) {
        pass = pass && write_opens == previous_opens && close_calls == previous_closes && !write_calls;
        if (point == DIRECTORY_ERROR) pass = pass && remove_calls == previous_removes;
        pass = pass && bytes_match(1, retained, sizeof(retained) - 1);
    } else if (point == WRITE_ERROR && retain_partial) {
        const unsigned char *expected_temp = full_flag ? (const unsigned char *)draft : (const unsigned char *)"new compl";
        pass = pass && bytes_match(1, expected_temp, full_flag ? strlen(draft) : 9) && close_calls == previous_closes + 1;
    } else {
        pass = pass && bytes_match(1, NULL, 0);
    }
    report(name, pass, strlen(draft), strlen(lv_textarea_get_text(note_area)), 0);
}
static void repeat_boundary_retry(const char *name, fault_t point)
{
    bool pass = true;
#ifdef NOTES_REAL_LVGL
    clear_content(); lv_obj_update_layout(content); lv_mem_monitor_t before, after; lv_mem_monitor(&before);
#endif
    for (unsigned i = 0; i < 25; i++) {
        prepare(old_note, sizeof(old_note) - 1, old_backup, sizeof(old_backup) - 1);
        notes_clicked(NULL); lv_textarea_set_text(note_area, "new complete note\n");
        fault = point; no_errno = stale_success = true; close_cleanup_error = remove_cleanup_error = point == WRITE_ERROR;
        errno = EEXIST; boundary_save();
        pass = pass && recorded_error == EIO && error_reports == 1 && !handles && !allocations;
        pass = pass && bytes_match(0, old_note, sizeof(old_note) - 1) && bytes_match(2, old_backup, sizeof(old_backup) - 1);
        pass = pass && bytes_match(1, point == WRITE_ERROR ? (const unsigned char *)"new compl" : retained,
                                  point == WRITE_ERROR ? 9 : sizeof(retained) - 1);
        fault = NO_FAULT; no_errno = stale_success = close_cleanup_error = remove_cleanup_error = false;
        boundary_save();
        pass = pass && !strncmp(boundary_status_text(), "Saved", 5) && error_reports == 1 && !handles && !allocations;
        pass = pass && bytes_match(0, (const unsigned char *)"new complete note\n", 18) && bytes_match(2, old_note, sizeof(old_note) - 1) && bytes_match(1, NULL, 0);
        notes_clicked(NULL);
        pass = pass && !strcmp(lv_textarea_get_text(note_area), "new complete note\n") && !handles && !allocations;
        clear_content();
#ifdef NOTES_REAL_LVGL
        lv_obj_update_layout(content);
#endif
    }
#ifdef NOTES_REAL_LVGL
    lv_mem_monitor(&after); pass = pass && before.free_size == after.free_size && before.used_cnt == after.used_cnt;
#endif
    report(name, pass, 18, 18, 0);
}
static void preparation_success(const char *name, bool directory_exists, bool temp_missing)
{
    prepare(old_note, sizeof(old_note) - 1, old_backup, sizeof(old_backup) - 1);
    notes_clicked(NULL); lv_textarea_set_text(note_area, "new complete note\n");
    existing_directory = directory_exists;
    if (temp_missing) assert(remove(paths[1]) == 0);
    boundary_save();
    bool pass = !strncmp(boundary_status_text(), "Saved", 5) && !error_reports && !handles && !allocations;
    pass = pass && bytes_match(0, (const unsigned char *)"new complete note\n", 18) && bytes_match(2, old_note, sizeof(old_note) - 1) && bytes_match(1, NULL, 0);
    report(name, pass, 18, strlen(lv_textarea_get_text(note_area)), 0);
}
#ifndef NOTES_REAL_LVGL
static void load_case(const char *name, const unsigned char *data, size_t length,
                      const unsigned char *backup, size_t backup_length, fault_t injected,
                      bool loaded, int expected_error)
{
    prepare(data, length, backup, backup_length); fault = injected; sd_ready = injected != NO_SD;
    notes_clicked(NULL); assert(object_count == 6 && note_area == &objects[4]);
    lv_obj_t *status = &objects[1], *save = &objects[2];
    const unsigned char *expected = data ? data : backup ? backup : (const unsigned char *)"";
    size_t expected_length = data ? length : backup ? backup_length : 0;
    size_t displayed = strlen(note_area->text);
    bool pass = !handles && !allocations;
    if (loaded) pass = pass && displayed == expected_length && !memcmp(note_area->text, expected, expected_length) && !(save->state & LV_STATE_DISABLED);
    else pass = pass && (save->state & LV_STATE_DISABLED) && (note_area->state & LV_STATE_DISABLED) && (objects[5].state & LV_STATE_DISABLED);
    if (expected_error) pass = pass && recorded_error == expected_error;
    if (injected == NO_SD || injected == ALLOC_ERROR) pass = pass && !fs_calls;
    unsigned previous_mutations = mutations;
    lv_event_t event = {.user_data = status}; save_note(&event);
    unsigned blocked_mutations = loaded ? 0 : mutations - previous_mutations;
    if (loaded) {
        pass = pass && bytes_match(0, expected, expected_length) && bytes_match(1, NULL, 0);
        pass = pass && bytes_match(2, data || backup ? expected : NULL, expected_length);
        pass = pass && !strncmp(status->text, "Saved", 5);
    } else {
        pass = pass && !blocked_mutations && bytes_match(0, data, length) && bytes_match(2, backup, backup_length);
        pass = pass && bytes_match(1, retained, sizeof(retained) - 1) && strncmp(status->text, "Saved", 5);
    }
    report(name, pass && !handles && !allocations, expected_length, displayed, blocked_mutations);
}
static void save_case(const char *name, const char *draft, fault_t injected, bool saved, bool blocked)
{
    prepare(old_note, sizeof(old_note) - 1, old_backup, sizeof(old_backup) - 1);
    notes_clicked(NULL); assert(note_area); lv_textarea_set_text(note_area, draft);
    unsigned previous_mutations = mutations;
    fault = injected; if (injected == NO_SD) sd_ready = false;
    lv_event_t event = {.user_data = &objects[1]}; save_note(&event);
    bool pass = !handles && !allocations && (!strncmp(objects[1].text, "Saved", 5) == saved);
    if (saved) {
        pass = pass && bytes_match(0, (const unsigned char *)draft, strlen(draft)) && bytes_match(2, old_note, sizeof(old_note) - 1) && bytes_match(1, NULL, 0);
    } else if (injected == PUBLISH_ERROR || injected == ROLLBACK_ERROR) {
        pass = pass && bytes_match(0, injected == ROLLBACK_ERROR ? NULL : old_note, sizeof(old_note) - 1);
        pass = pass && bytes_match(2, injected == ROLLBACK_ERROR ? old_note : NULL, sizeof(old_note) - 1);
        pass = pass && bytes_match(1, (const unsigned char *)draft, strlen(draft));
    } else {
        pass = pass && bytes_match(0, old_note, sizeof(old_note) - 1) && bytes_match(2, old_backup, sizeof(old_backup) - 1);
        pass = pass && bytes_match(1, blocked ? retained : NULL, sizeof(retained) - 1);
    }
    unsigned blocked_mutations = blocked ? mutations - previous_mutations : 0;
    pass = pass && (!blocked || !blocked_mutations);
    report(name, pass, strlen(draft), strlen(note_area->text), blocked_mutations);
}

int main(void)
{
    for (unsigned i = 0; i < 3; i++) {
        assert(snprintf(paths[i], sizeof(paths[i]), ".notes_io_%ld_%u", (long)fixture_pid(), i) > 0);
        struct stat info; assert(stat(paths[i], &info) != 0 && errno == ENOENT);
    }
    load_case("sd-unavailable", old_note, sizeof(old_note) - 1, old_backup, sizeof(old_backup) - 1, NO_SD, false, 0);
    load_case("missing-note", NULL, 0, NULL, 0, NO_FAULT, true, 0);
    load_case("empty-note", (const unsigned char *)"", 0, NULL, 0, NO_FAULT, true, 0);
    load_case("complete-note", old_note, sizeof(old_note) - 1, old_backup, sizeof(old_backup) - 1, NO_FAULT, true, 0);
    load_case("backup-recovery", NULL, 0, old_backup, sizeof(old_backup) - 1, NO_FAULT, true, 0);
    char *large = malloc(NOTE_MAX_BYTES + 2); assert(large); memset(large, 'x', NOTE_MAX_BYTES + 1); large[NOTE_MAX_BYTES + 1] = 0;
    load_case("previous-2k-boundary", (const unsigned char *)large, 2047, NULL, 0, NO_FAULT, true, 0);
    load_case("previously-truncated-2k", (const unsigned char *)large, 2048, NULL, 0, NO_FAULT, true, 0);
    load_case("previously-truncated-5k", (const unsigned char *)large, 5120, NULL, 0, NO_FAULT, true, 0);
    load_case("maximum-note", (const unsigned char *)large, NOTE_MAX_BYTES, NULL, 0, NO_FAULT, true, 0);
    load_case("over-limit-note", (const unsigned char *)large, NOTE_MAX_BYTES + 1, old_backup, sizeof(old_backup) - 1, NO_FAULT, false, 0);
    const unsigned char nul_note[] = {'a', 0, 'b', '\n'};
    load_case("embedded-nul", nul_note, sizeof(nul_note), old_backup, sizeof(old_backup) - 1, NO_FAULT, false, 0);
    const char *fault_names[] = {"allocation-error", "final-stat-error", "backup-stat-error", "recovery-error", "open-read-error", "partial-read-error", "zero-errno-read-error", "probe-read-error", "read-close-error", "first-read-error-retained"};
    for (unsigned f = ALLOC_ERROR; f <= READ_AND_CLOSE_ERROR; f++) {
        bool absent = f == BACKUP_STAT_ERROR || f == RECOVERY_ERROR;
        int error = f == ALLOC_ERROR ? 0 : f == ZERO_ERRNO_READ_ERROR || f == PROBE_READ_ERROR ? EIO : f == READ_CLOSE_ERROR ? EPERM : EACCES;
        load_case(fault_names[f - ALLOC_ERROR], absent ? NULL : (const unsigned char *)large, absent ? 0 : NOTE_MAX_BYTES,
            old_backup, sizeof(old_backup) - 1, (fault_t)f, false, error);
    }
    save_case("save-complete", "new complete note\n", NO_FAULT, true, false);
    large[NOTE_MAX_BYTES] = 0;
    save_case("save-maximum", large, NO_FAULT, true, false);
    large[NOTE_MAX_BYTES] = 'x';
    save_case("save-over-limit", large, NO_FAULT, false, true);
    save_case("save-card-lost", "new note", NO_SD, false, true);
    const char *save_faults[] = {"save-open-error", "save-write-error", "save-flush-error", "save-sync-error", "save-close-error", "save-publish-error", "save-rollback-error"};
    for (unsigned f = OPEN_WRITE_ERROR; f <= ROLLBACK_ERROR; f++) save_case(save_faults[f - OPEN_WRITE_ERROR], "new complete note\n", (fault_t)f, false, false);
    for (size_t i = 0; i + 1 < NOTE_MAX_BYTES + 1; i += 2) { large[i] = (char)0xc3; large[i + 1] = (char)0xa9; }
    large[NOTE_MAX_BYTES + 1] = 0;
    save_case("save-utf8-byte-limit", large, NO_FAULT, false, true);
    large[NOTE_MAX_BYTES - 1] = 0;
    load_case("complete-utf8", (const unsigned char *)large, NOTE_MAX_BYTES - 1, NULL, 0, NO_FAULT, true, 0);
    memset(large, 'x', NOTE_MAX_BYTES + 1); large[NOTE_MAX_BYTES + 1] = '\0';
    load_boundary("open-no-errno-existing", old_note, sizeof(old_note) - 1, OPEN_READ_ERROR, true, false, false, false, EIO);
    load_boundary("open-no-errno-missing", NULL, 0, OPEN_READ_ERROR, true, false, false, false, EIO);
    load_boundary("probe-no-errno", old_note, sizeof(old_note) - 1, PROBE_READ_ERROR, true, false, false, false, EIO);
    load_boundary("positive-probe-error", (const unsigned char *)large, NOTE_MAX_BYTES + 1, PROBE_READ_ERROR, false, false, true, false, EIO);
    load_boundary("positive-probe-no-errno", (const unsigned char *)large, NOTE_MAX_BYTES + 1, PROBE_READ_ERROR, true, false, true, false, EIO);
    load_boundary("close-no-errno", old_note, sizeof(old_note) - 1, READ_CLOSE_ERROR, true, false, false, false, EIO);
    load_boundary("full-read-error", (const unsigned char *)large, NOTE_MAX_BYTES, PARTIAL_READ_ERROR, false, true, false, false, EACCES);
    load_boundary("full-read-no-errno", (const unsigned char *)large, NOTE_MAX_BYTES, PARTIAL_READ_ERROR, true, true, false, false, EIO);
    load_boundary("first-probe-no-errno", old_note, sizeof(old_note) - 1, PROBE_READ_ERROR, true, false, false, true, EIO);
    load_boundary("first-full-read-error", (const unsigned char *)large, NOTE_MAX_BYTES, PARTIAL_READ_ERROR, false, true, false, true, EACCES);
    const fault_t preparation[] = {DIRECTORY_ERROR, REMOVE_TEMP_ERROR, OPEN_WRITE_ERROR, WRITE_ERROR};
    const char *preparation_names[] = {"directory", "temp-removal", "open-write", "partial-write"};
    for (unsigned i = 0; i < 4; i++) for (unsigned missing = 0; missing < 2; missing++) {
        char name[64]; snprintf(name, sizeof(name), "%s-%s", preparation_names[i], missing ? "no-errno" : "error");
        save_boundary(name, preparation[i], missing, false, false, false);
    }
    save_boundary("positive-write-error", WRITE_ERROR, false, true, false, false);
    save_boundary("positive-write-no-errno", WRITE_ERROR, true, true, false, false);
    save_boundary("first-write-close-error", WRITE_ERROR, false, false, true, false);
    save_boundary("first-write-no-errno-cleanup", WRITE_ERROR, true, false, true, true);
    save_boundary("first-positive-write-cleanup", WRITE_ERROR, true, true, true, true);
    repeat_boundary_retry("preparation-failure-retry-25", DIRECTORY_ERROR);
    repeat_boundary_retry("write-failure-retry-25", WRITE_ERROR);
    preparation_success("existing-directory-accepted", true, false);
    preparation_success("missing-temp-accepted", false, true);
    free(large);
#if NOTES_HAS_LEAVE
    prepare(old_note, sizeof(old_note) - 1, NULL, 0); notes_clicked(NULL);
    unsigned previous_mutations = mutations; notes_leave();
    lv_event_t event = {.user_data = &objects[1]}; save_note(&event);
    report("leave-rejects-stale-save", !note_area && !note_can_save && mutations == previous_mutations && bytes_match(0, old_note, sizeof(old_note) - 1), 0, 0, mutations - previous_mutations);
    bool stable = true;
    for (unsigned cycle = 0; cycle < 25; cycle++) { notes_clicked(NULL); notes_leave(); stable = stable && !allocations && !handles && !note_area && !note_can_save; }
    report("reopen-25", stable, 0, 0, 0);
#else
    report("leave-rejects-stale-save", false, 0, 0, 0);
    report("reopen-25", false, 0, 0, 0);
#endif
    clear_content();
    for (unsigned i = 0; i < 3; i++) { if (remove(paths[i]) != 0) assert(errno == ENOENT); }
    printf("%s %u Notes cases failures=%u handles=%u allocations=%u (native files; controlled UI/heap/I/O)\n", failures ? "FAIL" : "PASS", cases, failures, handles, allocations);
    return failures ? 1 : 0;
}
#else
static lv_obj_t *save_button(void) { return lv_obj_get_child(lv_obj_get_child(content, 0), 1); }
static lv_obj_t *status_label(void) { return lv_obj_get_child(lv_obj_get_child(content, 0), 0); }
static void real_load_case(const char *name, const unsigned char *data, size_t length, bool loaded)
{
    prepare(data, length, old_backup, sizeof(old_backup) - 1);
    notes_clicked(NULL); lv_obj_update_layout(content);
    size_t displayed = strlen(lv_textarea_get_text(note_area));
    bool pass = !handles && !allocations && note_can_save == loaded && lv_textarea_get_max_length(note_area) == NOTE_MAX_BYTES;
    if (loaded) pass = pass && displayed == length && !memcmp(lv_textarea_get_text(note_area), data, length);
    else pass = pass && lv_obj_has_state(save_button(), LV_STATE_DISABLED) && lv_obj_has_state(note_area, LV_STATE_DISABLED);
    unsigned previous_mutations = mutations;
    lv_obj_send_event(save_button(), LV_EVENT_CLICKED, NULL);
    if (loaded) pass = pass && bytes_match(0, data, length) && bytes_match(2, data, length) && !strncmp(lv_label_get_text(status_label()), "Saved", 5);
    else pass = pass && mutations == previous_mutations && bytes_match(0, data, length) && bytes_match(1, retained, sizeof(retained) - 1) && bytes_match(2, old_backup, sizeof(old_backup) - 1);
    report(name, pass && !handles && !allocations, length, displayed, loaded ? 0 : mutations - previous_mutations);
}
int main(void)
{
    lv_init(); lv_display_t *display = lv_display_create(720, 1280); assert(display);
    content = lv_obj_create(lv_screen_active()); lv_obj_set_size(content, 680, 1100);
    for (unsigned i = 0; i < 3; i++) {
        assert(snprintf(paths[i], sizeof(paths[i]), ".notes_io_%ld_%u", (long)fixture_pid(), i) > 0);
        struct stat info; assert(stat(paths[i], &info) != 0 && errno == ENOENT);
    }
    char *large = malloc(NOTE_MAX_BYTES + 2); assert(large); memset(large, 'x', NOTE_MAX_BYTES + 1); large[NOTE_MAX_BYTES + 1] = 0;
    real_load_case("real-lvgl-maximum", (const unsigned char *)large, NOTE_MAX_BYTES, true);
    real_load_case("real-lvgl-over-limit", (const unsigned char *)large, NOTE_MAX_BYTES + 1, false);
    for (size_t i = 0; i + 1 < NOTE_MAX_BYTES + 1; i += 2) { large[i] = (char)0xc3; large[i + 1] = (char)0xa9; }
    large[NOTE_MAX_BYTES - 1] = 0;
    real_load_case("real-lvgl-utf8", (const unsigned char *)large, NOTE_MAX_BYTES - 1, true);
    large[NOTE_MAX_BYTES - 1] = (char)0xa9;
    prepare(old_note, sizeof(old_note) - 1, old_backup, sizeof(old_backup) - 1); notes_clicked(NULL);
    /* The character limit permits 32,768 two-byte characters. Load in bulk. */
    lv_textarea_set_max_length(note_area, 0); lv_textarea_set_text(note_area, large);
    lv_textarea_set_max_length(note_area, NOTE_MAX_BYTES);
    unsigned previous_mutations = mutations; lv_obj_send_event(save_button(), LV_EVENT_CLICKED, NULL);
    report("real-lvgl-utf8-byte-guard", mutations == previous_mutations && bytes_match(0, old_note, sizeof(old_note) - 1) && bytes_match(2, old_backup, sizeof(old_backup) - 1) && bytes_match(1, retained, sizeof(retained) - 1) && strncmp(lv_label_get_text(status_label()), "Saved", 5), NOTE_MAX_BYTES + 1, strlen(lv_textarea_get_text(note_area)), mutations - previous_mutations);
    load_boundary("real-lvgl-open-no-errno", old_note, sizeof(old_note) - 1, OPEN_READ_ERROR, true, false, false, false, EIO);
    load_boundary("real-lvgl-probe-no-errno", old_note, sizeof(old_note) - 1, PROBE_READ_ERROR, true, false, false, false, EIO);
    save_boundary("real-lvgl-directory-error", DIRECTORY_ERROR, false, false, false, false);
    save_boundary("real-lvgl-temp-removal-error", REMOVE_TEMP_ERROR, false, false, false, false);
    save_boundary("real-lvgl-first-write-error", WRITE_ERROR, true, false, true, true);
    repeat_boundary_retry("real-lvgl-failure-retry-25", WRITE_ERROR);
    preparation_success("real-lvgl-existing-directory-missing-temp", true, true);
    free(large);
    prepare(old_note, sizeof(old_note) - 1, NULL, 0); notes_clicked(NULL); clear_content(); lv_obj_update_layout(content);
    lv_mem_monitor_t before, after; lv_mem_monitor(&before);
    bool stable = true;
    for (unsigned cycle = 0; cycle < 25; cycle++) {
        notes_clicked(NULL); lv_obj_update_layout(content); clear_content();
        stable = stable && !allocations && !handles && !note_area && !note_can_save;
    }
    lv_mem_monitor(&after);
    stable = stable && before.free_size == after.free_size && before.used_cnt == after.used_cnt;
    report("real-lvgl-reopen-25", stable, 0, 0, 0);
    printf("HEAP before_free=%zu after_free=%zu before_used=%zu after_used=%zu max_used=%zu pool=%zu\n", before.free_size, after.free_size, before.used_cnt, after.used_cnt, after.max_used, after.total_size);
    for (unsigned i = 0; i < 3; i++) { if (remove(paths[i]) != 0) assert(errno == ENOENT); }
    lv_obj_delete(content); content = NULL; lv_display_delete(display); lv_deinit();
    printf("%s %u real LVGL Notes cases failures=%u handles=%u allocations=%u\n", failures ? "FAIL" : "PASS", cases, failures, handles, allocations);
    return failures ? 1 : 0;
}
#endif
