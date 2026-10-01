#include "subnet_data.h"

#include <assert.h>
#include <string.h>

static subnet_status_t parse_mask(const char *text, uint32_t *mask, unsigned *prefix)
{
    bool dotted = false;
    for (unsigned i = 0; text[i]; i++) {
        if (i == SUBNET_INPUT_MAX) return SUBNET_BAD_MASK;
        if (text[i] == '.') dotted = true;
    }
    if (dotted) {
        if (!ipv4_parse(text, mask)) return SUBNET_BAD_MASK;
        uint32_t inverse = ~*mask;
        if ((inverse & (inverse + UINT32_C(1))) != 0) return SUBNET_NONCONTIGUOUS_MASK;
        *prefix = 0;
        for (uint32_t bits = *mask; bits; bits <<= 1) (*prefix)++;
    } else {
        if (*text == '/') text++;
        if (*text < '0' || *text > '9') return SUBNET_BAD_MASK;
        unsigned value = (unsigned)(*text++ - '0');
        if (*text >= '0' && *text <= '9') {
            if (value == 0) return SUBNET_BAD_MASK;
            value = value * 10 + (unsigned)(*text++ - '0');
        }
        if (*text || value > 32) return SUBNET_BAD_MASK;
        *prefix = value;
        /* Shifting a 32-bit value by 32 is undefined, so /0 is explicit. */
        *mask = value == 0 ? 0 : UINT32_MAX << (32 - value);
    }
    return SUBNET_OK;
}

static subnet_role_t address_role(const subnet_data_t *data, uint32_t address)
{
    if ((address & data->mask) != data->network) return SUBNET_OUTSIDE;
    if (data->has_broadcast) {
        if (address == data->network) return SUBNET_NETWORK_ADDRESS;
        if (address == data->last_address) return SUBNET_BROADCAST_ADDRESS;
    }
    return SUBNET_HOST;
}

subnet_status_t subnet_calculate(const char *address, const char *mask,
                                 const char *peer, subnet_data_t *result)
{
    if (!result) return SUBNET_BAD_ARGUMENT;
    memset(result, 0, sizeof(*result));
    if (!address || !mask || !peer) return SUBNET_BAD_ARGUMENT;

    subnet_data_t parsed = {0};
    if (!ipv4_parse(address, &parsed.address)) return SUBNET_BAD_ADDRESS;
    subnet_status_t status = parse_mask(mask, &parsed.mask, &parsed.prefix);
    if (status != SUBNET_OK) return status;
    parsed.has_peer = *peer != '\0';
    if (parsed.has_peer && !ipv4_parse(peer, &parsed.peer)) return SUBNET_BAD_PEER;

    parsed.wildcard = ~parsed.mask;
    parsed.network = parsed.address & parsed.mask;
    parsed.last_address = parsed.network | parsed.wildcard;
    parsed.address_count = UINT64_C(1) << (32 - parsed.prefix);
    parsed.has_broadcast = parsed.prefix <= 30;
    parsed.host_count = parsed.address_count - (parsed.has_broadcast ? 2 : 0);
    parsed.first_host = parsed.network + (parsed.has_broadcast ? 1 : 0);
    parsed.last_host = parsed.last_address - (parsed.has_broadcast ? 1 : 0);
    parsed.address_role = address_role(&parsed, parsed.address);
    parsed.peer_role = parsed.has_peer ? address_role(&parsed, parsed.peer) : SUBNET_OUTSIDE;
    *result = parsed;
    return SUBNET_OK;
}

void subnet_format_ipv4(uint32_t address, char text[SUBNET_IPV4_TEXT_SIZE])
{
    ipv4_format(address, text);
}

const char *subnet_error(subnet_status_t status)
{
    switch (status) {
    case SUBNET_OK: return "";
    case SUBNET_BAD_ADDRESS: return "IPv4 address: use four decimal octets (0-255), without spaces or leading zeros.";
    case SUBNET_BAD_MASK: return "Mask: enter 0-32, /0-/32, or a dotted netmask such as 255.255.255.0.";
    case SUBNET_NONCONTIGUOUS_MASK: return "Netmask bits must be contiguous: all 1s followed by all 0s. Wildcard masks are not input netmasks.";
    case SUBNET_BAD_PEER: return "Peer: enter a dotted IPv4 address without spaces or leading zeros, or leave it empty.";
    case SUBNET_BAD_ARGUMENT: return "Invalid calculator input.";
    }
    return "Invalid calculator input.";
}

void subnet_data_self_test(void)
{
#ifndef NDEBUG
    subnet_data_t data;
    assert(subnet_calculate("192.168.1.42", "24", "192.168.1.255", &data) == SUBNET_OK);
    assert(data.network == UINT32_C(0xc0a80100) && data.host_count == 254);
    assert(data.peer_role == SUBNET_BROADCAST_ADDRESS);
    assert(subnet_calculate("192.0.2.1", "/31", "192.0.2.0", &data) == SUBNET_OK);
    assert(!data.has_broadcast && data.host_count == 2 && data.peer_role == SUBNET_HOST);
    assert(subnet_calculate("0.0.0.0", "0", "", &data) == SUBNET_OK);
    assert(data.address_count == UINT64_C(4294967296));
    assert(subnet_calculate("192.0.2.1", "255.0.255.0", "", &data) == SUBNET_NONCONTIGUOUS_MASK);
    assert(data.host_count == 0 && data.address == 0);
#endif
}
