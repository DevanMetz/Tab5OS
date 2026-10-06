#pragma once
#include "compat.h"
/* SDK source remains unchanged; all of its allocation paths are counted. */
#define malloc mqtt_net_malloc
#define calloc mqtt_net_calloc
#define realloc mqtt_net_realloc
#define free mqtt_net_free
