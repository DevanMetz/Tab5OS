#include "wol_data.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    wol_data_self_test();
    uint8_t mac[WOL_MAC_BYTES];
    const uint8_t expected[] = {0x02, 0xab, 0xcd, 0x12, 0x34, 0xef};
    const char *valid[] = {"02:ab:CD:12:34:eF", "02-AB-CD-12-34-EF", "02abcd1234ef"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        assert(wol_parse_mac(valid[i], mac));
        assert(memcmp(mac, expected, sizeof(mac)) == 0);
    }
    const char *invalid[] = {NULL, "", "02:ab:cd:12:34", "02:ab:cd:12:34:ef:00", "02:ab-cd:12:34:ef",
        "02.ab.cd.12.34.ef", "02 ab cd 12 34 ef", "02:ab:cd:12:34:eg", "0:ab:cd:12:34:ef",
        " 02:ab:cd:12:34:ef", "02:ab:cd:12:34:ef ", "02abcd1234ef\n", "02abcd1234ef00",
        "00:00:00:00:00:00", "FF:FF:FF:FF:FF:FF", "01:00:5e:00:00:01", "33:33:00:00:00:01"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        memset(mac, 0xff, sizeof(mac));
        assert(!wol_parse_mac(invalid[i], mac));
        for (size_t j = 0; j < sizeof(mac); j++) assert(mac[j] == 0);
    }
    assert(!wol_parse_mac(valid[0], NULL));
    assert(wol_parse_mac("00:00:00:00:00:01", mac));
    assert(wol_parse_mac(valid[0], mac));
    uint8_t packet[WOL_PACKET_BYTES + 2];
    memset(packet, 0x55, sizeof(packet));
    assert(wol_build_packet(mac, packet + 1));
    assert(packet[0] == 0x55 && packet[WOL_PACKET_BYTES + 1] == 0x55);
    for (size_t i = 1; i <= 6; i++) assert(packet[i] == 0xff);
    for (size_t i = 0; i < 16; i++) assert(memcmp(packet + 7 + i * 6, expected, 6) == 0);
    mac[0] |= 1;
    assert(!wol_build_packet(mac, packet));
    for (size_t i = 0; i < WOL_PACKET_BYTES; i++) assert(packet[i] == 0);
    assert(!wol_build_packet(NULL, packet));
    memset(mac, 0, sizeof(mac));
    assert(!wol_build_packet(mac, packet));
    assert(!wol_build_packet(mac, NULL));

    uint8_t ip[4];
    assert(wol_parse_ipv4("192.168.1.255", ip) && ip[0] == 192 && ip[3] == 255);
    assert(wol_parse_ipv4("255.255.255.255", ip) && ip[0] == 255 && ip[3] == 255);
    assert(wol_parse_ipv4("127.0.0.1", ip));
    const char *bad_ip[] = {NULL, "", "0.0.0.0", "0.1.2.3", "224.0.0.1", "240.1.2.3", "255.0.0.1",
        "192.168.1", "192.168.1.1.", "192.168.1.256", "192.168.-1.1", "192.168.01.1", "192.168.1.1 ",
        "192.168..1", "1.2.3.4x", "example.com", "1234.1.1.1"};
    for (size_t i = 0; i < sizeof(bad_ip) / sizeof(bad_ip[0]); i++) {
        memset(ip, 0xff, sizeof(ip));
        assert(!wol_parse_ipv4(bad_ip[i], ip));
        assert(ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0);
    }
    assert(!wol_parse_ipv4("1.2.3.4", NULL));
    uint16_t port;
    assert(wol_parse_port("9", &port) && port == 9);
    assert(wol_parse_port("65535", &port) && port == 65535);
    assert(wol_parse_port("00001", &port) && port == 1);
    const char *bad_port[] = {NULL, "", "0", "65536", "000009", "-1", "+9", " 9", "9 ", "9x", "9.0"};
    for (size_t i = 0; i < sizeof(bad_port) / sizeof(bad_port[0]); i++) {
        port = 123;
        assert(!wol_parse_port(bad_port[i], &port) && port == 0);
    }
    assert(!wol_parse_port("9", NULL));
    puts("wake-on-lan data tests passed");
    return 0;
}
