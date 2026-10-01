#pragma once
#include "host.h"
/* lwIP descriptors are signed ints; WinSock's FD_SET macro expects SOCKET. */
static inline void host_fd_set(int fd, fd_set *set) { FD_SET((SOCKET)fd, set); }
#undef FD_SET
#define FD_SET(fd, set) host_fd_set(fd, set)
#define socket host_socket
#define connect host_connect
#define bind host_bind
#define getsockname host_getsockname
#define select host_select
#define send host_send
#define recv host_recv
#define setsockopt host_setsockopt
#define getsockopt host_getsockopt
#define close host_close
