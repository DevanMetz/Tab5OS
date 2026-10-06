#pragma once
#include <io.h>
#include <sys/stat.h>
#include "adapter.h"
#define localtime_r mqtt_host_localtime_r
#undef mkdir
#define mkdir(path, mode) mqtt_host_mkdir(path)
#define fopen mqtt_host_fopen
#define fclose mqtt_host_fclose
#define fflush mqtt_host_fflush
#define _commit mqtt_host_commit
