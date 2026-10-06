#include "network_tool.h"
#include "network_ping.h"
#include "network_resolver.h"

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
#include "lwip/ip_addr.h"
#include "mdns.h"

#define NETWORK_HOST_MAX 127
#define NETWORK_RESULT_MAX 768
#define MDNS_RESULT_LIMIT 8
#define NETWORK_DNS_WAIT_US 10000000

typedef enum {
    NETWORK_LOOKUP,
    NETWORK_PING,
    NETWORK_MDNS,
} network_operation_t;

static portMUX_TYPE network_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool operation_busy;
static bool operation_done;
static bool operation_cancel_requested;
static int64_t pending_dns_deadline;
static network_operation_t pending_operation;
static char pending_host[NETWORK_HOST_MAX + 1] = "example.com";
static char operation_result[NETWORK_RESULT_MAX];
static lv_timer_t *network_timer;
static lv_obj_t *host_area;
static lv_obj_t *link_label;
static lv_obj_t *result_label;
static lv_obj_t *operation_buttons[3];
static lv_obj_t *cancel_button;
static lv_obj_t *network_keyboard;
static bool network_connected;
static bool mdns_started;

static bool normalize_host(const char *input, char *output, size_t output_size)
{
    // Retain and reject the first excess input character, including whitespace.
    if (strlen(input) >= output_size) return false;
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
    assert(!normalize_host("                                x", host, sizeof(host)));
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

static bool operation_cancelled(void *context)
{
    (void)context;
    portENTER_CRITICAL(&network_lock);
    bool cancelled = operation_cancel_requested;
    portEXIT_CRITICAL(&network_lock);
    return cancelled;
}

static bool resolve_host(const char *host, char *addresses, size_t addresses_size,
                         ip_addr_t *first_address, int64_t deadline)
{
    ip_addr_t results[NETWORK_RESOLVER_MAX_ADDRESSES];
    size_t count;
    int error = network_resolve_host(host, results, NETWORK_RESOLVER_MAX_ADDRESSES,
                                     &count, deadline, operation_cancelled, NULL);
    if (error) {
        if (error == ETIMEDOUT)
            snprintf(addresses, addresses_size, "DNS lookup timed out (10 seconds)");
        else
            snprintf(addresses, addresses_size, "DNS lookup failed: %s", strerror(error));
        return false;
    }

    addresses[0] = '\0';
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        char address[64];
        if (!ipaddr_ntoa_r(&results[i], address, sizeof(address))) continue;
        size_t used = strlen(addresses);
        int written = snprintf(addresses + used, addresses_size - used, "%s%s",
                               used ? "\n" : "", address);
        if (written < 0 || (size_t)written >= addresses_size - used) break;
        if (!found) *first_address = results[i];
        found = true;
    }
    if (!found) snprintf(addresses, addresses_size, "DNS returned no usable address");
    return found;
}

static void finish_operation(const char *result)
{
    portENTER_CRITICAL(&network_lock);
    if (operation_cancel_requested)
        snprintf(operation_result, sizeof(operation_result), "Cancelled\n%s",
                 pending_operation == NETWORK_MDNS ? "mDNS discovery" : pending_host);
    else
        snprintf(operation_result, sizeof(operation_result), "%s", result);
    operation_busy = false;
    operation_done = true;
    portEXIT_CRITICAL(&network_lock);
}

static void network_worker(void *argument)
{
    (void)argument;
    network_operation_t operation;
    char host[NETWORK_HOST_MAX + 1];
    int64_t deadline;
    portENTER_CRITICAL(&network_lock);
    operation = pending_operation;
    snprintf(host, sizeof(host), "%s", pending_host);
    deadline = pending_dns_deadline;
    portEXIT_CRITICAL(&network_lock);
    char result[NETWORK_RESULT_MAX];
    if (operation_cancelled(NULL)) {
        snprintf(result, sizeof(result), "%s", host);
    } else if (operation == NETWORK_MDNS) {
        esp_err_t error = ESP_OK;
        if (!mdns_started) {
            error = mdns_init();
            mdns_started = error == ESP_OK;
        }
        mdns_result_t *services = NULL;
        if (error == ESP_OK && !operation_cancelled(NULL))
            error = mdns_query_ptr("_services._dns-sd", "_udp", 3000,
                                   MDNS_RESULT_LIMIT, &services);
        if (error != ESP_OK) {
            snprintf(result, sizeof(result), "mDNS discovery failed: %s",
                     esp_err_to_name(error));
        } else if (!format_mdns_services(services, result, sizeof(result))) {
            snprintf(result, sizeof(result), "No mDNS services answered in 3 seconds");
        }
        mdns_query_results_free(services);
    } else {
        char addresses[256];
        ip_addr_t target;
        if (!resolve_host(host, addresses, sizeof(addresses), &target, deadline)) {
            snprintf(result, sizeof(result), "%s\n\n%s", host, addresses);
        } else if (operation == NETWORK_LOOKUP) {
            snprintf(result, sizeof(result), "%s\n\n%s", host, addresses);
        } else {
            uint32_t sent = 0, received = 0, total_ms = 0;
            int error = network_ping_target(&target, &sent, &received, &total_ms,
                                             operation_cancelled, NULL);

            if (error)
                snprintf(result, sizeof(result), "%s\n%s\n\nPing failed: %s", host, addresses, strerror(error));
            else
                snprintf(result, sizeof(result), "%s\n%s\n\nSent %lu  Received %lu  Loss %lu%%\nAverage reply %lu ms",
                         host, addresses, (unsigned long)sent, (unsigned long)received,
                         (unsigned long)ping_loss_percent(sent, received),
                         (unsigned long)(received ? total_ms / received : 0));
        }
    }
    finish_operation(result);
    vTaskDelete(NULL);
}

static void update_controls(void)
{
    portENTER_CRITICAL(&network_lock);
    bool busy = operation_busy;
    bool cancelling = operation_cancel_requested;
    portEXIT_CRITICAL(&network_lock);
    for (unsigned i = 0; i < 3; i++) if (operation_buttons[i]) {
        if (busy || !network_connected) lv_obj_add_state(operation_buttons[i], LV_STATE_DISABLED);
        else lv_obj_remove_state(operation_buttons[i], LV_STATE_DISABLED);
    }
    if (cancel_button) {
        if (!busy || cancelling) lv_obj_add_state(cancel_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(cancel_button, LV_STATE_DISABLED);
    }
}

static bool show_finished_operation(void)
{
    char result[NETWORK_RESULT_MAX];
    portENTER_CRITICAL(&network_lock);
    bool done = operation_done;
    if (done) {
        snprintf(result, sizeof(result), "%s", operation_result);
        operation_done = false;
    }
    portEXIT_CRITICAL(&network_lock);
    if (done && result_label) lv_label_set_text(result_label, result);
    return done;
}

static void start_operation(network_operation_t operation)
{
    // A stale action tap consumes a waiting completion before accepting traffic.
    if (show_finished_operation()) { update_controls(); return; }
    if (network_tool_busy()) return;
    if (!network_connected) {
        lv_label_set_text(result_label, "Connect to Wi-Fi first");
        return;
    }
    char host[NETWORK_HOST_MAX + 1];
    if (operation != NETWORK_MDNS &&
        !normalize_host(lv_textarea_get_text(host_area), host, sizeof(host))) {
        lv_label_set_text(result_label, "Use at most 127 ASCII bytes; enter a hostname or IP address without scheme or path");
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
    operation_cancel_requested = false;
    pending_dns_deadline = esp_timer_get_time() + NETWORK_DNS_WAIT_US;
    portEXIT_CRITICAL(&network_lock);
    lv_label_set_text(result_label, operation == NETWORK_LOOKUP ? "Resolving..." :
                      operation == NETWORK_PING ? "Resolving, then sending four pings..." :
                      "Discovering mDNS service types for 3 seconds...");
    update_controls();
    if (xTaskCreate(network_worker, "network-check", 5120, NULL, 4, NULL) != pdPASS) {
        finish_operation("Could not start network check");
        show_finished_operation();
        update_controls();
        return;
    }
}

static void request_cancel(void)
{
    portENTER_CRITICAL(&network_lock);
    if (operation_busy) operation_cancel_requested = true;
    portEXIT_CRITICAL(&network_lock);
}

static void cancel_clicked(lv_event_t *event)
{
    (void)event;
    if (show_finished_operation()) { update_controls(); return; }
    request_cancel();
    portENTER_CRITICAL(&network_lock);
    bool busy = operation_busy;
    bool mdns = pending_operation == NETWORK_MDNS;
    portEXIT_CRITICAL(&network_lock);
    if (busy)
        lv_label_set_text(result_label, mdns ?
                          "Cancelling...\nmDNS is finishing its 3-second discovery" : "Cancelling...");
    update_controls();
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

static bool update_link(void)
{
    if (!link_label) return false;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    esp_netif_dns_info_t dns = {0};
    wifi_ap_record_t access_point = {0};
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK ||
        esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
        lv_label_set_text(link_label, "Wi-Fi is not connected");
        return false;
    }
    esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    char dns_text[IPADDR_STRLEN_MAX] = "unavailable";
    if (dns.ip.type == ESP_IPADDR_TYPE_V4)
        snprintf(dns_text, sizeof(dns_text), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    lv_label_set_text_fmt(link_label,
                          "%s   Ch %u   %d dBm\nIP " IPSTR "   Gateway " IPSTR "\nMask " IPSTR "   DNS %s",
                          access_point.ssid, (unsigned)access_point.primary, access_point.rssi,
                          IP2STR(&ip.ip), IP2STR(&ip.gw), IP2STR(&ip.netmask), dns_text);
    return true;
}

static void refresh_clicked(lv_event_t *event)
{
    (void)event;
    network_connected = update_link();
    update_controls();
}

static void network_tick(lv_timer_t *timer)
{
    (void)timer;
    show_finished_operation();
    update_controls();
}

static lv_obj_t *action_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_size(control, 110, 68);
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
    request_cancel();
    if (network_timer) {
        lv_timer_delete(network_timer);
        network_timer = NULL;
    }
    host_area = NULL;
    link_label = NULL;
    result_label = NULL;
    for (unsigned i = 0; i < 3; i++) operation_buttons[i] = NULL;
    cancel_button = NULL;
    if (network_keyboard) lv_keyboard_set_textarea(network_keyboard, NULL);
    network_keyboard = NULL;
    network_connected = false;
}

void network_tool_show(lv_obj_t *parent, bool connected)
{
    lv_obj_set_style_pad_row(parent, 16, 0);
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Network Diagnostics");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    link_label = lv_label_create(parent);
    lv_obj_set_size(link_label, 640, 100);
    lv_obj_set_style_text_align(link_label, LV_TEXT_ALIGN_CENTER, 0);
    bool link_connected = update_link();
    network_connected = connected && link_connected;

    host_area = lv_textarea_create(parent);
    lv_obj_set_size(host_area, 640, 72);
    lv_textarea_set_one_line(host_area, true);
    lv_textarea_set_max_length(host_area, NETWORK_HOST_MAX + 1);
    lv_textarea_set_text(host_area, pending_host);
    lv_textarea_set_placeholder_text(host_area, "Hostname or IP address");

    lv_obj_t *actions = lv_obj_create(parent);
    lv_obj_set_size(actions, 640, 82);
    lv_obj_set_style_pad_all(actions, 4, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    operation_buttons[0] = action_button(actions, "LOOK UP", lookup_clicked);
    operation_buttons[1] = action_button(actions, "PING x4", ping_clicked);
    operation_buttons[2] = action_button(actions, "mDNS", mdns_clicked);
    action_button(actions, "REFRESH", refresh_clicked);
    cancel_button = action_button(actions, "CANCEL", cancel_clicked);

    result_label = lv_label_create(parent);
    lv_obj_set_size(result_label, 640, 220);
    lv_label_set_long_mode(result_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(result_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(result_label, network_connected ? "Enter a target, then look it up or ping it" : "Connect to Wi-Fi first");
    if (network_tool_busy()) lv_label_set_text(result_label, "Cancelling the previous network check...");
    show_finished_operation();
    update_controls();

    network_keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(network_keyboard, 640, 420);
    lv_keyboard_set_textarea(network_keyboard, host_area);
    network_timer = lv_timer_create(network_tick, 200, NULL);
}
