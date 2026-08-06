#include <dirent.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp/esp-bsp.h"
#include "esp_cache.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "cJSON.h"

#ifdef CHAT_HAS_SECRETS
#include "chat_secrets.h"
#else
#define CHAT_RELAY_URL ""
#define CHAT_DEVICE_TOKEN ""
#endif

#define INTERNAL_PATH BSP_SPIFFS_MOUNT_POINT
#define SD_PATH "/sdcard"
#define SCREEN_WIDTH 720
#define SCREEN_HEIGHT 1280

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t version;
    uint8_t type;
    uint8_t encoding;
    uint8_t reserved;
    uint16_t width;
    uint16_t height;
    uint32_t payload_size;
    uint32_t frame_number;
} remote_frame_header_t;

static lv_obj_t *content;
static lv_obj_t *note_area;
static lv_obj_t *counter_label;
static bool internal_ready;
static bool sd_ready;
static int counter;
static char current_directory[256];
static char file_paths[64][256];
static size_t file_path_count;
static bool wifi_ready;
static volatile bool wifi_connecting;
static volatile bool wifi_connected;
static volatile bool wifi_scan_busy;
static volatile bool wifi_scan_done;
static esp_err_t wifi_scan_error;
static unsigned wifi_retries;
static bool wifi_should_connect;
static char wifi_ssid[33];
static char wifi_ip[16];
static char selected_ssid[33];
static wifi_ap_record_t wifi_aps[12];
static uint16_t wifi_ap_count;
static lv_obj_t *wifi_status;
static lv_obj_t *wifi_list;
static lv_obj_t *wifi_password_area;
static lv_timer_t *wifi_timer;
static lv_obj_t *chat_output;
static lv_obj_t *chat_input;
static lv_obj_t *chat_status;
static lv_obj_t *chat_send_button;
static lv_timer_t *chat_timer;
static TaskHandle_t chat_task;
static volatile bool chat_busy;
static volatile bool chat_done;
static bool chat_ok;
static char chat_prompt[2001];
static char chat_response[8192];
static char chat_error[96];
static char chat_response_id[128];
static char chat_history[12000];
static volatile int16_t remote_x;
static volatile int16_t remote_y;
static volatile bool remote_pressed;
static uint32_t remote_frame_number;

static void show_launcher(void);
static void show_files(const char *path);
static void show_settings(void);
static void show_chat(void);

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} http_buffer_t;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && wifi_should_connect) {
        wifi_connecting = true;
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        if (wifi_should_connect && wifi_retries++ < 3) {
            wifi_connecting = true;
            esp_wifi_connect();
        } else {
            wifi_connecting = false;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        snprintf(wifi_ip, sizeof(wifi_ip), IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retries = 0;
        wifi_connecting = false;
        wifi_connected = true;
    }
}

static bool start_wifi(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    if (error != ESP_OK || esp_netif_init() != ESP_OK ||
        esp_event_loop_create_default() != ESP_OK || !esp_netif_create_default_wifi_sta()) return false;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK ||
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL) != ESP_OK ||
        esp_wifi_set_storage(WIFI_STORAGE_FLASH) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;

    wifi_config_t saved = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK) {
        snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", (char *)saved.sta.ssid);
        wifi_should_connect = wifi_ssid[0] != '\0';
    }
    error = esp_wifi_start();
    if (error != ESP_OK) ESP_LOGE("tab5-os", "Wi-Fi start failed: %s", esp_err_to_name(error));
    return error == ESP_OK;
}

static void remote_pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = remote_x;
    data->point.y = remote_y;
    data->state = remote_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static bool usb_write_all(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    while (size) {
        size_t chunk = size < 4096 ? size : 4096;
        int written = usb_serial_jtag_write_bytes(bytes, chunk, pdMS_TO_TICKS(2000));
        if (written <= 0) return false;
        bytes += written;
        size -= written;
    }
    return true;
}

static void send_remote_frame(uint8_t *framebuffer, uint8_t *pixels, uint8_t *encoded)
{
    const size_t pixel_bytes = SCREEN_WIDTH * SCREEN_HEIGHT * 2;
    bsp_display_lock(0);
    esp_cache_msync(framebuffer, pixel_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    memcpy(pixels, framebuffer, pixel_bytes);
    bsp_display_unlock();

    const uint16_t *source = (const uint16_t *)pixels;
    size_t source_count = SCREEN_WIDTH * SCREEN_HEIGHT;
    size_t output = 0;
    for (size_t i = 0; i < source_count;) {
        uint16_t value = source[i];
        uint16_t count = 1;
        while (i + count < source_count && source[i + count] == value && count < UINT16_MAX) count++;
        memcpy(encoded + output, &count, sizeof(count));
        memcpy(encoded + output + 2, &value, sizeof(value));
        output += 4;
        i += count;
    }

    remote_frame_header_t header = {
        .magic = {'T', '5', 'R', 'D'}, .version = 1, .type = 1, .encoding = 1,
        .width = SCREEN_WIDTH, .height = SCREEN_HEIGHT,
        .payload_size = output, .frame_number = ++remote_frame_number,
    };
    usb_write_all(&header, sizeof(header));
    usb_write_all(encoded, output);
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(5000));
}

static void remote_desktop_task(void *argument)
{
    (void)argument;
    const size_t pixel_bytes = SCREEN_WIDTH * SCREEN_HEIGHT * 2;
    uint8_t *pixels = heap_caps_malloc(pixel_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *encoded = heap_caps_malloc(pixel_bytes * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *framebuffer = NULL;
    esp_err_t frame_error = esp_lcd_dpi_panel_get_frame_buffer(
        bsp_display_get_panel_handle(), 1, (void **)&framebuffer);
    if (!pixels || !encoded || frame_error != ESP_OK) {
        ESP_LOGE("tab5-os", "Remote desktop buffer allocation failed");
        vTaskDelete(NULL);
    }

    uint8_t packet[10];
    size_t used = 0;
    while (true) {
        uint8_t byte;
        if (usb_serial_jtag_read_bytes(&byte, 1, portMAX_DELAY) != 1) continue;
        if (used == 0 && byte != 'T') continue;
        if (used == 1 && byte != '5') {
            used = byte == 'T' ? 1 : 0;
            continue;
        }
        packet[used++] = byte;
        if (used != sizeof(packet)) continue;
        used = 0;

        if (packet[2] == 1) {
            send_remote_frame(framebuffer, pixels, encoded);
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (packet[2] == 2) {
            uint16_t x, y;
            memcpy(&x, packet + 4, sizeof(x));
            memcpy(&y, packet + 6, sizeof(y));
            remote_x = x < SCREEN_WIDTH ? x : SCREEN_WIDTH - 1;
            remote_y = y < SCREEN_HEIGHT ? y : SCREEN_HEIGHT - 1;
            remote_pressed = packet[3] != 0;
        }
    }
}

static void start_remote_desktop(lv_display_t *display)
{
    lv_indev_t *remote_pointer = lv_indev_create();
    lv_indev_set_type(remote_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(remote_pointer, remote_pointer_read);
    lv_indev_set_display(remote_pointer, display);

    usb_serial_jtag_driver_config_t config = {.tx_buffer_size = 16384, .rx_buffer_size = 256};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    usb_serial_jtag_vfs_use_driver();
    xTaskCreate(remote_desktop_task, "remote-desktop", 6144, NULL, 4, NULL);
}

static bool mount_internal(void)
{
    const esp_vfs_spiffs_conf_t config = {
        .base_path = INTERNAL_PATH,
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t error = esp_vfs_spiffs_register(&config);
    if (error != ESP_OK) ESP_LOGW("tab5-os", "Internal storage unavailable: %s", esp_err_to_name(error));
    return error == ESP_OK;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 280, 110);
    if (callback) lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_center(label);
    return btn;
}

static void app_icon(lv_obj_t *parent, const char *symbol, const char *name,
                     uint32_t color, lv_event_cb_t callback, int column, int row)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, column, 1, LV_GRID_ALIGN_STRETCH, row, 1);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, 8, 0);

    lv_obj_t *tile = lv_button_create(cell);
    lv_obj_set_size(tile, 150, 150);
    lv_obj_set_style_radius(tile, 28, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(color), 0);
    lv_obj_add_event_cb(tile, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *icon = lv_label_create(tile);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_center(icon);

    lv_obj_t *label = lv_label_create(cell);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
}

static esp_err_t chat_http_event(esp_http_client_event_t *event)
{
    http_buffer_t *buffer = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !buffer) return ESP_OK;
    size_t available = buffer->capacity - buffer->length - 1;
    if ((size_t)event->data_len > available) return ESP_ERR_NO_MEM;
    memcpy(buffer->data + buffer->length, event->data, event->data_len);
    buffer->length += event->data_len;
    buffer->data[buffer->length] = '\0';
    return ESP_OK;
}

static void chat_request_task(void *argument)
{
    (void)argument;
    // ponytail: keep the worker alive; ESP-IDF rejects its PSRAM-linked pthread cleanup callback on deletion.
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        chat_ok = false;
        chat_error[0] = '\0';
    char *response_data = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cJSON *request_json = cJSON_CreateObject();
    char *request_body = NULL;
    esp_http_client_handle_t client = NULL;
    if (!response_data || !request_json) {
        snprintf(chat_error, sizeof(chat_error), "Out of memory");
        goto done;
    }

    cJSON_AddStringToObject(request_json, "message", chat_prompt);
    if (chat_response_id[0]) cJSON_AddStringToObject(request_json, "previous_response_id", chat_response_id);
    request_body = cJSON_PrintUnformatted(request_json);
    if (!request_body) {
        snprintf(chat_error, sizeof(chat_error), "Could not prepare request");
        goto done;
    }

    http_buffer_t buffer = {.data = response_data, .capacity = 16384};
    esp_http_client_config_t config = {
        .url = CHAT_RELAY_URL,
        .event_handler = chat_http_event,
        .user_data = &buffer,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 90000,
        .buffer_size = 2048,
    };
    client = esp_http_client_init(&config);
    if (!client) {
        snprintf(chat_error, sizeof(chat_error), "Could not start HTTPS");
        goto done;
    }

    char authorization[160];
    snprintf(authorization, sizeof(authorization), "Bearer %s", CHAT_DEVICE_TOKEN);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_post_field(client, request_body, strlen(request_body));
    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (error != ESP_OK || status != 200) {
        snprintf(chat_error, sizeof(chat_error), "Relay request failed (%d)", status);
        goto done;
    }

    cJSON *response_json = cJSON_Parse(response_data);
    cJSON *text = response_json ? cJSON_GetObjectItemCaseSensitive(response_json, "text") : NULL;
    cJSON *response_id = response_json ? cJSON_GetObjectItemCaseSensitive(response_json, "response_id") : NULL;
    if (!cJSON_IsString(text) || !cJSON_IsString(response_id)) {
        snprintf(chat_error, sizeof(chat_error), "Invalid relay response");
    } else {
        snprintf(chat_response, sizeof(chat_response), "%s", text->valuestring);
        snprintf(chat_response_id, sizeof(chat_response_id), "%s", response_id->valuestring);
        chat_ok = true;
    }
    cJSON_Delete(response_json);

done:
    if (client) esp_http_client_cleanup(client);
    cJSON_free(request_body);
    cJSON_Delete(request_json);
    free(response_data);
    chat_busy = false;
    chat_done = true;
    }
}

static void chat_append(const char *role, const char *text)
{
    size_t used = strlen(chat_history);
    size_t needed = strlen(role) + strlen(text) + 5;
    if (used + needed >= sizeof(chat_history)) {
        // ponytail: keep only the current window; add persisted transcripts if users need long sessions.
        snprintf(chat_history, sizeof(chat_history), "(Earlier messages omitted)\n\n");
        used = strlen(chat_history);
    }
    snprintf(chat_history + used, sizeof(chat_history) - used, "%s: %s\n\n", role, text);
}

static void chat_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!chat_done) return;
    chat_done = false;
    if (chat_ok) chat_append("AI", chat_response);
    else chat_append("Error", chat_error[0] ? chat_error : "Unknown error");
    lv_textarea_set_text(chat_output, chat_history);
    lv_textarea_set_cursor_pos(chat_output, LV_TEXTAREA_CURSOR_LAST);
    lv_label_set_text(chat_status, chat_ok ? "Ready" : "Request failed");
    lv_obj_remove_state(chat_send_button, LV_STATE_DISABLED);
}

static void chat_send_clicked(lv_event_t *event)
{
    (void)event;
    if (chat_busy) return;
    if (!wifi_connected) {
        lv_label_set_text(chat_status, "Connect to Wi-Fi first");
        return;
    }
    if (!CHAT_RELAY_URL[0] || !CHAT_DEVICE_TOKEN[0]) {
        lv_label_set_text(chat_status, "Relay is not configured");
        return;
    }

    const char *input = lv_textarea_get_text(chat_input);
    while (isspace((unsigned char)*input)) input++;
    if (!input[0]) return;
    snprintf(chat_prompt, sizeof(chat_prompt), "%s", input);
    chat_append("You", chat_prompt);
    lv_textarea_set_text(chat_output, chat_history);
    lv_textarea_set_text(chat_input, "");
    lv_label_set_text(chat_status, "Thinking...");
    lv_obj_add_state(chat_send_button, LV_STATE_DISABLED);
    chat_busy = true;
    chat_done = false;
    if (!chat_task && xTaskCreate(chat_request_task, "ai-chat", 16384, NULL, 4, &chat_task) != pdPASS) {
        chat_busy = false;
        lv_obj_remove_state(chat_send_button, LV_STATE_DISABLED);
        lv_label_set_text(chat_status, "Could not start request");
        return;
    }
    xTaskNotifyGive(chat_task);
}

static void chat_new_clicked(lv_event_t *event)
{
    (void)event;
    if (chat_busy) return;
    chat_history[0] = '\0';
    chat_response_id[0] = '\0';
    lv_textarea_set_text(chat_output, "Ask me anything.");
    lv_label_set_text(chat_status, "New conversation");
}

static void clear_content(void)
{
    if (wifi_timer) {
        lv_timer_delete(wifi_timer);
        wifi_timer = NULL;
    }
    if (chat_timer) {
        lv_timer_delete(chat_timer);
        chat_timer = NULL;
    }
    wifi_status = NULL;
    wifi_list = NULL;
    chat_output = NULL;
    chat_input = NULL;
    chat_status = NULL;
    chat_send_button = NULL;
    lv_obj_clean(content);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

static void home_clicked(lv_event_t *event)
{
    (void)event;
    show_launcher();
}

static void open_file(const char *path)
{
    clear_content();
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, path);
    lv_obj_set_width(title, 620);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    static char text[4096];
    FILE *file = fopen(path, "rb");
    size_t read = file ? fread(text, 1, sizeof(text) - 1, file) : 0;
    if (file) fclose(file);
    text[read] = '\0';
    if (!file) snprintf(text, sizeof(text), "Could not open this file.");

    lv_obj_t *viewer = lv_textarea_create(content);
    lv_obj_set_size(viewer, 640, 900);
    lv_textarea_set_text(viewer, text);
    lv_textarea_set_one_line(viewer, false);
}

static void file_clicked(lv_event_t *event)
{
    const char *path = lv_event_get_user_data(event);
    struct stat info;
    if (stat(path, &info) == 0 && S_ISDIR(info.st_mode)) show_files(path);
    else open_file(path);
}

static void show_files(const char *path)
{
    if (path) {
        snprintf(current_directory, sizeof(current_directory), "%s", path);
        path = current_directory;
    }
    clear_content();
    file_path_count = 0;

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text_fmt(title, "Files  %s", path ? path : "");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *list = lv_list_create(content);
    lv_obj_set_size(list, 640, 940);

    if (!path) {
        lv_obj_t *back = lv_list_add_button(list, LV_SYMBOL_LEFT, "Back to apps");
        lv_obj_add_event_cb(back, home_clicked, LV_EVENT_CLICKED, NULL);
        if (internal_ready) {
            lv_obj_t *item = lv_list_add_button(list, LV_SYMBOL_DIRECTORY, "Internal storage");
            lv_obj_add_event_cb(item, file_clicked, LV_EVENT_CLICKED, INTERNAL_PATH);
        }
        if (sd_ready) {
            lv_obj_t *item = lv_list_add_button(list, LV_SYMBOL_SD_CARD, "SD card");
            lv_obj_add_event_cb(item, file_clicked, LV_EVENT_CLICKED, SD_PATH);
        }
        if (!internal_ready && !sd_ready) lv_list_add_text(list, "No storage mounted");
        return;
    }

    char parent[256];
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = '\0';
    else parent[0] = '\0';
    lv_obj_t *up = lv_list_add_button(list, LV_SYMBOL_UP, "..");
    if (parent[0]) {
        snprintf(file_paths[file_path_count], sizeof(file_paths[0]), "%s", parent);
        lv_obj_add_event_cb(up, file_clicked, LV_EVENT_CLICKED, file_paths[file_path_count++]);
    } else {
        lv_obj_add_event_cb(up, home_clicked, LV_EVENT_CLICKED, NULL);
    }

    DIR *dir = opendir(path);
    if (!dir) {
        lv_list_add_text(list, "Could not open directory");
        return;
    }
    struct dirent *entry;
    while (file_path_count < 64 && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char *full = file_paths[file_path_count++];
        size_t path_len = strlen(path);
        size_t name_len = strlen(entry->d_name);
        if (path_len + name_len + 2 > sizeof(file_paths[0])) {
            file_path_count--;
            continue;
        }
        memcpy(full, path, path_len);
        full[path_len] = '/';
        memcpy(full + path_len + 1, entry->d_name, name_len + 1);
        struct stat info;
        bool is_dir = stat(full, &info) == 0 && S_ISDIR(info.st_mode);
        lv_obj_t *item = lv_list_add_button(list, is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, entry->d_name);
        lv_obj_add_event_cb(item, file_clicked, LV_EVENT_CLICKED, full);
    }
    closedir(dir);
}

static void files_clicked(lv_event_t *event)
{
    (void)event;
    show_files(NULL);
}

static void save_note(lv_event_t *event)
{
    lv_obj_t *status = lv_event_get_user_data(event);
    FILE *file = fopen(SD_PATH "/DOCS/NOTE.TXT", "wb");
    if (!file) {
        lv_label_set_text(status, "Save failed");
        return;
    }
    fputs(lv_textarea_get_text(note_area), file);
    fclose(file);
    lv_label_set_text(status, "Saved to /sdcard/DOCS/NOTE.TXT");
}

static void notes_clicked(lv_event_t *event)
{
    (void)event;
    clear_content();
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *row = lv_obj_create(content);
    lv_obj_set_size(row, 640, 70);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_t *status = lv_label_create(row);
    lv_label_set_text(status, sd_ready ? "Notes" : "SD card unavailable");
    lv_obj_set_flex_grow(status, 1);
    lv_obj_t *save = lv_button_create(row);
    lv_obj_add_event_cb(save, save_note, LV_EVENT_CLICKED, status);
    lv_obj_t *save_label = lv_label_create(save);
    lv_label_set_text(save_label, "Save");
    lv_obj_center(save_label);
    if (!sd_ready) lv_obj_add_state(save, LV_STATE_DISABLED);

    static char note[2048];
    FILE *file = fopen(SD_PATH "/DOCS/NOTE.TXT", "rb");
    size_t read = file ? fread(note, 1, sizeof(note) - 1, file) : 0;
    if (file) fclose(file);
    note[read] = '\0';

    note_area = lv_textarea_create(content);
    lv_obj_set_size(note_area, 640, 420);
    lv_textarea_set_text(note_area, note);
    lv_obj_t *keyboard = lv_keyboard_create(content);
    lv_obj_set_size(keyboard, 640, 500);
    lv_keyboard_set_textarea(keyboard, note_area);
}

static void update_counter(void)
{
    lv_label_set_text_fmt(counter_label, "%d", counter);
}

static void counter_change(lv_event_t *event)
{
    counter += (int)(intptr_t)lv_event_get_user_data(event);
    update_counter();
}

static void counter_reset(lv_event_t *event)
{
    (void)event;
    counter = 0;
    update_counter();
}

static void counter_clicked(lv_event_t *event)
{
    (void)event;
    clear_content();
    counter_label = lv_label_create(content);
    lv_obj_set_style_text_font(counter_label, &lv_font_montserrat_48, 0);
    update_counter();
    lv_obj_t *minus = button(content, "-1", NULL);
    lv_obj_add_event_cb(minus, counter_change, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *plus = button(content, "+1", NULL);
    lv_obj_add_event_cb(plus, counter_change, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    button(content, "Reset", counter_reset);
}

static void wifi_scan_task(void *argument)
{
    (void)argument;
    wifi_scan_error = esp_wifi_scan_start(NULL, true);
    wifi_ap_count = 12;
    if (wifi_scan_error == ESP_OK) wifi_scan_error = esp_wifi_scan_get_ap_records(&wifi_ap_count, wifi_aps);
    else wifi_ap_count = 0;
    wifi_scan_busy = false;
    wifi_scan_done = true;
    vTaskDelete(NULL);
}

static void wifi_scan_clicked(lv_event_t *event)
{
    (void)event;
    if (!wifi_ready || wifi_scan_busy) return;
    wifi_scan_busy = true;
    wifi_scan_done = false;
    lv_obj_clean(wifi_list);
    lv_list_add_text(wifi_list, "Scanning...");
    if (xTaskCreate(wifi_scan_task, "wifi-scan", 4096, NULL, 4, NULL) != pdPASS) {
        wifi_scan_busy = false;
        lv_obj_clean(wifi_list);
        lv_list_add_text(wifi_list, "Could not start scan");
    }
}

static void wifi_connect_clicked(lv_event_t *event)
{
    (void)event;
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, selected_ssid, strnlen(selected_ssid, sizeof(config.sta.ssid)));
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", lv_textarea_get_text(wifi_password_area));
    wifi_should_connect = false;
    esp_wifi_disconnect();
    esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (error == ESP_OK) {
        snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", selected_ssid);
        wifi_retries = 0;
        wifi_should_connect = true;
        wifi_connecting = true;
        error = esp_wifi_connect();
    }
    if (error != ESP_OK) {
        wifi_connecting = false;
        ESP_LOGE("tab5-os", "Wi-Fi connect failed: %s", esp_err_to_name(error));
    }
    show_settings();
}

static void wifi_network_clicked(lv_event_t *event)
{
    wifi_ap_record_t *ap = lv_event_get_user_data(event);
    snprintf(selected_ssid, sizeof(selected_ssid), "%s", (char *)ap->ssid);
    clear_content();

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text_fmt(title, "Connect to\n%s", selected_ssid);
    lv_obj_set_width(title, 640);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    wifi_password_area = lv_textarea_create(content);
    lv_obj_set_size(wifi_password_area, 640, 100);
    lv_textarea_set_placeholder_text(wifi_password_area, "Wi-Fi password (blank for open networks)");
    lv_textarea_set_password_mode(wifi_password_area, true);
    lv_textarea_set_max_length(wifi_password_area, 63);
    lv_textarea_set_one_line(wifi_password_area, true);
    button(content, "Connect", wifi_connect_clicked);

    lv_obj_t *keyboard = lv_keyboard_create(content);
    lv_obj_set_size(keyboard, 640, 540);
    lv_keyboard_set_textarea(keyboard, wifi_password_area);
}

static void wifi_forget_clicked(lv_event_t *event)
{
    (void)event;
    if (!wifi_ready) return;
    wifi_config_t empty = {0};
    wifi_should_connect = false;
    wifi_connecting = false;
    wifi_connected = false;
    wifi_ssid[0] = '\0';
    wifi_ip[0] = '\0';
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &empty);
}

static void wifi_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!wifi_ready) lv_label_set_text(wifi_status, "Wi-Fi hardware unavailable");
    else if (wifi_connected) lv_label_set_text_fmt(wifi_status, "Connected: %s\nIP: %s", wifi_ssid, wifi_ip);
    else if (wifi_connecting) lv_label_set_text_fmt(wifi_status, "Connecting to %s...", wifi_ssid);
    else if (wifi_ssid[0]) lv_label_set_text_fmt(wifi_status, "Not connected: %s", wifi_ssid);
    else lv_label_set_text(wifi_status, "Not connected");

    if (!wifi_scan_done) return;
    wifi_scan_done = false;
    lv_obj_clean(wifi_list);
    if (wifi_scan_error != ESP_OK) {
        lv_list_add_text(wifi_list, "Scan failed - try again");
        return;
    }
    if (!wifi_ap_count) {
        lv_list_add_text(wifi_list, "No networks found");
        return;
    }
    for (uint16_t i = 0; i < wifi_ap_count; i++) {
        char label[96];
        snprintf(label, sizeof(label), "%s   %d dBm%s", wifi_aps[i].ssid, wifi_aps[i].rssi,
                 wifi_aps[i].authmode == WIFI_AUTH_OPEN ? "" : "   locked");
        lv_obj_t *network = lv_list_add_button(wifi_list, LV_SYMBOL_WIFI, label);
        lv_obj_add_event_cb(network, wifi_network_clicked, LV_EVENT_CLICKED, &wifi_aps[i]);
    }
}

static void show_settings(void)
{
    clear_content();
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "Wi-Fi Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    wifi_status = lv_label_create(content);
    lv_obj_set_size(wifi_status, 640, 75);

    lv_obj_t *actions = lv_obj_create(content);
    lv_obj_set_size(actions, 640, 100);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *scan = button(actions, "Scan", wifi_scan_clicked);
    lv_obj_t *forget = button(actions, "Forget", wifi_forget_clicked);

    wifi_list = lv_list_create(content);
    lv_obj_set_size(wifi_list, 640, 800);
    lv_list_add_text(wifi_list, wifi_ready ? "Tap Scan to find networks" : "Wi-Fi hardware unavailable");
    if (!wifi_ready) {
        lv_obj_add_state(scan, LV_STATE_DISABLED);
        lv_obj_add_state(forget, LV_STATE_DISABLED);
    }
    wifi_timer = lv_timer_create(wifi_tick, 250, NULL);
    wifi_tick(wifi_timer);
}

static void settings_clicked(lv_event_t *event)
{
    (void)event;
    show_settings();
}

static void show_chat(void)
{
    clear_content();
    lv_obj_set_style_pad_row(content, 12, 0);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "AI Chat");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    chat_status = lv_label_create(content);
    lv_label_set_text(chat_status, chat_busy ? "Thinking..." :
        (!CHAT_RELAY_URL[0] || !CHAT_DEVICE_TOKEN[0]) ? "Relay is not configured" : "Ready");
    lv_obj_set_width(chat_status, 640);

    chat_output = lv_textarea_create(content);
    lv_obj_set_size(chat_output, 640, 360);
    lv_textarea_set_text(chat_output, chat_history[0] ? chat_history : "Ask me anything.");
    lv_textarea_set_cursor_pos(chat_output, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_set_one_line(chat_output, false);

    chat_input = lv_textarea_create(content);
    lv_obj_set_size(chat_input, 640, 90);
    lv_textarea_set_placeholder_text(chat_input, "Message");
    lv_textarea_set_max_length(chat_input, 2000);

    lv_obj_t *actions = lv_obj_create(content);
    lv_obj_set_size(actions, 640, 90);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    chat_send_button = button(actions, "Send", chat_send_clicked);
    lv_obj_set_height(chat_send_button, 76);
    lv_obj_t *new_chat = button(actions, "New Chat", chat_new_clicked);
    lv_obj_set_height(new_chat, 76);
    if (chat_busy) lv_obj_add_state(chat_send_button, LV_STATE_DISABLED);

    lv_obj_t *keyboard = lv_keyboard_create(content);
    lv_obj_set_size(keyboard, 640, 390);
    lv_keyboard_set_textarea(keyboard, chat_input);
    chat_timer = lv_timer_create(chat_tick, 200, NULL);
}

static void chat_clicked(lv_event_t *event)
{
    (void)event;
    show_chat();
}

static void system_clicked(lv_event_t *event)
{
    (void)event;
    clear_content();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    lv_obj_t *info = lv_label_create(content);
    lv_label_set_text_fmt(info,
        "System\n\nESP32-P4 rev %d.%d\n%d CPU cores\n%lu KB free RAM\n32 MB PSRAM\n720 x 1280 ST7121\n\nInternal: %s\nSD card: %s",
        chip.revision / 100, chip.revision % 100, chip.cores,
        (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
        internal_ready ? "mounted" : "unavailable", sd_ready ? "mounted" : "not inserted");
    lv_obj_set_style_text_font(info, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_line_space(info, 16, 0);
}

static void show_launcher(void)
{
    static int32_t columns[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t rows[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    clear_content();
    lv_obj_set_grid_dsc_array(content, columns, rows);
    lv_obj_set_style_pad_row(content, 12, 0);
    lv_obj_set_style_pad_column(content, 12, 0);
    app_icon(content, LV_SYMBOL_DIRECTORY, "Files", 0x2196F3, files_clicked, 0, 0);
    app_icon(content, LV_SYMBOL_EDIT, "Notes", 0x00A896, notes_clicked, 1, 0);
    app_icon(content, LV_SYMBOL_PLUS, "Counter", 0xF59E0B, counter_clicked, 2, 0);
    app_icon(content, LV_SYMBOL_SETTINGS, "System", 0x7C4DFF, system_clicked, 0, 1);
    app_icon(content, LV_SYMBOL_WIFI, "Settings", 0x0288D1, settings_clicked, 1, 1);
    app_icon(content, LV_SYMBOL_ENVELOPE, "AI Chat", 0xE91E63, chat_clicked, 2, 1);
}

void app_main(void)
{
    ESP_LOGI("tab5-os", "Starting Tab5 OS");
    ESP_ERROR_CHECK(bsp_i2c_init());
    bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
    vTaskDelay(pdMS_TO_TICKS(300));

    wifi_ready = start_wifi();

    internal_ready = mount_internal();
    sd_ready = bsp_sdcard_init(SD_PATH, 5) == ESP_OK;

    lv_display_t *display = bsp_display_start();
    bsp_display_lock(0);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x10141f), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, 720, 100);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x20283a), 0);
    lv_obj_set_style_text_color(header, lv_color_white(), 0);
    lv_obj_t *home = lv_button_create(header);
    lv_obj_set_size(home, 120, 64);
    lv_obj_align(home, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(home, home_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_label = lv_label_create(home);
    lv_label_set_text(home_label, LV_SYMBOL_HOME);
    lv_obj_center(home_label);
    lv_obj_t *brand = lv_label_create(header);
    lv_label_set_text(brand, "Tab5 OS");
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_28, 0);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, 0);

    content = lv_obj_create(screen);
    lv_obj_set_size(content, 720, 1180);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x10141f), 0);
    lv_obj_set_style_text_color(content, lv_color_white(), 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 28, 0);
    lv_obj_set_style_pad_row(content, 24, 0);
    show_launcher();
    start_remote_desktop(display);

    bsp_display_unlock();
    bsp_display_backlight_on();
}
