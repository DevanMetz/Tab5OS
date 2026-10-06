#pragma once
#include "esp_err.h"
#include <net/if.h>
typedef enum { ESP_TLS_VER_ANY, ESP_TLS_VER_TLS_1_2, ESP_TLS_VER_TLS_1_3, ESP_TLS_VER_TLS_MAX } esp_tls_proto_ver_t;
typedef enum { ESP_TLS_AF_UNSPEC, ESP_TLS_AF_INET, ESP_TLS_AF_INET6 } esp_tls_addr_family_t;
typedef struct psk_key_hint { const uint8_t *key; const size_t key_size; const char *hint; } psk_hint_key_t;
typedef struct esp_tls_last_error { esp_err_t last_error; int esp_tls_error_code, esp_tls_flags; } esp_tls_last_error_t;
typedef esp_tls_last_error_t *esp_tls_error_handle_t;
#define ESP_TLS_ERR_TYPE_SYSTEM 1
#define ESP_ERR_ESP_TLS_TCP_CLOSED_FIN 0x8008
#define ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT 0x8006
#define ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST 0x8004
static inline esp_err_t esp_tls_get_and_clear_error_type(esp_tls_error_handle_t h, int type, int *value)
{
    (void)type;
    if (!h) return ESP_ERR_INVALID_ARG;
    *value = h->esp_tls_error_code; h->esp_tls_error_code = 0; return ESP_OK;
}
static inline esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t h, int *code, int *flags)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    *code = h->esp_tls_error_code; *flags = h->esp_tls_flags;
    h->esp_tls_error_code = h->esp_tls_flags = 0;
    esp_err_t error = h->last_error; h->last_error = 0; return error;
}
