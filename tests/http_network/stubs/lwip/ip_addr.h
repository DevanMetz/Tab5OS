#pragma once
#include "dns.h"

static inline int ip_addr_isany(const ip_addr_t *address)
{
    const unsigned char *bytes = (const unsigned char *)&address->value;
    size_t length = address->family == AF_INET ? 4 : 16;
    for (size_t i = 0; i < length; i++) if (bytes[i]) return 0;
    return 1;
}
