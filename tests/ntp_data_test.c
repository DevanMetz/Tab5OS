#include "ntp_data.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void timestamp(uint8_t *output, int64_t unix_us)
{
    uint8_t packet[NTP_PACKET_BYTES];
    assert(ntp_build_request(unix_us, packet));
    memcpy(output, packet + 40, 8);
}

static void response(uint8_t packet[NTP_PACKET_BYTES], const uint8_t *request,
                     int64_t server_receive, int64_t server_send)
{
    memset(packet, 0, NTP_PACKET_BYTES);
    packet[0] = 0x24;
    packet[1] = 2;
    memcpy(packet + 12, "GPS\0", 4);
    memcpy(packet + 24, request + 40, 8);
    timestamp(packet + 32, server_receive);
    timestamp(packet + 40, server_send);
}

static void near(double value, double expected)
{
    assert(fabs(value - expected) < 0.001);
}

int main(void)
{
    ntp_data_self_test();
    uint16_t port = 99;
    assert(ntp_parse_port("123", &port) && port == 123);
    assert(ntp_parse_port("65535", &port) && port == 65535);
    assert(ntp_parse_port("00001", &port) && port == 1);
    const char *bad_ports[] = {"", "0", "65536", "000001", " 1", "+1", "-1", "1.0", "1x"};
    for (size_t i = 0; i < sizeof(bad_ports) / sizeof(bad_ports[0]); i++) {
        assert(!ntp_parse_port(bad_ports[i], &port) && port == 0);
    }
    assert(!ntp_parse_port(NULL, &port));
    assert(!ntp_parse_port("123", NULL));
    assert(ntp_clock_sane(NTP_MIN_UNIX_US));
    assert(ntp_clock_sane(NTP_MAX_UNIX_US - 1));
    assert(!ntp_clock_sane(NTP_MIN_UNIX_US - 1));
    assert(!ntp_clock_sane(NTP_MAX_UNIX_US));

    uint8_t request[NTP_PACKET_BYTES], packet[NTP_PACKET_BYTES + 1];
    const int64_t start = INT64_C(1800000000123456);
    assert(!ntp_build_request(start, NULL));
    memset(request, 0xff, sizeof(request));
    assert(!ntp_build_request(0, request));
    for (size_t i = 0; i < sizeof(request); i++) assert(request[i] == 0);
    assert(ntp_build_request(start, request));
    assert(request[0] == 0x23);
    for (size_t i = 4; i < 40; i++) assert(request[i] == 0);
    response(packet, request, start + 60000, start + 70000);
    ntp_result_t result;
#define PARSE() ntp_parse_response(packet, NTP_PACKET_BYTES, request, start, start + 100000, 100000, &result)
    assert(PARSE() == NTP_DATA_OK);
    near(result.round_trip_ms, 100);
    near(result.delay_ms, 90);
    near(result.offset_ms, 15);
    assert(result.stratum == 2 && result.version == 4 && result.leap == 0);
    assert(memcmp(result.reference_id, "GPS\0", 4) == 0 && !result.delay_clamped);
    packet[0] = 0x9c; /* NTPv3, leap warning2, mode4. */
    assert(PARSE() == NTP_DATA_OK && result.version == 3 && result.leap == 2);
    packet[0] = 0x24;
    assert(ntp_parse_response(packet, 47, request, start, start + 100000, 100000, &result) == NTP_DATA_BAD_LENGTH);
    assert(result.offset_ms == 0 && result.stratum == 0);
    assert(ntp_parse_response(packet, 49, request, start, start + 100000, 100000, &result) == NTP_DATA_BAD_LENGTH);
    assert(ntp_parse_response(NULL, 48, request, start, start + 100000, 100000, &result) == NTP_DATA_BAD_LENGTH);
    assert(ntp_parse_response(packet, 48, NULL, start, start + 100000, 100000, &result) == NTP_DATA_BAD_HEADER);
    assert(ntp_parse_response(packet, 48, request, start, start + 100000, 100000, NULL) == NTP_DATA_BAD_HEADER);
    packet[0] = 0x23;
    assert(PARSE() == NTP_DATA_BAD_HEADER);
    packet[0] = 0x14;
    assert(PARSE() == NTP_DATA_BAD_HEADER);
    packet[0] = 0x24;
    packet[1] = 16;
    assert(PARSE() == NTP_DATA_BAD_HEADER);
    packet[1] = 2;
    packet[24] ^= 1;
    assert(PARSE() == NTP_DATA_WRONG_ORIGIN);
    packet[24] ^= 1;
    packet[0] = 0xe4;
    assert(PARSE() == NTP_DATA_UNSYNCHRONIZED && result.leap == 3 && result.offset_ms == 0);
    packet[1] = 0;
    memset(packet + 32, 0, 16);
    memcpy(packet + 12, "RATE", 4);
    assert(PARSE() == NTP_DATA_KISS_OF_DEATH && memcmp(result.reference_id, "RATE", 4) == 0);
    packet[24] ^= 1;
    assert(PARSE() == NTP_DATA_WRONG_ORIGIN);

    response(packet, request, start + 80000, start + 70000);
    assert(PARSE() == NTP_DATA_BAD_TIMESTAMPS);
    response(packet, request, start, start + 102000);
    assert(PARSE() == NTP_DATA_BAD_TIMESTAMPS);
    response(packet, request, start, start + 100500);
    assert(PARSE() == NTP_DATA_OK && result.delay_clamped);
    near(result.delay_ms, 0.001);
    response(packet, request, start + 10000, start + 20000);
    assert(PARSE() == NTP_DATA_OK);
    near(result.offset_ms, -35);
    memset(packet + 32, 0, 8);
    assert(PARSE() == NTP_DATA_BAD_TIMESTAMPS);
    response(packet, request, start + 60000, start + 70000);
    assert(ntp_parse_response(packet, 48, request, 0, start + 100000, 100000, &result) == NTP_DATA_BAD_CLOCK);
    assert(ntp_parse_response(packet, 48, request, start, start - 1, 100000, &result) == NTP_DATA_BAD_CLOCK);
    assert(ntp_parse_response(packet, 48, request, start, start + 100000, -1, &result) == NTP_DATA_BAD_CLOCK);
    assert(ntp_parse_response(packet, 48, request, start, start + 100000, 2000001, &result) == NTP_DATA_BAD_CLOCK);
    assert(ntp_parse_response(packet, 48, request, start, start + 110000, 100000, &result) == NTP_DATA_BAD_CLOCK);

    const int64_t rollover = INT64_C(2085978496000000);
    assert(ntp_build_request(rollover - 50000, request));
    response(packet, request, rollover - 30000, rollover + 10000);
    assert(ntp_parse_response(packet, 48, request, rollover - 50000, rollover + 50000, 100000, &result) == NTP_DATA_OK);
    near(result.delay_ms, 60);
    near(result.offset_ms, -10);
    assert(ntp_build_request(rollover + 500000, request));
    response(packet, request, rollover + 510000, rollover + 520000);
    assert(ntp_parse_response(packet, 48, request, rollover + 500000, rollover + 600000, 100000, &result) == NTP_DATA_OK);
    near(result.offset_ms, -35);
    assert(ntp_build_request(NTP_MAX_UNIX_US - 1000000, request));
    response(packet, request, NTP_MIN_UNIX_US, NTP_MIN_UNIX_US + 10000);
    assert(ntp_parse_response(packet, 48, request, NTP_MAX_UNIX_US - 1000000,
                              NTP_MAX_UNIX_US - 900000, 100000, &result) == NTP_DATA_BAD_TIMESTAMPS);
    puts("NTP data tests passed");
    return 0;
}
