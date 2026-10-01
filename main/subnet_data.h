#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ipv4_data.h"

#define SUBNET_INPUT_MAX 32
#define SUBNET_IPV4_TEXT_SIZE IPV4_TEXT_SIZE

typedef enum {
    SUBNET_OK,
    SUBNET_BAD_ADDRESS,
    SUBNET_BAD_MASK,
    SUBNET_NONCONTIGUOUS_MASK,
    SUBNET_BAD_PEER,
    SUBNET_BAD_ARGUMENT
} subnet_status_t;

typedef enum {
    SUBNET_HOST,
    SUBNET_NETWORK_ADDRESS,
    SUBNET_BROADCAST_ADDRESS,
    SUBNET_OUTSIDE
} subnet_role_t;

typedef struct {
    /* Numeric IPv4 values, most significant octet first (not socket byte order). */
    uint32_t address;
    uint32_t mask;
    uint32_t wildcard;
    uint32_t network;
    uint32_t last_address;
    uint32_t first_host;
    uint32_t last_host;
    uint32_t peer;
    uint64_t address_count;
    uint64_t host_count;
    unsigned prefix;
    bool has_broadcast;
    bool has_peer;
    subnet_role_t address_role;
    subnet_role_t peer_role;
} subnet_data_t;

/* Strict dotted decimal: no leading zeros, spaces, hostnames, CIDR suffixes,
 * or alternate bases in addresses. Mask accepts 0..32, /0../32, or a contiguous
 * dotted netmask. An empty peer skips comparison; NULL is an invalid argument.
 * Counts describe address arithmetic, not assignment/routability. /31 uses
 * point-to-point semantics; /32 is one address. Errors clear the entire result. */
subnet_status_t subnet_calculate(const char *address, const char *mask,
                                 const char *peer, subnet_data_t *result);
void subnet_format_ipv4(uint32_t address, char text[SUBNET_IPV4_TEXT_SIZE]);
const char *subnet_error(subnet_status_t status);
void subnet_data_self_test(void);
