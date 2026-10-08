#pragma once
#include <stdio.h>

FILE *csv_fixture_open(const char *path, const char *mode);
int csv_fixture_seek(FILE *file, long offset, int origin);
char *csv_fixture_gets(char *text, int size, FILE *file);
int csv_fixture_error(FILE *file);
int csv_fixture_close(FILE *file);

#define fopen csv_fixture_open
#define fseek csv_fixture_seek
#define fgets csv_fixture_gets
#define ferror csv_fixture_error
#define fclose csv_fixture_close
