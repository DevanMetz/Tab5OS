#pragma once
#include "compat.h"
/* Count SDK allocations without changing its parser/client implementation. */
#define malloc http_host_malloc
#define calloc http_host_calloc
#define realloc http_host_realloc
#define free http_host_free
