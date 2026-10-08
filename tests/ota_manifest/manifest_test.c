/* Actual manifest fetch/check and SDK cJSON; controlled HTTP events, no image writes. */
#include "ota_manifest.h"
#include "cJSON.h"
#include "host.h"
#include "http_transport.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char valid_json[] =
    "{\"schema\":1,\"version\":\"v0.6.0-a1b2c3d4\",\"hardware\":\"m5stack-tab5\","
    "\"size\":2085280,\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
    "\"url\":\"https://github.com/DevanMetz/Tab5OS/releases/download/v0.6.0/tab5_os.bin\","
    "\"channel\":\"stable\",\"minimum_predecessor\":\"v0.5.1\"}";
static const char *body;
static size_t body_size, chunk_size, allocations, allocation_calls;
static size_t fail_allocation = SIZE_MAX;
static bool redirect_response, fail_init;
static bool response_complete = true;
static bool transport_complete = true, fail_transport;
static int transports;
static http_transport_stop_t transport_stopped;
static int status = 200, clients, checks, failures;
static esp_err_t request_error;
static esp_http_client_config_t request;

static void *json_allocate(size_t size)
{
    if (allocation_calls++ == fail_allocation) return NULL;
    void *memory = malloc(size);
    if (memory) allocations++;
    return memory;
}
static void json_free(void *memory)
{
    if (!memory) return;
    assert(allocations);
    allocations--;
    free(memory);
}

const char *esp_err_to_name(esp_err_t error) { (void)error; return "fixture error"; }
esp_err_t esp_crt_bundle_attach(void *configuration) { (void)configuration; return ESP_OK; }
int64_t esp_timer_get_time(void) { return 1234; }
esp_transport_handle_t http_transport_init(bool secure, int64_t deadline_us,
                                          http_transport_cancel_cb_t cancelled, void *context)
{
    assert(secure && deadline_us == 15001234 && cancelled && !cancelled(context));
    assert(!transports && !clients);
    if (fail_transport) return NULL;
    transports++;
    return &transports;
}
http_transport_stop_t http_transport_stop_reason(esp_transport_handle_t transport)
{ assert(transport == &transports && transports == 1 && clients == 1); return transport_stopped; }
bool http_transport_response_complete(esp_transport_handle_t transport)
{ assert(transport == &transports && transports == 1 && clients == 1); return transport_complete; }
esp_err_t esp_transport_destroy(esp_transport_handle_t transport)
{ assert(transport == &transports && transports == 1 && !clients); transports--; return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(!clients && config->crt_bundle_attach == esp_crt_bundle_attach);
    assert(config->transport == &transports && transports == 1);
    assert(config->timeout_ms == 15000 && config->max_redirection_count == 5);
    if (fail_init) return NULL;
    request = *config;
    clients++;
    return &request;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name,
                                     const char *value)
{
    assert(client == &request && !strcmp(name, "User-Agent") && !strcmp(value, "Tab5OS/1.0"));
    return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    assert(client == &request);
    esp_http_client_event_t event = {.event_id = HTTP_EVENT_ON_DATA, .user_data = request.user_data,
                                     .client = client};
    if (redirect_response) {
        char previous[1537];
        memset(previous, 'x', sizeof(previous));
        event.data = previous;
        event.data_len = (int)sizeof(previous);
        assert(request.event_handler(&event) == ESP_OK);
        event.event_id = HTTP_EVENT_REDIRECT;
        assert(request.event_handler(&event) == ESP_OK);
        event.event_id = HTTP_EVENT_ON_DATA;
    }
    event.event_id = HTTP_EVENT_ON_HEADER;
    event.header_key = "Content-Length";
    assert(request.event_handler(&event) == ESP_OK);
    event.event_id = HTTP_EVENT_ON_DATA;
    event.header_key = NULL;
    for (size_t offset = 0; offset < body_size;) {
        size_t count = body_size - offset;
        if (count > chunk_size) count = chunk_size;
        event.data = (void *)(body + offset);
        event.data_len = (int)count;
        assert(request.event_handler(&event) == ESP_OK);
        offset += count;
    }
    return request_error;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client)
{ assert(client == &request); return status; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client)
{ assert(client == &request && request_error == ESP_OK); return response_complete; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{ assert(client == &request && clients == 1); clients--; return ESP_OK; }

/* These fail closed if a manifest test ever reaches image/hash operations. */
#define NO_IMAGE() assert(0 && "Manifest test attempted an image operation")
void http_transport_use_streaming_reads(esp_transport_handle_t transport)
{ (void)transport; NO_IMAGE(); }
void mbedtls_sha256_init(mbedtls_sha256_context *context) { (void)context; NO_IMAGE(); }
void mbedtls_sha256_free(mbedtls_sha256_context *context) { (void)context; NO_IMAGE(); }
int mbedtls_sha256_starts(mbedtls_sha256_context *context, int is224)
{ (void)context; (void)is224; NO_IMAGE(); return -1; }
int mbedtls_sha256_update(mbedtls_sha256_context *context, const unsigned char *data, size_t size)
{ (void)context; (void)data; (void)size; NO_IMAGE(); return -1; }
int mbedtls_sha256_finish(mbedtls_sha256_context *context, unsigned char output[32])
{ (void)context; (void)output; NO_IMAGE(); return -1; }
esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *config, esp_https_ota_handle_t *handle)
{ (void)config; (void)handle; NO_IMAGE(); return ESP_FAIL; }
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t handle, esp_app_desc_t *description)
{ (void)handle; (void)description; NO_IMAGE(); return ESP_FAIL; }
int esp_https_ota_get_image_size(esp_https_ota_handle_t handle)
{ (void)handle; NO_IMAGE(); return -1; }
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle)
{ (void)handle; NO_IMAGE(); return ESP_FAIL; }
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle)
{ (void)handle; NO_IMAGE(); return false; }
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle)
{ (void)handle; NO_IMAGE(); return ESP_FAIL; }
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle)
{ (void)handle; NO_IMAGE(); return ESP_FAIL; }

static void result(const char *name, esp_err_t actual, esp_err_t expected)
{
    checks++;
    if (actual != expected) {
        printf("FAIL %s actual=%d expected=%d\n", name, actual, expected);
        failures++;
    }
    assert(!clients && !allocations && !transports);
}
static ota_manifest_t fetch(const char *name, const char *json, size_t size, esp_err_t expected)
{
    body = json;
    body_size = size;
    allocation_calls = 0;
    ota_manifest_t manifest = {0};
    char message[96] = {0};
    esp_err_t error = ota_manifest_fetch("https://fixture.invalid/manifest", &manifest,
                                        message, sizeof(message));
    result(name, error, expected);
    if (error != ESP_OK) assert(message[0]);
    if (error == ESP_ERR_INVALID_RESPONSE) {
        ota_manifest_t empty = {0};
        assert(!memcmp(&manifest, &empty, sizeof(manifest)));
    }
    if (!strcmp(name, "deep-array-before-recursion") || !strcmp(name, "incomplete-body"))
        assert(allocation_calls == 0);
    return manifest;
}
static void changed(const char *name, const char *before, const char *after, esp_err_t expected)
{
    char json[1538];
    const char *at = strstr(valid_json, before);
    assert(at && strlen(valid_json) + strlen(after) < sizeof(json) + strlen(before));
    size_t prefix = (size_t)(at - valid_json);
    memcpy(json, valid_json, prefix);
    snprintf(json + prefix, sizeof(json) - prefix, "%s%s", after, at + strlen(before));
    fetch(name, json, strlen(json), expected);
}
static void appended(const char *name, const char *extra, esp_err_t expected)
{
    char json[1538];
    size_t length = strlen(valid_json);
    memcpy(json, valid_json, length - 1);
    snprintf(json + length - 1, sizeof(json) - length + 1, ",%s}", extra);
    fetch(name, json, strlen(json), expected);
}

int main(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_allocate, .free_fn = json_free};
    cJSON_InitHooks(&hooks);
    chunk_size = SIZE_MAX;
    ota_manifest_self_test();
    assert(!allocations);
    ota_manifest_t manifest = fetch("valid", valid_json, strlen(valid_json), ESP_OK);
    assert(manifest.size == 2085280 && manifest.sha256[0] == 0x01 && manifest.sha256[31] == 0xef);
    assert(!strcmp(manifest.version, "v0.6.0-a1b2c3d4"));
    assert(!strcmp(manifest.minimum_predecessor, "v0.5.1"));

    for (chunk_size = 1; chunk_size <= strlen(valid_json); chunk_size++)
        fetch("fragmented-valid", valid_json, strlen(valid_json), ESP_OK);
    chunk_size = SIZE_MAX;
    redirect_response = true;
    fetch("redirect-resets-overflow", valid_json, strlen(valid_json), ESP_OK);
    redirect_response = false;
    char padded[1538];
    memcpy(padded, valid_json, strlen(valid_json));
    memset(padded + strlen(valid_json), ' ', 1537 - strlen(valid_json));
    padded[1537] = '\0';
    fetch("exact-cap", padded, 1536, ESP_OK);
    fetch("overflow", padded, 1537, ESP_ERR_INVALID_SIZE);
    fetch("empty", "", 0, ESP_ERR_INVALID_SIZE);
    fetch("truncated", valid_json, strlen(valid_json) - 1, ESP_ERR_INVALID_RESPONSE);
    response_complete = false;
    fetch("incomplete-body", valid_json, strlen(valid_json), ESP_ERR_INVALID_SIZE);
    status = 404;
    fetch("incomplete-non-200", valid_json, strlen(valid_json), ESP_FAIL);
    response_complete = true;
    status = 404;
    fetch("non-200", valid_json, strlen(valid_json), ESP_FAIL);
    status = 200;
    request_error = ESP_FAIL;
    fetch("request-error", valid_json, strlen(valid_json), ESP_FAIL);
    request_error = ESP_OK;
    fail_init = true;
    fetch("client-allocation-error", valid_json, strlen(valid_json), ESP_ERR_NO_MEM);
    fail_init = false;
    fail_transport = true;
    fetch("transport-allocation-error", valid_json, strlen(valid_json), ESP_ERR_NO_MEM);
    fail_transport = false;
    transport_complete = false;
    fetch("incomplete-transport", valid_json, strlen(valid_json), ESP_ERR_INVALID_SIZE);
    transport_complete = true;
    transport_stopped = HTTP_TRANSPORT_DEADLINE;
    request_error = ESP_FAIL;
    fetch("transport-deadline", valid_json, strlen(valid_json), ESP_ERR_TIMEOUT);
    transport_stopped = HTTP_TRANSPORT_INVALID_HEADERS;
    fetch("invalid-headers", valid_json, strlen(valid_json), ESP_ERR_INVALID_RESPONSE);
    transport_stopped = HTTP_TRANSPORT_INVALID_BODY;
    fetch("invalid-body", valid_json, strlen(valid_json), ESP_ERR_INVALID_RESPONSE);
    transport_stopped = HTTP_TRANSPORT_RUNNING;
    request_error = ESP_OK;

    char hidden[sizeof(valid_json) + 8];
    memcpy(hidden, valid_json, sizeof(valid_json));
    memcpy(hidden + sizeof(valid_json), "garbage", 8);
    fetch("raw-nul-suffix", hidden, sizeof(hidden), ESP_ERR_INVALID_RESPONSE);
    fetch("raw-nul-final-byte", valid_json, sizeof(valid_json), ESP_ERR_INVALID_RESPONSE);
    changed("escaped-nul-version", "v0.6.0-a1b2c3d4", "v0.6.0-a1b2c3d4\\u0000hidden", ESP_ERR_INVALID_RESPONSE);
    changed("escaped-nul-hardware", "m5stack-tab5", "m5stack-tab5\\u0000hidden", ESP_ERR_INVALID_RESPONSE);
    changed("escaped-nul-channel", "stable", "stable\\u0000beta", ESP_ERR_INVALID_RESPONSE);
    changed("escaped-nul-key", "\"schema\"", "\"schema\\u0000hidden\"", ESP_ERR_INVALID_RESPONSE);
    changed("escaped-nul-sha", "abcdef\"", "abcdef\\u0000hidden\"", ESP_ERR_INVALID_RESPONSE);
    changed("escaped-nul-url", "tab5_os.bin", "tab5_os.bin\\u0000hidden", ESP_ERR_INVALID_RESPONSE);
    changed("escaped-nul-predecessor", "v0.5.1", "v0.5.1\\u0000hidden", ESP_ERR_INVALID_RESPONSE);
    changed("literal-backslash-u", "v0.6.0-a1b2c3d4", "v0.6.0-\\\\u0000", ESP_OK);
    changed("braces-in-string", "v0.6.0-a1b2c3d4", "v0.6.0-{[]}", ESP_OK);
    changed("escaped-quote", "v0.6.0-a1b2c3d4", "v0.6.0-\\\"{[]}", ESP_OK);
    changed("json-whitespace", "\"schema\":1", "\"schema\":\t\r\n1", ESP_OK);
    changed("escaped-ascii", "m5stack-tab5", "m5stack-\\u0074ab5", ESP_OK);
    changed("bad-hardware", "m5stack-tab5", "m5stack-other", ESP_ERR_INVALID_RESPONSE);
    changed("beta-channel", "stable", "beta", ESP_ERR_INVALID_RESPONSE);
    changed("schema-version", "\"schema\":1", "\"schema\":2", ESP_ERR_INVALID_RESPONSE);
    changed("schema-string", "\"schema\":1", "\"schema\":\"1\"", ESP_ERR_INVALID_RESPONSE);
    changed("empty-version", "v0.6.0-a1b2c3d4", "", ESP_ERR_INVALID_RESPONSE);
    changed("exact-version", "v0.6.0-a1b2c3d4", "v0.6.0-123456789012345678901234", ESP_OK);
    changed("oversized-version", "v0.6.0-a1b2c3d4", "v0.6.0-1234567890123456789012345678", ESP_ERR_INVALID_RESPONSE);
    changed("zero-size", "2085280", "0", ESP_ERR_INVALID_RESPONSE);
    changed("negative-size", "2085280", "-1", ESP_ERR_INVALID_RESPONSE);
    changed("fractional-size", "2085280", "1.5", ESP_ERR_INVALID_RESPONSE);
    changed("overflow-size", "2085280", "1e400", ESP_ERR_INVALID_RESPONSE);
    changed("host-size-boundary", "2085280", "18446744073709551616", ESP_ERR_INVALID_RESPONSE);
    changed("uint32-size-boundary", "2085280", "4294967296", ESP_ERR_INVALID_RESPONSE);
    changed("integer-exponent", "2085280", "1e3", ESP_OK);
    changed("wrong-sha-character", "0123456789abcdef", "g123456789abcdef", ESP_ERR_INVALID_RESPONSE);
    changed("uppercase-sha", "0123456789abcdef", "0123456789ABCDEF", ESP_OK);
    changed("raw-tab-in-string", "v0.6.0-a1b2c3d4", "v0.6.0-\tbad", ESP_ERR_INVALID_RESPONSE);
    changed("non-json-whitespace", "\"schema\":1", "\"schema\":\v1", ESP_ERR_INVALID_RESPONSE);
    appended("duplicate-version", "\"version\":\"v1.0.0\"", ESP_ERR_INVALID_RESPONSE);
    appended("duplicate-hardware", "\"hardware\":\"other\"", ESP_ERR_INVALID_RESPONSE);
    appended("unknown-member", "\"extra\":1", ESP_ERR_INVALID_RESPONSE);
    appended("nested-object", "\"extra\":{}", ESP_ERR_INVALID_RESPONSE);
    appended("nested-array", "\"extra\":[]", ESP_ERR_INVALID_RESPONSE);
    fetch("array-root", "[]", 2, ESP_ERR_INVALID_RESPONSE);
    fetch("missing-members", "{\"schema\":1}", 12, ESP_ERR_INVALID_RESPONSE);
    char deep[1200], nested[800];
    memset(nested, '[', 300);
    memset(nested + 300, ']', 300);
    nested[600] = '\0';
    snprintf(deep, sizeof(deep), "\"extra\":%s", nested);
    appended("deep-array-before-recursion", deep, ESP_ERR_INVALID_RESPONSE);

    size_t valid_allocations;
    fetch("allocation-baseline", valid_json, strlen(valid_json), ESP_OK);
    valid_allocations = allocation_calls;
    for (fail_allocation = 0; fail_allocation < valid_allocations; fail_allocation++)
        fetch("json-allocation-failure", valid_json, strlen(valid_json), ESP_ERR_INVALID_RESPONSE);
    fail_allocation = SIZE_MAX;
    fetch("allocation-recovery", valid_json, strlen(valid_json), ESP_OK);

    char message[96];
#define CHECK_VERSION(name, version, expected) \
    result(name, ota_manifest_check(&manifest, version, message, sizeof(message)), expected)
    CHECK_VERSION("newer-compatible", "v0.5.1-dirty", ESP_OK);
    CHECK_VERSION("minimum-too-old", "v0.4.9", ESP_ERR_INVALID_VERSION);
    CHECK_VERSION("same-version", "v0.6.0", ESP_ERR_INVALID_VERSION);
    CHECK_VERSION("downgrade", "v0.7.0", ESP_ERR_INVALID_VERSION);
    CHECK_VERSION("invalid-current", "broken", ESP_ERR_INVALID_VERSION);
    manifest.size = 0x600001U;
    CHECK_VERSION("oversized-image", "v0.5.1", ESP_ERR_INVALID_SIZE);
    manifest.size = 0x600000U;
    CHECK_VERSION("exact-slot", "v0.5.1", ESP_OK);
    snprintf(manifest.url, sizeof(manifest.url), "%s", "https://example.com/tab5_os.bin");
    CHECK_VERSION("untrusted-url", "v0.5.1", ESP_ERR_INVALID_ARG);
    assert(!clients && !allocations);
    printf("%s %d OTA manifest checks failures=%d clients=%d json_allocations=%zu (no image operations)\n",
           failures ? "FAIL" : "PASS", checks, failures, clients, allocations);
    cJSON_InitHooks(NULL);
    return failures ? 1 : 0;
}
