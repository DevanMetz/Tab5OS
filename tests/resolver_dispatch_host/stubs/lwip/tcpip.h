#pragma once
#include "dns.h"
typedef void (*tcpip_callback_fn)(void *);
err_t tcpip_try_callback(tcpip_callback_fn function, void *argument);
