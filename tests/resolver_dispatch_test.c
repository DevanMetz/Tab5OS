/* Actual shared resolver; deterministic core dispatch and clock boundaries.
 * No SDK scheduler, DNS service, socket, UI or physical network is executed. */
#include "network_resolver.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "freertos/task.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef assert
#define assert(condition) do { if (!(condition)) { \
    fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); exit(3); } } while (0)

typedef struct {
    ip_addr_t addresses[4];
    unsigned char guard[32];
    size_t count;
    int error;
    bool copies_valid;
} result_t;
enum { IMMEDIATE, HELD, NESTED };
static int dispatch_mode;
static int64_t now, dispatch_delay;
static unsigned posts, requests, delays, cases, failures;
static bool delivering_core, old_before_fresh, nested_ok;
static tcpip_callback_fn old_dispatch;
static void *old_argument;
static result_t resolve(const char *host, size_t capacity, int64_t budget);

int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(TickType_t ticks) { assert(ticks == 10); now += (int64_t)ticks * 1000; delays++; }
int ipaddr_aton(const char *host, ip_addr_t *address) { (void)host; (void)address; return 0; }
static void dispatch(tcpip_callback_fn function, void *argument)
{
    assert(!delivering_core); delivering_core = true;
    function(argument);
    delivering_core = false;
}
err_t tcpip_try_callback(tcpip_callback_fn function, void *argument)
{
    assert(function && argument && !delivering_core); posts++;
    if (dispatch_mode == HELD) { old_dispatch = function; old_argument = argument; return ERR_OK; }
    now += dispatch_mode == NESTED && posts == 2 ? 1000 : dispatch_delay;
    if (dispatch_mode == NESTED && posts == 1) {
        result_t child = resolve("fresh.fixture.test", 4, 100000);
        nested_ok = child.error == 0 && child.count == DNS_MAX_HOST_IP && child.copies_valid;
    }
    if (old_before_fresh) { old_before_fresh = false; dispatch(old_dispatch, old_argument); }
    /* The caller has not resumed after posting; its lookup stack is still attached. */
    dispatch(function, argument);
    return ERR_OK;
}
err_t dns_gethostbyname_addrtype(const char *host, ip_addr_t *addresses,
                               dns_found_callback found, void *argument, unsigned char type)
{
    assert(delivering_core && found && argument && type == LWIP_DNS_ADDRTYPE_DEFAULT);
    assert(!strcmp(host, "expired.fixture.test") || !strcmp(host, "fresh.fixture.test"));
    requests++;
    for (unsigned i = 0; i < DNS_MAX_HOST_IP; i++) addresses[i].value = 0xc0000201U + i;
    return ERR_OK;
}
static result_t resolve(const char *host, size_t capacity, int64_t budget)
{
    result_t result; memset(&result, 0xa5, sizeof(result)); result.count = 99;
    result.error = network_resolve_host(host, result.addresses, capacity, &result.count,
                                        now + budget, NULL, NULL);
    result.copies_valid = result.count <= capacity;
    for (size_t i = 0; i < 4; i++) {
        uint32_t expected = i < result.count ? 0xc0000201U + (uint32_t)i : 0xa5a5a5a5U;
        result.copies_valid &= result.addresses[i].value == expected;
    }
    for (size_t i = 0; i < sizeof(result.guard); i++) result.copies_valid &= result.guard[i] == 0xa5;
    return result;
}
static void reset(int mode, int64_t delay)
{
    now = 1000000; dispatch_mode = mode; dispatch_delay = delay;
    posts = requests = delays = 0; nested_ok = old_before_fresh = delivering_core = false;
    old_dispatch = NULL; old_argument = NULL;
}
static void report(const char *name, bool pass, result_t result)
{
    cases++; failures += !pass;
    printf("%s %s error=%s count=%zu posts=%u dns_requests=%u delays=%u copies=%u\n", pass ? "PASS" : "FAIL",
           name, result.error == 0 ? "ok" : result.error == ETIMEDOUT ? "timeout" : "other", result.count,
           posts, requests, delays, result.copies_valid);
}
static void queued(const char *name, int64_t delay, size_t capacity, bool expired)
{
    reset(IMMEDIATE, delay); result_t result = resolve("expired.fixture.test", capacity, 100000);
    size_t count = expired ? 0 : (DNS_MAX_HOST_IP < capacity ? DNS_MAX_HOST_IP : capacity);
    report(name, result.error == (expired ? ETIMEDOUT : 0) && result.count == count && result.copies_valid &&
                 posts == 1 && requests == (unsigned)!expired && !delays, result);
}
int main(void)
{
    assert(DNS_MAX_HOST_IP == 1 || DNS_MAX_HOST_IP == 4);
    queued("dispatch-live", 1000, 4, false);
    queued("dispatch-one-us-remaining", 99999, 1, false);
    queued("dispatch-at-deadline", 100000, 4, true);
    queued("dispatch-after-deadline", 100001, 4, true);
    queued("dispatch-long-queue", 3100000, 4, true);

    reset(HELD, 1000); result_t detached = resolve("expired.fixture.test", 4, 30000);
    bool detached_ok = detached.error == ETIMEDOUT && !detached.count && detached.copies_valid && !requests;
    dispatch(old_dispatch, old_argument); detached_ok &= !requests;
    dispatch_mode = IMMEDIATE; old_before_fresh = true;
    result_t fresh = resolve("fresh.fixture.test", 4, 100000);
    report("detached-and-reused-slot", detached_ok && !fresh.error && fresh.count == DNS_MAX_HOST_IP &&
           fresh.copies_valid && posts == 2 && requests == 1, fresh);

    reset(NESTED, 100001); result_t outer = resolve("expired.fixture.test", 4, 100000);
    report("expired-and-live-clients", outer.error == ETIMEDOUT && !outer.count && outer.copies_valid &&
           nested_ok && posts == 2 && requests == 1, outer);

    reset(IMMEDIATE, 100001); bool recovery = true;
    for (unsigned i = 0; i < 25; i++) {
        dispatch_delay = 100001; unsigned previous = requests;
        result_t rejected = resolve("expired.fixture.test", 4, 100000);
        recovery &= rejected.error == ETIMEDOUT && !rejected.count && rejected.copies_valid && requests == previous;
        dispatch_delay = 1000; fresh = resolve("fresh.fixture.test", 4, 100000);
        recovery &= !fresh.error && fresh.count == DNS_MAX_HOST_IP && fresh.copies_valid && requests == previous + 1;
    }
    report("expired-fresh-retry-25", recovery && posts == 50 && requests == 25, fresh);
    printf("%s %u resolver dispatch cases failures=%u capacity=%d (controlled clock/core/SDK APIs; no network or scheduler)\n",
           failures ? "FAIL" : "PASS", cases, failures, DNS_MAX_HOST_IP);
    return failures ? 1 : 0;
}
