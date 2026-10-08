#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#undef small
typedef intptr_t ssize_t;
extern volatile LONG host_open_sockets, host_active_tasks, host_tasks_started, host_sockets_opened;
extern volatile LONG host_online;
extern volatile LONG host_fail_next_task;
extern volatile LONG64 host_wall_offset_us;
extern volatile LONG host_hold_task_start, host_task_start_waiters;
extern volatile LONG64 host_monotonic_offset_us;
int64_t esp_timer_get_time(void);
int host_gettimeofday(struct timeval *, void *);
struct tm *host_gmtime_r(const time_t *, struct tm *);
int host_task_create(void (*function)(void *), const char *, unsigned, void *, unsigned, void *);
int host_socket(int, int, int);
int host_fcntl(int, int, int);
int host_connect(int, const struct sockaddr *, socklen_t);
int host_bind(int, const struct sockaddr *, socklen_t);
int host_getsockname(int, struct sockaddr *, socklen_t *);
int host_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
ssize_t host_send(int, const void *, size_t, int);
ssize_t host_recv(int, void *, size_t, int);
int host_setsockopt(int, int, int, const void *, socklen_t);
int host_getsockopt(int, int, int, void *, socklen_t *);
int host_close(int);
