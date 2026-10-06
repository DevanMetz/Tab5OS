#pragma once
#include "host.h"
typedef int err_t;
#define ERR_OK 0
#define ERR_MEM (-1)
#define ERR_INPROGRESS (-5)
#define ERR_ARG (-16)
#define LWIP_DNS_ADDRTYPE_DEFAULT 2
#ifndef DNS_MAX_HOST_IP
#define DNS_MAX_HOST_IP 1
#endif
typedef struct { int family; union { struct in_addr v4; struct in6_addr v6; } value; } ip_addr_t;
typedef void (*dns_found_callback)(const char *, const ip_addr_t *, void *);
int ipaddr_aton(const char *text, ip_addr_t *address);
char *ipaddr_ntoa_r(const ip_addr_t *address, char *text, int capacity);
err_t dns_gethostbyname_addrtype(const char *host, ip_addr_t *address,
                               dns_found_callback found, void *argument, unsigned char type);
