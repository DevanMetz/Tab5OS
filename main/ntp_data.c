#include "ntp_data.h"

#include <assert.h>
#include <string.h>

/* RFC 5905 sections 6, 7.3, 7.4, 8: https://www.rfc-editor.org/rfc/rfc5905.html
 * This inspector measures four exchanges; it does not discipline a clock. */
#define NTP_UNIX_OFFSET INT64_C(2208988800)
#define NTP_ERA_SECONDS INT64_C(4294967296)

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

bool ntp_clock_sane(int64_t unix_us)
{
    return unix_us >= NTP_MIN_UNIX_US && unix_us < NTP_MAX_UNIX_US;
}

bool ntp_parse_port(const char *text, uint16_t *port)
{
    if (!port) return false;
    *port = 0;
    if (!text || !*text) return false;
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

bool ntp_build_request(int64_t unix_us, uint8_t packet[NTP_PACKET_BYTES])
{
    if (!packet) return false;
    memset(packet, 0, NTP_PACKET_BYTES);
    if (!ntp_clock_sane(unix_us)) return false;
    packet[0] = 0x23; /* Version 4, client mode 3. */
    packet[2] = 4;    /* Nominal poll exponent; this is a bounded diagnostic. */
    packet[3] = (uint8_t)-20; /* Microsecond timestamp representation. */
    uint64_t seconds = (uint64_t)(unix_us / 1000000 + NTP_UNIX_OFFSET);
    uint64_t fraction = ((uint64_t)(unix_us % 1000000) << 32) / 1000000;
    write_u32(packet + 40, (uint32_t)seconds);
    write_u32(packet + 44, (uint32_t)fraction);
    return true;
}

static bool decode_timestamp(const uint8_t *bytes, int64_t pivot_us, int64_t *unix_us)
{
    uint32_t seconds = read_u32(bytes);
    uint32_t fraction = read_u32(bytes + 4);
    if (!seconds && !fraction) return false; /* Undefined, including at an era boundary. */
    int64_t pivot = pivot_us / 1000000 + NTP_UNIX_OFFSET;
    int64_t full = (pivot / NTP_ERA_SECONDS) * NTP_ERA_SECONDS + seconds;
    int64_t difference = full - pivot;
    if (difference == NTP_ERA_SECONDS / 2 || difference == -NTP_ERA_SECONDS / 2) return false;
    if (difference > NTP_ERA_SECONDS / 2) full -= NTP_ERA_SECONDS;
    else if (difference < -NTP_ERA_SECONDS / 2) full += NTP_ERA_SECONDS;
    int64_t micros = (int64_t)(((uint64_t)fraction * 1000000 + UINT64_C(2147483648)) >> 32);
    *unix_us = (full - NTP_UNIX_OFFSET) * 1000000 + micros;
    return ntp_clock_sane(*unix_us);
}

ntp_status_t ntp_parse_response(const uint8_t *packet, size_t length,
                               const uint8_t request[NTP_PACKET_BYTES],
                               int64_t sent_unix_us, int64_t received_unix_us,
                               int64_t monotonic_rtt_us, ntp_result_t *result)
{
    if (!result) return NTP_DATA_BAD_HEADER;
    memset(result, 0, sizeof(*result));
    if (!ntp_clock_sane(sent_unix_us) || !ntp_clock_sane(received_unix_us) ||
        received_unix_us < sent_unix_us || monotonic_rtt_us < 0 || monotonic_rtt_us > 2000000)
        return NTP_DATA_BAD_CLOCK;
    int64_t elapsed = received_unix_us - sent_unix_us;
    int64_t drift = elapsed - monotonic_rtt_us;
    if (drift < -5000 || drift > 5000) return NTP_DATA_BAD_CLOCK;
    if (!packet || length != NTP_PACKET_BYTES) return NTP_DATA_BAD_LENGTH;
    if (!request) return NTP_DATA_BAD_HEADER;
    uint8_t version = (packet[0] >> 3) & 7;
    if ((version != 3 && version != 4) || (packet[0] & 7) != 4 || packet[1] > 15)
        return NTP_DATA_BAD_HEADER;
    if (memcmp(packet + 24, request + 40, 8)) return NTP_DATA_WRONG_ORIGIN;
    ntp_result_t parsed = {0};
    parsed.version = version;
    parsed.leap = packet[0] >> 6;
    parsed.stratum = packet[1];
    memcpy(parsed.reference_id, packet + 12, 4);
    /* KoD timestamps may be undefined, and LI may say unsynchronized. */
    if (!parsed.stratum || parsed.leap == 3) {
        *result = parsed;
        return !parsed.stratum ? NTP_DATA_KISS_OF_DEATH : NTP_DATA_UNSYNCHRONIZED;
    }
    int64_t received_server, sent_server;
    if (!decode_timestamp(packet + 32, sent_unix_us, &received_server) ||
        !decode_timestamp(packet + 40, sent_unix_us, &sent_server) || sent_server < received_server)
        return NTP_DATA_BAD_TIMESTAMPS;
    int64_t delay = elapsed - (sent_server - received_server);
    if (delay < -1000) return NTP_DATA_BAD_TIMESTAMPS;
    /* RFC 5905 permits small negative delays from clock-rate differences.
     * Clamp at 1us and disclose this instead of displaying negative latency. */
    parsed.delay_clamped = delay < 1;
    if (delay < 1) delay = 1;
    parsed.round_trip_ms = (double)monotonic_rtt_us / 1000;
    parsed.delay_ms = (double)delay / 1000;
    parsed.offset_ms = ((double)(received_server - sent_unix_us) +
                        (double)(sent_server - received_unix_us)) / 2000;
    *result = parsed;
    return NTP_DATA_OK;
}

const char *ntp_status_text(ntp_status_t status)
{
    switch (status) {
    case NTP_DATA_OK: return "OK";
    case NTP_DATA_BAD_CLOCK: return "Local clock invalid or changed during sample";
    case NTP_DATA_BAD_LENGTH: return "Unsupported reply length (plain 48-byte NTP only)";
    case NTP_DATA_BAD_HEADER: return "Invalid version, mode or stratum";
    case NTP_DATA_WRONG_ORIGIN: return "Reply does not match this request";
    case NTP_DATA_KISS_OF_DEATH: return "Server refused requests (KoD)";
    case NTP_DATA_UNSYNCHRONIZED: return "Server clock is unsynchronized (LI 3)";
    case NTP_DATA_BAD_TIMESTAMPS: return "Inconsistent or unsupported server timestamps";
    }
    return "Invalid NTP reply";
}

void ntp_data_self_test(void)
{
#ifndef NDEBUG
    const int64_t start = INT64_C(1800000000000000);
    uint8_t request[NTP_PACKET_BYTES], reply[NTP_PACKET_BYTES], stamp[NTP_PACKET_BYTES];
    assert(ntp_build_request(start, request));
    memset(reply, 0, sizeof(reply));
    reply[0] = 0x24;
    reply[1] = 2;
    memcpy(reply + 24, request + 40, 8);
    assert(ntp_build_request(start + 60000, stamp));
    memcpy(reply + 32, stamp + 40, 8);
    assert(ntp_build_request(start + 70000, stamp));
    memcpy(reply + 40, stamp + 40, 8);
    ntp_result_t result;
    assert(ntp_parse_response(reply, sizeof(reply), request, start, start + 100000, 100000, &result) == NTP_DATA_OK);
    assert(result.delay_ms == 90 && result.offset_ms == 15);
    reply[0] = 0xe4;
    assert(ntp_parse_response(reply, sizeof(reply), request, start, start + 100000, 100000, &result) == NTP_DATA_UNSYNCHRONIZED);
#endif
}
