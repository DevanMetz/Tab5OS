#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_INVALID_VERSION 0x10a
#define ESP_ERR_HTTPS_OTA_IN_PROGRESS 0x9001

const char *esp_err_to_name(esp_err_t error);
esp_err_t esp_crt_bundle_attach(void *configuration);

typedef enum {
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_REDIRECT,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
} esp_http_client_event_id_t;
typedef struct {
    esp_http_client_event_id_t event_id;
    void *data;
    int data_len;
    void *user_data;
    void *client;
    char *header_key;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    esp_err_t (*crt_bundle_attach)(void *);
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
    int max_redirection_count;
    bool keep_alive_enable;
    void *transport;
} esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name,
                                     const char *value);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

typedef struct { char version[32]; } esp_app_desc_t;
typedef struct { const esp_http_client_config_t *http_config; } esp_https_ota_config_t;
typedef void *esp_https_ota_handle_t;
esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *config, esp_https_ota_handle_t *handle);
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t handle, esp_app_desc_t *description);
int esp_https_ota_get_image_size(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle);
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle);

typedef struct { unsigned unused; } mbedtls_sha256_context;
void mbedtls_sha256_init(mbedtls_sha256_context *context);
void mbedtls_sha256_free(mbedtls_sha256_context *context);
int mbedtls_sha256_starts(mbedtls_sha256_context *context, int is224);
int mbedtls_sha256_update(mbedtls_sha256_context *context, const unsigned char *data, size_t size);
int mbedtls_sha256_finish(mbedtls_sha256_context *context, unsigned char output[32]);
