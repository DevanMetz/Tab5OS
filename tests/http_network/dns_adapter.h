#pragma once
#include "host.h"
extern volatile LONG http_host_dns_posts, http_host_dns_requests, http_host_dns_callbacks;
extern char http_host_last_peer[64], http_host_tls_name[256];
void http_dns_test_init(const char *mode);
void http_dns_test_close(void);
void http_dns_test_release_oldest(bool success);
void http_dns_test_release_posts(void);
