#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WOL_MAC_BYTES 6
#define WOL_PACKET_BYTES 102

/* Exactly 12 hex digits or six pairs with consistent ':' or '-' separators.
 * Rejects zero, multicast and broadcast target MACs. Clears output on error. */
bool wol_parse_mac(const char *text, uint8_t mac[WOL_MAC_BYTES]);
/* Four decimal octets, no whitespace, signs or leading zeroes. Accepts
 * unicast/subnet broadcast and 255.255.255.255; rejects 0/8 and multicast/reserved. */
bool wol_parse_ipv4(const char *text, uint8_t address[4]);
bool wol_parse_port(const char *text, uint16_t *port);
bool wol_build_packet(const uint8_t mac[WOL_MAC_BYTES], uint8_t packet[WOL_PACKET_BYTES]);
void wol_data_self_test(void);
