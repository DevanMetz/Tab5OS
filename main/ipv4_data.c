#include "ipv4_data.h"

#include <stdio.h>

bool ipv4_parse(const char *text, uint32_t *address)
{
    if (!address) return false;
    *address = 0;
    if (!text) return false;
    uint32_t value = 0;
    for (unsigned octet = 0; octet < 4; octet++) {
        unsigned digits = 0, part = 0;
        bool leading_zero = *text == '0';
        while (*text >= '0' && *text <= '9') {
            if (++digits > 3 || (leading_zero && digits > 1)) return false;
            part = part * 10 + (unsigned)(*text++ - '0');
        }
        if (!digits || part > 255) return false;
        value = (value << 8) | part;
        if (octet == 3) {
            if (*text) return false;
        } else if (*text++ != '.') return false;
    }
    *address = value;
    return true;
}

void ipv4_format(uint32_t address, char text[IPV4_TEXT_SIZE])
{
    if (!text) return;
    snprintf(text, IPV4_TEXT_SIZE, "%u.%u.%u.%u", (unsigned)(address >> 24),
             (unsigned)((address >> 16) & 255), (unsigned)((address >> 8) & 255),
             (unsigned)(address & 255));
}
