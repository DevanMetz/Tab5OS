#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "storage_io.h"

#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

static void remember_errno(int *first_error)
{
    if (*first_error == 0) {
        *first_error = errno != 0 ? errno : EIO;
    }
}

static int sync_descriptor(FILE *file)
{
#ifdef _WIN32
    return _commit(_fileno(file));
#else
    return fsync(fileno(file));
#endif
}

int storage_sync_file(FILE *file)
{
    if (file == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ferror(file)) {
        if (errno == 0) {
            errno = EIO;
        }
        return -1;
    }
    if (fflush(file) != 0) {
        return -1;
    }
    return sync_descriptor(file);
}

static int close_synced(FILE **file_pointer)
{
    FILE *file = *file_pointer;
    int first_error = 0;

    if (storage_sync_file(file) != 0) {
        remember_errno(&first_error);
    }
    if (fclose(file) != 0) {
        remember_errno(&first_error);
    }
    *file_pointer = NULL;
    return first_error;
}

static int path_exists(const char *path)
{
    struct stat info;
    if (stat(path, &info) == 0) {
        return 1;
    }
    return errno == ENOENT ? 0 : -1;
}

static int discard_failed_stream(const char *temporary_path, int first_error)
{
    remove(temporary_path);
    errno = first_error;
    return -1;
}

int storage_commit_new_file(FILE **temporary_file,
                            const char *temporary_path,
                            const char *final_path)
{
    if (temporary_file == NULL || *temporary_file == NULL ||
        temporary_path == NULL || final_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    int first_error = close_synced(temporary_file);
    if (first_error != 0) {
        errno = first_error;
        return -1;
    }

    int final_exists = path_exists(final_path);
    if (final_exists > 0) {
        first_error = EEXIST;
    } else if (final_exists < 0) {
        remember_errno(&first_error);
    }
    if (first_error == 0 && rename(temporary_path, final_path) != 0) {
        remember_errno(&first_error);
    }

    if (first_error != 0) {
        errno = first_error;
        return -1;
    }

    return 0;
}

int storage_commit_replace_file(FILE **temporary_file,
                                const char *temporary_path,
                                const char *final_path,
                                const char *backup_path)
{
    if (temporary_file == NULL || *temporary_file == NULL ||
        temporary_path == NULL || final_path == NULL || backup_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    int first_error = close_synced(temporary_file);
    if (first_error != 0) {
        return discard_failed_stream(temporary_path, first_error);
    }
    int final_exists = 0;
    int final_was_backed_up = 0;

    if (first_error == 0) {
        final_exists = path_exists(final_path);
        if (final_exists < 0) {
            remember_errno(&first_error);
        }
    }
    if (first_error == 0 && final_exists > 0 &&
        remove(backup_path) != 0 && errno != ENOENT) {
        remember_errno(&first_error);
    }
    if (first_error == 0 && final_exists > 0) {
        if (rename(final_path, backup_path) != 0) {
            remember_errno(&first_error);
        } else {
            final_was_backed_up = 1;
        }
    }
    if (first_error == 0 && rename(temporary_path, final_path) != 0) {
        remember_errno(&first_error);
        if (final_was_backed_up) {
            (void)rename(backup_path, final_path);
        }
    }

    if (first_error != 0) {
        errno = first_error;
        return -1;
    }

    return 0;
}

int storage_recover_replace(const char *final_path, const char *backup_path)
{
    if (final_path == NULL || backup_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    int final_exists = path_exists(final_path);
    if (final_exists > 0) {
        return 0;
    }
    if (final_exists < 0) {
        return -1;
    }

    int backup_exists = path_exists(backup_path);
    if (backup_exists == 0) {
        return 0;
    }
    if (backup_exists < 0) {
        return -1;
    }
    return rename(backup_path, final_path);
}

static int truncate_file(FILE *file, long length)
{
#ifdef _WIN32
    int result = _chsize_s(_fileno(file), length);
    if (result != 0) {
        errno = result;
        return -1;
    }
    return 0;
#else
    return ftruncate(fileno(file), length);
#endif
}

int storage_repair_csv_tail(const char *path)
{
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    FILE *file = fopen(path, "r+b");
    if (file == NULL) {
        return errno == ENOENT ? 0 : -1;
    }

    int first_error = 0;
    long length = 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        remember_errno(&first_error);
    } else {
        length = ftell(file);
        if (length < 0) {
            remember_errno(&first_error);
        }
    }

    long repaired_length = length;
    if (first_error == 0 && length > 0) {
        if (fseek(file, length - 1, SEEK_SET) != 0) {
            remember_errno(&first_error);
        } else {
            int last_byte = fgetc(file);
            if (last_byte == EOF) {
                remember_errno(&first_error);
            } else if (last_byte != '\n') {
                repaired_length = 0;
                for (long offset = length - 2; offset >= 0; --offset) {
                    if (fseek(file, offset, SEEK_SET) != 0) {
                        remember_errno(&first_error);
                        break;
                    }
                    int byte = fgetc(file);
                    if (byte == EOF) {
                        remember_errno(&first_error);
                        break;
                    }
                    if (byte == '\n') {
                        repaired_length = offset + 1;
                        break;
                    }
                }
            }
        }
    }

    if (first_error == 0 && repaired_length < length) {
        if (truncate_file(file, repaired_length) != 0) {
            remember_errno(&first_error);
        } else if (sync_descriptor(file) != 0) {
            remember_errno(&first_error);
        }
    }
    if (fclose(file) != 0) {
        remember_errno(&first_error);
    }

    if (first_error != 0) {
        errno = first_error;
        return -1;
    }
    return 0;
}
