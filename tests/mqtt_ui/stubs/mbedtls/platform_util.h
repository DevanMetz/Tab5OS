#pragma once
#include <stddef.h>
static inline void mbedtls_platform_zeroize(void *memory, size_t length)
{
    volatile unsigned char *bytes = memory;
    while (length--) *bytes++ = 0;
}
