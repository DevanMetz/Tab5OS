#pragma once
#include <stdint.h>
/* Single-threaded dispatch-order model; real locks are covered by network_ui. */
typedef int portMUX_TYPE;
typedef unsigned TickType_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
