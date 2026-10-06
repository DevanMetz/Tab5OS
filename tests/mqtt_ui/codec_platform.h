#pragma once
/* The actual SDK codec uses only allocation and message-ID randomness here. */
#include <stdlib.h>
int platform_random(int max);
