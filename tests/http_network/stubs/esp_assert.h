#pragma once
#include <assert.h>
#define ESP_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
