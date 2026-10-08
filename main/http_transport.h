#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_transport.h"

typedef enum {
    HTTP_TRANSPORT_RUNNING,
    HTTP_TRANSPORT_CANCELLED,
    HTTP_TRANSPORT_DEADLINE,
    HTTP_TRANSPORT_INVALID_HEADERS,
    HTTP_TRANSPORT_INVALID_BODY,
} http_transport_stop_t;

typedef bool (*http_transport_cancel_cb_t)(void *context);

/* The request worker owns this handle and must destroy it after client cleanup.
 * lwIP core callbacks resolve names asynchronously; late replies carry IDs
 * instead of borrowed request pointers and are ignored after lookup detaches. */
esp_transport_handle_t http_transport_init(bool secure, int64_t deadline_us,
                                           http_transport_cancel_cb_t cancelled,
                                           void *context);
http_transport_stop_t http_transport_stop_reason(esp_transport_handle_t transport);
int http_transport_get_errno(esp_transport_handle_t transport);
bool http_transport_response_complete(esp_transport_handle_t transport);
/* The SDK streaming reader needs FIN rather than perform()'s zero EOF.
 * Select this before initializing the client; framing validation is unchanged. */
void http_transport_use_streaming_reads(esp_transport_handle_t transport);
