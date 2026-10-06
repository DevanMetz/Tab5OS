#include "network_ping.h"
#include "host.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

enum {
    GOOD, OPTIONS, SILENT, FLOOD, ZERO_FLOOD, AGAIN_FLOOD, LATE_REPLY, OLD_PAYLOAD,
    BAD_VERSION, BAD_HEADER, BAD_LENGTH, BAD_PROTOCOL, FRAGMENT, BAD_SOURCE,
    BAD_IP_CHECKSUM, BAD_CHECKSUM, BAD_TYPE, BAD_CODE, BAD_ID, BAD_SEQUENCE,
    BAD_PAYLOAD, SHORT_PACKET, EXTRA_BYTE, SOCKET_FAIL, GETFL_FAIL, SETFL_FAIL,
    SEND_FAIL, SHORT_SEND, SEND_AGAIN, SEND_INTERRUPTED, RECEIVE_FAIL,
    SELECT_FAIL, SELECT_INTERRUPTED, LATE_READY, SCHEDULER_DELAY
};
static int mode, family, opened, closed, writes, reads, send_calls, select_calls, interrupts;
static int64_t now;
static bool nonblocking;
static uint8_t echo[40], old_payload[32], peer_bytes[16];
static uint32_t random_state = 0x31415926;
static unsigned scenarios;
enum { BEFORE = 1, SETUP, WRITE_POLL, READ_POLL, SEND_RETRY, AFTER_SEND,
       AFTER_RECEIVE, GAP, NOISE, LAST_RECEIVE };
static int cancel_phase;
static bool cancel_signal;
static bool cancelled(void *context)
{
    assert(context == &mode);
    return cancel_signal;
}

/* Independent byte-wise checksum oracle, returning a network-order field. */
static uint16_t wire_checksum(const uint8_t *bytes, size_t length, uint32_t extra)
{
    uint32_t sum = extra;
    for (size_t i = 0; i < length; i++) sum += (uint32_t)bytes[i] << ((i & 1) ? 0 : 8);
    while (sum > 65535) sum = (sum & 65535) + sum / 65536;
    return (uint16_t)~sum;
}
static void put_checksum(uint8_t *bytes, size_t length, size_t offset, uint32_t extra)
{
    bytes[offset] = bytes[offset + 1] = 0;
    uint16_t value = wire_checksum(bytes, length, extra);
    bytes[offset] = (uint8_t)(value >> 8);
    bytes[offset + 1] = (uint8_t)value;
}
uint16_t inet_chksum(const void *bytes, uint16_t length)
{
    uint16_t value = wire_checksum(bytes, length, 0), result;
    uint8_t encoded[2] = {(uint8_t)(value >> 8), (uint8_t)value};
    memcpy(&result, encoded, 2);
    return result;
}
int64_t esp_timer_get_time(void) { return now; }
uint32_t esp_random(void) { random_state = random_state * 1664525U + 1013904223U; return random_state; }
void test_delay(unsigned ticks)
{
    assert(ticks > 0 && ticks <= 10);
    now += (int64_t)ticks * 1000;
    if ((cancel_phase == GAP && reads) || (cancel_phase == NOISE && reads == 3)) cancel_signal = true;
    if (mode == SCHEDULER_DELAY && writes == 1 && reads) now += 6000000;
}
int test_socket(int address_family, int type, int protocol)
{
    assert(!opened && type == SOCK_RAW && address_family == family);
    assert(protocol == (family == AF_INET ? IPPROTO_ICMP : IPPROTO_ICMPV6));
    if (mode == SOCKET_FAIL) { errno = ENOMEM; return -1; }
    opened = 1;
    return 7;
}
int test_fcntl(int fd, int command, int flags)
{
    assert(fd == 7 && opened && !closed);
    if ((mode == GETFL_FAIL && command == F_GETFL) || (mode == SETFL_FAIL && command == F_SETFL)) {
        errno = EACCES; return -1;
    }
    if (command == F_GETFL) {
        if (cancel_phase == SETUP) cancel_signal = true;
        return 0x400;
    }
    assert(command == F_SETFL && flags == (0x400 | O_NONBLOCK));
    nonblocking = true;
    return 0;
}
int test_select(int count, fd_set *readable, fd_set *writable, fd_set *errors, struct timeval *wait)
{
    assert(opened && !closed && nonblocking && count == 8 && !errors);
    assert((readable == NULL) != (writable == NULL));
    assert((readable ? readable : writable)->fd == 7);
    assert(wait->tv_sec == 0 && wait->tv_usec > 0 && wait->tv_usec <= 100000);
    assert(++select_calls < 2000);  // A regression must fail instead of hanging forever.
    if ((cancel_phase == WRITE_POLL && writable) || (cancel_phase == READ_POLL && readable)) cancel_signal = true;
    if (mode == LATE_READY) { now += 6000000; return 1; }
    if (mode == SELECT_FAIL) { errno = EIO; return -1; }
    if (mode == SELECT_INTERRUPTED && interrupts++ < 3) {
        now += 10000; errno = EINTR; return -1;
    }
    if (writable) { now += 100; return 1; }
    if (mode == SILENT) { now += wait->tv_usec; readable->fd = -1; return 0; }
    now += 1000;
    return 1;
}
ssize_t test_sendto(int fd, const void *bytes, size_t length, int flags,
                    const struct sockaddr *peer, socklen_t peer_length)
{
    assert(opened && !closed && nonblocking && fd == 7 && length == 40 && flags == 0);
    assert(++send_calls < 1000);
    if (cancel_phase == SEND_RETRY || cancel_phase == AFTER_SEND) cancel_signal = true;
    if (mode == SEND_FAIL) { errno = EIO; return -1; }
    if (mode == SHORT_SEND) { errno = EAGAIN; return 39; }
    if (mode == SEND_AGAIN) { errno = EAGAIN; return -1; }
    if (mode == SEND_INTERRUPTED && send_calls == 1) { errno = EINTR; return -1; }
    assert(peer->sa_family == family);
    if (family == AF_INET) {
        const struct sockaddr_in *address = (const struct sockaddr_in *)peer;
        assert(peer_length == sizeof(*address) && address->sin_port == 0);
        memcpy(peer_bytes, &address->sin_addr, 4);
        assert(wire_checksum(bytes, length, 0) == 0);
    } else {
        const struct sockaddr_in6 *address = (const struct sockaddr_in6 *)peer;
        assert(peer_length == sizeof(*address) && address->sin6_port == 0 && address->sin6_scope_id == 7);
        memcpy(peer_bytes, &address->sin6_addr, 16);
        assert(((const uint8_t *)bytes)[2] == 0 && ((const uint8_t *)bytes)[3] == 0);
    }
    writes++;
    memcpy(echo, bytes, 40);
    assert(echo[0] == (family == AF_INET ? 8 : 128) && echo[1] == 0);
    assert(echo[6] == 0 && echo[7] == writes && writes <= 4);
    return 40;
}
ssize_t test_recvfrom(int fd, void *bytes, size_t capacity, int flags,
                      struct sockaddr *peer, socklen_t *peer_length)
{
    assert(opened && !closed && nonblocking && fd == 7 && capacity >= 100 && !flags && !peer && !peer_length);
    assert(++reads < 1000);
    if (mode == RECEIVE_FAIL) { errno = EIO; return -1; }
    if (mode == AGAIN_FLOOD) { errno = EAGAIN; return -1; }
    if (mode == ZERO_FLOOD) return 0;
    uint8_t packet[128] = {0};
    size_t offset = family == AF_INET ? (mode == OPTIONS ? 60 : 20) : 40;
    size_t length = offset + 40;
    if (family == AF_INET) {
        packet[0] = (uint8_t)(0x40 | (offset / 4));
        packet[2] = 0; packet[3] = (uint8_t)length;
        packet[8] = 64; packet[9] = 1;
        memcpy(packet + 12, peer_bytes, 4);
        memcpy(packet + 16, (uint8_t[]){192, 0, 2, 9}, 4);
    } else {
        packet[0] = 0x60; packet[5] = 40; packet[6] = 58; packet[7] = 64;
        memcpy(packet + 8, peer_bytes, 16);
        memcpy(packet + 24, (uint8_t[]){0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9}, 16);
    }
    memcpy(packet + offset, echo, 40);
    packet[offset] = family == AF_INET ? 0 : 129;
    switch (mode) {
        case BAD_VERSION: packet[0] ^= 0x10; break;
        case BAD_HEADER: packet[0] = 0x41; break;
        case BAD_LENGTH: packet[family == AF_INET ? 3 : 5]--; break;
        case BAD_PROTOCOL: packet[family == AF_INET ? 9 : 6] = 17; break;
        case FRAGMENT: packet[6] = 0x20; break;
        case BAD_SOURCE: packet[family == AF_INET ? 12 : 8] ^= 1; break;
        case BAD_TYPE: packet[offset] = 3; break;
        case BAD_CODE: packet[offset + 1] = 1; break;
        case BAD_ID: packet[offset + 4] ^= 1; break;
        case BAD_SEQUENCE: case FLOOD: packet[offset + 7] ^= 1; break;
        case BAD_PAYLOAD: packet[offset + 39] ^= 1; break;
        case OLD_PAYLOAD: memcpy(packet + offset + 8, old_payload, 32); break;
        default: break;
    }
    uint32_t pseudo = 0;
    if (family != AF_INET) {
        pseudo = 40 + 58;
        for (unsigned i = 8; i < 40; i += 2) pseudo += ((uint32_t)packet[i] << 8) + packet[i + 1];
    }
    put_checksum(packet + offset, 40, 2, pseudo);
    if (family == AF_INET) put_checksum(packet, offset, 10, 0);
    if (mode == BAD_IP_CHECKSUM) packet[10] ^= 1;
    if (mode == BAD_CHECKSUM) packet[offset + 2] ^= 1;
    if (mode == SHORT_PACKET) length--;
    if (mode == EXTRA_BYTE) length++;
    if (mode == LATE_REPLY) now += 1000000;
    memcpy(bytes, packet, length);
    if (cancel_phase == AFTER_RECEIVE || (cancel_phase == LAST_RECEIVE && writes == 4)) cancel_signal = true;
    return (ssize_t)length;
}
int test_close(int fd)
{
    assert(fd == 7 && opened && !closed);
    closed++;
    return 0;
}

static void check(int scenario, int address_family, int expected_error, unsigned sent, unsigned received)
{
    mode = scenario; family = address_family;
    opened = closed = writes = reads = send_calls = select_calls = interrupts = 0;
    now = 0; nonblocking = false; errno = 0;
    cancel_signal = cancel_phase == BEFORE;
    ip_addr_t target = {.family = family, .v4 = {{192, 0, 2, 1}},
                        .v6 = {{0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 7}};
    uint32_t actual_sent = 123, actual_received = 123, total = 123;
    int error = network_ping_target(&target, &actual_sent, &actual_received, &total,
                                     cancel_phase ? cancelled : NULL, &mode);
    if (error != expected_error || actual_sent != sent || actual_received != received) {
        fprintf(stderr, "scenario=%d family=%d error=%d sent=%u received=%u time=%lld\n",
                scenario, family, error, (unsigned)actual_sent, (unsigned)actual_received, (long long)now);
        assert(false);
    }
    assert(opened == closed && opened <= 1 && writes == (int)sent);
    if (cancel_phase) assert(opened == (cancel_phase == BEFORE ? 0 : 1));
    if (!received) assert(total == 0);
    else assert(total == (scenario == SELECT_INTERRUPTED ? 34U : scenario == SEND_INTERRUPTED ? 14U : received));
    if (scenario != SCHEDULER_DELAY) assert(now <= 6000000);
    if (!cancel_phase && (scenario == SILENT || scenario == FLOOD || scenario == ZERO_FLOOD || scenario == AGAIN_FLOOD || scenario == LATE_REPLY))
        assert(now >= 5499000 && now <= 5505000);
    if (scenario == GOOD) memcpy(old_payload, echo + 8, sizeof(old_payload));
    scenarios++;
}

static void check_cancellation(int address_family)
{
    for (cancel_phase = BEFORE; cancel_phase <= LAST_RECEIVE; cancel_phase++) {
        unsigned sent = cancel_phase <= WRITE_POLL || cancel_phase == SEND_RETRY ? 0 :
                        cancel_phase == LAST_RECEIVE ? 4 : 1;
        unsigned received = cancel_phase == GAP ? 1 : cancel_phase == LAST_RECEIVE ? 3 : 0;
        int scenario = cancel_phase == SEND_RETRY ? SEND_AGAIN : cancel_phase == NOISE ? FLOOD : GOOD;
        check(scenario, address_family, ECANCELED, sent, received);
    }
    cancel_phase = 0;
}

int main(void)
{
    check(GOOD, AF_INET, 0, 4, 4);
    check(OPTIONS, AF_INET, 0, 4, 4);
    check(OLD_PAYLOAD, AF_INET, 0, 4, 0);
    for (int scenario = SILENT; scenario <= EXTRA_BYTE; scenario++) {
        if (scenario == OLD_PAYLOAD) continue;
        check(scenario, AF_INET, 0, 4, 0);
    }
    check(SOCKET_FAIL, AF_INET, ENOMEM, 0, 0);
    check(GETFL_FAIL, AF_INET, EACCES, 0, 0);
    check(SETFL_FAIL, AF_INET, EACCES, 0, 0);
    check(SEND_FAIL, AF_INET, EIO, 0, 0);
    check(SHORT_SEND, AF_INET, EIO, 0, 0);
    check(SEND_AGAIN, AF_INET, ETIMEDOUT, 0, 0);
    check(SEND_INTERRUPTED, AF_INET, 0, 4, 4);
    check(RECEIVE_FAIL, AF_INET, EIO, 1, 0);
    check(SELECT_FAIL, AF_INET, EIO, 0, 0);
    check(SELECT_INTERRUPTED, AF_INET, 0, 4, 4);
    check(LATE_READY, AF_INET, ETIMEDOUT, 0, 0);
    check(SCHEDULER_DELAY, AF_INET, ETIMEDOUT, 1, 1);
    check_cancellation(AF_INET);
#if LWIP_IPV6
    check(GOOD, AF_INET6, 0, 4, 4);
    check(OLD_PAYLOAD, AF_INET6, 0, 4, 0);
    for (int scenario = SILENT; scenario <= EXTRA_BYTE; scenario++) {
        if (scenario == OLD_PAYLOAD || scenario == BAD_HEADER || scenario == FRAGMENT || scenario == BAD_IP_CHECKSUM) continue;
        check(scenario, AF_INET6, 0, 4, 0);
    }
    check_cancellation(AF_INET6);
#else
    check(GOOD, AF_INET6, EAFNOSUPPORT, 0, 0);
#endif
    printf("Network ping: %u deterministic socket/deadline cases passed (IPv6=%d)\n", scenarios, LWIP_IPV6);
    return 0;
}
