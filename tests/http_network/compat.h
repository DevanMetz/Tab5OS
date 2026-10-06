#pragma once
#include <direct.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strdup http_host_strdup
#define mkdir(path, mode) _mkdir(path)
int vasprintf(char **output, const char *format, va_list arguments);
int asprintf(char **output, const char *format, ...);
char *strndup(const char *input, size_t length);
char *strcasestr(const char *input, const char *needle);
char *http_host_strdup(const char *input);
void *http_host_malloc(size_t size);
void *http_host_calloc(size_t count, size_t size);
void *http_host_realloc(void *memory, size_t size);
void http_host_free(void *memory);
