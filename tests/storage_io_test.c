#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "storage_io.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <process.h>
#define test_getpid _getpid
#else
#include <unistd.h>
#define test_getpid getpid
#endif

static void make_path(char *path, size_t size, const char *suffix)
{
    int length = snprintf(path, size, ".storage_io_test_%ld_%s",
                          (long)test_getpid(), suffix);
    assert(length > 0 && (size_t)length < size);
}

static int path_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

static FILE *open_temporary(const char *path, const char *contents)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(contents, 1, strlen(contents), file) == strlen(contents));
    return file;
}

static void write_file(const char *path, const char *contents)
{
    FILE *file = open_temporary(path, contents);
    assert(fclose(file) == 0);
}

static void assert_file_contents(const char *path, const char *expected)
{
    char contents[64];
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    size_t length = fread(contents, 1, sizeof(contents), file);
    assert(!ferror(file));
    assert(fclose(file) == 0);
    assert(length == strlen(expected));
    assert(memcmp(contents, expected, length) == 0);
}

static void clear_pair(const char *temporary_path, const char *final_path)
{
    remove(temporary_path);
    remove(final_path);
}

static void clear_replacement(const char *temporary_path,
                              const char *final_path,
                              const char *backup_path)
{
    clear_pair(temporary_path, final_path);
    remove(backup_path);
}

static void test_success(void)
{
    char temporary_path[128];
    char final_path[128];
    make_path(temporary_path, sizeof(temporary_path), "success.tmp");
    make_path(final_path, sizeof(final_path), "success.bin");
    clear_pair(temporary_path, final_path);

    FILE *temporary_file = open_temporary(temporary_path, "exact payload\n");
    assert(!path_exists(final_path));
    assert(storage_commit_new_file(&temporary_file, temporary_path, final_path) == 0);
    assert(temporary_file == NULL);
    assert(!path_exists(temporary_path));
    assert_file_contents(final_path, "exact payload\n");

    clear_pair(temporary_path, final_path);
}

static void test_existing_final_is_preserved(void)
{
    char temporary_path[128];
    char final_path[128];
    make_path(temporary_path, sizeof(temporary_path), "existing.tmp");
    make_path(final_path, sizeof(final_path), "existing.bin");
    clear_pair(temporary_path, final_path);

    FILE *temporary_file = open_temporary(temporary_path, "replacement");
    write_file(final_path, "original");
    assert(storage_commit_new_file(&temporary_file, temporary_path, final_path) == -1);
    int saved_errno = errno;
    assert(saved_errno == EEXIST);
    assert(temporary_file == NULL);
    assert_file_contents(temporary_path, "replacement");
    assert_file_contents(final_path, "original");

    clear_pair(temporary_path, final_path);
}

static void test_failed_new_stream_is_retained(void)
{
    char temporary_path[128];
    char final_path[128];
    make_path(temporary_path, sizeof(temporary_path), "failed.tmp");
    make_path(final_path, sizeof(final_path), "failed.bin");
    clear_pair(temporary_path, final_path);

    write_file(temporary_path, "incomplete");
    FILE *temporary_file = fopen(temporary_path, "rb");
    assert(temporary_file != NULL);
    errno = 0;
    assert(fputc('x', temporary_file) == EOF);
    int write_errno = errno;
    assert(write_errno != 0);
    assert(ferror(temporary_file));
    assert(storage_commit_new_file(&temporary_file, temporary_path, final_path) == -1);
    assert(errno == write_errno);
    assert(temporary_file == NULL);
    assert_file_contents(temporary_path, "incomplete");
    assert(!path_exists(final_path));

    clear_pair(temporary_path, final_path);
}

static void test_replace_first_creation(void)
{
    char temporary_path[128];
    char final_path[128];
    char backup_path[128];
    make_path(temporary_path, sizeof(temporary_path), "replace_first.tmp");
    make_path(final_path, sizeof(final_path), "replace_first.bin");
    make_path(backup_path, sizeof(backup_path), "replace_first.bak");
    clear_replacement(temporary_path, final_path, backup_path);

    FILE *temporary_file = open_temporary(temporary_path, "first");
    assert(!path_exists(final_path));
    assert(!path_exists(backup_path));
    assert(storage_commit_replace_file(&temporary_file, temporary_path,
                                       final_path, backup_path) == 0);
    assert(temporary_file == NULL);
    assert(!path_exists(temporary_path));
    assert_file_contents(final_path, "first");
    assert(!path_exists(backup_path));

    clear_replacement(temporary_path, final_path, backup_path);
}

static void test_replace_keeps_previous_final(void)
{
    char temporary_path[128];
    char final_path[128];
    char backup_path[128];
    make_path(temporary_path, sizeof(temporary_path), "replace_next.tmp");
    make_path(final_path, sizeof(final_path), "replace_next.bin");
    make_path(backup_path, sizeof(backup_path), "replace_next.bak");
    clear_replacement(temporary_path, final_path, backup_path);

    write_file(final_path, "previous");
    write_file(backup_path, "stale");
    FILE *temporary_file = open_temporary(temporary_path, "current");
    assert(storage_commit_replace_file(&temporary_file, temporary_path,
                                       final_path, backup_path) == 0);
    assert(temporary_file == NULL);
    assert(!path_exists(temporary_path));
    assert_file_contents(final_path, "current");
    assert_file_contents(backup_path, "previous");

    clear_replacement(temporary_path, final_path, backup_path);
}

static void test_replace_publish_failure_retains_temp(void)
{
    char temporary_path[192];
    char final_path[128];
    char backup_path[128];
    make_path(final_path, sizeof(final_path), "replace_fail.bin");
    make_path(backup_path, sizeof(backup_path), "replace_fail.bak");

#ifdef _WIN32
    make_path(temporary_path, sizeof(temporary_path), "replace_fail.tmp");
#else
    struct stat shared_memory;
    struct stat current_directory;
    if (stat("/dev/shm", &shared_memory) != 0 ||
        stat(".", &current_directory) != 0 ||
        shared_memory.st_dev == current_directory.st_dev) {
        return;
    }
    int length = snprintf(temporary_path, sizeof(temporary_path),
                          "/dev/shm/.storage_io_test_%ld_replace_fail.tmp",
                          (long)test_getpid());
    assert(length > 0 && (size_t)length < sizeof(temporary_path));
#endif

    clear_replacement(temporary_path, final_path, backup_path);
    write_file(final_path, "previous");
    FILE *temporary_file = open_temporary(temporary_path, "current");

#ifdef _WIN32
    FILE *rename_lock = fopen(temporary_path, "rb");
    assert(rename_lock != NULL);
#endif

    assert(storage_commit_replace_file(&temporary_file, temporary_path,
                                       final_path, backup_path) == -1);
    int saved_errno = errno;

#ifdef _WIN32
    assert(fclose(rename_lock) == 0);
#else
    assert(saved_errno == EXDEV);
#endif

    assert(saved_errno != 0);
    assert(temporary_file == NULL);
    assert_file_contents(temporary_path, "current");
    assert_file_contents(final_path, "previous");
    assert(!path_exists(backup_path));

    clear_replacement(temporary_path, final_path, backup_path);
}

static void test_replace_recovery(void)
{
    char temporary_path[128];
    char final_path[128];
    char backup_path[128];
    make_path(temporary_path, sizeof(temporary_path), "recover.tmp");
    make_path(final_path, sizeof(final_path), "recover.bin");
    make_path(backup_path, sizeof(backup_path), "recover.bak");
    clear_replacement(temporary_path, final_path, backup_path);

    write_file(backup_path, "last good");
    assert(!path_exists(final_path));
    assert(storage_recover_replace(final_path, backup_path) == 0);
    assert_file_contents(final_path, "last good");
    assert(!path_exists(backup_path));

    clear_replacement(temporary_path, final_path, backup_path);
}

static void test_repair_absent_csv(void)
{
    char path[128];
    make_path(path, sizeof(path), "repair_absent.csv");
    remove(path);

    assert(storage_repair_csv_tail(path) == 0);
    assert(!path_exists(path));
}

static void test_repair_complete_csv(void)
{
    char path[128];
    make_path(path, sizeof(path), "repair_complete.csv");
    remove(path);

    write_file(path, "time,value\n1,2\n");
    assert(storage_repair_csv_tail(path) == 0);
    assert_file_contents(path, "time,value\n1,2\n");

    remove(path);
}

static void test_repair_partial_csv_row(void)
{
    char path[128];
    make_path(path, sizeof(path), "repair_partial.csv");
    remove(path);

    write_file(path, "time,value\n1,2\n3,");
    assert(storage_repair_csv_tail(path) == 0);
    assert_file_contents(path, "time,value\n1,2\n");

    remove(path);
}

static void test_repair_csv_without_newline(void)
{
    char path[128];
    make_path(path, sizeof(path), "repair_no_newline.csv");
    remove(path);

    write_file(path, "incomplete");
    assert(storage_repair_csv_tail(path) == 0);
    assert_file_contents(path, "");

    remove(path);
}

int main(void)
{
    test_success();
    test_existing_final_is_preserved();
    test_failed_new_stream_is_retained();
    test_replace_first_creation();
    test_replace_keeps_previous_final();
    test_replace_publish_failure_retains_temp();
    test_replace_recovery();
    test_repair_absent_csv();
    test_repair_complete_csv();
    test_repair_partial_csv_row();
    test_repair_csv_without_newline();
    puts("storage_io tests passed");
    return 0;
}
