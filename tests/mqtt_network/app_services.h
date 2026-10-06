#pragma once
#include "adapter.h"
#include <stdio.h>
extern volatile LONG mqtt_app_open_files, mqtt_app_closed_files;
extern volatile LONG mqtt_app_append_attempts, mqtt_app_file_waiting, mqtt_app_file_faults;
extern volatile LONG mqtt_app_storage_reports, mqtt_app_report_waiting;
void mqtt_app_storage_setup(const char *output, const char *mode);
bool mqtt_app_fault_mode(void);
void mqtt_app_release_file(void);
void mqtt_app_release_report(void);
void mqtt_app_storage_error(int error);
void mqtt_app_storage_shutdown(void);
struct tm *mqtt_app_localtime_r(const time_t *, struct tm *);
int mqtt_app_mkdir(const char *);
FILE *mqtt_app_fopen(const char *, const char *);
int mqtt_app_fclose(FILE *);
int mqtt_app_fflush(FILE *);
int mqtt_app_commit(int);
