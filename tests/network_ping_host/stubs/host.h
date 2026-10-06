#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Deterministic socket/clock boundary only; no real network or OS descriptors. */
typedef ptrdiff_t ssize_t;
typedef unsigned socklen_t;
struct in_addr { uint8_t bytes[4]; };
struct in6_addr { uint8_t bytes[16]; };
struct sockaddr { unsigned short sa_family; char data[14]; };
struct sockaddr_in { unsigned short sin_family, sin_port; struct in_addr sin_addr; char padding[8]; };
struct sockaddr_in6 { unsigned short sin6_family, sin6_port; uint32_t flow;
                      struct in6_addr sin6_addr; uint32_t sin6_scope_id; };
struct sockaddr_storage { unsigned short ss_family; char padding[126]; };
struct timeval { long tv_sec, tv_usec; };
typedef struct { int fd; } fd_set;
#define FD_ZERO(set) ((set)->fd = -1)
#define FD_SET(value, set) ((set)->fd = (value))
#define AF_INET 2
#define AF_INET6 10
#define SOCK_RAW 3
#define IPPROTO_ICMP 1
#define IPPROTO_ICMPV6 58
#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 0x800
#define pdMS_TO_TICKS(ms) (ms)

typedef struct { uint8_t bytes[4]; } ip4_addr_t;
typedef struct { uint8_t bytes[16]; unsigned zone; } ip6_addr_t;
typedef struct { int family; ip4_addr_t v4; ip6_addr_t v6; } ip_addr_t;
#define IP_IS_V4(ip) ((ip)->family == AF_INET)
#define ip_2_ip4(ip) (&(ip)->v4)
#define ip_2_ip6(ip) (&(ip)->v6)
#define ip6_addr_zone(ip) ((ip)->zone)
#define inet_addr_from_ip4addr(dest, src) memcpy((dest), (src)->bytes, 4)
#define inet6_addr_from_ip6addr(dest, src) memcpy((dest), (src)->bytes, 16)

int64_t esp_timer_get_time(void);
uint32_t esp_random(void);
void test_delay(unsigned ticks);
uint16_t inet_chksum(const void *bytes, uint16_t length);
int test_socket(int family, int type, int protocol);
int test_fcntl(int fd, int command, int flags);
int test_select(int count, fd_set *readable, fd_set *writable, fd_set *errors, struct timeval *wait);
ssize_t test_sendto(int fd, const void *bytes, size_t length, int flags,
                    const struct sockaddr *peer, socklen_t peer_length);
ssize_t test_recvfrom(int fd, void *bytes, size_t capacity, int flags,
                      struct sockaddr *peer, socklen_t *peer_length);
int test_close(int fd);
#define socket test_socket
#define fcntl test_fcntl
#define select test_select
#define sendto test_sendto
#define recvfrom test_recvfrom
#define close test_close
#define vTaskDelay test_delay
