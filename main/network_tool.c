#include "network_tool.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/icmp6.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/ip6.h"
#include "lwip/sockets.h"
#include "mdns.h"

#define NETWORK_HOST_MAX 127
#define NETWORK_RESULT_MAX 768
#define MDNS_RESULT_LIMIT 8

typedef enum {
    NETWORK_LOOKUP,
    NETWORK_PING,
    NETWORK_MDNS,
} network_operation_t;

static portMUX_TYPE network_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool operation_busy;
static bool operation_done;
static network_operation_t pending_operation;
static char pending_host[NETWORK_HOST_MAX + 1] = "example.com";
static char operation_result[NETWORK_RESULT_MAX];
static TaskHandle_t network_worker_handle;
static lv_timer_t *network_timer;
static lv_obj_t *host_area;
static lv_obj_t *link_label;
static lv_obj_t *result_label;
static bool mdns_started;

static bool normalize_host(const char *input, char *output, size_t output_size)
{
    while (*input == ' ' || *input == '\t') input++;
    size_t length = strlen(input);
    while (length && (input[length - 1] == ' ' || input[length - 1] == '\t')) length--;
    if (!length || length >= output_size) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char)input[i];
        if (value < 0x21 || value > 0x7e || strchr("/\\?#@", value)) return false;
    }
    memcpy(output, input, length);
    output[length] = '\0';
    return true;
}

static unsigned ping_loss_percent(unsigned sent, unsigned received)
{
    return sent ? (sent - received) * 100U / sent : 100U;
}

static unsigned format_mdns_services(const mdns_result_t *results, char *output,
                                     size_t output_size)
{
    snprintf(output, output_size, "mDNS service types (up to %u)", MDNS_RESULT_LIMIT);
    unsigned count = 0;
    for (const mdns_result_t *result = results; result && count < MDNS_RESULT_LIMIT;
         result = result->next) {
        if (!result->service_type || !result->proto) continue;
        bool duplicate = false;
        // ponytail: O(n^2) de-dup is simpler and bounded to eight network results.
        for (const mdns_result_t *prior = results; prior != result; prior = prior->next) {
            if (prior->service_type && prior->proto &&
                !strcmp(prior->service_type, result->service_type) &&
                !strcmp(prior->proto, result->proto)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        size_t used = strlen(output);
        int written = snprintf(output + used, output_size - used, "\n%s.%s",
                               result->service_type, result->proto);
        if (written < 0 || (size_t)written >= output_size - used) break;
        count++;
    }
    return count;
}

void network_tool_self_test(void)
{
    char host[32];
    assert(normalize_host(" example.com ", host, sizeof(host)) && !strcmp(host, "example.com"));
    assert(normalize_host("192.0.2.1", host, sizeof(host)) && !strcmp(host, "192.0.2.1"));
    assert(normalize_host("2001:db8::1", host, sizeof(host)) && !strcmp(host, "2001:db8::1"));
    assert(!normalize_host("https://example.com", host, sizeof(host)));
    assert(!normalize_host("bad host", host, sizeof(host)));
    assert(ping_loss_percent(4, 4) == 0);
    assert(ping_loss_percent(4, 3) == 25);
    assert(ping_loss_percent(0, 0) == 100);

    mdns_result_t third = {.service_type = "_http", .proto = "_tcp"};
    mdns_result_t second = {.next = &third, .service_type = "_http", .proto = "_tcp"};
    mdns_result_t first = {.next = &second, .service_type = "_ipp", .proto = "_tcp"};
    char services[128];
    assert(format_mdns_services(NULL, services, sizeof(services)) == 0);
    assert(format_mdns_services(&first, services, sizeof(services)) == 2);
    assert(strstr(services, "_ipp._tcp") && strstr(services, "_http._tcp"));
}

static bool resolve_host(const char *host, char *addresses, size_t addresses_size,
                         ip_addr_t *first_address)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_protocol = IPPROTO_TCP};
    struct addrinfo *results = NULL;
    int error = getaddrinfo(host, NULL, &hints, &results);
    if (error != 0 || !results) {
        snprintf(addresses, addresses_size, "DNS lookup failed (%d)", error);
        return false;
    }

    addresses[0] = '\0';
    bool found = false;
    unsigned count = 0;
    for (struct addrinfo *entry = results; entry && count < 4; entry = entry->ai_next) {
        const void *source = NULL;
        if (entry->ai_family == AF_INET)
            source = &((const struct sockaddr_in *)entry->ai_addr)->sin_addr;
#if LWIP_IPV6
        else if (entry->ai_family == AF_INET6)
            source = &((const struct sockaddr_in6 *)entry->ai_addr)->sin6_addr;
#endif
        if (!source) continue;
        char address[INET6_ADDRSTRLEN];
        if (!inet_ntop(entry->ai_family, source, address, sizeof(address))) continue;
        if (!found && !ipaddr_aton(address, first_address)) continue;
        size_t used = strlen(addresses);
        int written = snprintf(addresses + used, addresses_size - used, "%s%s",
                               used ? "\n" : "", address);
        if (written < 0 || (size_t)written >= addresses_size - used) break;
        found = true;
        count++;
    }
    freeaddrinfo(results);
    if (!found) snprintf(addresses, addresses_size, "DNS returned no usable address");
    return found;
}

static bool ping_reply_matches(const uint8_t *packet, size_t length, int family,
                               uint16_t identifier, uint16_t sequence)
{
    if (family == AF_INET) {
        if (length < sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr)) return false;
        const struct ip_hdr *ip = (const struct ip_hdr *)packet;
        size_t header_length = IPH_HL_BYTES(ip);
        if (header_length + sizeof(struct icmp_echo_hdr) > length) return false;
        const struct icmp_echo_hdr *reply =
            (const struct icmp_echo_hdr *)(packet + header_length);
        return reply->type == ICMP_ER && reply->id == identifier && reply->seqno == sequence;
    }
#if LWIP_IPV6
    if (family == AF_INET6) {
        if (length < sizeof(struct ip6_hdr) + sizeof(struct icmp6_echo_hdr)) return false;
        const struct icmp6_echo_hdr *reply =
            (const struct icmp6_echo_hdr *)(packet + sizeof(struct ip6_hdr));
        return reply->type == ICMP6_TYPE_EREP && reply->id == identifier &&
               reply->seqno == sequence;
    }
#endif
    return false;
}

static int ping_target(const ip_addr_t *target, uint32_t *sent, uint32_t *received,
                       uint32_t *total_reply_ms)
{
    int family;
    int protocol;
    struct sockaddr_storage destination = {0};
    socklen_t destination_length;
    if (IP_IS_V4(target)) {
        family = AF_INET;
        protocol = IPPROTO_ICMP;
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)&destination;
        ipv4->sin_family = AF_INET;
        inet_addr_from_ip4addr(&ipv4->sin_addr, ip_2_ip4(target));
        destination_length = sizeof(*ipv4);
    }
#if LWIP_IPV6
    else {
        family = AF_INET6;
        protocol = IP6_NEXTH_ICMP6;
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&destination;
        ipv6->sin6_family = AF_INET6;
        inet6_addr_from_ip6addr(&ipv6->sin6_addr, ip_2_ip6(target));
        destination_length = sizeof(*ipv6);
    }
#else
    else {
        return EAFNOSUPPORT;
    }
#endif

    int socket_fd = socket(family, SOCK_RAW, protocol);
    if (socket_fd < 0) return errno ? errno : EIO;
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        int error = errno ? errno : EIO;
        close(socket_fd);
        return error;
    }

    uint8_t echo[sizeof(struct icmp_echo_hdr) + 32] = {0};
    struct icmp_echo_hdr *header = (struct icmp_echo_hdr *)echo;
    header->type = family == AF_INET ? ICMP_ECHO : ICMP6_TYPE_EREQ;
    header->id = lwip_htons((uint16_t)((uintptr_t)xTaskGetCurrentTaskHandle() & 0xffffU));
    memset(echo + sizeof(*header), 'A', sizeof(echo) - sizeof(*header));
    *sent = *received = *total_reply_ms = 0;

    for (uint16_t probe = 1; probe <= 4; probe++) {
        header->seqno = lwip_htons(probe);
        header->chksum = 0;
        if (family == AF_INET) header->chksum = inet_chksum(echo, sizeof(echo));
        int64_t started_us = esp_timer_get_time();
        ssize_t written = sendto(socket_fd, echo, sizeof(echo), 0,
                                 (struct sockaddr *)&destination, destination_length);
        if (written != sizeof(echo)) {
            int error = errno ? errno : EIO;
            close(socket_fd);
            return error;
        }
        (*sent)++;

        uint8_t response[96];
        ssize_t length;
        do {
            length = recvfrom(socket_fd, response, sizeof(response), 0, NULL, NULL);
            if (length > 0 && ping_reply_matches(response, (size_t)length, family,
                                                  header->id, header->seqno)) {
                (*received)++;
                *total_reply_ms += (uint32_t)((esp_timer_get_time() - started_us) / 1000);
                break;
            }
        } while (length > 0);
        if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            int error = errno ? errno : EIO;
            close(socket_fd);
            return error;
        }
        if (probe != 4) vTaskDelay(pdMS_TO_TICKS(500));
    }
    close(socket_fd);
    return 0;
}

static void finish_operation(const char *result)
{
    portENTER_CRITICAL(&network_lock);
    snprintf(operation_result, sizeof(operation_result), "%s", result);
    operation_busy = false;
    operation_done = true;
    portEXIT_CRITICAL(&network_lock);
}

static void network_worker(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        network_operation_t operation;
        char host[NETWORK_HOST_MAX + 1];
        portENTER_CRITICAL(&network_lock);
        operation = pending_operation;
        snprintf(host, sizeof(host), "%s", pending_host);
        portEXIT_CRITICAL(&network_lock);

        if (operation == NETWORK_MDNS) {
            esp_err_t error = ESP_OK;
            if (!mdns_started) {
                error = mdns_init();
                mdns_started = error == ESP_OK;
            }
            mdns_result_t *services = NULL;
            if (error == ESP_OK)
                error = mdns_query_ptr("_services._dns-sd", "_udp", 3000,
                                       MDNS_RESULT_LIMIT, &services);
            char result[NETWORK_RESULT_MAX];
            if (error != ESP_OK) {
                snprintf(result, sizeof(result), "mDNS discovery failed: %s",
                         esp_err_to_name(error));
            } else if (!format_mdns_services(services, result, sizeof(result))) {
                snprintf(result, sizeof(result), "No mDNS services answered in 3 seconds");
            }
            mdns_query_results_free(services);
            finish_operation(result);
            continue;
        }

        char addresses[256];
        ip_addr_t target;
        if (!resolve_host(host, addresses, sizeof(addresses), &target)) {
            finish_operation(addresses);
            continue;
        }
        if (operation == NETWORK_LOOKUP) {
            char result[NETWORK_RESULT_MAX];
            snprintf(result, sizeof(result), "%s\n\n%s", host, addresses);
            finish_operation(result);
            continue;
        }

        uint32_t sent = 0, received = 0, total_ms = 0;
        int error = ping_target(&target, &sent, &received, &total_ms);

        char result[NETWORK_RESULT_MAX];
        if (error)
            snprintf(result, sizeof(result), "Ping failed: %s", strerror(error));
        else
            snprintf(result, sizeof(result), "%s\n%s\n\nSent %lu  Received %lu  Loss %lu%%\nAverage reply %lu ms",
                     host, addresses, (unsigned long)sent, (unsigned long)received,
                     (unsigned long)ping_loss_percent(sent, received),
                     (unsigned long)(received ? total_ms / received : 0));
        finish_operation(result);
    }
}

static void start_operation(network_operation_t operation)
{
    char host[NETWORK_HOST_MAX + 1];
    if (operation != NETWORK_MDNS &&
        !normalize_host(lv_textarea_get_text(host_area), host, sizeof(host))) {
        lv_label_set_text(result_label, "Enter a hostname or IP address only; omit scheme and path");
        return;
    }
    portENTER_CRITICAL(&network_lock);
    if (operation_busy) {
        portEXIT_CRITICAL(&network_lock);
        return;
    }
    pending_operation = operation;
    if (operation != NETWORK_MDNS) snprintf(pending_host, sizeof(pending_host), "%s", host);
    operation_busy = true;
    operation_done = false;
    portEXIT_CRITICAL(&network_lock);
    lv_label_set_text(result_label, operation == NETWORK_LOOKUP ? "Resolving..." :
                      operation == NETWORK_PING ? "Resolving, then sending four pings..." :
                      "Discovering mDNS service types for 3 seconds...");
    if (!network_worker_handle &&
        xTaskCreate(network_worker, "network-check", 5120, NULL, 4,
                    &network_worker_handle) != pdPASS) {
        network_worker_handle = NULL;
        finish_operation("Could not start network check");
        return;
    }
    xTaskNotifyGive(network_worker_handle);
}

static void lookup_clicked(lv_event_t *event)
{
    (void)event;
    start_operation(NETWORK_LOOKUP);
}

static void ping_clicked(lv_event_t *event)
{
    (void)event;
    start_operation(NETWORK_PING);
}

static void mdns_clicked(lv_event_t *event)
{
    (void)event;
    start_operation(NETWORK_MDNS);
}

static void update_link(void)
{
    if (!link_label) return;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    esp_netif_dns_info_t dns = {0};
    wifi_ap_record_t access_point = {0};
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK ||
        esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
        lv_label_set_text(link_label, "Wi-Fi is not connected");
        return;
    }
    esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    char dns_text[IPADDR_STRLEN_MAX] = "unavailable";
    if (dns.ip.type == ESP_IPADDR_TYPE_V4)
        snprintf(dns_text, sizeof(dns_text), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    lv_label_set_text_fmt(link_label,
                          "%s   Ch %u   %d dBm\nIP " IPSTR "   Gateway " IPSTR "\nMask " IPSTR "   DNS %s",
                          access_point.ssid, (unsigned)access_point.primary, access_point.rssi,
                          IP2STR(&ip.ip), IP2STR(&ip.gw), IP2STR(&ip.netmask), dns_text);
}

static void refresh_clicked(lv_event_t *event)
{
    (void)event;
    update_link();
}

static void network_tick(lv_timer_t *timer)
{
    (void)timer;
    char result[NETWORK_RESULT_MAX];
    bool done;
    portENTER_CRITICAL(&network_lock);
    done = operation_done;
    if (done) {
        snprintf(result, sizeof(result), "%s", operation_result);
        operation_done = false;
    }
    portEXIT_CRITICAL(&network_lock);
    if (done && result_label) lv_label_set_text(result_label, result);
}

static lv_obj_t *action_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_size(control, 140, 68);
    lv_obj_add_event_cb(control, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(control);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return control;
}

bool network_tool_busy(void)
{
    portENTER_CRITICAL(&network_lock);
    bool busy = operation_busy;
    portEXIT_CRITICAL(&network_lock);
    return busy;
}

void network_tool_stop(void)
{
    if (network_timer) {
        lv_timer_delete(network_timer);
        network_timer = NULL;
    }
    host_area = NULL;
    link_label = NULL;
    result_label = NULL;
}

void network_tool_show(lv_obj_t *parent, bool connected)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Network Diagnostics");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    link_label = lv_label_create(parent);
    lv_obj_set_size(link_label, 640, 100);
    lv_obj_set_style_text_align(link_label, LV_TEXT_ALIGN_CENTER, 0);
    update_link();

    host_area = lv_textarea_create(parent);
    lv_obj_set_size(host_area, 640, 72);
    lv_textarea_set_one_line(host_area, true);
    lv_textarea_set_max_length(host_area, NETWORK_HOST_MAX);
    lv_textarea_set_text(host_area, pending_host);
    lv_textarea_set_placeholder_text(host_area, "Hostname or IP address");

    lv_obj_t *actions = lv_obj_create(parent);
    lv_obj_set_size(actions, 640, 82);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *lookup = action_button(actions, "LOOK UP", lookup_clicked);
    lv_obj_t *ping = action_button(actions, "PING x4", ping_clicked);
    lv_obj_t *discover = action_button(actions, "mDNS", mdns_clicked);
    action_button(actions, "REFRESH", refresh_clicked);
    if (!connected) {
        lv_obj_add_state(lookup, LV_STATE_DISABLED);
        lv_obj_add_state(ping, LV_STATE_DISABLED);
        lv_obj_add_state(discover, LV_STATE_DISABLED);
    }

    result_label = lv_label_create(parent);
    lv_obj_set_size(result_label, 640, 220);
    lv_label_set_long_mode(result_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(result_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(result_label, connected ? "Enter a target, then look it up or ping it" : "Connect to Wi-Fi first");

    lv_obj_t *keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(keyboard, 640, 420);
    lv_keyboard_set_textarea(keyboard, host_area);
    network_timer = lv_timer_create(network_tick, 200, NULL);
}
