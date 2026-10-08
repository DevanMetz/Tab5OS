/* Windows adapter only: native sockets/threads/locks and POSIX error translation.
 * Wi-Fi readiness is supplied by the tiny headers; payloads come from local peers. */
#include "host.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <process.h>
#include <stdlib.h>
volatile LONG host_open_sockets, host_active_tasks, host_tasks_started, host_sockets_opened;
volatile LONG host_online = 1;
volatile LONG host_fail_next_task;
volatile LONG64 host_wall_offset_us;
volatile LONG host_hold_task_start, host_task_start_waiters;
volatile LONG64 host_monotonic_offset_us;
static int posix_error(int error)
{
    switch (error) {
    case 0:
        return 0;
    case WSAEWOULDBLOCK:
        return EWOULDBLOCK;
    case WSAEINPROGRESS:
        return EINPROGRESS;
    case WSAEINTR:
        return EINTR;
    case WSAECONNREFUSED:
        return ECONNREFUSED;
    case WSAECONNRESET:
        return ECONNRESET;
    case WSAETIMEDOUT:
        return ETIMEDOUT;
    case WSAENOTSOCK:
        return EBADF;
    case WSAEADDRINUSE:
        return EADDRINUSE;
    default:
        return EIO;
    }
}
static int socket_result(int result)
{
    if (result == SOCKET_ERROR)
        errno = posix_error(WSAGetLastError());
    return result;
}
int64_t esp_timer_get_time(void)
{
    LARGE_INTEGER value, frequency;
    QueryPerformanceCounter(&value);
    QueryPerformanceFrequency(&frequency);
    return (value.QuadPart / frequency.QuadPart) * 1000000 +
           (value.QuadPart % frequency.QuadPart) * 1000000 / frequency.QuadPart +
           InterlockedCompareExchange64(&host_monotonic_offset_us, 0, 0);
}
int host_gettimeofday(struct timeval *result, void *timezone)
{
    (void)timezone;
    FILETIME time;
    GetSystemTimePreciseAsFileTime(&time);
    ULARGE_INTEGER ticks = {.LowPart = time.dwLowDateTime, .HighPart = time.dwHighDateTime};
    int64_t micros = (int64_t)((ticks.QuadPart - UINT64_C(116444736000000000)) / 10) +
                     InterlockedCompareExchange64(&host_wall_offset_us, 0, 0);
    result->tv_sec = (long)(micros / 1000000);
    result->tv_usec = (long)(micros % 1000000);
    return 0;
}
struct tm *host_gmtime_r(const time_t *time, struct tm *result)
{
    return gmtime_s(result, time) == 0 ? result : NULL;
}
struct launch {
    void (*function)(void *);
    void *argument;
};
static unsigned __stdcall worker_entry(void *argument)
{
    struct launch job = *(struct launch *)argument;
    free(argument);
    if (InterlockedCompareExchange(&host_hold_task_start, 0, 0)) {
        InterlockedIncrement(&host_task_start_waiters);
        while (InterlockedCompareExchange(&host_hold_task_start, 0, 0)) Sleep(1);
        InterlockedDecrement(&host_task_start_waiters);
    }
    job.function(job.argument);
    InterlockedDecrement(&host_active_tasks);
    return 0;
}
int host_task_create(void (*function)(void *), const char *name, unsigned stack, void *argument,
                     unsigned priority, void *handle)
{
    (void)name;
    (void)stack;
    (void)priority;
    (void)handle;
    if (InterlockedExchange(&host_fail_next_task, 0)) return 0;
    struct launch *job = malloc(sizeof(*job));
    if (!job)
        return 0;
    *job = (struct launch){function, argument};
    InterlockedIncrement(&host_active_tasks);
    uintptr_t thread = _beginthreadex(NULL, 0, worker_entry, job, 0, NULL);
    if (!thread) {
        free(job);
        InterlockedDecrement(&host_active_tasks);
        return 0;
    }
    CloseHandle((HANDLE)thread);
    InterlockedIncrement(&host_tasks_started);
    return 1;
}
int host_socket(int domain, int type, int protocol)
{
    SOCKET s = socket(domain, type, protocol);
    if (s == INVALID_SOCKET) {
        errno = posix_error(WSAGetLastError());
        return -1;
    }
    assert(s <= INT_MAX);
    InterlockedIncrement(&host_open_sockets);
    InterlockedIncrement(&host_sockets_opened);
    return (int)s;
}
int host_fcntl(int fd, int command, int value)
{
    (void)command;
    (void)value;
    u_long mode = 1;
    return socket_result(ioctlsocket((SOCKET)fd, FIONBIO, &mode));
}
int host_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    return socket_result(connect((SOCKET)fd, address, length));
}
int host_bind(int fd, const struct sockaddr *address, socklen_t length)
{
    return socket_result(bind((SOCKET)fd, address, length));
}
int host_getsockname(int fd, struct sockaddr *address, socklen_t *length)
{
    return socket_result(getsockname((SOCKET)fd, address, length));
}
int host_select(int count, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
                struct timeval *timeout)
{
    fd_set exceptions;
    FD_ZERO(&exceptions);
    if (writefds)
        exceptions = *writefds;
    int result = select(count, readfds, writefds, writefds ? &exceptions : exceptfds, timeout);
    if (result > 0 && writefds)
        for (u_int i = 0; i < exceptions.fd_count; i++)
            FD_SET(exceptions.fd_array[i], writefds);
    return socket_result(result);
}
ssize_t host_send(int fd, const void *bytes, size_t length, int flags)
{
    return socket_result(send((SOCKET)fd, bytes, (int)length, flags));
}
ssize_t host_recv(int fd, void *bytes, size_t length, int flags)
{
    int count = recv((SOCKET)fd, bytes, (int)length, flags);
    /* WinSock fills the supplied buffer but reports WSAEMSGSIZE for a larger
     * datagram. lwIP/POSIX recv returns the copied prefix length instead. */
    if (count == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE) return (ssize_t)length;
    return socket_result(count);
}
int host_setsockopt(int fd, int level, int option, const void *value, socklen_t length)
{
    if (option == SO_SNDTIMEO || option == SO_RCVTIMEO) {
        const struct timeval *time = value;
        DWORD ms = (DWORD)(time->tv_sec * 1000 + time->tv_usec / 1000);
        return socket_result(setsockopt((SOCKET)fd, level, option, (const char *)&ms, sizeof(ms)));
    }
    return socket_result(setsockopt((SOCKET)fd, level, option, value, length));
}
int host_getsockopt(int fd, int level, int option, void *value, socklen_t *length)
{
    int result = socket_result(getsockopt((SOCKET)fd, level, option, value, length));
    if (!result && option == SO_ERROR)
        *(int *)value = posix_error(*(int *)value);
    return result;
}
int host_close(int fd)
{
    int result = socket_result(closesocket((SOCKET)fd));
    if (!result)
        InterlockedDecrement(&host_open_sockets);
    return result;
}
