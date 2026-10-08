#pragma once
#include <stdint.h>
/* Opaque copied address values; this fixture does not parse IP packets. */
typedef struct { uint32_t value; } ip_addr_t;
int ipaddr_aton(const char *host, ip_addr_t *address);
#define ip_addr_isany(address) ((address)->value == 0)
