#pragma once
#include <io.h>
#include <sys/stat.h>
#include "storage.h"

/* Only the app and actual storage_io.c use this VFS/file-fault adapter. */
#undef mkdir
#define mkdir(path, mode) http_storage_mkdir(path)
#define fopen http_storage_fopen
#define fclose http_storage_fclose
#define fflush http_storage_fflush
#define _commit http_storage_commit
#define fputc http_storage_fputc
#define fputs http_storage_fputs
#define fprintf http_storage_fprintf
