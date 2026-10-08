#include "http_transport.h"
#include "network_resolver.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_parser.h"

#define HTTP_POLL_MS 100
#define HTTP_RESPONSE_HEADERS_MAX 8192

typedef struct {
    esp_transport_handle_t inner;
    int64_t deadline_us;
    http_transport_cancel_cb_t cancelled;
    void *context;
    http_transport_stop_t stopped;
    bool secure;
    int connect_error;
    char host[256];
    size_t received;
    size_t delivered;
    size_t safe_until;
    bool headers_complete;
    bool reading_trailers;
    bool message_complete;
    http_parser parser;
    char headers[HTTP_RESPONSE_HEADERS_MAX];
} http_transport_t;

static bool stopped(http_transport_t *state)
{
    if (state->stopped == HTTP_TRANSPORT_RUNNING) {
        if (state->cancelled(state->context)) state->stopped = HTTP_TRANSPORT_CANCELLED;
        else if (esp_timer_get_time() >= state->deadline_us) state->stopped = HTTP_TRANSPORT_DEADLINE;
    }
    if (state->stopped == HTTP_TRANSPORT_RUNNING) return false;
    errno = state->stopped == HTTP_TRANSPORT_CANCELLED ? ECANCELED :
            state->stopped == HTTP_TRANSPORT_DEADLINE ? ETIMEDOUT : EINVAL;
    return true;
}

static int64_t operation_deadline(http_transport_t *state, int timeout_ms)
{
    int64_t limit = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    return timeout_ms < 0 || limit > state->deadline_us ? state->deadline_us : limit;
}

static int poll_ms(int64_t limit)
{
    int64_t remaining = limit - esp_timer_get_time();
    if (remaining <= 0) return 0;
    int64_t ms = (remaining + 999) / 1000;
    return ms < HTTP_POLL_MS ? (int)ms : HTTP_POLL_MS;
}

static void reset_response(http_transport_t *state);

static bool resolve_host(http_transport_t *state, const char *host, char address[64], int64_t limit)
{
    if (strlen(host) >= sizeof(state->host)) { errno = EINVAL; return false; }
    strcpy(state->host, host);
    if (state->secure) esp_transport_ssl_set_common_name(state->inner, state->host);
    ip_addr_t resolved;
    size_t count;
    int error = network_resolve_host(host, &resolved, 1, &count, limit,
                                     state->cancelled, state->context);
    if (error) { errno = error; return false; }
    if (!ipaddr_ntoa_r(&resolved, address, 64)) { errno = EINVAL; return false; }
    return true;
}

static int connect_guarded(esp_transport_handle_t transport, const char *host, int port, int timeout_ms)
{
    http_transport_t *state = esp_transport_get_context_data(transport);
    int64_t limit = operation_deadline(state, timeout_ms);
    if (stopped(state)) return -1;
    reset_response(state);
    char address[64];
    state->connect_error = 0;
    bool resolved = resolve_host(state, host, address, limit);
    int resolve_error = errno;
    if (stopped(state)) { state->connect_error = errno; return -1; }
    if (!resolved) {
        state->connect_error = resolve_error;
        return -1;
    }
    do {
        if (stopped(state)) return -1;
        /* A numeric peer avoids ESP-TLS's synchronous DNS wait. The original
         * name remains the TLS verification/SNI name and the client's Host. */
        int result = esp_transport_connect_async(state->inner, address, port, HTTP_POLL_MS);
        if (stopped(state)) return -1;
        if (result < 0) return -1;
        if (result == 1) return 0;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (esp_timer_get_time() < limit);
    stopped(state);
    errno = ETIMEDOUT;
    return -1;
}

static int message_complete(http_parser *parser)
{
    http_transport_t *state = parser->data;
    state->message_complete = true;
    http_parser_pause(parser, 1);
    return 0;
}

static int chunk_header(http_parser *parser)
{
    if (parser->content_length == 0) {
        http_transport_t *state = parser->data;
        state->reading_trailers = true;
        http_parser_pause(parser, 1);
    }
    return 0;
}

static int headers_complete(http_parser *parser)
{
    /* A 304 Content-Length describes the selected representation, not a body. */
    return parser->status_code < 200 || parser->status_code == 204 || parser->status_code == 304;
}

static const http_parser_settings framing_settings = {
    .on_headers_complete = headers_complete,
    .on_message_complete = message_complete,
    .on_chunk_header = chunk_header,
};

static void reset_parser(http_transport_t *state)
{
    http_parser_init(&state->parser, HTTP_RESPONSE);
    state->parser.data = state;
    state->message_complete = false;
    state->reading_trailers = false;
}

static void reset_response(http_transport_t *state)
{
    state->received = state->delivered = state->safe_until = 0;
    state->headers_complete = false;
    reset_parser(state);
}

static size_t block_end(const char *buffer, size_t length, bool trailers)
{
    if (trailers && length >= 2 && !memcmp(buffer, "\r\n", 2)) return 2;
    for (size_t i = 0; i + 4 <= length; i++)
        if (!memcmp(buffer + i, "\r\n\r\n", 4)) return i + 4;
    return 0;
}

/* Return 0 to read more, 1 to process buffered data again, or -1 on rejection. */
static int prepare_metadata(http_transport_t *state)
{
    size_t end = block_end(state->headers, state->received, state->reading_trailers);
    if (!end) return 0;
    if (state->reading_trailers) http_parser_pause(&state->parser, 0);
    size_t parsed = http_parser_execute(&state->parser, &framing_settings, state->headers, end);
    enum http_errno error = HTTP_PARSER_ERRNO(&state->parser);
    if (parsed != end || (error != HPE_OK && error != HPE_PAUSED) ||
        state->parser.upgrade || state->parser.status_code < 100 ||
        state->parser.status_code == 101 ||
        (state->parser.status_code == 204 &&
         (state->parser.flags & (F_CHUNKED | F_CONTENTLENGTH)))) goto invalid;
    if (state->reading_trailers) {
        if (!state->message_complete) goto invalid;
        /* IDF 5.4.2 leaks the last trailer value even on a complete response.
         * Validate and discard trailers, then expose an empty section to it. */
        memcpy(state->headers, "\r\n", 2);
        state->received = state->safe_until = 2;
        state->delivered = 0;
        state->reading_trailers = false;
    } else if (state->parser.status_code < 200) {
        if (state->parser.flags & (F_CHUNKED | F_CONTENTLENGTH)) goto invalid;
        /* The SDK stops at a separated 1xx. Only deliver the final response. */
        memmove(state->headers, state->headers + end, state->received - end);
        state->received -= end;
        reset_parser(state);
    } else {
        state->headers_complete = true;
        state->safe_until = end;
        if (state->message_complete) state->received = end;
    }
    return 1;
invalid:
    state->stopped = HTTP_TRANSPORT_INVALID_HEADERS;
    return -1;
}

static int frame_body(http_transport_t *state, char *buffer, int length)
{
    size_t parsed = http_parser_execute(&state->parser, &framing_settings, buffer, (size_t)length);
    enum http_errno error = HTTP_PARSER_ERRNO(&state->parser);
    if (error != HPE_OK && error != HPE_PAUSED) {
        state->stopped = HTTP_TRANSPORT_INVALID_BODY;
    } else if (state->reading_trailers) {
        size_t tail = (size_t)length - parsed;
        size_t pending = state->received - state->delivered;
        if (tail + pending > sizeof(state->headers)) {
            state->stopped = HTTP_TRANSPORT_INVALID_HEADERS;
        } else {
            memmove(state->headers + tail, state->headers + state->delivered, pending);
            memcpy(state->headers, buffer + parsed, tail);
            state->received = tail + pending;
            state->delivered = 0;
        }
    } else if (state->message_complete) {
        /* A request consumes one final response, including on a reused peer. */
        state->delivered = state->received;
    }
    return parsed ? (int)parsed : ERR_TCP_TRANSPORT_CONNECTION_FAILED;
}

static int read_guarded(esp_transport_handle_t transport, char *buffer, int length, int timeout_ms)
{
    http_transport_t *state = esp_transport_get_context_data(transport);
    int64_t limit = operation_deadline(state, timeout_ms);
    for (;;) {
        /* IDF 5.4.2 does not free a partially parsed header value on cleanup.
         * Finish delivering validated metadata before observing cancellation. */
        if (state->delivered < state->safe_until) {
            size_t available = state->safe_until - state->delivered;
            size_t count = available < (size_t)length ? available : (size_t)length;
            memcpy(buffer, state->headers + state->delivered, count);
            state->delivered += count;
            if (state->delivered == state->safe_until) state->safe_until = 0;
            return (int)count;
        }
        if (stopped(state)) return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
        if (state->message_complete && !state->reading_trailers) return 0;
        int result;
        if (!state->headers_complete || state->reading_trailers) {
            int prepared = prepare_metadata(state);
            if (prepared < 0) return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
            if (prepared > 0) continue;
            size_t space = sizeof(state->headers) - state->received;
            if (!space) {
                state->stopped = HTTP_TRANSPORT_INVALID_HEADERS;
                return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
            }
            int capacity = (size_t)length < space ? length : (int)space;
            result = esp_transport_read(state->inner, state->headers + state->received, capacity, poll_ms(limit));
            if (stopped(state)) return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
            if (result > 0) {
                state->received += (size_t)result;
                limit = operation_deadline(state, timeout_ms);
                continue;
            }
        } else {
            size_t pending = state->received - state->delivered;
            if (pending) {
                size_t count = pending < (size_t)length ? pending : (size_t)length;
                memcpy(buffer, state->headers + state->delivered, count);
                state->delivered += count;
                return frame_body(state, buffer, (int)count);
            }
            result = esp_transport_read(state->inner, buffer, length, poll_ms(limit));
            if (stopped(state)) return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
            if (result > 0) return frame_body(state, buffer, result);
            if (result == ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN) {
                http_parser_execute(&state->parser, &framing_settings, "", 0);
                /* Only an actual FIN may complete a close-delimited response.
                 * Returning 0 lets the SDK observe EOF for its own parser. */
                if (state->message_complete) return 0;
            }
        }
        if (result != ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT) return result;
        if (esp_timer_get_time() >= limit) {
            stopped(state);
            errno = ETIMEDOUT;
            /* A timeout must not masquerade as EOF to the SDK body parser. */
            return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
        }
    }
}

static int write_guarded(esp_transport_handle_t transport, const char *buffer, int length, int timeout_ms)
{
    http_transport_t *state = esp_transport_get_context_data(transport);
    /* Automatic redirects may reuse the connection after draining a response. */
    if (state->message_complete && !state->reading_trailers && !state->safe_until)
        reset_response(state);
    int64_t limit = operation_deadline(state, timeout_ms);
    do {
        if (stopped(state)) return -1;
        int result = esp_transport_write(state->inner, buffer, length, poll_ms(limit));
        if (stopped(state)) return -1;
        if (result != 0) return result;
    } while (esp_timer_get_time() < limit);
    stopped(state);
    errno = ETIMEDOUT;
    return 0;
}

static int poll_guarded(esp_transport_handle_t transport, int timeout_ms, bool reading)
{
    http_transport_t *state = esp_transport_get_context_data(transport);
    int64_t limit = operation_deadline(state, timeout_ms);
    do {
        if (stopped(state)) return -1;
        int ms = poll_ms(limit);
        int result = reading ? esp_transport_poll_read(state->inner, ms) :
                               esp_transport_poll_write(state->inner, ms);
        if (stopped(state)) return -1;
        if (result != 0) return result;
    } while (esp_timer_get_time() < limit);
    stopped(state);
    return 0;
}

static int poll_read_guarded(esp_transport_handle_t transport, int timeout_ms)
{
    return poll_guarded(transport, timeout_ms, true);
}

static int poll_write_guarded(esp_transport_handle_t transport, int timeout_ms)
{
    return poll_guarded(transport, timeout_ms, false);
}

static int close_guarded(esp_transport_handle_t transport)
{
    http_transport_t *state = esp_transport_get_context_data(transport);
    return esp_transport_close(state->inner);
}

static int destroy_guarded(esp_transport_handle_t transport)
{
    http_transport_t *state = esp_transport_get_context_data(transport);
    esp_transport_destroy(state->inner);
    heap_caps_free(state);
    return 0;
}

esp_transport_handle_t http_transport_init(bool secure, int64_t deadline_us,
                                           http_transport_cancel_cb_t cancelled,
                                           void *context)
{
    if (!cancelled) return NULL;
    http_transport_t *state = heap_caps_calloc(1, sizeof(*state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!state) return NULL;
    state->inner = secure ? esp_transport_ssl_init() : esp_transport_tcp_init();
    esp_transport_handle_t transport = state->inner ? esp_transport_init() : NULL;
    if (!transport) {
        esp_transport_destroy(state->inner);
        heap_caps_free(state);
        return NULL;
    }
    state->deadline_us = deadline_us;
    state->secure = secure;
    state->cancelled = cancelled;
    state->context = context;
    reset_parser(state);
    if (secure) esp_transport_ssl_crt_bundle_attach(state->inner, esp_crt_bundle_attach);
    esp_transport_set_default_port(transport, secure ? 443 : 80);
    esp_transport_set_context_data(transport, state);
    esp_transport_set_func(transport, connect_guarded, read_guarded, write_guarded,
                           close_guarded, poll_read_guarded, poll_write_guarded, destroy_guarded);
    return transport;
}

http_transport_stop_t http_transport_stop_reason(esp_transport_handle_t transport)
{
    http_transport_t *state = esp_transport_get_context_data(transport);
    return state->stopped;
}

int http_transport_get_errno(esp_transport_handle_t transport)
{
    http_transport_t *state = esp_transport_get_context_data(transport);
    if (state->connect_error) return state->connect_error;
    int error = esp_transport_get_errno(state->inner);
    return error > 0 ? error : 0;
}

bool http_transport_response_complete(esp_transport_handle_t transport)
{
    http_transport_t *state = esp_transport_get_context_data(transport);
    return state->headers_complete && state->message_complete &&
           !state->reading_trailers && state->safe_until == 0;
}
