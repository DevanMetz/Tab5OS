#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NTP_PACKET_BYTES 48
#define NTP_SAMPLE_COUNT 4
#define NTP_MIN_UNIX_US INT64_C(1577836800000000)
#define NTP_MAX_UNIX_US INT64_C(4102444800000000)

typedef enum {
    NTP_DATA_OK,
    NTP_DATA_BAD_CLOCK,
    NTP_DATA_BAD_LENGTH,
    NTP_DATA_BAD_HEADER,
    NTP_DATA_WRONG_ORIGIN,
    NTP_DATA_KISS_OF_DEATH,
    NTP_DATA_UNSYNCHRONIZED,
    NTP_DATA_BAD_TIMESTAMPS,
} ntp_status_t;

typedef struct {
    double round_trip_ms;
    double delay_ms;
    double offset_ms;
    uint8_t version;
    uint8_t leap;
    uint8_t stratum;
    uint8_t reference_id[4];
    bool delay_clamped;
} ntp_result_t;

bool ntp_clock_sane(int64_t unix_us);
bool ntp_parse_port(const char *text, uint16_t *port);
/* Client mode 3, version 4; transmit timestamp is the response correlation token. */
bool ntp_build_request(int64_t unix_us, uint8_t packet[NTP_PACKET_BYTES]);
/* Plain 48-byte replies only. Timestamps are Unix microseconds; monotonic RTT
 * detects a local clock step (>5ms). Era is selected nearest to client time;
 * client and server dates must be 2020..2099 and within 68 years of one another.
 * Metadata survives only OK/KoD/unsynchronized; errors never retain old math. */
ntp_status_t ntp_parse_response(const uint8_t *packet, size_t length,
                               const uint8_t request[NTP_PACKET_BYTES],
                               int64_t sent_unix_us, int64_t received_unix_us,
                               int64_t monotonic_rtt_us, ntp_result_t *result);
const char *ntp_status_text(ntp_status_t status);
void ntp_data_self_test(void);
