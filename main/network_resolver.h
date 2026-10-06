#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lwip/ip_addr.h"

#define NETWORK_RESOLVER_MAX_ADDRESSES 4

typedef bool (*network_resolver_cancel_cb_t)(void *context);

/* Called by a worker, never LVGL. Returns zero or errno, with an absolute
 * cooperative deadline. Up to two workers may resolve concurrently. Numeric
 * addresses bypass DNS; names use lwIP's IPv4-first/IPv6-fallback policy.
 * Late DNS callbacks carry an ID, never a pointer to the caller's state. */
int network_resolve_host(const char *host, ip_addr_t *addresses, size_t capacity,
                         size_t *count, int64_t deadline_us,
                         network_resolver_cancel_cb_t cancelled, void *context);
