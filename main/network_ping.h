#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lwip/ip_addr.h"

typedef bool (*network_ping_cancel_cb_t)(void *context);

/* Four unicast echo probes. The worker owns the socket until this returns.
 * One-second probe waits and a six-second cooperative budget exclude DNS.
 * A cancellation callback is optional. Returns zero for a completed run
 * (including packet loss), otherwise errno; ECANCELED includes partial counts. */
int network_ping_target(const ip_addr_t *target, uint32_t *sent,
                        uint32_t *received, uint32_t *total_reply_ms,
                        network_ping_cancel_cb_t cancelled, void *context);
