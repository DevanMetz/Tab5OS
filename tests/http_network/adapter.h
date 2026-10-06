#pragma once
#include "host.h"
extern volatile LONG http_host_allocations, http_host_transports;
extern volatile LONG http_host_fail_allocation, http_host_fail_transport;
extern volatile LONG http_host_certificates;
extern volatile LONG http_host_sdk_allocations;
