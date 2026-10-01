#pragma once
#include "host.h"
typedef SRWLOCK portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED SRWLOCK_INIT
#define portENTER_CRITICAL(lock) AcquireSRWLockExclusive(lock)
#define portEXIT_CRITICAL(lock) ReleaseSRWLockExclusive(lock)
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
