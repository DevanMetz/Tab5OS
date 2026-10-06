#pragma once
#include <io.h>
#include <sys/stat.h>
#include "app_services.h"
#define localtime_r mqtt_app_localtime_r
#undef mkdir
#define mkdir(path, mode) mqtt_app_mkdir(path)
#define fopen mqtt_app_fopen
#define fclose mqtt_app_fclose
#define fflush mqtt_app_fflush
#define _commit mqtt_app_commit
