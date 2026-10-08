/* Actual main.c result functions; NVS and OTA state are controlled RAM APIs. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 0x102, ESP_ERR_NOT_FOUND = 0x105,
       ESP_ERR_NOT_SUPPORTED = 0x106, ESP_ERR_NVS_NOT_FOUND = 0x1102, ESP_ERR_NVS_INVALID_LENGTH = 0x110c };
typedef enum { ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID,
               ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED = -1 } esp_ota_img_states_t;
typedef unsigned nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef struct { unsigned id; } esp_partition_t;
typedef struct { char version[32]; } esp_app_desc_t;

static esp_err_t nvs_init_error;
static char ota_last_result[64];
static char saved_result[64], saved_pending[32];
static bool has_result, has_pending, invalid_present;
static esp_app_desc_t running_description, invalid_description;
static const esp_partition_t running_partition = {1}, invalid_partition = {2};
static unsigned handles, readonly_opens, write_opens, sets, erases, commits, state_reads, warnings;
static esp_err_t open_error, pending_error, state_error, description_error, set_error, erase_error, commit_error;
static esp_ota_img_states_t running_state;
static unsigned cases, failures;

static const char *esp_err_to_name(esp_err_t error) { return error == ESP_OK ? "ESP_OK" : "controlled error"; }
static void fixture_warning(const char *format, ...)
{
    char message[128];
    va_list args;
    va_start(args, format);
    assert(vsnprintf(message, sizeof(message), format, args) > 0);
    va_end(args);
    warnings++;
}
#define ESP_LOGW(tag, ...) ((void)(tag), fixture_warning(__VA_ARGS__))

static esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "tab5") && !handles);
    if (mode == NVS_READONLY) readonly_opens++; else { assert(mode == NVS_READWRITE); write_opens++; }
    if (open_error) return open_error;
    *handle = 1;
    handles++;
    return ESP_OK;
}
static void nvs_close(nvs_handle_t handle) { assert(handle == 1 && handles == 1); handles--; }
static esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size)
{
    assert(handle == 1 && handles == 1);
    bool pending = !strcmp(key, "ota_pending");
    assert(pending || !strcmp(key, "ota_result"));
    if (pending && pending_error) return pending_error;
    if (!(pending ? has_pending : has_result)) return ESP_ERR_NVS_NOT_FOUND;
    const char *saved = pending ? saved_pending : saved_result;
    size_t needed = strlen(saved) + 1;
    if (*size < needed) { *size = needed; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(value, saved, needed);
    *size = needed;
    return ESP_OK;
}
static esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    assert(handle == 1 && handles == 1);
    sets++;
    if (set_error) return set_error;
    bool pending = !strcmp(key, "ota_pending");
    assert(pending || !strcmp(key, "ota_result"));
    assert(strlen(value) < (pending ? sizeof(saved_pending) : sizeof(saved_result)));
    strcpy(pending ? saved_pending : saved_result, value);
    if (pending) has_pending = true; else has_result = true;
    return ESP_OK;
}
static esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(handle == 1 && handles == 1 && !strcmp(key, "ota_pending"));
    erases++;
    if (erase_error) return erase_error;
    if (!has_pending) return ESP_ERR_NVS_NOT_FOUND;
    has_pending = false;
    saved_pending[0] = '\0';
    return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t handle)
{ assert(handle == 1 && handles == 1); commits++; return commit_error; }
static const esp_app_desc_t *esp_app_get_description(void) { return &running_description; }
static const esp_partition_t *esp_ota_get_running_partition(void) { return &running_partition; }
static esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *state)
{
    assert(partition == &running_partition);
    state_reads++;
    *state = running_state; /* A failed return must take precedence over this value. */
    return state_error;
}
static const esp_partition_t *esp_ota_get_last_invalid_partition(void)
{ return invalid_present ? &invalid_partition : NULL; }
static esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition, esp_app_desc_t *description)
{
    assert(partition == &invalid_partition);
    if (description_error) return description_error;
    *description = invalid_description;
    return ESP_OK;
}

#include "ota_result.inc"

static void reset(void)
{
    assert(!handles);
    readonly_opens = write_opens = sets = erases = commits = state_reads = warnings = 0;
    nvs_init_error = open_error = pending_error = state_error = description_error = set_error = erase_error = commit_error = ESP_OK;
    has_result = has_pending = true;
    invalid_present = false;
    running_state = ESP_OTA_IMG_VALID;
    strcpy(ota_last_result, "none recorded");
    strcpy(saved_result, "Previous result");
    strcpy(saved_pending, "v0.7.0-12345678");
    strcpy(running_description.version, saved_pending);
    strcpy(invalid_description.version, saved_pending);
}
static void check(const char *name, const char *message, bool retain_pending, unsigned writes, unsigned reads)
{
    bool pass = !strcmp(ota_last_result, message) && has_pending == retain_pending &&
                !handles && write_opens == writes && state_reads == reads;
    if (!writes) pass = pass && !sets && !erases && !commits && !strcmp(saved_result, "Previous result");
    else if (!set_error && !erase_error && !commit_error)
        pass = pass && sets == writes && erases == writes && commits == writes && !strcmp(saved_result, message);
    cases++;
    failures += !pass;
    printf("%s %s pending=%u writes=%u sets=%u erases=%u commits=%u state_reads=%u handles=%u status=\"%s\"\n",
           pass ? "PASS" : "FAIL", name, (unsigned)has_pending, write_opens, sets, erases, commits, state_reads, handles, ota_last_result);
}

int main(void)
{
    const char *installed = "Installed v0.7.0-12345678";
    const char *pending = "Installing v0.7.0-12345678; health pending";
    const char *unconfirmed = "Running v0.7.0-12345678; health not confirmed";
    const char *unavailable = "Running v0.7.0-12345678; OTA state unavailable";
    const char *state_names[] = {"new", "pending-verify", "valid", "invalid", "aborted"};
    for (unsigned state = 0; state < 5; state++) {
        reset(); running_state = (esp_ota_img_states_t)state; ota_load_result();
        check(state_names[state], state == 2 ? installed : state < 2 ? pending : unconfirmed, state != 2, state == 2, 1);
    }
    reset(); running_state = ESP_OTA_IMG_UNDEFINED; ota_load_result(); check("undefined", unconfirmed, true, 0, 1);
    reset(); running_state = (esp_ota_img_states_t)99; ota_load_result(); check("unknown-state", unconfirmed, true, 0, 1);
    const esp_err_t errors[] = {ESP_FAIL, ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED, ESP_ERR_INVALID_ARG};
    const char *error_names[] = {"state-read-error", "state-not-found", "state-not-supported", "state-invalid-arg"};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        reset(); state_error = errors[i]; ota_load_result(); check(error_names[i], unavailable, true, 0, 1);
    }
    reset(); strcpy(running_description.version, "v0.6.0"); invalid_present = true; ota_load_result();
    check("rolled-back", "Rolled back v0.7.0-12345678", false, 1, 0);
    reset(); strcpy(running_description.version, "v0.6.0"); ota_load_result();
    check("did-not-activate", "Update to v0.7.0-12345678 did not activate", false, 1, 0);
    reset(); strcpy(running_description.version, "v0.6.0"); invalid_present = true;
    strcpy(invalid_description.version, "v0.5.1"); ota_load_result();
    check("other-invalid-image", "Update to v0.7.0-12345678 did not activate", false, 1, 0);
    reset(); strcpy(running_description.version, "v0.6.0"); invalid_present = true; description_error = ESP_FAIL; ota_load_result();
    check("invalid-description-error", "Update to v0.7.0-12345678 did not activate", false, 1, 0);
    reset(); nvs_init_error = ESP_FAIL; ota_load_result(); check("nvs-unavailable", "none recorded", true, 0, 0);
    reset(); open_error = ESP_FAIL; ota_load_result(); check("nvs-open-error", "none recorded", true, 0, 0);
    reset(); pending_error = ESP_FAIL; ota_load_result(); check("pending-read-error", "Previous result", true, 0, 0);
    reset(); has_pending = false; ota_load_result(); check("pending-absent", "Previous result", false, 0, 0);
    reset(); saved_pending[0] = '\0'; ota_load_result(); check("pending-empty", "Previous result", true, 0, 0);
    reset(); has_result = false; ota_load_result(); check("previous-result-absent", installed, false, 1, 1);
    reset(); state_error = ESP_FAIL; ota_load_result();
    state_error = ESP_OK; running_state = ESP_OTA_IMG_NEW; ota_load_result();
    running_state = ESP_OTA_IMG_PENDING_VERIFY; ota_load_result();
    running_state = ESP_OTA_IMG_VALID; ota_load_result(); check("state-read-recovery", installed, false, 1, 4);
    reset(); set_error = ESP_FAIL; ota_load_result();
    assert(sets == 1 && !erases && !commits && warnings == 1); check("result-set-error", installed, true, 1, 1);
    reset(); erase_error = ESP_FAIL; ota_load_result();
    assert(sets == 1 && erases == 1 && !commits && warnings == 1); check("pending-erase-error", installed, true, 1, 1);
    reset(); commit_error = ESP_FAIL; ota_load_result();
    assert(sets == 1 && erases == 1 && commits == 1 && warnings == 1); check("result-commit-error", installed, false, 1, 1);
    reset(); ota_record_pending("v0.8.0-87654321");
    assert(has_pending && !strcmp(saved_pending, "v0.8.0-87654321") && sets == 1 && commits == 1 && !erases && !handles);
    cases++; printf("PASS record-pending handles=%u\n", handles);
    reset(); strcpy(saved_pending, "v0.7.0-123456789012345678901234"); strcpy(running_description.version, saved_pending);
    state_error = ESP_FAIL; ota_load_result();
    assert(strlen(ota_last_result) == 62);
    check("maximum-version-state-error", "Running v0.7.0-123456789012345678901234; OTA state unavailable", true, 0, 1);
    reset(); strcpy(saved_pending, "v0.7.0-123456789012345678901234"); strcpy(running_description.version, saved_pending);
    running_state = ESP_OTA_IMG_UNDEFINED; ota_load_result();
    check("maximum-version-unconfirmed", "Running v0.7.0-123456789012345678901234; health not confirmed", true, 0, 1);
    for (unsigned state = 0; state < 2; state++) {
        reset(); strcpy(saved_pending, "v0.7.0-123456789012345678901234"); strcpy(running_description.version, saved_pending);
        assert(strlen(saved_pending) == 31);
        running_state = (esp_ota_img_states_t)state; ota_load_result();
        check(state ? "maximum-version-pending-verify" : "maximum-version-new",
              "Installing v0.7.0-123456789012345678901234; health pending", true, 0, 1);
    }
    printf("%s %u OTA result cases failures=%u handles=%u (controlled NVS/state APIs)\n",
           failures ? "FAIL" : "PASS", cases, failures, handles);
    return failures ? 1 : 0;
}
