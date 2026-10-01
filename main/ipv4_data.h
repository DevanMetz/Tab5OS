#pragma once

#include <stdbool.h>
#include <stdint.h>

#define IPV4_TEXT_SIZE 16

/* Exactly four decimal octets, each 0..255. No leading zeros, whitespace,
 * alternate bases, abbreviated forms, hostnames or CIDR suffixes. Values use
 * the most-significant octet first (1.2.3.4 = 0x01020304), not socket byte order.
 * Syntax only: callers apply unicast/broadcast policy. Errors clear *address. */
bool ipv4_parse(const char *text, uint32_t *address);
void ipv4_format(uint32_t address, char text[IPV4_TEXT_SIZE]);
