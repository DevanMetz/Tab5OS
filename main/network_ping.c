#include "network_ping.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/inet_chksum.h"
#include "lwip/sockets.h"

#define PING_BYTES 40
#define PING_POLL_US 100000
#define PING_PROBE_US 1000000
#define PING_RUN_US 6000000

static unsigned read_u16(const uint8_t *bytes)
{
    return ((unsigned)bytes[0] << 8) | bytes[1];
}

#if LWIP_IPV6
static uint32_t checksum_sum(const uint8_t *bytes, size_t length)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < length; i += 2) sum += read_u16(bytes + i);
    return sum;
}
#endif

static bool reply_matches(const uint8_t *packet, size_t length, int family,
                          const void *peer, const uint8_t request[PING_BYTES])
{
    size_t offset;
    unsigned reply_type;
    if (family == AF_INET) {
        if (length < 20 || packet[0] >> 4 != 4 || packet[9] != IPPROTO_ICMP) return false;
        offset = (packet[0] & 15U) * 4U;
        if (offset < 20 || offset > 60 || length != offset + PING_BYTES ||
            read_u16(packet + 2) != length || (read_u16(packet + 6) & 0x3fffU) ||
            memcmp(packet + 12, peer, 4) || inet_chksum(packet, (uint16_t)offset) ||
            inet_chksum(packet + offset, PING_BYTES)) return false;
        reply_type = 0;
    }
#if LWIP_IPV6
    else if (family == AF_INET6) {
        /* The pinned lwIP raw socket delivers the IPv6 header with bare ICMPv6. */
        if (length != 40 + PING_BYTES || packet[0] >> 4 != 6 ||
            packet[6] != IPPROTO_ICMPV6 || read_u16(packet + 4) != PING_BYTES ||
            memcmp(packet + 8, peer, 16)) return false;
        uint32_t sum = checksum_sum(packet + 8, 32) + PING_BYTES + IPPROTO_ICMPV6;
        sum += checksum_sum(packet + 40, PING_BYTES);
        while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
        if (sum != 0xffffU) return false;
        offset = 40;
        reply_type = 129;
    }
#endif
    else return false;
    return packet[offset] == reply_type && packet[offset + 1] == 0 &&
           !memcmp(packet + offset + 4, request + 4, PING_BYTES - 4);
}

static int wait_socket(int fd, bool writing, int64_t deadline,
                       network_ping_cancel_cb_t cancelled, void *context)
{
    for (;;) {
        if (cancelled && cancelled(context)) return ECANCELED;
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) return ETIMEDOUT;
        if (remaining > PING_POLL_US) remaining = PING_POLL_US;
        struct timeval wait = {.tv_sec = 0, .tv_usec = (long)remaining};
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(fd, &ready);
        int count = select(fd + 1, writing ? NULL : &ready, writing ? &ready : NULL, NULL, &wait);
        if (cancelled && cancelled(context)) return ECANCELED;
        if (esp_timer_get_time() >= deadline) return ETIMEDOUT;
        if (count > 0) return 0;
        if (count < 0 && errno != EINTR) return errno ? errno : EIO;
    }
}

int network_ping_target(const ip_addr_t *target, uint32_t *sent,
                        uint32_t *received, uint32_t *total_reply_ms,
                        network_ping_cancel_cb_t cancelled, void *context)
{
    *sent = *received = *total_reply_ms = 0;
    if (cancelled && cancelled(context)) return ECANCELED;
    int64_t deadline = esp_timer_get_time() + PING_RUN_US;
    int family, protocol;
    struct sockaddr_storage destination = {0};
    socklen_t destination_length;
    const void *peer;
    if (IP_IS_V4(target)) {
        family = AF_INET;
        protocol = IPPROTO_ICMP;
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)&destination;
        ipv4->sin_family = AF_INET;
        inet_addr_from_ip4addr(&ipv4->sin_addr, ip_2_ip4(target));
        peer = &ipv4->sin_addr;
        destination_length = sizeof(*ipv4);
    }
#if LWIP_IPV6
    else {
        family = AF_INET6;
        protocol = IPPROTO_ICMPV6;
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&destination;
        ipv6->sin6_family = AF_INET6;
        inet6_addr_from_ip6addr(&ipv6->sin6_addr, ip_2_ip6(target));
        ipv6->sin6_scope_id = ip6_addr_zone(ip_2_ip6(target));
        peer = &ipv6->sin6_addr;
        destination_length = sizeof(*ipv6);
    }
#else
    else return EAFNOSUPPORT;
#endif
    int fd = socket(family, SOCK_RAW, protocol);
    if (fd < 0) return errno ? errno : EIO;
    int error = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        error = errno ? errno : EIO;
        goto cleanup;
    }

    uint8_t request[PING_BYTES];
    memset(request, 'A', sizeof(request));
    request[0] = family == AF_INET ? 8 : 128;
    request[1] = 0;
    /* A fresh payload prevents a late reply from an earlier run matching again. */
    uint32_t nonce[2] = {esp_random(), esp_random()};
    memcpy(request + 8, nonce, sizeof(nonce));
    memcpy(request + 4, nonce, 2);
    for (unsigned probe = 1; probe <= 4; probe++) {
        if (cancelled && cancelled(context)) { error = ECANCELED; break; }
        int64_t started = esp_timer_get_time();
        if (started >= deadline) { error = ETIMEDOUT; break; }
        int64_t probe_deadline = started + PING_PROBE_US;
        if (probe_deadline > deadline) probe_deadline = deadline;
        request[2] = request[3] = request[6] = 0;
        request[7] = (uint8_t)probe;
        if (family == AF_INET) {
            uint16_t checksum = inet_chksum(request, sizeof(request));
            memcpy(request + 2, &checksum, sizeof(checksum));
        }
        for (;;) {
            error = wait_socket(fd, true, probe_deadline, cancelled, context);
            if (error) break;
            ssize_t length = sendto(fd, request, sizeof(request), 0,
                                    (struct sockaddr *)&destination, destination_length);
            if (length == (ssize_t)sizeof(request)) { (*sent)++; break; }
            if (length >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                error = length >= 0 ? EIO : errno ? errno : EIO;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (error) break;

        uint8_t response[128];
        for (;;) {
            error = wait_socket(fd, false, probe_deadline, cancelled, context);
            if (error == ETIMEDOUT) { error = 0; break; }  // Lost probe, same absolute wait.
            if (error) break;
            ssize_t length = recvfrom(fd, response, sizeof(response), 0, NULL, NULL);
            if (cancelled && cancelled(context)) { error = ECANCELED; break; }
            if (esp_timer_get_time() >= probe_deadline) break;
            if (length > 0 && (size_t)length <= sizeof(response) &&
                reply_matches(response, (size_t)length, family, peer, request)) {
                (*received)++;
                *total_reply_ms += (uint32_t)((esp_timer_get_time() - started) / 1000);
                break;
            }
            if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                error = errno ? errno : EIO;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (error) break;
        if (probe != 4) {
            int64_t pause = esp_timer_get_time() + 500000;
            while (esp_timer_get_time() < pause && esp_timer_get_time() < deadline) {
                if (cancelled && cancelled(context)) { error = ECANCELED; break; }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (error) break;
        }
    }
cleanup:
    close(fd);
    return error;
}
