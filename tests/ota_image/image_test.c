/* Real SDK OTA/client/SHA code; partition writes and boot selection stay in RAM. */
#include "adapter.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "ota_manifest.h"
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(esp_app_desc_t) == 256, "P4 application description size");
_Static_assert(offsetof(esp_app_desc_t, version) == 16, "P4 application version offset");

#undef assert
#define assert(condition) do { if (!(condition)) { \
    fprintf(stderr, "Requirement failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); exit(3); } } while (0)

char http_host_last_peer[64], http_host_tls_name[256];
static esp_partition_t slot = {.subtype = 16, .size = 0x600000};
static const char *mode;
static unsigned char expected[32768], written[32768];
static size_t expected_size, written_size;
static unsigned begins, writes, ends, aborts, boots, selected, active;
static unsigned redirect_events;
static int64_t image_event_bytes, non_image_event_bytes, previous_non_image_bytes;
static int64_t response_length;
static bool response_complete, response_finished;

esp_err_t esp_event_post(const char *base, int32_t id, const void *data, size_t size, unsigned ticks)
{
    (void)ticks;
    if (!strcmp(base, "ESP_HTTP_CLIENT_EVENT")) {
        if (id == HTTP_EVENT_HEADERS_SENT) previous_non_image_bytes = 0;
        if (id == HTTP_EVENT_REDIRECT) redirect_events++;
        if (id == HTTP_EVENT_ON_FINISH) {
            assert(data && size == sizeof(esp_http_client_handle_t));
            esp_http_client_handle_t client = *(const esp_http_client_handle_t *)data;
            response_length = esp_http_client_get_content_length(client);
            response_complete = esp_http_client_is_complete_data_received(client);
            response_finished = true;
        }
        if (id == HTTP_EVENT_ON_DATA) {
            assert(data && size == sizeof(esp_http_client_on_data_t));
            const esp_http_client_on_data_t *event = data;
            if (esp_http_client_get_status_code(event->client) == 200) {
                image_event_bytes = event->data_process;
            } else {
                non_image_event_bytes += event->data_process - previous_non_image_bytes;
                previous_non_image_bytes = event->data_process;
            }
        }
    }
    return ESP_OK;
}

void *heap_caps_malloc(size_t size, unsigned capabilities)
{ (void)capabilities; return http_host_malloc(size); }

void mbedtls_platform_zeroize(void *buffer, size_t size)
{
    volatile unsigned char *bytes = buffer;
    while (size--) *bytes++ = 0;
}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *partition)
{ assert(!partition); return !strcmp(mode, "missing-slot") ? NULL : &slot; }
esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t size, esp_ota_handle_t *handle)
{
    /* The SDK passes a 32-bit sentinel through int before native size_t. */
    assert(partition == &slot && (uint32_t)size == OTA_WITH_SEQUENTIAL_WRITES && !active);
    begins++;
    if (!strcmp(mode, "begin-fault")) return ESP_FAIL;
    *handle = 1;
    active = 1;
    written_size = 0;
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{
    assert(handle == 1 && active && size <= sizeof(written) - written_size);
    writes++;
    if ((!strcmp(mode, "write-fault") && writes == 1) ||
        (!strcmp(mode, "late-write-fault") && writes == 2)) return ESP_FAIL;
    memcpy(written + written_size, data, size);
    written_size += size;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    assert(handle == 1 && active && written_size == expected_size);
    assert(!memcmp(written, expected, expected_size));
    active = 0;
    ends++;
    return !strcmp(mode, "end-fault") ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{ assert(handle == 1 && active); active = 0; aborts++; return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    assert(partition == &slot && ends == 1 && !active);
    boots++;
    if (!strcmp(mode, "boot-fault")) return ESP_FAIL;
    selected++;
    return ESP_OK;
}

int main(int argc, char **argv)
{
    assert(argc == 6);
    unsigned port = (unsigned)strtoul(argv[1], NULL, 10);
    assert(port && port <= 65535);
    mode = argv[2];
    FILE *file = fopen(argv[3], "rb");
    assert(file);
    expected_size = fread(expected, 1, sizeof(expected), file);
    assert(expected_size >= 1024 && !ferror(file) && feof(file));
    assert(!fclose(file));
    ota_manifest_t manifest = {.size = expected_size};
    snprintf(manifest.version, sizeof(manifest.version), "%s", "v0.6.0-fixture");
    char request_url[256];
    snprintf(request_url, sizeof(request_url), "http://127.0.0.1:%u/%s", port, mode);
    snprintf(manifest.url, sizeof(manifest.url), "%s", request_url);
    assert(strlen(argv[4]) == 64);
    for (size_t i = 0; i < 32; i++) {
        unsigned value;
        assert(sscanf(argv[4] + 2 * i, "%2x", &value) == 1);
        manifest.sha256[i] = (uint8_t)value;
    }
    if (!strcmp(mode, "wrong-hash")) manifest.sha256[0] ^= 1;
    if (!strcmp(mode, "wrong-size")) manifest.size++;
    uint8_t expected_digest[32];
    memcpy(expected_digest, manifest.sha256, sizeof(expected_digest));
    esp_err_t expected_error = (esp_err_t)strtol(argv[5], NULL, 0);
    WSADATA sockets;
    assert(!WSAStartup(MAKEWORD(2, 2), &sockets));
    char message[128] = {0};
    bool fetching = !strncmp(mode, "manifest-", 9);
    unsigned cycles = !strcmp(mode, "repeat") || !strcmp(mode, "manifest-repeat") ? 26 : 1;
    DWORD handles_before = 0, handles_after = 0;
    esp_err_t error = ESP_OK;
    for (unsigned cycle = 0; cycle < cycles; cycle++) {
        begins = writes = ends = aborts = boots = selected = active = redirect_events = 0;
        written_size = 0;
        image_event_bytes = non_image_event_bytes = previous_non_image_bytes = 0;
        response_length = 0;
        response_complete = false;
        response_finished = false;
        error = fetching ? ota_manifest_fetch(request_url, &manifest, message, sizeof(message)) :
                           ota_manifest_install(&manifest, message, sizeof(message));
        if (error != expected_error) {
            fprintf(stderr, "FAIL %s actual=%d expected=%d image_events=%lld non_image_events=%lld redirect_events=%u message=%s\n",
                    mode, error, expected_error, (long long)image_event_bytes, (long long)non_image_event_bytes,
                    redirect_events, message);
            return 2;
        }
        assert(!active && !host_open_sockets && !host_active_tasks && !http_host_allocations &&
               !http_host_sdk_allocations && !http_host_transports);
        if (fetching) {
            assert(!begins && !writes && !ends && !aborts && !boots && !selected);
            if (error == ESP_OK) {
                assert(manifest.size == expected_size && !memcmp(manifest.sha256, expected_digest, sizeof(expected_digest)));
                assert(!strcmp(manifest.version, "v0.6.0-fixture") && !strcmp(manifest.minimum_predecessor, "v0.5.1"));
                assert(!strcmp(manifest.url, "https://github.com/DevanMetz/Tab5OS/releases/download/v0.6.0/tab5_os.bin"));
                assert(ota_manifest_check(&manifest, "v0.5.1", message, sizeof(message)) == ESP_OK);
                if (!strcmp(mode, "manifest-close-delimited"))
                    assert(response_length == -1 && !response_complete);
            } else {
                assert(message[0]);
            }
        } else if (error == ESP_OK) {
            assert(begins == 1 && ends == 1 && !aborts && boots == 1 && selected == 1);
            assert(image_event_bytes == (int64_t)expected_size);
            if (!strncmp(mode, "redirect-", 9)) {
                assert(redirect_events == 0);
                if (strcmp(mode, "redirect-empty")) assert(non_image_event_bytes > 0);
            }
        } else {
            assert(message[0] && selected == 0);
            if (strcmp(mode, "boot-fault")) assert(!boots);
            if (!strcmp(mode, "wrong-hash") || !strcmp(mode, "corrupt-image") || !strcmp(mode, "long-image") ||
                !strcmp(mode, "long-image-matching-hash") || !strcmp(mode, "short-image-matching-hash"))
                assert(ends == 0 && aborts == 1);
            if (!strcmp(mode, "wrong-version") || !strcmp(mode, "wrong-size"))
                assert(begins == 0 && writes == 0 && ends == 0 && aborts == 0);
        }
        if (cycles > 1) {
            assert(GetProcessHandleCount(GetCurrentProcess(), &handles_after));
            if (!cycle) handles_before = handles_after;
            assert(handles_after == handles_before);
        }
    }
    printf("PASS %s result=%d bytes=%zu begins=%u writes=%u ends=%u aborts=%u boots=%u selected=%u image_events=%lld non_image_events=%lld redirect_events=%u cycles=%u handles=%lu->%lu SDK_allocations=0 transports=0 sockets=0 tasks=0\n",
           mode, error, written_size, begins, writes, ends, aborts, boots, selected,
           (long long)image_event_bytes, (long long)non_image_event_bytes, redirect_events,
           cycles, (unsigned long)handles_before, (unsigned long)handles_after);
    if (fetching && response_finished) printf("HTTP_RESPONSE %s content_length=%lld complete=%u\n",
                                              mode, (long long)response_length, (unsigned)response_complete);
    WSACleanup();
    return 0;
}
