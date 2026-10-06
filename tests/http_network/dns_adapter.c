/* Deterministic platform resolver adapter. A separate thread dispatches core
 * callbacks and DNS replies; no external DNS service or real names are used. */
#include "dns_adapter.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

volatile LONG http_host_dns_posts, http_host_dns_requests, http_host_dns_callbacks;
char http_host_last_peer[64], http_host_tls_name[256];
typedef struct {
    int kind;
    int64_t due;
    uint64_t order;
    tcpip_callback_fn dispatch;
    dns_found_callback found;
    void *argument;
    char host[256];
    bool success;
} dns_event_t;
static dns_event_t events[16];
static SRWLOCK lock = SRWLOCK_INIT;
static HANDLE thread, wake;
static DWORD core_thread;
static bool closing, hold_posts, fail_post;
static uint64_t next_order;

static void fill_addresses(const char *host, ip_addr_t *addresses)
{
    memset(addresses, 0, DNS_MAX_HOST_IP * sizeof(*addresses));
    if (!strcmp(host, "multi.fixture.test") || !strcmp(host, "multi-slow.fixture.test")) {
        for (unsigned i = 0; i < DNS_MAX_HOST_IP; i++) {
            char text[32]; snprintf(text, sizeof(text), "127.0.0.%u", i + 1);
            assert(ipaddr_aton(text, &addresses[i]));
        }
    } else assert(ipaddr_aton(!strcmp(host, "v6.fixture.test") ? "::1" : "127.0.0.1", addresses));
}

int ipaddr_aton(const char *text, ip_addr_t *address)
{
    memset(address, 0, sizeof(*address));
    if (InetPtonA(AF_INET, text, &address->value.v4) == 1) address->family = AF_INET;
    else if (InetPtonA(AF_INET6, text, &address->value.v6) == 1) address->family = AF_INET6;
    else return 0;
    return 1;
}
char *ipaddr_ntoa_r(const ip_addr_t *address, char *text, int capacity)
{
    void *value = address->family == AF_INET ? (void *)&address->value.v4 : (void *)&address->value.v6;
    return (char *)InetNtopA(address->family, value, text, (DWORD)capacity);
}
static err_t enqueue(const dns_event_t *event)
{
    err_t result = ERR_MEM;
    AcquireSRWLockExclusive(&lock);
    for (unsigned i = 0; i < 16; i++) if (!events[i].kind) {
        events[i] = *event; events[i].order = ++next_order; result = ERR_OK; break;
    }
    ReleaseSRWLockExclusive(&lock);
    SetEvent(wake);
    return result;
}
err_t tcpip_try_callback(tcpip_callback_fn function, void *argument)
{
    assert(function && argument);
    InterlockedIncrement(&http_host_dns_posts);
    if (fail_post) { fail_post = false; return ERR_MEM; }
    dns_event_t event = {.kind = 1, .due = hold_posts ? INT64_MAX : esp_timer_get_time() + 1000,
                         .dispatch = function, .argument = argument};
    return enqueue(&event);
}
err_t dns_gethostbyname_addrtype(const char *host, ip_addr_t *address,
                               dns_found_callback found, void *argument, unsigned char type)
{
    assert(GetCurrentThreadId() == core_thread && type == LWIP_DNS_ADDRTYPE_DEFAULT);
    InterlockedIncrement(&http_host_dns_requests);
    if (!strcmp(host, "cache.fixture.test") || !strcmp(host, "v6.fixture.test") || !strcmp(host, "multi.fixture.test")) {
        fill_addresses(host, address); return ERR_OK;
    }
    if (!strcmp(host, "memory.fixture.test")) return ERR_MEM;
    bool held = !strcmp(host, "hold.fixture.test") || !strcmp(host, "next.fixture.test");
    assert(held || !strcmp(host, "slow.fixture.test") || !strcmp(host, "multi-slow.fixture.test") || !strcmp(host, "fail.fixture.test"));
    dns_event_t event = {.kind = 2, .due = held ? INT64_MAX : esp_timer_get_time() + 50000,
                         .found = found, .argument = argument, .success = strcmp(host, "fail.fixture.test") != 0};
    snprintf(event.host, sizeof(event.host), "%s", host);
    return enqueue(&event) == ERR_OK ? ERR_INPROGRESS : ERR_MEM;
}
static DWORD WINAPI run(void *argument)
{
    (void)argument; core_thread = GetCurrentThreadId();
    for (;;) {
        dns_event_t event = {0}; bool stop;
        AcquireSRWLockExclusive(&lock);
        stop = closing;
        for (unsigned i = 0; i < 16; i++) if (events[i].kind &&
            (stop || events[i].due <= esp_timer_get_time())) {
            event = events[i]; events[i].kind = 0; break;
        }
        ReleaseSRWLockExclusive(&lock);
        if (event.kind == 1) event.dispatch(event.argument);
        else if (event.kind == 2) {
            ip_addr_t addresses[DNS_MAX_HOST_IP]; fill_addresses(event.host, addresses);
            InterlockedIncrement(&http_host_dns_callbacks);
            event.found(event.host, !stop && event.success ? addresses : NULL, event.argument);
        } else if (stop) return 0;
        else WaitForSingleObject(wake, 10);
    }
}
void http_dns_test_init(const char *mode)
{
    hold_posts = !strcmp(mode, "dns-queued-cancel") || !strcmp(mode, "dns-queued-next");
    fail_post = !strcmp(mode, "dns-post-failure");
    wake = CreateEvent(NULL, FALSE, FALSE, NULL); assert(wake);
    thread = CreateThread(NULL, 0, run, NULL, 0, NULL); assert(thread);
}
void http_dns_test_release_oldest(bool success)
{
    int selected = -1;
    AcquireSRWLockExclusive(&lock);
    for (unsigned i = 0; i < 16; i++) if (events[i].kind == 2 && events[i].due == INT64_MAX) {
        if (selected < 0 || events[i].order < events[selected].order) selected = (int)i;
    }
    if (selected >= 0) { events[selected].success = success; events[selected].due = esp_timer_get_time(); }
    ReleaseSRWLockExclusive(&lock);
    assert(selected >= 0); SetEvent(wake);
}
void http_dns_test_release_posts(void)
{
    AcquireSRWLockExclusive(&lock);
    for (unsigned i = 0; i < 16; i++) if (events[i].kind == 1) events[i].due = esp_timer_get_time();
    ReleaseSRWLockExclusive(&lock); SetEvent(wake);
}
void http_dns_test_close(void)
{
    AcquireSRWLockExclusive(&lock); closing = true; ReleaseSRWLockExclusive(&lock);
    SetEvent(wake); assert(WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0);
    CloseHandle(thread); CloseHandle(wake);
    for (unsigned i = 0; i < 16; i++) assert(!events[i].kind);
}
