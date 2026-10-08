#pragma once
#include "ip_addr.h"
typedef int err_t;
#define ERR_OK 0
#define ERR_MEM (-1)
#define ERR_INPROGRESS (-5)
#define LWIP_DNS_ADDRTYPE_DEFAULT 2
#ifndef DNS_MAX_HOST_IP
#define DNS_MAX_HOST_IP 1
#endif
typedef void (*dns_found_callback)(const char *, const ip_addr_t *, void *);
err_t dns_gethostbyname_addrtype(const char *, ip_addr_t *, dns_found_callback, void *, unsigned char);
