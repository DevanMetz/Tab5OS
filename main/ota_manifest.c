#include "ota_manifest.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"

#define OTA_MANIFEST_SCHEMA 1
#define OTA_MANIFEST_JSON_MAX 1536
#define OTA_HARDWARE "m5stack-tab5"
#define OTA_CHANNEL "stable"
#define OTA_IMAGE_URL_PREFIX "https://github.com/DevanMetz/Tab5OS/releases/download/"

typedef struct {
    char body[OTA_MANIFEST_JSON_MAX + 1];
    size_t length;
    bool overflow;
} manifest_response_t;

typedef struct {
    mbedtls_sha256_context context;
    size_t bytes;
    bool failed;
} image_hash_t;

static void set_message(char *message, size_t size, const char *text)
{
    if (message && size) snprintf(message, size, "%s", text);
}

static bool copy_ascii(cJSON *root, const char *key, char *output, size_t capacity)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    size_t length = strlen(item->valuestring);
    if (!length || length >= capacity) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)item->valuestring[i];
        if (byte < 0x21 || byte > 0x7e) return false;
    }
    memcpy(output, item->valuestring, length + 1);
    return true;
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    value = (char)tolower((unsigned char)value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static bool decode_sha256(const char *text, uint8_t output[32])
{
    if (!text || strlen(text) != 64) return false;
    for (size_t i = 0; i < 32; i++) {
        int high = hex_nibble(text[i * 2]);
        int low = hex_nibble(text[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool parse_manifest(const char *json, ota_manifest_t *manifest)
{
    memset(manifest, 0, sizeof(*manifest));
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(json, &end, true);
    cJSON *schema = root ? cJSON_GetObjectItemCaseSensitive(root, "schema") : NULL;
    cJSON *hardware = root ? cJSON_GetObjectItemCaseSensitive(root, "hardware") : NULL;
    cJSON *channel = root ? cJSON_GetObjectItemCaseSensitive(root, "channel") : NULL;
    cJSON *size = root ? cJSON_GetObjectItemCaseSensitive(root, "size") : NULL;
    cJSON *sha256 = root ? cJSON_GetObjectItemCaseSensitive(root, "sha256") : NULL;
    bool valid = cJSON_IsObject(root) && end && *end == '\0' &&
                 cJSON_IsNumber(schema) && schema->valuedouble == OTA_MANIFEST_SCHEMA &&
                 cJSON_IsString(hardware) && !strcmp(hardware->valuestring, OTA_HARDWARE) &&
                 cJSON_IsString(channel) && !strcmp(channel->valuestring, OTA_CHANNEL) &&
                 cJSON_IsNumber(size) && isfinite(size->valuedouble) && size->valuedouble > 0 &&
                 size->valuedouble <= SIZE_MAX &&
                 (double)(size_t)size->valuedouble == size->valuedouble &&
                 copy_ascii(root, "version", manifest->version, sizeof(manifest->version)) &&
                 copy_ascii(root, "url", manifest->url, sizeof(manifest->url)) &&
                 copy_ascii(root, "minimum_predecessor", manifest->minimum_predecessor,
                            sizeof(manifest->minimum_predecessor)) &&
                 cJSON_IsString(sha256) && decode_sha256(sha256->valuestring, manifest->sha256);
    if (valid) manifest->size = (size_t)size->valuedouble;
    cJSON_Delete(root);
    return valid;
}

static bool parse_version(const char *text, unsigned parts[3])
{
    if (*text == 'v' || *text == 'V') text++;
    for (size_t i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*text)) return false;
        char *end;
        unsigned long value = strtoul(text, &end, 10);
        if (value > 65535 || end == text) return false;
        parts[i] = (unsigned)value;
        if (i < 2) {
            if (*end != '.') return false;
            text = end + 1;
        } else {
            return *end == '\0' || *end == '-' || *end == '+';
        }
    }
    return false;
}

static int compare_versions(const char *left, const char *right, bool *valid)
{
    unsigned a[3], b[3];
    *valid = parse_version(left, a) && parse_version(right, b);
    if (!*valid) return 0;
    for (size_t i = 0; i < 3; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

static bool image_url_valid(const char *url)
{
    static const char suffix[] = "/tab5_os.bin";
    size_t length = strlen(url);
    return !strncmp(url, OTA_IMAGE_URL_PREFIX, strlen(OTA_IMAGE_URL_PREFIX)) &&
           length > strlen(OTA_IMAGE_URL_PREFIX) + strlen(suffix) &&
           !strcmp(url + length - strlen(suffix), suffix) &&
           !strstr(url, "..") && !strchr(url, '?') && !strchr(url, '#');
}

static esp_err_t manifest_event(esp_http_client_event_t *event)
{
    manifest_response_t *response = event->user_data;
    if (!response) return ESP_OK;
    if (event->event_id == HTTP_EVENT_REDIRECT) {
        response->length = 0;
        response->overflow = false;
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t available = OTA_MANIFEST_JSON_MAX - response->length;
        size_t copy = (size_t)event->data_len < available ? (size_t)event->data_len : available;
        memcpy(response->body + response->length, event->data, copy);
        response->length += copy;
        if (copy != (size_t)event->data_len) response->overflow = true;
        response->body[response->length] = '\0';
    }
    return ESP_OK;
}

esp_err_t ota_manifest_fetch(const char *manifest_url, ota_manifest_t *manifest,
                             char *message, size_t message_size)
{
    manifest_response_t *response = calloc(1, sizeof(*response));
    if (!response) {
        set_message(message, message_size, "Manifest buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_config_t config = {
        .url = manifest_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = manifest_event,
        .user_data = response,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(response);
        set_message(message, message_size, "Could not initialize manifest request");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "User-Agent", "Tab5OS/1.0");
    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (error == ESP_OK && status != 200) {
        set_message(message, message_size, "Manifest server did not return HTTP 200");
        error = ESP_FAIL;
    } else if (error == ESP_OK && (response->overflow || !response->length)) {
        set_message(message, message_size, response->overflow ? "Manifest exceeds 1536 bytes" :
                                                               "Manifest response was empty");
        error = ESP_ERR_INVALID_SIZE;
    } else if (error == ESP_OK && !parse_manifest(response->body, manifest)) {
        set_message(message, message_size, "Manifest is malformed or incompatible");
        error = ESP_ERR_INVALID_RESPONSE;
    } else if (error != ESP_OK) {
        snprintf(message, message_size, "Manifest request failed: %s", esp_err_to_name(error));
    }
    free(response);
    return error;
}

esp_err_t ota_manifest_check(const ota_manifest_t *manifest, const char *current_version,
                             char *message, size_t message_size)
{
    bool valid;
    int minimum = compare_versions(current_version, manifest->minimum_predecessor, &valid);
    if (!valid) {
        set_message(message, message_size, "Current or minimum firmware version is invalid");
        return ESP_ERR_INVALID_VERSION;
    }
    if (minimum < 0) {
        snprintf(message, message_size, "Install %s before this update", manifest->minimum_predecessor);
        return ESP_ERR_INVALID_VERSION;
    }
    int update = compare_versions(manifest->version, current_version, &valid);
    if (!valid || update <= 0) {
        set_message(message, message_size, valid ? "No newer stable update is available" :
                                                  "Manifest firmware version is invalid");
        return ESP_ERR_INVALID_VERSION;
    }
    if (manifest->size > 0x600000U) {
        set_message(message, message_size, "Manifest image exceeds the Tab5 OTA slot");
        return ESP_ERR_INVALID_SIZE;
    }
    if (!image_url_valid(manifest->url)) {
        set_message(message, message_size, "Manifest image URL is outside the trusted release path");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t image_hash_event(esp_http_client_event_t *event)
{
    image_hash_t *hash = event->user_data;
    if (!hash) return ESP_OK;
    if (event->event_id == HTTP_EVENT_REDIRECT) {
        hash->bytes = 0;
        hash->failed = mbedtls_sha256_starts(&hash->context, false) != 0;
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0 && !hash->failed) {
        hash->failed = mbedtls_sha256_update(&hash->context, event->data,
                                             (size_t)event->data_len) != 0;
        hash->bytes += (size_t)event->data_len;
    }
    return ESP_OK;
}

esp_err_t ota_manifest_install(const ota_manifest_t *manifest,
                               char *message, size_t message_size)
{
    image_hash_t hash = {0};
    mbedtls_sha256_init(&hash.context);
    if (mbedtls_sha256_starts(&hash.context, false) != 0) {
        mbedtls_sha256_free(&hash.context);
        set_message(message, message_size, "Could not initialize image verification");
        return ESP_FAIL;
    }
    esp_http_client_config_t http = {
        .url = manifest->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = image_hash_event,
        .user_data = &hash,
        .timeout_ms = 30000,
        .buffer_size = 1024,
        .buffer_size_tx = 1536,
        .keep_alive_enable = true,
        .max_redirection_count = 5,
    };
    esp_https_ota_config_t config = {.http_config = &http};
    esp_https_ota_handle_t handle = NULL;
    esp_err_t error = esp_https_ota_begin(&config, &handle);
    if (error != ESP_OK) {
        snprintf(message, message_size, "Image request failed: %s", esp_err_to_name(error));
        goto done;
    }
    esp_app_desc_t description;
    error = esp_https_ota_get_img_desc(handle, &description);
    if (error != ESP_OK || strcmp(description.version, manifest->version)) {
        set_message(message, message_size, error == ESP_OK ? "Image version does not match manifest" :
                                                             "Could not read image version");
        if (error == ESP_OK) error = ESP_ERR_INVALID_VERSION;
        goto abort;
    }
    int announced_size = esp_https_ota_get_image_size(handle);
    if (announced_size >= 0 && (size_t)announced_size != manifest->size) {
        set_message(message, message_size, "Image Content-Length does not match manifest");
        error = ESP_ERR_INVALID_SIZE;
        goto abort;
    }
    do {
        error = esp_https_ota_perform(handle);
    } while (error == ESP_ERR_HTTPS_OTA_IN_PROGRESS);
    if (error != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        if (error == ESP_OK) error = ESP_ERR_INVALID_SIZE;
        snprintf(message, message_size, "Image download failed: %s", esp_err_to_name(error));
        goto abort;
    }
    uint8_t digest[32];
    if (hash.failed || hash.bytes != manifest->size ||
        mbedtls_sha256_finish(&hash.context, digest) != 0 ||
        memcmp(digest, manifest->sha256, sizeof(digest))) {
        set_message(message, message_size, hash.bytes != manifest->size ?
                    "Downloaded image size does not match manifest" :
                    "Downloaded image SHA-256 does not match manifest");
        error = ESP_ERR_INVALID_CRC;
        goto abort;
    }
    error = esp_https_ota_finish(handle);
    handle = NULL;
    if (error != ESP_OK)
        snprintf(message, message_size, "Image validation failed: %s", esp_err_to_name(error));
    goto done;

abort:
    esp_https_ota_abort(handle);
done:
    mbedtls_sha256_free(&hash.context);
    return error;
}

void ota_manifest_self_test(void)
{
    static const char valid_json[] =
        "{\"schema\":1,\"version\":\"v0.6.0-a1b2c3d4\",\"hardware\":\"m5stack-tab5\","
        "\"size\":2085280,\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"url\":\"https://github.com/DevanMetz/Tab5OS/releases/download/v0.6.0/tab5_os.bin\","
        "\"channel\":\"stable\",\"minimum_predecessor\":\"v0.5.1\"}";
    ota_manifest_t manifest;
    assert(parse_manifest(valid_json, &manifest));
    assert(manifest.size == 2085280 && manifest.sha256[0] == 0x01 && manifest.sha256[31] == 0xef);
    char message[96];
    assert(ota_manifest_check(&manifest, "v0.5.1-dirty", message, sizeof(message)) == ESP_OK);
    assert(ota_manifest_check(&manifest, "v0.4.9", message, sizeof(message)) == ESP_ERR_INVALID_VERSION);
    assert(image_url_valid(manifest.url));
    snprintf(manifest.url, sizeof(manifest.url), "%s", "https://example.com/tab5_os.bin");
    assert(!image_url_valid(manifest.url));
    snprintf(manifest.url, sizeof(manifest.url), "%s",
             "https://github.com/DevanMetz/Tab5OS/releases/download/v0.6.0/tab5_os.bin");
    snprintf(manifest.version, sizeof(manifest.version), "v0.5.1");
    assert(ota_manifest_check(&manifest, "v0.5.1-dirty", message, sizeof(message)) == ESP_ERR_INVALID_VERSION);
    assert(!parse_manifest("{\"schema\":1}", &manifest));
    assert(!parse_manifest("{\"schema\":1} trailing", &manifest));
}
