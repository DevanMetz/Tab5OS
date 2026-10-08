/* Actual storage_io.c with counted stdio calls and controlled I/O faults. */
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
#include <io.h>
#include <process.h>
#define fixture_pid _getpid
#else
#include <unistd.h>
#define fixture_pid getpid
#endif

static unsigned seeks, block_reads, scalar_reads, closes, handles, truncates, syncs;
static size_t largest_read;
static unsigned fail_seek, fail_read, short_read;
static bool fail_getc, fail_truncate, fail_sync, fail_close;
static FILE *fixture_open(const char *path, const char *mode);
static int fixture_seek(FILE *file, long offset, int origin);
static int fixture_getc(FILE *file);
size_t fixture_read(void *buffer, size_t size, size_t count, FILE *file);
static int fixture_close(FILE *file);
#ifdef _WIN32
static int fixture_commit(int descriptor);
static int fixture_chsize(int descriptor, int64_t length);
#define _commit fixture_commit
#define _chsize_s fixture_chsize
#else
static int fixture_sync(int descriptor);
static int fixture_truncate(int descriptor, off_t length);
#define fsync fixture_sync
#define ftruncate fixture_truncate
#endif
#define fopen fixture_open
#define fseek fixture_seek
#define fgetc fixture_getc
#define fread fixture_read
#define fclose fixture_close
#include "storage_source.inc"
#undef fopen
#undef fseek
#undef fgetc
#undef fread
#undef fclose
#ifdef _WIN32
#undef _commit
#undef _chsize_s
#else
#undef fsync
#undef ftruncate
#endif

static FILE *fixture_open(const char *path, const char *mode)
{
    assert(!handles && !strcmp(mode, "r+b"));
    FILE *file = fopen(path, mode);
    if (file) handles++;
    return file;
}
static int fixture_seek(FILE *file, long offset, int origin)
{
    seeks++;
    if (seeks == fail_seek) { errno = EACCES; return -1; }
    return fseek(file, offset, origin);
}
static int fixture_getc(FILE *file)
{
    scalar_reads++;
    if (fail_getc) { errno = 0; return EOF; }
    return fgetc(file);
}
/* External linkage also compiles the preceding scalar-only implementation. */
size_t fixture_read(void *buffer, size_t size, size_t count, FILE *file)
{
    block_reads++;
    if (size * count > largest_read) largest_read = size * count;
    if (block_reads == fail_read) { errno = EIO; return 0; }
    if (block_reads == short_read) {
        assert(size == 1 && count > 1);
        size_t read = fread(buffer, size, count / 2, file);
        errno = 0;
        return read;
    }
    return fread(buffer, size, count, file);
}
static int fixture_close(FILE *file)
{
    assert(handles == 1);
    closes++; handles--;
    int result = fclose(file);
    if (fail_close) { errno = EPERM; return EOF; }
    return result;
}
#ifdef _WIN32
static int fixture_commit(int descriptor)
{
    syncs++;
    if (fail_sync) { errno = EIO; return -1; }
    return _commit(descriptor);
}
static int fixture_chsize(int descriptor, int64_t length)
{
    truncates++;
    if (fail_truncate) return EACCES;
    return _chsize_s(descriptor, length);
}
#else
static int fixture_sync(int descriptor)
{
    syncs++;
    if (fail_sync) { errno = EIO; return -1; }
    return fsync(descriptor);
}
static int fixture_truncate(int descriptor, off_t length)
{
    truncates++;
    if (fail_truncate) { errno = EACCES; return -1; }
    return ftruncate(descriptor, length);
}
#endif

static unsigned cases, failures;
static char case_path[128];
static void reset(void)
{
    seeks = block_reads = scalar_reads = closes = handles = truncates = syncs = 0;
    largest_read = 0;
    fail_seek = fail_read = short_read = 0;
    fail_getc = fail_truncate = fail_sync = fail_close = false;
}
static bool verify_bytes(const unsigned char *data, size_t length, long *actual_length)
{
    FILE *file = fopen(case_path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    *actual_length = ftell(file);
    bool matches = *actual_length == (long)length;
    assert(fseek(file, 0, SEEK_SET) == 0);
    unsigned char buffer[512];
    for (size_t offset = 0; matches && offset < length;) {
        size_t count = length - offset;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        matches = fread(buffer, 1, count, file) == count && !memcmp(buffer, data + offset, count);
        offset += count;
    }
    assert(!ferror(file) && fclose(file) == 0);
    return matches;
}
static unsigned char *partial_data(size_t tail, size_t *length)
{
    const char *prefix = "t,v\n1,2\n";
    *length = strlen(prefix) + tail;
    unsigned char *data = malloc(*length);
    assert(data);
    memcpy(data, prefix, strlen(prefix));
    memset(data + strlen(prefix), 'x', tail);
    return data;
}
typedef enum { NO_FAULT, SEEK_END_ERROR, SEEK_LAST_ERROR, SEEK_BLOCK_ERROR, SEEK_NEXT_BLOCK_ERROR,
    GETC_ERROR, READ_ERROR, NEXT_READ_ERROR, SHORT_READ, TRUNCATE_ERROR, SYNC_ERROR, CLOSE_ERROR,
    FIRST_ERROR_WITH_CLOSE_ERROR } fault_t;

static void run_case(const char *name, const unsigned char *data, size_t length, size_t kept,
                     int expected_error, fault_t fault, bool budget)
{
    struct stat info;
    assert(stat(case_path, &info) != 0 && errno == ENOENT);
    FILE *file = fopen(case_path, "wb");
    assert(file && fwrite(data, 1, length, file) == length && fclose(file) == 0);
    reset();
    switch (fault) {
        case NO_FAULT: break;
        case SEEK_END_ERROR: fail_seek = 1; break;
        case SEEK_LAST_ERROR: fail_seek = 2; break;
        case SEEK_BLOCK_ERROR: fail_seek = 3; break;
        case SEEK_NEXT_BLOCK_ERROR: fail_seek = 4; break;
        case GETC_ERROR: fail_getc = true; break;
        case READ_ERROR: fail_read = 1; break;
        case NEXT_READ_ERROR: fail_read = 2; break;
        case SHORT_READ: short_read = 1; break;
        case TRUNCATE_ERROR: fail_truncate = true; break;
        case SYNC_ERROR: fail_sync = true; break;
        case CLOSE_ERROR: fail_close = true; break;
        case FIRST_ERROR_WITH_CLOSE_ERROR: fail_seek = 3; fail_close = true; break;
    }
    errno = 0;
    int result = storage_repair_csv_tail(case_path);
    int error = errno;
    long actual_length;
    bool data_matches = verify_bytes(data, kept, &actual_length);
    bool pass = result == (expected_error ? -1 : 0) && (!expected_error || error == expected_error) &&
        !handles && closes == 1 && largest_read <= 512 && data_matches;
    if (budget) pass = pass && seeks <= 2 + (length + 511) / 512 && scalar_reads <= 1;
    if (!expected_error) {
        pass = pass && truncates == (unsigned)(kept < length) && syncs == truncates;
        if (!length || data[length - 1] == '\n') pass = pass && !block_reads;
    }
    cases++; failures += !pass;
    printf("%s %s input=%zu kept=%ld expected_kept=%zu seeks=%u blocks=%u scalar=%u largest_read=%zu truncates=%u syncs=%u closes=%u handles=%u result=%d error=%d budget=%u\n",
        pass ? "PASS" : "FAIL", name, length, actual_length, kept, seeks, block_reads, scalar_reads, largest_read,
        truncates, syncs, closes, handles, result, expected_error ? error : 0, (unsigned)budget);
    assert(remove(case_path) == 0);
}

int main(void)
{
    int path_length = snprintf(case_path, sizeof(case_path), ".storage_csv_tail_%ld.csv", (long)fixture_pid());
    assert(path_length > 0 && (size_t)path_length < sizeof(case_path));
    reset(); errno = 0;
    bool pass = storage_repair_csv_tail(NULL) == -1 && errno == EINVAL && !handles && !seeks;
    cases++; failures += !pass; printf("%s null-path handles=%u\n", pass ? "PASS" : "FAIL", handles);
    reset();
    pass = storage_repair_csv_tail(case_path) == 0 && !handles && !closes && !seeks;
    cases++; failures += !pass; printf("%s missing-file handles=%u\n", pass ? "PASS" : "FAIL", handles);
    run_case("empty", (const unsigned char *)"", 0, 0, 0, NO_FAULT, false);
    run_case("complete-lf", (const unsigned char *)"a,b\n1,2\n", 8, 8, 0, NO_FAULT, false);
    run_case("complete-crlf", (const unsigned char *)"a,b\r\n1,2\r\n", 10, 10, 0, NO_FAULT, false);
    run_case("single-lf", (const unsigned char *)"\n", 1, 1, 0, NO_FAULT, false);
    run_case("single-byte", (const unsigned char *)"x", 1, 0, 0, NO_FAULT, false);
    run_case("leading-lf", (const unsigned char *)"\nxxx", 4, 1, 0, NO_FAULT, false);
    run_case("partial-cr", (const unsigned char *)"a,b\n1,2\nx\r", 10, 8, 0, NO_FAULT, false);
    const unsigned char binary[] = {'h', '\n', 0, 0xff, 'x'};
    run_case("binary-tail", binary, sizeof(binary), 2, 0, NO_FAULT, false);
    run_case("multiple-rows", (const unsigned char *)"a,b\n1,2\n3,4\nbroken", 18, 12, 0, NO_FAULT, false);
    const size_t tails[] = {511, 512, 513, 1023, 1024, 1025};
    for (unsigned i = 0; i < sizeof(tails) / sizeof(tails[0]); i++) {
        char name[48]; size_t length;
        assert(snprintf(name, sizeof(name), "partial-%zu", tails[i]) > 0);
        unsigned char *data = partial_data(tails[i], &length);
        run_case(name, data, length, 8, 0, NO_FAULT, true);
        free(data);
    }
    size_t length;
    unsigned char *large = partial_data(65536, &length);
    large[length - 1] = '\n';
    run_case("large-complete", large, length, length, 0, NO_FAULT, true);
    large[length - 1] = 'x';
    run_case("long-partial-tail", large, length, 8, 0, NO_FAULT, true);
    memset(large, 'x', length);
    run_case("long-first-row", large, length, 0, 0, NO_FAULT, true);
    free(large);
    unsigned char *data = partial_data(1025, &length);
    const char *fault_names[] = {"seek-end-error", "seek-last-error", "seek-block-error", "seek-next-block-error",
        "scalar-read-error", "block-read-error", "next-block-read-error", "short-block-read", "truncate-error",
        "sync-error", "close-error", "first-error-retained"};
    for (unsigned fault = SEEK_END_ERROR; fault <= FIRST_ERROR_WITH_CLOSE_ERROR; fault++) {
        int error = fault <= SEEK_NEXT_BLOCK_ERROR || fault == TRUNCATE_ERROR || fault == FIRST_ERROR_WITH_CLOSE_ERROR ?
            EACCES : fault == CLOSE_ERROR ? EPERM : EIO;
        run_case(fault_names[fault - 1], data, length, fault == SYNC_ERROR || fault == CLOSE_ERROR ? 8 : length,
            error, (fault_t)fault, false);
    }
    free(data);
    printf("%s %u CSV tail cases failures=%u handles=%u (native files; controlled I/O faults)\n",
        failures ? "FAIL" : "PASS", cases, failures, handles);
    return failures ? 1 : 0;
}
