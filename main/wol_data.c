#include "wol_data.h"
#include "ipv4_data.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static int hex_digit(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static bool valid_mac(const uint8_t mac[WOL_MAC_BYTES])
{
    if (!mac || (mac[0] & 1)) return false;
    uint8_t any = 0;
    for (size_t i = 0; i < WOL_MAC_BYTES; i++) any |= mac[i];
    return any != 0;
}

bool wol_parse_mac(const char *text, uint8_t mac[WOL_MAC_BYTES])
{
    if (!mac) return false;
    memset(mac, 0, WOL_MAC_BYTES);
    if (!text) return false;
    size_t length = 0;
    while (length <= 17 && text[length]) length++;
    if (length != 12 && length != 17) return false;
    char separator = length == 17 ? text[2] : '\0';
    if (length == 17 && separator != ':' && separator != '-') return false;
    uint8_t parsed[WOL_MAC_BYTES];
    for (size_t i = 0; i < WOL_MAC_BYTES; i++) {
        size_t offset = i * (length == 17 ? 3 : 2);
        int high = hex_digit((unsigned char)text[offset]);
        int low = hex_digit((unsigned char)text[offset + 1]);
        if (high < 0 || low < 0 || (length == 17 && i < 5 && text[offset + 2] != separator)) return false;
        parsed[i] = (uint8_t)((high << 4) | low);
    }
    if (!valid_mac(parsed)) return false;
    memcpy(mac, parsed, WOL_MAC_BYTES);
    return true;
}

bool wol_parse_ipv4(const char *text, uint8_t address[4])
{
    if (!address) return false;
    memset(address, 0, 4);
    if (!text) return false;
    uint32_t parsed;
    if (!ipv4_parse(text, &parsed)) return false;
    unsigned first = (unsigned)(parsed >> 24);
    if (!first || (first >= 224 && parsed != UINT32_MAX)) return false;
    for (unsigned i = 0; i < 4; i++) address[i] = (uint8_t)(parsed >> (24 - 8 * i));
    return true;
}

bool wol_parse_port(const char *text, uint16_t *port)
{
    if (!port) return false;
    *port = 0;
    if (!text || !text[0]) return false;
    uint32_t value = 0;
    for (size_t i = 0; text[i]; i++) {
        if (i == 5 || text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + (unsigned)(text[i] - '0');
        if (value > 65535) return false;
    }
    if (!value) return false;
    *port = (uint16_t)value;
    return true;
}

bool wol_build_packet(const uint8_t mac[WOL_MAC_BYTES], uint8_t packet[WOL_PACKET_BYTES])
{
    if (!packet) return false;
    memset(packet, 0, WOL_PACKET_BYTES);
    if (!valid_mac(mac)) return false;
    /* Intel Ethernet documentation: six FF bytes, then sixteen MAC copies.
     * https://cdrdv2-public.intel.com/705253/ug_ethernet-19-2-0-683402-705253.pdf */
    memset(packet, 0xff, 6);
    for (size_t i = 0; i < 16; i++) memcpy(packet + 6 + i * WOL_MAC_BYTES, mac, WOL_MAC_BYTES);
    return true;
}

void wol_data_self_test(void)
{
#ifndef NDEBUG
    uint8_t mac[WOL_MAC_BYTES], packet[WOL_PACKET_BYTES];
    assert(wol_parse_mac("02:11:22:33:44:55", mac));
    assert(wol_build_packet(mac, packet));
    assert(packet[0] == 0xff && packet[5] == 0xff);
    assert(memcmp(packet + 6, mac, WOL_MAC_BYTES) == 0);
    assert(memcmp(packet + 96, mac, WOL_MAC_BYTES) == 0);
    assert(!wol_parse_mac("FF:FF:FF:FF:FF:FF", mac));
#endif
}
