#pragma once
#include <time.h>
static inline struct tm *localtime_r(const time_t *value, struct tm *out)
{
    return localtime_s(out, value) == 0 ? out : NULL;
}
