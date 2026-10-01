#include "subnet_data.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void expect_address(uint32_t address, const char *expected)
{
    char text[SUBNET_IPV4_TEXT_SIZE];
    subnet_format_ipv4(address, text);
    assert(strcmp(text, expected) == 0);
}

static void expect_error(const char *address, const char *mask, const char *peer,
                         subnet_status_t expected)
{
    subnet_data_t data;
    memset(&data, 0xa5, sizeof(data));
    assert(subnet_calculate(address, mask, peer, &data) == expected);
    const unsigned char *bytes = (const unsigned char *)&data;
    for (size_t i = 0; i < sizeof(data); i++) assert(bytes[i] == 0);
    assert(*subnet_error(expected));
}

/* Used by subnet_reference_test.py to compare the real C implementation with
 * Python's independent ipaddress implementation across every IPv4 prefix. */
static void print_vectors(void)
{
    uint32_t seed = UINT32_C(0x936dfa12);
    for (unsigned prefix = 0; prefix <= 32; prefix++) {
        for (unsigned i = 0; i < 64; i++) {
            seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
            uint32_t value = i == 0 ? 0 : i == 1 ? UINT32_MAX : seed;
            char address[16], mask[16], peer[16];
            subnet_format_ipv4(value, address);
            snprintf(mask, sizeof(mask), "%u", prefix);
            /* Alternate a neighbouring address and a different network. */
            subnet_format_ipv4(value ^ (i % 2 ? 1 : UINT32_C(0x12345678)), peer);
            subnet_data_t data;
            assert(subnet_calculate(address, mask, peer, &data) == SUBNET_OK);
            printf("%s/%u,%s,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
                   ",%" PRIu32 ",%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%u,%u,%u\n",
                   address, prefix, peer, data.network, data.last_address, data.mask,
                   data.wildcard, data.first_host, data.last_host, data.address_count,
                   data.host_count, data.has_broadcast, (unsigned)data.address_role,
                   (unsigned)data.peer_role);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--vectors") == 0) {
        print_vectors();
        return 0;
    }
    subnet_data_self_test();
    subnet_data_t data;
    assert(subnet_calculate("192.168.1.42", "24", "192.168.1.100", &data) == SUBNET_OK);
    expect_address(data.network, "192.168.1.0");
    expect_address(data.mask, "255.255.255.0");
    expect_address(data.wildcard, "0.0.0.255");
    expect_address(data.last_address, "192.168.1.255");
    expect_address(data.first_host, "192.168.1.1");
    expect_address(data.last_host, "192.168.1.254");
    assert(data.address_count == 256 && data.host_count == 254 && data.prefix == 24);
    assert(data.has_broadcast && data.has_peer);
    assert(data.address_role == SUBNET_HOST && data.peer_role == SUBNET_HOST);

    assert(subnet_calculate("10.20.30.40", "255.255.240.0", "10.20.32.1", &data) == SUBNET_OK);
    expect_address(data.network, "10.20.16.0");
    expect_address(data.last_address, "10.20.31.255");
    expect_address(data.first_host, "10.20.16.1");
    expect_address(data.last_host, "10.20.31.254");
    assert(data.prefix == 20 && data.host_count == 4094 && data.peer_role == SUBNET_OUTSIDE);

    assert(subnet_calculate("192.0.2.0", "/30", "192.0.2.3", &data) == SUBNET_OK);
    assert(data.host_count == 2 && data.address_role == SUBNET_NETWORK_ADDRESS);
    assert(data.peer_role == SUBNET_BROADCAST_ADDRESS);
    assert(subnet_calculate("192.0.2.3", "30", "192.0.2.0", &data) == SUBNET_OK);
    assert(data.address_role == SUBNET_BROADCAST_ADDRESS && data.peer_role == SUBNET_NETWORK_ADDRESS);

    assert(subnet_calculate("192.0.2.1", "255.255.255.254", "192.0.2.0", &data) == SUBNET_OK);
    expect_address(data.first_host, "192.0.2.0");
    expect_address(data.last_host, "192.0.2.1");
    assert(data.prefix == 31 && data.host_count == 2 && !data.has_broadcast);
    assert(data.address_role == SUBNET_HOST && data.peer_role == SUBNET_HOST);
    assert(subnet_calculate("192.0.2.1", "31", "192.0.2.2", &data) == SUBNET_OK);
    assert(data.peer_role == SUBNET_OUTSIDE);

    assert(subnet_calculate("255.255.255.255", "/32", "255.255.255.255", &data) == SUBNET_OK);
    assert(data.network == UINT32_MAX && data.first_host == UINT32_MAX && data.last_host == UINT32_MAX);
    assert(data.address_count == 1 && data.host_count == 1 && !data.has_broadcast);
    assert(data.address_role == SUBNET_HOST && data.peer_role == SUBNET_HOST);
    assert(subnet_calculate("0.0.0.0", "255.255.255.255", "0.0.0.1", &data) == SUBNET_OK);
    assert(data.first_host == 0 && data.last_host == 0 && data.peer_role == SUBNET_OUTSIDE);

    assert(subnet_calculate("203.0.113.9", "0.0.0.0", "255.255.255.255", &data) == SUBNET_OK);
    assert(data.network == 0 && data.mask == 0 && data.wildcard == UINT32_MAX);
    assert(data.address_count == UINT64_C(4294967296) && data.host_count == UINT64_C(4294967294));
    assert(data.first_host == 1 && data.last_host == UINT32_MAX - 1);
    assert(data.peer_role == SUBNET_BROADCAST_ADDRESS);
    assert(subnet_calculate("192.0.2.0", "24", "", &data) == SUBNET_OK && !data.has_peer);

    /* Round-trip all contiguous netmasks, including both zero-width endpoints. */
    uint32_t mask_value = 0;
    for (unsigned prefix = 0; prefix <= 32; prefix++) {
        char mask[16];
        subnet_format_ipv4(mask_value, mask);
        assert(subnet_calculate("198.51.100.219", mask, "", &data) == SUBNET_OK);
        assert(data.prefix == prefix && data.mask == mask_value);
        if (prefix < 32) mask_value |= UINT32_C(1) << (31 - prefix);
    }
    static const char *const bad_addresses[] = {
        "", " ", "1.2.3", "1.2.3.4.5", ".1.2.3", "1..2.3", "1.2.3.",
        "256.0.0.1", "1.2.3.256", "99999999999999999999999999999999999",
        "192.168.001.1", "01.2.3.4", "00.0.0.0", "+1.2.3.4", "-1.2.3.4",
        "1.2.3.4 ", " 1.2.3.4", "1.2.3.4\n", "1.2.3.4/24", "0xc0.168.1.1",
        "localhost", "::1", "1.2.3.\xc2\xb9", "1.2.3.4:80", "1.2.3.4%eth0"
    };
    for (size_t i = 0; i < sizeof(bad_addresses) / sizeof(bad_addresses[0]); i++) {
        expect_error(bad_addresses[i], "24", "", SUBNET_BAD_ADDRESS);
        if (*bad_addresses[i]) expect_error("192.0.2.1", "24", bad_addresses[i], SUBNET_BAD_PEER);
    }
    static const char *const bad_masks[] = {
        "", " ", "/", "33", "/33", "100", "-1", "+24", "024", "/00", "00",
        "24 ", " 24", "24\n", "24/", "//24", "24.0", "255.255.255.256",
        "255.255.255.00", "/255.255.255.0", "255.255.255", "0xffffff00",
        "333333333333333333333333333333333", "\xc2\xb2\xc2\xb4"
    };
    for (size_t i = 0; i < sizeof(bad_masks) / sizeof(bad_masks[0]); i++)
        expect_error("192.0.2.1", bad_masks[i], "", SUBNET_BAD_MASK);
    static const char *const noncontiguous[] = {
        "255.0.255.0", "255.255.255.1", "0.0.0.255", "127.255.255.255",
        "254.255.255.255", "255.255.254.128"
    };
    for (size_t i = 0; i < sizeof(noncontiguous) / sizeof(noncontiguous[0]); i++)
        expect_error("192.0.2.1", noncontiguous[i], "", SUBNET_NONCONTIGUOUS_MASK);
    expect_error(NULL, "24", "", SUBNET_BAD_ARGUMENT);
    expect_error("192.0.2.1", NULL, "", SUBNET_BAD_ARGUMENT);
    expect_error("192.0.2.1", "24", NULL, SUBNET_BAD_ARGUMENT);
    assert(subnet_calculate("192.0.2.1", "24", "", NULL) == SUBNET_BAD_ARGUMENT);
    assert(!*subnet_error(SUBNET_OK));
    puts("Subnet parser/math: known ranges, all 33 masks, roles, invalid input, cleared errors PASS");
    return 0;
}
