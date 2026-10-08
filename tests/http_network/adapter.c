/* Only OS/services and TCP/TLS endpoints are adapted. The pinned SDK HTTP
 * client, parser, transport dispatch, app and LVGL all run their real code.
 * TLS fails closed on this host: certificate/handshake checks require hardware. */
#include "adapter.h"
#include "dns_adapter.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_transport_internal.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile LONG http_host_allocations, http_host_transports;
volatile LONG http_host_fail_allocation, http_host_fail_transport;
volatile LONG http_host_certificates;
volatile LONG http_host_sdk_allocations;

void *http_host_malloc(size_t size)
{
    void *memory = malloc(size);
    if (memory) InterlockedIncrement(&http_host_sdk_allocations);
    return memory;
}
void *http_host_calloc(size_t count, size_t size)
{
    void *memory = calloc(count, size);
    if (memory) InterlockedIncrement(&http_host_sdk_allocations);
    return memory;
}
void *http_host_realloc(void *memory, size_t size)
{
    assert(size);
    void *replacement = realloc(memory, size);
    if (!memory && replacement) InterlockedIncrement(&http_host_sdk_allocations);
    return replacement;
}
void http_host_free(void *memory)
{
    if (memory) { InterlockedDecrement(&http_host_sdk_allocations); free(memory); }
}
char *http_host_strdup(const char *input)
{
    size_t length = strlen(input) + 1;
    char *copy = http_host_malloc(length);
    if (copy) memcpy(copy, input, length);
    return copy;
}
char *strcasestr(const char *input, const char *needle)
{
    size_t length = strlen(needle);
    do { if (!_strnicmp(input, needle, length)) return (char *)input; } while (*input++);
    return NULL;
}

int vasprintf(char **output, const char *format, va_list arguments)
{
    va_list copy; va_copy(copy, arguments);
    int length = vsnprintf(NULL, 0, format, copy); va_end(copy);
    *output = length >= 0 ? http_host_malloc((size_t)length + 1) : NULL;
    if (!*output) return -1;
    return vsnprintf(*output, (size_t)length + 1, format, arguments);
}
int asprintf(char **output, const char *format, ...)
{
    va_list arguments; va_start(arguments, format);
    int result = vasprintf(output, format, arguments); va_end(arguments);
    return result;
}
char *strndup(const char *input, size_t length)
{
    size_t available = strlen(input);
    if (length > available) length = available;
    char *copy = http_host_malloc(length + 1);
    if (copy) { memcpy(copy, input, length); copy[length] = 0; }
    return copy;
}
void *heap_caps_calloc(size_t count, size_t size, unsigned capabilities)
{
    assert(capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (InterlockedExchange(&http_host_fail_allocation, 0)) return NULL;
    void *memory = calloc(count, size);
    if (memory) InterlockedIncrement(&http_host_allocations);
    return memory;
}
void heap_caps_free(void *memory)
{
    if (memory) { InterlockedDecrement(&http_host_allocations); free(memory); }
}
typedef struct { int fd; bool secure, connecting; const char *common_name; } native_transport_t;
static int native_poll(esp_transport_handle_t transport, int timeout_ms, bool reading)
{
    native_transport_t *state = esp_transport_get_context_data(transport);
    assert(state->fd >= 0 && timeout_ms >= 0);
#if HTTP_HOST_BLOCKING
    assert(timeout_ms <= 30000);
#else
    assert(timeout_ms <= 100);
#endif
    fd_set ready; FD_ZERO(&ready); FD_SET((SOCKET)state->fd, &ready);
    struct timeval timeout = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    return host_select(state->fd + 1, reading ? &ready : NULL, reading ? NULL : &ready, NULL, &timeout);
}
static int native_poll_read(esp_transport_handle_t transport, int timeout_ms) { return native_poll(transport, timeout_ms, true); }
static int native_poll_write(esp_transport_handle_t transport, int timeout_ms) { return native_poll(transport, timeout_ms, false); }
static int native_connect(esp_transport_handle_t transport, const char *host, int port, int timeout_ms)
{
    native_transport_t *state = esp_transport_get_context_data(transport);
    assert(timeout_ms == 100);
    snprintf(http_host_last_peer, sizeof(http_host_last_peer), "%s", host);
    if (state->secure) {
        assert(state->common_name);
        snprintf(http_host_tls_name, sizeof(http_host_tls_name), "%s", state->common_name);
    }
    if (state->secure || strcmp(host, "127.0.0.1")) { errno = EINVAL; return -1; }
    if (state->fd < 0) {
        state->fd = host_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (state->fd < 0 || host_fcntl(state->fd, 0, 0) < 0) return -1;
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((u_short)port),
                                      .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        int result = host_connect(state->fd, (const struct sockaddr *)&address, sizeof(address));
        if (result == 0) return 1;
        if (errno != EINPROGRESS && errno != EWOULDBLOCK) { esp_transport_capture_errno(transport, errno); return -1; }
        state->connecting = true;
    }
    if (state->connecting) {
        int result = native_poll_write(transport, timeout_ms);
        if (result <= 0) return result;
        int error = 0; socklen_t length = sizeof(error);
        if (host_getsockopt(state->fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) return -1;
        state->connecting = false;
    }
    return 1;
}
static int native_read(esp_transport_handle_t transport, char *buffer, int length, int timeout_ms)
{
    int result = native_poll_read(transport, timeout_ms);
    if (result <= 0) return result == 0 ? 0 : ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    native_transport_t *state = esp_transport_get_context_data(transport);
    result = (int)host_recv(state->fd, buffer, (size_t)length, 0);
    if (result < 0) { esp_transport_capture_errno(transport, errno); return ERR_TCP_TRANSPORT_CONNECTION_FAILED; }
    return result == 0 ? ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN : result;
}
#if HTTP_HOST_BLOCKING
static int native_blocking_connect(esp_transport_handle_t transport, const char *host, int port, int timeout_ms)
{
    assert(timeout_ms >= 0 && timeout_ms <= 30000);
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int result;
    do {
        result = native_connect(transport, host, port, 100);
    } while (result == 0 && esp_timer_get_time() < deadline);
    return result > 0 ? 0 : -1;
}
#endif
static int native_write(esp_transport_handle_t transport, const char *buffer, int length, int timeout_ms)
{
    int result = native_poll_write(transport, timeout_ms);
    if (result <= 0) return result;
    native_transport_t *state = esp_transport_get_context_data(transport);
    result = (int)host_send(state->fd, buffer, (size_t)length, 0);
    if (result < 0) esp_transport_capture_errno(transport, errno);
    return result;
}
static int native_close(esp_transport_handle_t transport)
{
    native_transport_t *state = esp_transport_get_context_data(transport);
    if (state->fd < 0) return 0;
    int result = host_close(state->fd); state->fd = -1;
    return result;
}
static int native_destroy(esp_transport_handle_t transport)
{
    native_close(transport); free(esp_transport_get_context_data(transport));
    esp_transport_destroy_foundation_transport(transport->foundation);
    InterlockedDecrement(&http_host_transports);
    return 0;
}
static esp_transport_handle_t native_init(bool secure)
{
    if (InterlockedExchange(&http_host_fail_transport, 0)) return NULL;
    esp_transport_handle_t transport = esp_transport_init(); assert(transport);
    native_transport_t *state = calloc(1, sizeof(*state)); assert(state);
    state->fd = -1; state->secure = secure;
    transport->foundation = esp_transport_init_foundation_transport(); assert(transport->foundation);
    esp_transport_set_context_data(transport, state);
    esp_transport_set_func(transport,
#if HTTP_HOST_BLOCKING
                           native_blocking_connect,
#else
                           NULL,
#endif
                           native_read, native_write, native_close,
                           native_poll_read, native_poll_write, native_destroy);
    esp_transport_set_async_connect_func(transport, native_connect);
    InterlockedIncrement(&http_host_transports);
    return transport;
}
esp_transport_handle_t esp_transport_tcp_init(void) { return native_init(false); }
esp_transport_handle_t esp_transport_ssl_init(void) { return native_init(true); }
esp_err_t esp_crt_bundle_attach(void *configuration) { (void)configuration; return ESP_OK; }
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t transport, esp_err_t (*attach)(void *))
{
    assert(transport && attach == esp_crt_bundle_attach);
    InterlockedIncrement(&http_host_certificates);
}
#define IGNORE_DATA(name) void name(esp_transport_handle_t t, const char *data, int len) { (void)t; (void)data; (void)len; }
IGNORE_DATA(esp_transport_ssl_set_cert_data)
IGNORE_DATA(esp_transport_ssl_set_cert_data_der)
IGNORE_DATA(esp_transport_ssl_set_client_cert_data)
IGNORE_DATA(esp_transport_ssl_set_client_cert_data_der)
IGNORE_DATA(esp_transport_ssl_set_client_key_data)
IGNORE_DATA(esp_transport_ssl_set_client_key_data_der)
IGNORE_DATA(esp_transport_ssl_set_client_key_password)
void esp_transport_ssl_enable_global_ca_store(esp_transport_handle_t t) { (void)t; }
void esp_transport_ssl_set_tls_version(esp_transport_handle_t t, esp_tls_proto_ver_t version) { (void)t; (void)version; }
void esp_transport_ssl_skip_common_name_check(esp_transport_handle_t t) { (void)t; assert(0); }
void esp_transport_ssl_set_common_name(esp_transport_handle_t t, const char *name)
{
    native_transport_t *state = esp_transport_get_context_data(t); assert(state->secure);
    state->common_name = name;
}
void esp_transport_ssl_set_addr_family(esp_transport_handle_t t, esp_tls_addr_family_t family) { (void)t; (void)family; }
void esp_transport_tcp_set_keep_alive(esp_transport_handle_t t, esp_transport_keep_alive_t *config) { (void)t; (void)config; }
void esp_transport_tcp_set_interface_name(esp_transport_handle_t t, struct ifreq *name) { (void)t; (void)name; }
char *http_auth_basic(const char *username, const char *password)
{
    (void)username; (void)password; assert(0); return NULL;
}
