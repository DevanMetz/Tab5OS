/* Map the tablet VFS into one test directory; real storage_io.c repairs/syncs
 * real files. Inject faults at the file boundary, without emulating the card. */
#include "storage.h"
#include <assert.h>
#include <ctype.h>
#include <direct.h>
#include <errno.h>
#include <io.h>
#include <stdarg.h>
#include <string.h>

volatile LONG http_host_open_files, http_host_storage_calls, http_host_storage_failures;
volatile LONG http_host_sync_blocked, http_host_sync_entered;
static volatile LONG fault;
static int write_remaining = -1;
static bool storage_allowed;
static FILE *append_file;
static char directory[1024], csv_path[1100];

void http_storage_setup(const char *output, const char *mode, bool allowed)
{
    assert(!http_host_open_files && !append_file);
    for (const unsigned char *p = (const unsigned char *)mode; *p; p++) assert(isalnum(*p) || *p == '-');
    assert(snprintf(directory, sizeof(directory), "%s/storage-%s", output, mode) < (int)sizeof(directory));
    assert(_mkdir(directory) == 0 || errno == EEXIST);
    size_t used = strlen(directory);
    assert(used + 6 < sizeof(directory)); memcpy(directory + used, "/HTTP", 6);
    assert(snprintf(csv_path, sizeof(csv_path), "%s/HTTPLOG.CSV", directory) < (int)sizeof(csv_path));
    assert(remove(csv_path) == 0 || errno == ENOENT);
    storage_allowed = allowed;
}
const char *http_storage_path(void) { return csv_path; }
void http_storage_seed(const char *text)
{
    assert(storage_allowed && !http_host_open_files);
    assert(_mkdir(directory) == 0 || errno == EEXIST);
    FILE *file = fopen(csv_path, "wb"); assert(file);
    assert(fputs(text, file) >= 0 && fclose(file) == 0);
}
void http_storage_fault(http_storage_fault_t value) { InterlockedExchange(&fault, value); }
void http_storage_write_limit(int bytes) { assert(bytes >= 0); write_remaining = bytes; }
static bool fail(http_storage_fault_t value, int error)
{
    if (InterlockedCompareExchange(&fault, HTTP_STORAGE_OK, value) != value) return false;
    InterlockedIncrement(&http_host_storage_failures); errno = error; return true;
}
static void operation(void) { assert(storage_allowed); InterlockedIncrement(&http_host_storage_calls); }
int http_storage_mkdir(const char *path)
{
    operation(); assert(!strcmp(path, "/sdcard/HTTP"));
    return fail(HTTP_STORAGE_MKDIR, EROFS) ? -1 : _mkdir(directory);
}
FILE *http_storage_fopen(const char *path, const char *mode)
{
    operation(); assert(!strcmp(path, "/sdcard/HTTP/HTTPLOG.CSV"));
    bool append = !strcmp(mode, "a+"); assert(append || !strcmp(mode, "r+b"));
    if (fail(append ? HTTP_STORAGE_OPEN : HTTP_STORAGE_REPAIR, append ? EROFS : EIO)) return NULL;
    FILE *file = fopen(csv_path, mode);
    if (file) {
        InterlockedIncrement(&http_host_open_files);
        if (append) { assert(!append_file); append_file = file; }
    }
    return file;
}
int http_storage_fclose(FILE *file)
{
    operation(); bool append = file == append_file;
    int result = fclose(file); InterlockedDecrement(&http_host_open_files);
    if (append) { append_file = NULL; if (fail(HTTP_STORAGE_CLOSE, EIO)) return EOF; }
    return result;
}
int http_storage_fflush(FILE *file)
{
    operation();
    return fail(HTTP_STORAGE_FLUSH, EIO) ? EOF : fflush(file);
}
int http_storage_commit(int descriptor)
{
    operation();
    if (append_file && descriptor == _fileno(append_file)) {
        InterlockedExchange(&http_host_sync_entered, 1);
        int64_t until = esp_timer_get_time() + 3000000;
        while (InterlockedCompareExchange(&http_host_sync_blocked, 0, 0)) {
            assert(esp_timer_get_time() < until); Sleep(2);
        }
    }
    return fail(HTTP_STORAGE_SYNC, ENOSPC) ? -1 : _commit(descriptor);
}
static int write_text(FILE *file, const char *text, size_t length)
{
    operation(); assert(file == append_file);
    size_t count = write_remaining < 0 || (size_t)write_remaining >= length ? length : (size_t)write_remaining;
    size_t written = fwrite(text, 1, count, file);
    if (write_remaining >= 0) write_remaining -= (int)written;
    if (written != length) {
        write_remaining = -1; InterlockedIncrement(&http_host_storage_failures); errno = ENOSPC; return -1;
    }
    return (int)written;
}
int http_storage_fputc(int character, FILE *file)
{
    char byte = (char)character;
    return write_text(file, &byte, 1) < 0 ? EOF : (unsigned char)character;
}
int http_storage_fputs(const char *text, FILE *file) { return write_text(file, text, strlen(text)) < 0 ? EOF : 0; }
int http_storage_fprintf(FILE *file, const char *format, ...)
{
    char text[1024]; va_list arguments; va_start(arguments, format);
    int length = vsnprintf(text, sizeof(text), format, arguments); va_end(arguments);
    assert(length >= 0 && length < (int)sizeof(text));
    return write_text(file, text, (size_t)length);
}
