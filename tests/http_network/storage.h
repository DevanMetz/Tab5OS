#pragma once
#include "adapter.h"
#include <stdio.h>

typedef enum {
    HTTP_STORAGE_OK, HTTP_STORAGE_MKDIR, HTTP_STORAGE_REPAIR,
    HTTP_STORAGE_OPEN, HTTP_STORAGE_FLUSH, HTTP_STORAGE_SYNC, HTTP_STORAGE_CLOSE,
} http_storage_fault_t;
extern volatile LONG http_host_open_files, http_host_storage_calls, http_host_storage_failures;
extern volatile LONG http_host_sync_blocked, http_host_sync_entered;
void http_storage_setup(const char *output, const char *mode, bool allowed);
void http_storage_fault(http_storage_fault_t fault);
void http_storage_write_limit(int bytes);
const char *http_storage_path(void);
void http_storage_seed(const char *text);
FILE *http_storage_fopen(const char *path, const char *mode);
int http_storage_mkdir(const char *path);
int http_storage_fclose(FILE *file);
int http_storage_fflush(FILE *file);
int http_storage_commit(int descriptor);
int http_storage_fputc(int character, FILE *file);
int http_storage_fputs(const char *text, FILE *file);
int http_storage_fprintf(FILE *file, const char *format, ...);
