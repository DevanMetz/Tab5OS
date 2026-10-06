#pragma once
#include "esp_err.h"
#include <net/if.h>
typedef enum { ESP_TLS_VER_ANY, ESP_TLS_VER_TLS_1_2, ESP_TLS_VER_TLS_1_3, ESP_TLS_VER_TLS_MAX } esp_tls_proto_ver_t;
typedef enum { ESP_TLS_AF_UNSPEC, ESP_TLS_AF_INET, ESP_TLS_AF_INET6 } esp_tls_addr_family_t;
typedef struct { const unsigned char *key; unsigned size; const char *hint; } psk_hint_key_t;
typedef struct esp_tls_last_error { int last_error, sys_errno; } esp_tls_last_error_t;
typedef esp_tls_last_error_t *esp_tls_error_handle_t;
#define ESP_TLS_ERR_TYPE_SYSTEM 0
#define ESP_ERR_ESP_TLS_TCP_CLOSED_FIN 0x801
#define ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT 0x802
#define ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST 0x803
static inline esp_err_t esp_tls_get_and_clear_error_type(esp_tls_error_handle_t handle, int type, int *value)
{
    (void)type;
    if (!handle) return ESP_ERR_INVALID_ARG;
    *value = handle->sys_errno; handle->sys_errno = 0; return ESP_OK;
}
static inline esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t handle, int *code, int *flags)
{
    (void)handle; (void)code; (void)flags;
    return ESP_ERR_INVALID_ARG;
}
