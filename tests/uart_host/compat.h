/* Windows CRT spellings for the app's POSIX log calls. UI tests keep SD off. */
#pragma once
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
static inline struct tm *localtime_r(const time_t *value, struct tm *out)
{
    return localtime_s(out, value) == 0 ? out : NULL;
}
#define mkdir(path, mode) _mkdir(path)
#define open _open
#define close _close
#define unlink _unlink
#define fdopen _fdopen
