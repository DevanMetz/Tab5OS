#include "network_resolver.h"

#include <errno.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

typedef struct {
    uint32_t id;
    bool done;
    int error;
    size_t count;
    ip_addr_t addresses[NETWORK_RESOLVER_MAX_ADDRESSES];
    char host[256];
} lookup_t;

/* HTTP and Diagnostics each own at most one lookup. Detaching under this lock
 * lets either worker return while lwIP finishes its copied-name query. */
static portMUX_TYPE lookup_lock = portMUX_INITIALIZER_UNLOCKED;
static lookup_t *active[2];
static uint32_t next_id;

static lookup_t *find_lookup(uint32_t id)
{
    for (unsigned i = 0; i < 2; i++)
        if (active[i] && active[i]->id == id) return active[i];
    return NULL;
}

static void lookup_result(void *argument, const ip_addr_t *addresses, int error)
{
    portENTER_CRITICAL(&lookup_lock);
    lookup_t *lookup = find_lookup((uint32_t)(uintptr_t)argument);
    if (lookup && !lookup->done) {
        if (addresses) {
            size_t count = DNS_MAX_HOST_IP < NETWORK_RESOLVER_MAX_ADDRESSES ?
                           DNS_MAX_HOST_IP : NETWORK_RESOLVER_MAX_ADDRESSES;
            memcpy(lookup->addresses, addresses, count * sizeof(*addresses));
            while (count > 1 && ip_addr_isany(&lookup->addresses[count - 1])) count--;
            lookup->count = count;
        }
        lookup->error = error;
        lookup->done = true;
    }
    portEXIT_CRITICAL(&lookup_lock);
}

static void lookup_found(const char *name, const ip_addr_t *addresses, void *argument)
{
    (void)name;
    lookup_result(argument, addresses, addresses ? 0 : EHOSTUNREACH);
}

static void lookup_dispatch(void *argument)
{
    char host[256];
    portENTER_CRITICAL(&lookup_lock);
    lookup_t *lookup = find_lookup((uint32_t)(uintptr_t)argument);
    bool attached = lookup != NULL;
    if (lookup) memcpy(host, lookup->host, sizeof(host));
    portEXIT_CRITICAL(&lookup_lock);
    if (!attached) return;
    /* ESP-IDF's lwIP can write DNS_MAX_HOST_IP cached addresses. */
    ip_addr_t addresses[DNS_MAX_HOST_IP] = {0};
    err_t result = dns_gethostbyname_addrtype(host, addresses, lookup_found, argument,
                                             LWIP_DNS_ADDRTYPE_DEFAULT);
    if (result == ERR_OK) lookup_result(argument, addresses, 0);
    else if (result != ERR_INPROGRESS)
        lookup_result(argument, NULL, result == ERR_MEM ? ENOMEM : EHOSTUNREACH);
}

static int stop_error(int64_t deadline, network_resolver_cancel_cb_t cancelled, void *context)
{
    if (cancelled && cancelled(context)) return ECANCELED;
    return esp_timer_get_time() >= deadline ? ETIMEDOUT : 0;
}

int network_resolve_host(const char *host, ip_addr_t *addresses, size_t capacity,
                         size_t *count, int64_t deadline_us,
                         network_resolver_cancel_cb_t cancelled, void *context)
{
    if (count) *count = 0;
    if (!host || !host[0] || strlen(host) >= 256 || !addresses || !count ||
        !capacity || capacity > NETWORK_RESOLVER_MAX_ADDRESSES) return EINVAL;
    int error = stop_error(deadline_us, cancelled, context);
    if (error) return error;
    ip_addr_t numeric;
    if (ipaddr_aton(host, &numeric)) {
        error = stop_error(deadline_us, cancelled, context);
        if (!error) { addresses[0] = numeric; *count = 1; }
        return error;
    }

    lookup_t lookup = {0};
    memcpy(lookup.host, host, strlen(host) + 1);
    unsigned slot;
    portENTER_CRITICAL(&lookup_lock);
    for (slot = 0; slot < 2 && active[slot]; slot++) {}
    if (slot == 2) {
        portEXIT_CRITICAL(&lookup_lock);
        return EBUSY;
    }
    if (++next_id == 0) ++next_id;
    lookup.id = next_id;
    active[slot] = &lookup;
    portEXIT_CRITICAL(&lookup_lock);
    if (tcpip_try_callback(lookup_dispatch, (void *)(uintptr_t)lookup.id) != ERR_OK) {
        error = ENOMEM;
    } else {
        for (;;) {
            error = stop_error(deadline_us, cancelled, context);
            if (error) break;
            portENTER_CRITICAL(&lookup_lock);
            bool done = lookup.done;
            if (done) {
                error = lookup.error;
                if (!error) {
                    *count = lookup.count < capacity ? lookup.count : capacity;
                    memcpy(addresses, lookup.addresses, *count * sizeof(*addresses));
                }
            }
            portEXIT_CRITICAL(&lookup_lock);
            if (done) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    portENTER_CRITICAL(&lookup_lock);
    active[slot] = NULL;
    portEXIT_CRITICAL(&lookup_lock);
    return error;
}
