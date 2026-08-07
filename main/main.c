#include <dirent.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "esp_app_desc.h"
#include "esp_cache.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_crt_bundle.h"
#include "esp_codec_dev.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
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
#define VOICE_INPUT_RATE 48000
#define VOICE_RATE 16000
#define VOICE_CHANNELS 4
#define VOICE_MIC_CHANNEL 0
#define VOICE_MAX_SECONDS 30
#define BROWSER_MAX_HTML 65536
#define BROWSER_MAX_TEXT 12288
#define BROWSER_MAX_LINKS 12
#define EBOOK_PAGE_BYTES 8192
#define OTA_URL "https://github.com/DevanMetz/Tab5OS/releases/latest/download/tab5_os.bin"
#define BATTERY_EMPTY_MV 6000
#define BATTERY_FULL_MV 8230
#define BATTERY_HISTORY_POINTS 60
#define TIME_ZONE "CST6CDT,M3.2.0,M11.1.0"

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

typedef struct __attribute__((packed)) {
    char riff[4];
    uint32_t riff_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
} wav_header_t;
_Static_assert(sizeof(wav_header_t) == 44, "WAV header must be 44 bytes");

typedef enum {
    CHAT_JOB_MESSAGE,
    CHAT_JOB_VOICE,
} chat_job_t;

typedef struct {
    const char *filename;
    const char *url;
} ebook_default_t;

typedef struct {
    gpio_num_t pin;
    const char *port;
    uint8_t mode;
    lv_obj_t *mode_label;
    lv_obj_t *level_label;
} gpio_control_t;

static const ebook_default_t ebook_defaults[] = {
    {"ALICE.TXT", "https://www.gutenberg.org/cache/epub/11/pg11.txt"},
    {"FRANK.TXT", "https://www.gutenberg.org/cache/epub/84/pg84.txt"},
    {"HOLMES.TXT", "https://www.gutenberg.org/cache/epub/1661/pg1661.txt"},
};

#define GPIO_CONTROL(p, name) {.pin = (p), .port = (name)}
static gpio_control_t gpio_controls[] = {
    GPIO_CONTROL(GPIO_NUM_49, "EXT"), GPIO_CONTROL(GPIO_NUM_50, "EXT"), GPIO_CONTROL(GPIO_NUM_0, "EXT"),
    GPIO_CONTROL(GPIO_NUM_1, "EXT"), GPIO_CONTROL(GPIO_NUM_54, "EXT"), GPIO_CONTROL(GPIO_NUM_53, "EXT"),
    GPIO_CONTROL(GPIO_NUM_18, "M-BUS"), GPIO_CONTROL(GPIO_NUM_19, "M-BUS"), GPIO_CONTROL(GPIO_NUM_5, "M-BUS"),
    GPIO_CONTROL(GPIO_NUM_38, "M-BUS"), GPIO_CONTROL(GPIO_NUM_7, "M-BUS"), GPIO_CONTROL(GPIO_NUM_3, "M-BUS"),
    GPIO_CONTROL(GPIO_NUM_2, "M-BUS"), GPIO_CONTROL(GPIO_NUM_47, "M-BUS"), GPIO_CONTROL(GPIO_NUM_16, "M-BUS"),
    GPIO_CONTROL(GPIO_NUM_17, "M-BUS"), GPIO_CONTROL(GPIO_NUM_45, "M-BUS"), GPIO_CONTROL(GPIO_NUM_52, "M-BUS"),
    GPIO_CONTROL(GPIO_NUM_37, "M-BUS"), GPIO_CONTROL(GPIO_NUM_6, "M-BUS"), GPIO_CONTROL(GPIO_NUM_4, "M-BUS"),
    GPIO_CONTROL(GPIO_NUM_48, "M-BUS"), GPIO_CONTROL(GPIO_NUM_35, "M-BUS"), GPIO_CONTROL(GPIO_NUM_51, "M-BUS"),
};
#undef GPIO_CONTROL
#define GPIO_CONTROL_COUNT (sizeof(gpio_controls) / sizeof(gpio_controls[0]))
_Static_assert(GPIO_CONTROL_COUNT == 24, "Tab5 exposes 24 user GPIO pins");

static lv_obj_t *content;
static lv_obj_t *battery_label;
static i2c_master_dev_handle_t battery_monitor;
static i2c_master_dev_handle_t rtc;
static lv_obj_t *time_label;
static lv_obj_t *clock_time;
static lv_obj_t *clock_date;
static lv_obj_t *clock_status;
static bool rtc_time_loaded;
static bool sntp_started;
static volatile bool internet_time_synced;
static volatile bool rtc_sync_pending;
static lv_obj_t *battery_metrics;
static lv_obj_t *battery_chart;
static lv_chart_series_t *battery_series;
static uint8_t battery_history[BATTERY_HISTORY_POINTS];
static uint8_t battery_history_count;
static uint8_t battery_history_head;
static int battery_millivolts;
static int battery_milliamps;
static int battery_percent;
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
static TaskHandle_t wifi_connect_task_handle;
static lv_obj_t *chat_output;
static lv_obj_t *chat_input;
static lv_obj_t *chat_status;
static lv_obj_t *chat_send_button;
static lv_obj_t *chat_voice_button;
static lv_obj_t *chat_voice_label;
static lv_obj_t *chat_wave;
static lv_chart_series_t *chat_wave_series;
static lv_obj_t *chat_keyboard;
static lv_obj_t *chat_keyboard_label;
static lv_obj_t *chat_text_label;
static lv_timer_t *chat_timer;
static TaskHandle_t chat_task;
static esp_codec_dev_handle_t voice_mic;
static bool voice_mic_open;
static volatile bool chat_busy;
static volatile bool chat_done;
static volatile chat_job_t chat_job;
static volatile chat_job_t chat_completed_job;
static volatile bool voice_recording;
static volatile bool voice_stop_requested;
static volatile uint16_t voice_level;
static bool chat_ok;
static bool chat_keyboard_visible = true;
static bool chat_large_text;
static char chat_prompt[2001];
static char chat_response[8192];
static char chat_error[96];
static char chat_response_id[128];
static char chat_history[12000];
static char chat_relay_url[256];
static char chat_device_token[65];
static lv_obj_t *browser_status;
static lv_obj_t *browser_url_area;
static lv_obj_t *browser_page;
static lv_obj_t *browser_keyboard;
static lv_obj_t *browser_keys_label;
static lv_timer_t *browser_timer;
static TaskHandle_t browser_task;
static volatile bool browser_busy;
static volatile bool browser_done;
static bool browser_ok;
static bool browser_keyboard_visible = true;
static char browser_url[256] = "https://example.com";
static char browser_pending_url[256];
static char browser_error[96];
static char *browser_result;
static char browser_links[BROWSER_MAX_LINKS][256];
static char browser_link_labels[BROWSER_MAX_LINKS][64];
static size_t browser_link_count;
static char browser_history[8][256];
static size_t browser_history_count;
static lv_obj_t *ebook_text;
static lv_obj_t *ebook_status;
static lv_obj_t *ebook_prev;
static lv_obj_t *ebook_next;
static char *ebook_buffer;
static char ebook_path[256];
static long ebook_offset;
static long ebook_next_offset;
static bool ebook_large_text;
static lv_timer_t *ebook_timer;
static TaskHandle_t ebook_download_task_handle;
static volatile bool ebook_download_busy;
static volatile bool ebook_download_done;
static lv_obj_t *ota_status;
static lv_obj_t *ota_button;
static lv_timer_t *ota_timer;
static lv_timer_t *gpio_timer;
static TaskHandle_t ota_task_handle;
static volatile bool ota_busy;
static volatile bool ota_done;
static bool ota_ok;
static char ota_error[96];
static volatile int16_t remote_x;
static volatile int16_t remote_y;
static volatile bool remote_pressed;
static uint32_t remote_frame_number;

static void show_launcher(void);
static void show_files(const char *path);
static void show_settings(void);
static void show_chat(void);
static void show_browser(void);
static void show_ebooks(void);
static void show_clock(void);
static void show_gpio(void);
static void clear_content(void);
static void browser_link_clicked(lv_event_t *event);

static esp_err_t gpio_apply(gpio_control_t *control)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << control->pin,
        .mode = control->mode == 1 ? GPIO_MODE_INPUT : GPIO_MODE_OUTPUT,
        .pull_up_en = control->mode == 1 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&config);
    if (error == ESP_OK && control->mode > 1) error = gpio_set_level(control->pin, control->mode == 3);
    return error;
}

static void gpio_mode_clicked(lv_event_t *event)
{
    gpio_control_t *control = lv_event_get_user_data(event);
    control->mode = control->mode == 3 ? 1 : control->mode + 1;
    if (gpio_apply(control) != ESP_OK) {
        lv_label_set_text(control->mode_label, "ERROR");
        return;
    }
    lv_label_set_text(control->mode_label, control->mode == 1 ? "INPUT" : control->mode == 2 ? "LOW" : "HIGH");
}

static void gpio_tick(lv_timer_t *timer)
{
    (void)timer;
    for (size_t i = 0; i < GPIO_CONTROL_COUNT; i++) {
        if (gpio_controls[i].level_label)
            lv_label_set_text_fmt(gpio_controls[i].level_label, "%d", gpio_get_level(gpio_controls[i].pin));
    }
}

static uint8_t bcd(int value)
{
    return (value / 10 << 4) | value % 10;
}

static int unbcd(uint8_t value)
{
    return (value >> 4) * 10 + (value & 0x0f);
}

static void rtc_write_system_time(void)
{
    if (!rtc) return;
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    uint8_t values[] = {0x10, bcd(utc.tm_sec), bcd(utc.tm_min), bcd(utc.tm_hour),
                        bcd(utc.tm_wday), bcd(utc.tm_mday), bcd(utc.tm_mon + 1), bcd(utc.tm_year % 100)};
    if (i2c_master_transmit(rtc, values, sizeof(values), 50) == ESP_OK) {
        uint8_t flag_reg = 0x1d;
        uint8_t flag;
        if (i2c_master_transmit_receive(rtc, &flag_reg, 1, &flag, 1, 50) == ESP_OK) {
            uint8_t clear_vlf[] = {0x1d, flag & ~0x02};
            i2c_master_transmit(rtc, clear_vlf, sizeof(clear_vlf), 50);
        }
        rtc_time_loaded = true;
    }
}

static void time_synced(struct timeval *tv)
{
    (void)tv;
    internet_time_synced = true;
    rtc_sync_pending = true;
}

static void start_sntp(void)
{
    if (sntp_started) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_synced);
    esp_sntp_init();
    sntp_started = true;
}

static void clock_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x32,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &config, &rtc) != ESP_OK) rtc = NULL;

    uint8_t reg = 0x10;
    uint8_t raw[14];
    if (rtc && i2c_master_transmit_receive(rtc, &reg, 1, raw, sizeof(raw), 50) == ESP_OK) {
        struct tm value = {
            .tm_sec = unbcd(raw[0] & 0x7f), .tm_min = unbcd(raw[1] & 0x7f),
            .tm_hour = unbcd(raw[2] & 0x3f), .tm_wday = unbcd(raw[3] & 0x7f),
            .tm_mday = unbcd(raw[4] & 0x3f), .tm_mon = unbcd(raw[5] & 0x1f) - 1,
            .tm_year = unbcd(raw[6]) + 100, .tm_isdst = 0,
        };
        if (!(raw[13] & 0x02) && value.tm_year >= 124 && value.tm_mon >= 0 && value.tm_mon < 12 &&
            value.tm_mday > 0 && value.tm_mday <= 31 && value.tm_hour < 24 && value.tm_min < 60 && value.tm_sec < 60) {
            setenv("TZ", "UTC0", 1);
            tzset();
            struct timeval tv = {.tv_sec = mktime(&value)};
            settimeofday(&tv, NULL);
            rtc_time_loaded = true;
        }
    }
    setenv("TZ", TIME_ZONE, 1);
    tzset();
    assert(unbcd(bcd(59)) == 59);
}

static void clock_tick(lv_timer_t *timer)
{
    (void)timer;
    if (rtc_sync_pending) {
        rtc_sync_pending = false;
        rtc_write_system_time();
    }
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    bool valid = local.tm_year >= 124;
    char text[40];
    if (valid) strftime(text, sizeof(text), "%I:%M %p", &local);
    else snprintf(text, sizeof(text), "--:--");
    if (time_label) lv_label_set_text(time_label, text);
    if (clock_time) lv_label_set_text(clock_time, text);
    if (clock_date) {
        if (valid) strftime(text, sizeof(text), "%A, %B %d, %Y", &local);
        else snprintf(text, sizeof(text), "Time not set");
        lv_label_set_text(clock_date, text);
    }
    if (clock_status) lv_label_set_text(clock_status, internet_time_synced ? "Synced from internet" :
        rtc_time_loaded ? "Running from hardware RTC" : "Connect to Wi-Fi to set the clock");
}

static void battery_tick(lv_timer_t *timer)
{
    (void)timer;
    uint8_t reg = 0x02;
    uint8_t raw[6];
    if (!battery_monitor || i2c_master_transmit_receive(battery_monitor, &reg, 1, raw, sizeof(raw), 50) != ESP_OK) {
        lv_label_set_text(battery_label, "BAT --");
        return;
    }
    battery_millivolts = ((raw[0] << 8) | raw[1]) * 5 / 4;
    battery_milliamps = (int16_t)((raw[4] << 8) | raw[5]) * 3 / 10;
    battery_percent = (battery_millivolts - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
    battery_percent = battery_percent < 0 ? 0 : battery_percent > 100 ? 100 : battery_percent;
    battery_history[battery_history_head] = battery_percent;
    battery_history_head = (battery_history_head + 1) % BATTERY_HISTORY_POINTS;
    if (battery_history_count < BATTERY_HISTORY_POINTS) battery_history_count++;
    lv_label_set_text_fmt(battery_label, "BAT %d%%", battery_percent);
    if (battery_metrics) {
        lv_label_set_text_fmt(battery_metrics, "%d.%03d V    %+d mA    %d%%",
            battery_millivolts / 1000, battery_millivolts % 1000, battery_milliamps, battery_percent);
    }
    if (battery_chart) lv_chart_set_next_value(battery_chart, battery_series, battery_percent);
}

static void battery_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x41,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &config, &battery_monitor) != ESP_OK) {
        battery_monitor = NULL;
        return;
    }
    uint8_t ina_config[] = {0x00, 0x05, 0x27};
    uint8_t ina_calibration[] = {0x05, 0x0D, 0x55};
    i2c_master_transmit(battery_monitor, ina_config, sizeof(ina_config), 50);
    i2c_master_transmit(battery_monitor, ina_calibration, sizeof(ina_calibration), 50);
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} http_buffer_t;

static void wifi_connect_task(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        esp_err_t error = ESP_FAIL;
        for (int attempt = 0; attempt < 3 && wifi_should_connect; ++attempt) {
            wifi_connecting = true;
            error = esp_wifi_connect();
            if (error == ESP_OK) break;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (error != ESP_OK) {
            wifi_connecting = false;
            ESP_LOGE("tab5-os", "Wi-Fi connect failed: %s", esp_err_to_name(error));
        }
    }
}

static bool request_wifi_connect(void)
{
    if (!wifi_connect_task_handle &&
        xTaskCreate(wifi_connect_task, "wifi-connect", 4096, NULL, 4, &wifi_connect_task_handle) != pdPASS) {
        return false;
    }
    xTaskNotifyGive(wifi_connect_task_handle);
    return true;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && wifi_should_connect) {
        request_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        if (wifi_should_connect && wifi_retries++ < 3) {
            request_wifi_connect();
        } else {
            wifi_connecting = false;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        snprintf(wifi_ip, sizeof(wifi_ip), IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retries = 0;
        wifi_connecting = false;
        wifi_connected = true;
        start_sntp();
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

static void load_chat_config(void)
{
    nvs_handle_t handle;
    if (nvs_open("tab5", NVS_READWRITE, &handle) != ESP_OK) return;
    size_t url_size = sizeof(chat_relay_url);
    size_t token_size = sizeof(chat_device_token);
    bool changed = false;
    if (nvs_get_str(handle, "relay_url", chat_relay_url, &url_size) != ESP_OK && CHAT_RELAY_URL[0]) {
        snprintf(chat_relay_url, sizeof(chat_relay_url), "%s", CHAT_RELAY_URL);
        changed |= nvs_set_str(handle, "relay_url", chat_relay_url) == ESP_OK;
    }
    if (nvs_get_str(handle, "device_token", chat_device_token, &token_size) != ESP_OK && CHAT_DEVICE_TOKEN[0]) {
        snprintf(chat_device_token, sizeof(chat_device_token), "%s", CHAT_DEVICE_TOKEN);
        changed |= nvs_set_str(handle, "device_token", chat_device_token) == ESP_OK;
    }
    if (changed) nvs_commit(handle);
    nvs_close(handle);
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

static esp_err_t ebook_http_event(esp_http_client_event_t *event)
{
    FILE *file = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !file) return ESP_OK;
    return fwrite(event->data, 1, event->data_len, file) == (size_t)event->data_len ? ESP_OK : ESP_FAIL;
}

static bool start_voice_mic(void)
{
    if (!voice_mic) voice_mic = bsp_audio_codec_microphone_init();
    if (!voice_mic) {
        snprintf(chat_error, sizeof(chat_error), "Microphone unavailable");
        return false;
    }
    if (!voice_mic_open) {
        esp_codec_dev_sample_info_t format = {
            .sample_rate = VOICE_INPUT_RATE,
            .channel = VOICE_CHANNELS,
            .bits_per_sample = 16,
        };
        if (esp_codec_dev_open(voice_mic, &format) != ESP_CODEC_DEV_OK) {
            snprintf(chat_error, sizeof(chat_error), "Could not start microphone");
            return false;
        }
        esp_codec_dev_set_in_gain(voice_mic, 80.0f);
        voice_mic_open = true;
    }
    return true;
}

static void stop_voice_mic(void)
{
    if (!voice_mic_open) return;
    esp_codec_dev_close(voice_mic);
    voice_mic_open = false;
}

static char *capture_voice_wav(size_t *size)
{
    if (!start_voice_mic()) return NULL;

    const size_t sample_capacity = VOICE_MAX_SECONDS * VOICE_RATE;
    char *wav = heap_caps_malloc(sizeof(wav_header_t) + sample_capacity * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        snprintf(chat_error, sizeof(chat_error), "Out of memory");
        voice_recording = false;
        stop_voice_mic();
        return NULL;
    }

    int16_t raw[VOICE_CHANNELS * 3 * 160];
    int16_t *pcm = (int16_t *)(wav + sizeof(wav_header_t));
    size_t written = 0;
    while (written < sample_capacity && !voice_stop_requested) {
        size_t count = sample_capacity - written;
        if (count > 160) count = 160;
        size_t raw_bytes = count * 3 * VOICE_CHANNELS * sizeof(int16_t);
        if (esp_codec_dev_read(voice_mic, raw, raw_bytes) != ESP_CODEC_DEV_OK) {
            snprintf(chat_error, sizeof(chat_error), "Microphone read failed");
            free(wav);
            voice_recording = false;
            stop_voice_mic();
            return NULL;
        }
        uint16_t peak = 0;
        for (size_t i = 0; i < count; ++i) {
            int32_t sum = 0;
            for (size_t j = 0; j < 3; ++j) {
                sum += raw[(i * 3 + j) * VOICE_CHANNELS + VOICE_MIC_CHANNEL];
                for (size_t channel = 0; channel < VOICE_CHANNELS; ++channel) {
                    int32_t sample = raw[(i * 3 + j) * VOICE_CHANNELS + channel];
                    uint16_t level = sample < 0 ? (uint16_t)-sample : (uint16_t)sample;
                    if (level > peak) peak = level;
                }
            }
            pcm[written + i] = sum / 3;
        }
        voice_level = peak / 32 > 100 ? 100 : peak / 32;
        written += count;
    }
    voice_recording = false;
    voice_level = 0;
    stop_voice_mic();
    if (written < VOICE_RATE / 4) {
        snprintf(chat_error, sizeof(chat_error), "Recording was too short");
        free(wav);
        return NULL;
    }
    const size_t data_size = written * sizeof(int16_t);
    wav_header_t header = {
        .riff = {'R', 'I', 'F', 'F'}, .riff_size = 36 + data_size,
        .wave = {'W', 'A', 'V', 'E'}, .fmt = {'f', 'm', 't', ' '},
        .fmt_size = 16, .format = 1, .channels = 1,
        .sample_rate = VOICE_RATE, .byte_rate = VOICE_RATE * sizeof(int16_t),
        .block_align = sizeof(int16_t), .bits_per_sample = 16,
        .data = {'d', 'a', 't', 'a'}, .data_size = data_size,
    };
    memcpy(wav, &header, sizeof(header));
    *size = sizeof(header) + data_size;
    return wav;
}

static void chat_request_task(void *argument)
{
    (void)argument;
    // ponytail: keep the worker alive; ESP-IDF rejects its PSRAM-linked pthread cleanup callback on deletion.
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        chat_job_t job = chat_job;
        chat_ok = false;
        chat_error[0] = '\0';
        char *response_data = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        cJSON *request_json = NULL;
        char *request_body = NULL;
        size_t request_size = 0;
        esp_http_client_handle_t client = NULL;
        char url[sizeof(chat_relay_url)];
        snprintf(url, sizeof(url), "%s", chat_relay_url);
        if (!response_data) {
            snprintf(chat_error, sizeof(chat_error), "Out of memory");
            goto done;
        }

        if (job == CHAT_JOB_VOICE) {
            char *path = strrchr(url, '/');
            if (!path || sizeof(url) - (size_t)(path - url) < sizeof("/transcribe")) {
                snprintf(chat_error, sizeof(chat_error), "Invalid relay URL");
                goto done;
            }
            snprintf(path, sizeof(url) - (size_t)(path - url), "/transcribe");
            request_body = capture_voice_wav(&request_size);
        } else {
            request_json = cJSON_CreateObject();
            if (request_json) {
                cJSON_AddStringToObject(request_json, "message", chat_prompt);
                if (chat_response_id[0]) {
                    cJSON_AddStringToObject(request_json, "previous_response_id", chat_response_id);
                }
                request_body = cJSON_PrintUnformatted(request_json);
                if (request_body) request_size = strlen(request_body);
            }
        }
        if (!request_body) {
            if (!chat_error[0]) snprintf(chat_error, sizeof(chat_error), "Could not prepare request");
            goto done;
        }

        http_buffer_t buffer = {.data = response_data, .capacity = 16384};
        esp_http_client_config_t config = {
            .url = url,
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
        snprintf(authorization, sizeof(authorization), "Bearer %s", chat_device_token);
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", job == CHAT_JOB_VOICE ? "audio/wav" : "application/json");
        esp_http_client_set_header(client, "Authorization", authorization);
        esp_http_client_set_post_field(client, request_body, request_size);
        esp_err_t error = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        if (error != ESP_OK || status != 200) {
            snprintf(chat_error, sizeof(chat_error), "Relay request failed (%d)", status);
            goto done;
        }

        cJSON *response_json = cJSON_Parse(response_data);
        cJSON *text = response_json ? cJSON_GetObjectItemCaseSensitive(response_json, "text") : NULL;
        cJSON *response_id = response_json ? cJSON_GetObjectItemCaseSensitive(response_json, "response_id") : NULL;
        if (!cJSON_IsString(text) || (job == CHAT_JOB_MESSAGE && !cJSON_IsString(response_id))) {
            snprintf(chat_error, sizeof(chat_error), "Invalid relay response");
        } else if (!text->valuestring[0]) {
            snprintf(chat_error, sizeof(chat_error), "No speech heard");
        } else {
            snprintf(chat_response, sizeof(chat_response), "%s", text->valuestring);
            if (job == CHAT_JOB_MESSAGE) {
                snprintf(chat_response_id, sizeof(chat_response_id), "%s", response_id->valuestring);
            }
            chat_ok = true;
        }
        cJSON_Delete(response_json);

done:
        if (client) esp_http_client_cleanup(client);
        free(request_body);
        cJSON_Delete(request_json);
        free(response_data);
        chat_completed_job = job;
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
    if (chat_wave && chat_wave_series) {
        lv_chart_set_next_value(chat_wave, chat_wave_series, voice_recording ? voice_level : 0);
    }
    if (chat_busy && chat_job == CHAT_JOB_VOICE && !voice_recording) {
        lv_label_set_text(chat_status, "Transcribing...");
        lv_label_set_text(chat_voice_label, "Start");
        lv_obj_add_state(chat_voice_button, LV_STATE_DISABLED);
    }
    if (!chat_done) return;
    chat_done = false;
    if (chat_completed_job == CHAT_JOB_VOICE) {
        if (chat_ok) lv_textarea_set_text(chat_input, chat_response);
        lv_label_set_text(chat_status, chat_ok ? "Ready to send" :
            (chat_error[0] ? chat_error : "Transcription failed"));
    } else {
        if (chat_ok) chat_append("AI", chat_response);
        else chat_append("Error", chat_error[0] ? chat_error : "Unknown error");
        lv_textarea_set_text(chat_output, chat_history);
        lv_textarea_set_cursor_pos(chat_output, LV_TEXTAREA_CURSOR_LAST);
        lv_label_set_text(chat_status, chat_ok ? "Ready" : "Request failed");
    }
    lv_obj_remove_state(chat_send_button, LV_STATE_DISABLED);
    lv_obj_remove_state(chat_voice_button, LV_STATE_DISABLED);
    lv_label_set_text(chat_voice_label, "Start");
}

static bool chat_start_job(chat_job_t job)
{
    chat_job = job;
    chat_busy = true;
    chat_done = false;
    if (!chat_task && xTaskCreateWithCaps(chat_request_task, "ai-chat", 16384, NULL, 4, &chat_task,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        chat_busy = false;
        return false;
    }
    xTaskNotifyGive(chat_task);
    return true;
}

static void chat_send_clicked(lv_event_t *event)
{
    (void)event;
    if (chat_busy) return;
    if (!wifi_connected) {
        lv_label_set_text(chat_status, "Connect to Wi-Fi first");
        return;
    }
    if (!chat_relay_url[0] || !chat_device_token[0]) {
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
    lv_obj_add_state(chat_voice_button, LV_STATE_DISABLED);
    if (!chat_start_job(CHAT_JOB_MESSAGE)) {
        lv_obj_remove_state(chat_send_button, LV_STATE_DISABLED);
        lv_obj_remove_state(chat_voice_button, LV_STATE_DISABLED);
        lv_label_set_text(chat_status, "Could not start request");
    }
}

static void chat_voice_clicked(lv_event_t *event)
{
    (void)event;
    if (voice_recording) {
        voice_stop_requested = true;
        lv_label_set_text(chat_status, "Finishing recording...");
        lv_obj_add_state(chat_voice_button, LV_STATE_DISABLED);
        return;
    }
    if (chat_busy) return;
    if (!wifi_connected) {
        lv_label_set_text(chat_status, "Connect to Wi-Fi first");
        return;
    }
    if (!chat_relay_url[0] || !chat_device_token[0]) {
        lv_label_set_text(chat_status, "Relay is not configured");
        return;
    }
    voice_stop_requested = false;
    voice_recording = true;
    voice_level = 0;
    lv_label_set_text(chat_status, "Recording - press Stop when done");
    lv_label_set_text(chat_voice_label, "Stop");
    lv_obj_add_state(chat_send_button, LV_STATE_DISABLED);
    if (!chat_start_job(CHAT_JOB_VOICE)) {
        voice_recording = false;
        lv_obj_remove_state(chat_send_button, LV_STATE_DISABLED);
        lv_obj_remove_state(chat_voice_button, LV_STATE_DISABLED);
        lv_label_set_text(chat_voice_label, "Start");
        lv_label_set_text(chat_status, "Could not start microphone");
    }
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

static void chat_apply_preferences(void)
{
    const lv_font_t *font = chat_large_text ? &lv_font_montserrat_28 : &lv_font_montserrat_14;
    lv_obj_set_style_text_font(chat_output, font, 0);
    lv_obj_set_style_text_font(chat_input, font, 0);
    lv_label_set_text(chat_text_label, chat_large_text ? "Text: Large" : "Text: Small");
    lv_label_set_text(chat_keyboard_label, chat_keyboard_visible ? "Hide keys" : "Show keys");
    if (chat_keyboard_visible) {
        lv_obj_remove_flag(chat_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(chat_output, 230);
    } else {
        lv_obj_add_flag(chat_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(chat_output, 560);
    }
}

static void chat_keyboard_clicked(lv_event_t *event)
{
    (void)event;
    chat_keyboard_visible = !chat_keyboard_visible;
    chat_apply_preferences();
}

static void chat_text_clicked(lv_event_t *event)
{
    (void)event;
    chat_large_text = !chat_large_text;
    chat_apply_preferences();
}

static bool browser_prefix(const char *text, const char *prefix)
{
    while (*prefix) {
        if (!*text || tolower((unsigned char)*text++) != tolower((unsigned char)*prefix++)) return false;
    }
    return true;
}

static const char *browser_find(const char *text, const char *needle)
{
    for (; *text; ++text) if (browser_prefix(text, needle)) return text;
    return NULL;
}

static void browser_newline(char *output, size_t *length, size_t capacity)
{
    while (*length && output[*length - 1] == ' ') (*length)--;
    if (*length && output[*length - 1] != '\n' && *length + 1 < capacity) output[(*length)++] = '\n';
}

static void browser_html_to_text(const char *html, char *output, size_t capacity)
{
    size_t length = 0;
    bool space = false;
    for (const char *p = html; *p && length + 1 < capacity;) {
        if (*p == '<') {
            const char *tag = p + 1;
            while (isspace((unsigned char)*tag)) tag++;
            bool closing = *tag == '/';
            if (closing) tag++;
            while (isspace((unsigned char)*tag)) tag++;
            if (!closing && (browser_prefix(tag, "script") || browser_prefix(tag, "style"))) {
                const char *close = browser_find(tag, browser_prefix(tag, "script") ? "</script" : "</style");
                p = close ? close : p + strlen(p);
                continue;
            }
            if (browser_prefix(tag, "br") || browser_prefix(tag, "p") || browser_prefix(tag, "div") ||
                browser_prefix(tag, "li") || browser_prefix(tag, "h1") || browser_prefix(tag, "h2") ||
                browser_prefix(tag, "h3")) browser_newline(output, &length, capacity);
            const char *end = strchr(tag, '>');
            p = end ? end + 1 : p + strlen(p);
            continue;
        }
        if (*p == '&') {
            const struct { const char *entity; char value; } entities[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&nbsp;", ' '},
            };
            bool decoded = false;
            for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); ++i) {
                if (browser_prefix(p, entities[i].entity)) {
                    if (entities[i].value == ' ') space = true;
                    else output[length++] = entities[i].value;
                    p += strlen(entities[i].entity);
                    decoded = true;
                    break;
                }
            }
            if (decoded) continue;
            if (p[1] == '#') {
                char *end;
                long value = strtol(p + 2 + (p[2] == 'x' || p[2] == 'X'), &end, (p[2] == 'x' || p[2] == 'X') ? 16 : 10);
                if (*end == ';') {
                    output[length++] = value >= 32 && value < 127 ? (char)value : '?';
                    p = end + 1;
                    continue;
                }
            }
        }
        unsigned char value = (unsigned char)*p++;
        if (isspace(value)) {
            space = true;
        } else {
            if (space && length && output[length - 1] != '\n' && length + 1 < capacity) output[length++] = ' ';
            space = false;
            if (value < 128) output[length++] = value;
            else {
                output[length++] = '?';
                while ((*p & 0xc0) == 0x80) p++;
            }
        }
    }
    while (length && isspace((unsigned char)output[length - 1])) length--;
    output[length] = '\0';
}

static bool browser_resolve_url(const char *base, const char *link, char *output, size_t capacity)
{
    if (!link[0] || link[0] == '#' || browser_prefix(link, "mailto:") || browser_prefix(link, "javascript:")) return false;
    if (browser_prefix(link, "http://") || browser_prefix(link, "https://")) {
        size_t length = strlen(link);
        if (length >= capacity) return false;
        memcpy(output, link, length + 1);
        return true;
    }
    const char *scheme_end = strstr(base, "://");
    if (!scheme_end) return false;
    if (link[0] == '/' && link[1] == '/') {
        size_t prefix = scheme_end - base;
        size_t length = strlen(link);
        if (prefix + 1 + length >= capacity) return false;
        memcpy(output, base, prefix);
        output[prefix] = ':';
        memcpy(output + prefix + 1, link, length + 1);
        return true;
    }
    const char *host_end = strchr(scheme_end + 3, '/');
    if (!host_end) host_end = base + strlen(base);
    if (link[0] == '/') {
        size_t prefix = host_end - base;
        size_t length = strlen(link);
        if (prefix + length >= capacity) return false;
        memcpy(output, base, prefix);
        memcpy(output + prefix, link, length + 1);
        return true;
    }
    const char *path_end = strrchr(base, '/');
    if (!path_end || path_end < host_end) path_end = host_end;
    size_t prefix = path_end - base;
    size_t length = strlen(link);
    if (prefix + 1 + length >= capacity) return false;
    memcpy(output, base, prefix);
    output[prefix] = '/';
    memcpy(output + prefix + 1, link, length + 1);
    return true;
}

static void browser_extract_links(const char *html, const char *base)
{
    browser_link_count = 0;
    const char *anchor = html;
    while (browser_link_count < BROWSER_MAX_LINKS && (anchor = browser_find(anchor, "<a"))) {
        if (!isspace((unsigned char)anchor[2]) && anchor[2] != '>') {
            anchor += 2;
            continue;
        }
        const char *tag_end = strchr(anchor, '>');
        const char *href = browser_find(anchor, "href");
        if (!tag_end || !href || href > tag_end) {
            anchor += 2;
            continue;
        }
        href += 4;
        while (href < tag_end && isspace((unsigned char)*href)) href++;
        if (href == tag_end || *href++ != '=') {
            anchor = tag_end + 1;
            continue;
        }
        while (href < tag_end && isspace((unsigned char)*href)) href++;
        char quote = (*href == '"' || *href == '\'') ? *href++ : 0;
        const char *href_end = href;
        while (href_end < tag_end && (quote ? *href_end != quote : !isspace((unsigned char)*href_end) && *href_end != '>')) href_end++;
        char raw[256];
        size_t raw_length = href_end - href;
        if (raw_length >= sizeof(raw)) raw_length = sizeof(raw) - 1;
        memcpy(raw, href, raw_length);
        raw[raw_length] = '\0';
        char *amp;
        while ((amp = strstr(raw, "&amp;"))) memmove(amp + 1, amp + 5, strlen(amp + 5) + 1), *amp = '&';
        if (!browser_resolve_url(base, raw, browser_links[browser_link_count], sizeof(browser_links[0]))) {
            anchor = tag_end + 1;
            continue;
        }
        const char *close = browser_find(tag_end + 1, "</a");
        char label_html[192] = "Link";
        if (close) {
            size_t label_length = close - tag_end - 1;
            if (label_length >= sizeof(label_html)) label_length = sizeof(label_html) - 1;
            memcpy(label_html, tag_end + 1, label_length);
            label_html[label_length] = '\0';
        }
        browser_html_to_text(label_html, browser_link_labels[browser_link_count], sizeof(browser_link_labels[0]));
        if (!browser_link_labels[browser_link_count][0]) snprintf(browser_link_labels[browser_link_count], sizeof(browser_link_labels[0]), "Link");
        browser_link_count++;
        anchor = close ? close + 3 : tag_end + 1;
    }
}

static void browser_request_task(void *argument)
{
    (void)argument;
    // ponytail: persistent PSRAM task avoids repeated TLS stack allocation on scarce internal RAM.
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        browser_ok = false;
        browser_error[0] = '\0';
        char *html = heap_caps_calloc(1, BROWSER_MAX_HTML, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        char *text = heap_caps_malloc(BROWSER_MAX_TEXT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!html || !text) {
            snprintf(browser_error, sizeof(browser_error), "Out of memory");
            free(html);
            free(text);
            goto done;
        }
        http_buffer_t buffer = {.data = html, .capacity = BROWSER_MAX_HTML};
        esp_http_client_config_t config = {
            .url = browser_pending_url,
            .event_handler = chat_http_event,
            .user_data = &buffer,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 20000,
            .buffer_size = 2048,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            snprintf(browser_error, sizeof(browser_error), "Could not start HTTPS");
            free(html);
            free(text);
            goto done;
        }
        esp_http_client_set_header(client, "User-Agent", "Tab5OS/1.0");
        esp_http_client_set_header(client, "Accept", "text/html,text/plain");
        ESP_LOGI("tab5-os", "Browser loading %.120s", browser_pending_url);
        esp_err_t error = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        if (error != ESP_OK || status < 200 || status >= 300 || !buffer.length) {
            snprintf(browser_error, sizeof(browser_error), "Page failed (%d)", status);
            free(html);
            free(text);
            goto done;
        }
        browser_extract_links(html, browser_pending_url);
        browser_html_to_text(html, text, BROWSER_MAX_TEXT);
        ESP_LOGI("tab5-os", "Browser loaded %u bytes, %u links", (unsigned)buffer.length, (unsigned)browser_link_count);
        free(html);
        free(browser_result);
        browser_result = text;
        snprintf(browser_url, sizeof(browser_url), "%s", browser_pending_url);
        browser_ok = true;
done:
        browser_busy = false;
        browser_done = true;
    }
}

static void browser_apply_layout(void)
{
    if (!browser_keyboard || !browser_page) return;
    lv_label_set_text(browser_keys_label, browser_keyboard_visible ? "Hide keys" : "Show keys");
    if (browser_keyboard_visible) {
        lv_obj_remove_flag(browser_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(browser_page, 450);
    } else {
        lv_obj_add_flag(browser_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(browser_page, 780);
    }
}

static void browser_render(void)
{
    lv_obj_clean(browser_page);
    lv_obj_t *body = lv_label_create(browser_page);
    lv_obj_set_width(body, 580);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body, browser_ok ? (browser_result && browser_result[0] ? browser_result : "No readable text") : browser_error);
    if (!browser_ok || !browser_link_count) return;

    lv_obj_t *heading = lv_label_create(browser_page);
    lv_label_set_text(heading, "Links");
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_28, 0);
    for (size_t i = 0; i < browser_link_count; ++i) {
        lv_obj_t *link = button(browser_page, browser_link_labels[i], NULL);
        lv_obj_set_size(link, 580, 58);
        lv_obj_add_event_cb(link, browser_link_clicked, LV_EVENT_CLICKED, browser_links[i]);
        lv_obj_t *label = lv_obj_get_child(link, 0);
        lv_obj_set_width(label, 520);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    }
}

static bool browser_start_url(const char *requested, bool add_history)
{
    if (browser_busy) return false;
    while (isspace((unsigned char)*requested)) requested++;
    char normalized[256];
    if (!strstr(requested, "://")) snprintf(normalized, sizeof(normalized), "https://%s", requested);
    else snprintf(normalized, sizeof(normalized), "%s", requested);
    size_t length = strlen(normalized);
    while (length && isspace((unsigned char)normalized[length - 1])) normalized[--length] = '\0';
    if ((!browser_prefix(normalized, "http://") && !browser_prefix(normalized, "https://")) || length < 10) {
        lv_label_set_text(browser_status, "Enter an http:// or https:// address");
        return false;
    }
    if (add_history && browser_url[0] && strcmp(browser_url, normalized)) {
        if (browser_history_count == 8) {
            memmove(browser_history, browser_history + 1, sizeof(browser_history) - sizeof(browser_history[0]));
            browser_history_count--;
        }
        snprintf(browser_history[browser_history_count++], sizeof(browser_history[0]), "%s", browser_url);
    }
    snprintf(browser_pending_url, sizeof(browser_pending_url), "%s", normalized);
    browser_busy = true;
    browser_done = false;
    browser_keyboard_visible = false;
    browser_apply_layout();
    lv_label_set_text(browser_status, "Loading...");
    if (browser_page) {
        lv_obj_clean(browser_page);
        lv_obj_t *loading = lv_label_create(browser_page);
        lv_label_set_text(loading, "Loading...");
    }
    if (!browser_task && xTaskCreateWithCaps(browser_request_task, "browser", 12288, NULL, 4, &browser_task,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        browser_busy = false;
        lv_label_set_text(browser_status, "Could not start browser");
        return false;
    }
    xTaskNotifyGive(browser_task);
    return true;
}

static void browser_link_clicked(lv_event_t *event)
{
    browser_start_url(lv_event_get_user_data(event), true);
}

static void browser_go_clicked(lv_event_t *event)
{
    (void)event;
    if (!wifi_connected) {
        lv_label_set_text(browser_status, "Connect to Wi-Fi first");
        return;
    }
    browser_start_url(lv_textarea_get_text(browser_url_area), true);
}

static void browser_back_clicked(lv_event_t *event)
{
    (void)event;
    if (!browser_history_count || browser_busy) return;
    char previous[256];
    snprintf(previous, sizeof(previous), "%s", browser_history[--browser_history_count]);
    browser_start_url(previous, false);
}

static void browser_reload_clicked(lv_event_t *event)
{
    (void)event;
    browser_start_url(browser_url, false);
}

static void browser_keys_clicked(lv_event_t *event)
{
    (void)event;
    browser_keyboard_visible = !browser_keyboard_visible;
    browser_apply_layout();
}

static void browser_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!browser_done) return;
    browser_done = false;
    lv_label_set_text(browser_status, browser_ok ? "Loaded" : browser_error);
    if (browser_ok) lv_textarea_set_text(browser_url_area, browser_url);
    browser_render();
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
    if (browser_timer) {
        lv_timer_delete(browser_timer);
        browser_timer = NULL;
    }
    if (ebook_timer) {
        lv_timer_delete(ebook_timer);
        ebook_timer = NULL;
    }
    if (ota_timer) {
        lv_timer_delete(ota_timer);
        ota_timer = NULL;
    }
    if (gpio_timer) {
        lv_timer_delete(gpio_timer);
        gpio_timer = NULL;
        for (size_t i = 0; i < GPIO_CONTROL_COUNT; i++) {
            if (gpio_controls[i].mode) gpio_reset_pin(gpio_controls[i].pin);
            gpio_controls[i].mode_label = NULL;
            gpio_controls[i].level_label = NULL;
        }
    }
    wifi_status = NULL;
    wifi_list = NULL;
    chat_output = NULL;
    chat_input = NULL;
    chat_status = NULL;
    chat_send_button = NULL;
    chat_voice_button = NULL;
    chat_voice_label = NULL;
    chat_wave = NULL;
    chat_wave_series = NULL;
    chat_keyboard = NULL;
    chat_keyboard_label = NULL;
    chat_text_label = NULL;
    browser_status = NULL;
    browser_url_area = NULL;
    browser_page = NULL;
    browser_keyboard = NULL;
    browser_keys_label = NULL;
    ebook_text = NULL;
    ebook_status = NULL;
    ebook_prev = NULL;
    ebook_next = NULL;
    ota_status = NULL;
    ota_button = NULL;
    battery_metrics = NULL;
    battery_chart = NULL;
    battery_series = NULL;
    clock_time = NULL;
    clock_date = NULL;
    clock_status = NULL;
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

static bool ebook_supported(const char *name)
{
    const char *extension = strrchr(name, '.');
    return extension && browser_prefix(extension, ".txt") && !extension[4];
}

static bool ebook_default_installed(const ebook_default_t *book)
{
    char path[256];
    snprintf(path, sizeof(path), SD_PATH "/BOOKS/%s", book->filename);
    struct stat info;
    return stat(path, &info) == 0 && info.st_size > 1024;
}

static bool ebook_download_default(const ebook_default_t *book)
{
    if (ebook_default_installed(book)) return true;
    char path[256];
    char temporary[256];
    snprintf(path, sizeof(path), SD_PATH "/BOOKS/%s", book->filename);
    snprintf(temporary, sizeof(temporary), "%s", path);
    snprintf(strrchr(temporary, '.'), 5, ".TMP");
    FILE *file = fopen(temporary, "wb");
    if (!file) {
        ESP_LOGE("tab5-os", "Could not create %s", temporary);
        return false;
    }
    esp_http_client_config_t config = {
        .url = book->url,
        .event_handler = ebook_http_event,
        .user_data = file,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(file);
        remove(temporary);
        ESP_LOGE("tab5-os", "Could not start download for %s", book->filename);
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", "Tab5OS/1.0");
    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    bool saved = fclose(file) == 0 && error == ESP_OK && status >= 200 && status < 300 && rename(temporary, path) == 0;
    if (!saved) remove(temporary);
    ESP_LOGI("tab5-os", "Default ebook %s: %s (%d)", book->filename, saved ? "saved" : "failed", status);
    return saved;
}

static void ebook_download_task(void *argument)
{
    (void)argument;
    while (!wifi_connected) vTaskDelay(pdMS_TO_TICKS(500));
    for (size_t i = 0; i < sizeof(ebook_defaults) / sizeof(ebook_defaults[0]); ++i) {
        ebook_download_default(&ebook_defaults[i]);
    }
    ebook_download_busy = false;
    ebook_download_done = true;
    ebook_download_task_handle = NULL;
    vTaskDelete(NULL);
}

static void ebook_download_tick(lv_timer_t *timer)
{
    (void)timer;
    if (ebook_download_done) {
        ebook_download_done = false;
        show_ebooks();
    }
}

static bool ebook_defaults_missing(void)
{
    for (size_t i = 0; i < sizeof(ebook_defaults) / sizeof(ebook_defaults[0]); ++i) {
        if (!ebook_default_installed(&ebook_defaults[i])) return true;
    }
    return false;
}

static void ebook_start_default_downloads(void)
{
    if (!sd_ready || !ebook_defaults_missing() || ebook_download_busy) return;
    ebook_download_busy = true;
    ebook_download_done = false;
    if (xTaskCreateWithCaps(ebook_download_task, "ebooks", 10240, NULL, 4, &ebook_download_task_handle,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) ebook_download_busy = false;
}

static void ebook_load_page(void)
{
    if (!ebook_buffer) ebook_buffer = heap_caps_malloc(EBOOK_PAGE_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ebook_buffer) {
        lv_label_set_text(ebook_status, "Out of memory");
        return;
    }
    FILE *file = fopen(ebook_path, "rb");
    if (!file || fseek(file, ebook_offset, SEEK_SET)) {
        if (file) fclose(file);
        lv_label_set_text(ebook_status, "Could not open book");
        return;
    }
    size_t raw_read = fread(ebook_buffer, 1, EBOOK_PAGE_BYTES, file);
    fclose(file);
    size_t read = 0;
    for (size_t i = 0; i < raw_read;) {
        unsigned char value = ebook_buffer[i++];
        if (value < 128) ebook_buffer[read++] = value ? value : ' ';
        else {
            ebook_buffer[read++] = '?';
            while (i < raw_read && ((unsigned char)ebook_buffer[i] & 0xc0) == 0x80) i++;
        }
    }
    ebook_buffer[read] = '\0';
    ebook_next_offset = ebook_offset + raw_read;
    ESP_LOGI("tab5-os", "Ebook loaded %u bytes at %ld from %.120s", (unsigned)raw_read, ebook_offset, ebook_path);
    lv_textarea_set_text(ebook_text, raw_read ? ebook_buffer : "End of book");
    lv_obj_scroll_to_y(ebook_text, 0, LV_ANIM_OFF);
    lv_label_set_text_fmt(ebook_status, "Page %lu  -  %ld KB", (unsigned long)(ebook_offset / EBOOK_PAGE_BYTES + 1), ebook_offset / 1024);
    if (ebook_offset) lv_obj_remove_state(ebook_prev, LV_STATE_DISABLED);
    else lv_obj_add_state(ebook_prev, LV_STATE_DISABLED);
    if (raw_read == EBOOK_PAGE_BYTES) lv_obj_remove_state(ebook_next, LV_STATE_DISABLED);
    else lv_obj_add_state(ebook_next, LV_STATE_DISABLED);
}

static void ebook_library_clicked(lv_event_t *event)
{
    (void)event;
    show_ebooks();
}

static void ebook_prev_clicked(lv_event_t *event)
{
    (void)event;
    ebook_offset = ebook_offset > EBOOK_PAGE_BYTES ? ebook_offset - EBOOK_PAGE_BYTES : 0;
    ebook_load_page();
}

static void ebook_next_clicked(lv_event_t *event)
{
    (void)event;
    ebook_offset = ebook_next_offset;
    ebook_load_page();
}

static void ebook_text_clicked(lv_event_t *event)
{
    lv_obj_t *label = lv_obj_get_child(lv_event_get_target(event), 0);
    ebook_large_text = !ebook_large_text;
    lv_obj_set_style_text_font(ebook_text, ebook_large_text ? &lv_font_montserrat_28 : &lv_font_montserrat_14, 0);
    lv_label_set_text(label, ebook_large_text ? "Text: Large" : "Text: Small");
}

static void show_ebook_reader(const char *path)
{
    snprintf(ebook_path, sizeof(ebook_path), "%s", path);
    ebook_offset = 0;
    clear_content();
    lv_obj_set_style_pad_row(content, 12, 0);

    const char *name = strrchr(ebook_path, '/');
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, name ? name + 1 : ebook_path);
    lv_obj_set_width(title, 620);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    ebook_status = lv_label_create(content);

    lv_obj_t *actions = lv_obj_create(content);
    lv_obj_set_size(actions, 640, 80);
    lv_obj_remove_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *library = button(actions, "Library", ebook_library_clicked);
    ebook_prev = button(actions, "Prev", ebook_prev_clicked);
    ebook_next = button(actions, "Next", ebook_next_clicked);
    lv_obj_t *text_size = button(actions, ebook_large_text ? "Text: Large" : "Text: Small", ebook_text_clicked);
    lv_obj_set_size(library, 135, 64);
    lv_obj_set_size(ebook_prev, 135, 64);
    lv_obj_set_size(ebook_next, 135, 64);
    lv_obj_set_size(text_size, 170, 64);
    for (size_t i = 0; i < 4; ++i) lv_obj_set_style_text_font(lv_obj_get_child(lv_obj_get_child(actions, i), 0), &lv_font_montserrat_14, 0);

    ebook_text = lv_textarea_create(content);
    lv_obj_set_size(ebook_text, 640, 860);
    lv_textarea_set_one_line(ebook_text, false);
    lv_textarea_set_cursor_click_pos(ebook_text, false);
    lv_obj_remove_flag(ebook_text, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_text_font(ebook_text, ebook_large_text ? &lv_font_montserrat_28 : &lv_font_montserrat_14, 0);
    ebook_load_page();
}

static void ebook_open_clicked(lv_event_t *event)
{
    show_ebook_reader(lv_event_get_user_data(event));
}

static void show_ebooks(void)
{
    clear_content();
    file_path_count = 0;
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "Ebooks");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_t *list = lv_list_create(content);
    lv_obj_set_size(list, 640, 940);
    lv_obj_t *back = lv_list_add_button(list, LV_SYMBOL_LEFT, "Back to apps");
    lv_obj_add_event_cb(back, home_clicked, LV_EVENT_CLICKED, NULL);
    if (!sd_ready) {
        lv_list_add_text(list, "Insert an SD card to read books");
        return;
    }

    bool created = mkdir(SD_PATH "/BOOKS", 0775) == 0;
    if (created) {
        FILE *welcome = fopen(SD_PATH "/BOOKS/WELCOME.TXT", "wb");
        if (welcome) {
            fputs("Welcome to Tab5 Books!\n\nCopy .txt ebooks into the BOOKS folder on the SD card. Use Next and Prev to move through the book, and Text to change the reading size.\n", welcome);
            fclose(welcome);
        }
    }
    DIR *dir = opendir(SD_PATH "/BOOKS");
    if (!dir) {
        lv_list_add_text(list, "Could not open /sdcard/BOOKS");
        return;
    }
    struct dirent *entry;
    while (file_path_count < 64 && (entry = readdir(dir))) {
        if (!ebook_supported(entry->d_name)) continue;
        char *full = file_paths[file_path_count++];
        const char *books = SD_PATH "/BOOKS/";
        size_t books_length = strlen(books);
        size_t name_length = strlen(entry->d_name);
        if (books_length + name_length >= sizeof(file_paths[0])) {
            file_path_count--;
            continue;
        }
        memcpy(full, books, books_length);
        memcpy(full + books_length, entry->d_name, name_length + 1);
        lv_obj_t *item = lv_list_add_button(list, LV_SYMBOL_FILE, entry->d_name);
        lv_obj_add_event_cb(item, ebook_open_clicked, LV_EVENT_CLICKED, full);
    }
    closedir(dir);
    if (!file_path_count) lv_list_add_text(list, "Copy .txt books into /sdcard/BOOKS");
    if (ebook_download_busy) {
        lv_list_add_text(list, "Downloading free classics...");
    } else if (ebook_defaults_missing()) {
        lv_list_add_text(list, "Classics download failed; restart to retry");
    }
    ebook_timer = lv_timer_create(ebook_download_tick, 500, NULL);
}

static void ebooks_clicked(lv_event_t *event)
{
    (void)event;
    show_ebooks();
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
        error = request_wifi_connect() ? ESP_OK : ESP_FAIL;
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
    lv_label_set_text(chat_status, voice_recording ? "Recording - press Stop when done" :
        chat_busy ? (chat_job == CHAT_JOB_VOICE ? "Transcribing..." : "Thinking...") :
        (!chat_relay_url[0] || !chat_device_token[0]) ? "Relay is not configured" : "Ready");
    lv_obj_set_width(chat_status, 640);

    chat_wave = lv_chart_create(content);
    lv_obj_set_size(chat_wave, 640, 100);
    lv_chart_set_type(chat_wave, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chat_wave, 60);
    lv_chart_set_range(chat_wave, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(chat_wave, 0, 0);
    chat_wave_series = lv_chart_add_series(chat_wave, lv_color_hex(0x29B6F6), LV_CHART_AXIS_PRIMARY_Y);

    chat_output = lv_textarea_create(content);
    lv_obj_set_size(chat_output, 640, 230);
    lv_textarea_set_text(chat_output, chat_history[0] ? chat_history : "Ask me anything.");
    lv_textarea_set_cursor_pos(chat_output, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_set_one_line(chat_output, false);

    chat_input = lv_textarea_create(content);
    lv_obj_set_size(chat_input, 640, 90);
    lv_textarea_set_placeholder_text(chat_input, "Message");
    lv_textarea_set_max_length(chat_input, 2000);

    lv_obj_t *actions = lv_obj_create(content);
    lv_obj_set_size(actions, 640, 90);
    lv_obj_remove_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    chat_send_button = button(actions, "Send", chat_send_clicked);
    lv_obj_set_size(chat_send_button, 180, 76);
    chat_voice_button = button(actions, voice_recording ? "Stop" : "Start", chat_voice_clicked);
    chat_voice_label = lv_obj_get_child(chat_voice_button, 0);
    lv_obj_set_size(chat_voice_button, 180, 76);
    lv_obj_t *new_chat = button(actions, "New", chat_new_clicked);
    lv_obj_set_size(new_chat, 180, 76);
    if (chat_busy) {
        lv_obj_add_state(chat_send_button, LV_STATE_DISABLED);
        if (!voice_recording) lv_obj_add_state(chat_voice_button, LV_STATE_DISABLED);
    }

    lv_obj_t *controls = lv_obj_create(content);
    lv_obj_set_size(controls, 640, 64);
    lv_obj_remove_flag(controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *keyboard_button = button(controls, "Hide keys", chat_keyboard_clicked);
    lv_obj_set_size(keyboard_button, 290, 54);
    chat_keyboard_label = lv_obj_get_child(keyboard_button, 0);
    lv_obj_t *text_button = button(controls, "Text: Small", chat_text_clicked);
    lv_obj_set_size(text_button, 290, 54);
    chat_text_label = lv_obj_get_child(text_button, 0);

    chat_keyboard = lv_keyboard_create(content);
    lv_obj_set_size(chat_keyboard, 640, 330);
    lv_keyboard_set_textarea(chat_keyboard, chat_input);
    chat_apply_preferences();
    chat_timer = lv_timer_create(chat_tick, 200, NULL);
}

static void chat_clicked(lv_event_t *event)
{
    (void)event;
    show_chat();
}

static void show_browser(void)
{
    clear_content();
    lv_obj_set_style_pad_row(content, 12, 0);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "Browser");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    browser_status = lv_label_create(content);
    lv_label_set_text(browser_status, browser_busy ? "Loading..." : browser_result ? "Loaded" : "Ready");
    lv_obj_set_width(browser_status, 640);

    lv_obj_t *address = lv_obj_create(content);
    lv_obj_set_size(address, 640, 82);
    lv_obj_remove_flag(address, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(address, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(address, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    browser_url_area = lv_textarea_create(address);
    lv_obj_set_size(browser_url_area, 470, 68);
    lv_textarea_set_one_line(browser_url_area, true);
    lv_textarea_set_max_length(browser_url_area, sizeof(browser_url) - 1);
    lv_textarea_set_text(browser_url_area, browser_url);
    lv_obj_add_event_cb(browser_url_area, browser_go_clicked, LV_EVENT_READY, NULL);
    lv_obj_t *go = button(address, "Go", browser_go_clicked);
    lv_obj_set_size(go, 130, 68);

    lv_obj_t *tools = lv_obj_create(content);
    lv_obj_set_size(tools, 640, 64);
    lv_obj_remove_flag(tools, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tools, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tools, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *back = button(tools, "Back", browser_back_clicked);
    lv_obj_set_size(back, 190, 54);
    lv_obj_t *keys = button(tools, browser_keyboard_visible ? "Hide keys" : "Show keys", browser_keys_clicked);
    browser_keys_label = lv_obj_get_child(keys, 0);
    lv_obj_set_size(keys, 190, 54);
    lv_obj_t *reload = button(tools, "Reload", browser_reload_clicked);
    lv_obj_set_size(reload, 190, 54);

    browser_page = lv_obj_create(content);
    lv_obj_set_size(browser_page, 640, 450);
    lv_obj_set_flex_flow(browser_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(browser_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(browser_page, lv_color_white(), 0);
    lv_obj_set_style_text_color(browser_page, lv_color_black(), 0);
    lv_obj_set_style_pad_row(browser_page, 12, 0);
    if (browser_result) browser_render();
    else {
        lv_obj_t *message = lv_label_create(browser_page);
        lv_obj_set_width(message, 580);
        lv_label_set_text(message, "Enter a web address, or tap Go to open example.com.");
    }

    browser_keyboard = lv_keyboard_create(content);
    lv_obj_set_size(browser_keyboard, 640, 330);
    lv_keyboard_set_textarea(browser_keyboard, browser_url_area);
    browser_apply_layout();
    browser_timer = lv_timer_create(browser_tick, 200, NULL);
    if (!browser_result && !browser_busy && wifi_connected) browser_start_url(browser_url, false);
}

static void browser_clicked(lv_event_t *event)
{
    (void)event;
    show_browser();
}

static void ota_update_task(void *argument)
{
    (void)argument;
    for (;;) {
        esp_http_client_config_t http = {
            .url = OTA_URL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 30000,
            .buffer_size = 1024,
            .buffer_size_tx = 1536,
            .keep_alive_enable = true,
            .max_redirection_count = 5,
        };
        esp_https_ota_config_t config = {.http_config = &http};
        ESP_LOGI("tab5-os", "OTA update starting");
        esp_err_t error = esp_https_ota(&config);
        ota_ok = error == ESP_OK;
        if (!ota_ok) snprintf(ota_error, sizeof(ota_error), "Update failed: %s", esp_err_to_name(error));
        ota_busy = false;
        ota_done = true;
        if (ota_ok) {
            ESP_LOGI("tab5-os", "OTA update installed; restarting");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
        // ponytail: keep the PSRAM worker alive; ESP-IDF rejects its cleanup callback on deletion.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

static void ota_clicked(lv_event_t *event)
{
    (void)event;
    if (ota_busy) return;
    if (!wifi_connected) {
        lv_label_set_text(ota_status, "Connect to Wi-Fi first");
        return;
    }
    if (ebook_download_busy) {
        lv_label_set_text(ota_status, "Wait for book downloads to finish");
        return;
    }
    ota_busy = true;
    ota_done = false;
    ota_ok = false;
    lv_label_set_text(ota_status, "Downloading update...");
    lv_obj_add_state(ota_button, LV_STATE_DISABLED);
    if (ota_task_handle) {
        xTaskNotifyGive(ota_task_handle);
    } else if (xTaskCreate(ota_update_task, "ota", 6144, NULL, 4, &ota_task_handle) != pdPASS) {
        ota_busy = false;
        lv_obj_remove_state(ota_button, LV_STATE_DISABLED);
        lv_label_set_text(ota_status, "Could not start updater");
    }
}

static void ota_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!ota_done || !ota_status) return;
    ota_done = false;
    lv_label_set_text(ota_status, ota_ok ? "Installed. Restarting..." : ota_error);
    if (!ota_ok && ota_button) lv_obj_remove_state(ota_button, LV_STATE_DISABLED);
}

static void system_clicked(lv_event_t *event)
{
    (void)event;
    clear_content();
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "System");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    lv_obj_t *info = lv_label_create(content);
    lv_label_set_text_fmt(info,
        "Tab5 OS %s\n\nESP32-P4 rev %d.%d\n%d CPU cores\n%lu KB free RAM\n32 MB PSRAM\n720 x 1280 ST7121\n\nInternal: %s\nSD card: %s",
        esp_app_get_description()->version,
        chip.revision / 100, chip.revision % 100, chip.cores,
        (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
        internal_ready ? "mounted" : "unavailable", sd_ready ? "mounted" : "not inserted");
    lv_obj_set_style_text_line_space(info, 8, 0);
    battery_metrics = lv_label_create(content);
    lv_label_set_text_fmt(battery_metrics, "%d.%03d V    %+d mA    %d%%",
        battery_millivolts / 1000, battery_millivolts % 1000, battery_milliamps, battery_percent);
    lv_obj_t *history_title = lv_label_create(content);
    lv_label_set_text(history_title, "Battery percentage - last 5 minutes");
    battery_chart = lv_chart_create(content);
    lv_obj_set_size(battery_chart, 620, 260);
    lv_chart_set_type(battery_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(battery_chart, BATTERY_HISTORY_POINTS);
    lv_chart_set_range(battery_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(battery_chart, 5, 6);
    battery_series = lv_chart_add_series(battery_chart, lv_palette_main(LV_PALETTE_GREEN), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(battery_chart, battery_series, LV_CHART_POINT_NONE);
    uint8_t first = (battery_history_head + BATTERY_HISTORY_POINTS - battery_history_count) % BATTERY_HISTORY_POINTS;
    for (uint8_t i = 0; i < battery_history_count; i++) {
        lv_chart_set_next_value(battery_chart, battery_series,
            battery_history[(first + i) % BATTERY_HISTORY_POINTS]);
    }
    ota_button = button(content, "Install latest", ota_clicked);
    lv_obj_set_size(ota_button, 320, 82);
    if (ota_busy) lv_obj_add_state(ota_button, LV_STATE_DISABLED);
    ota_status = lv_label_create(content);
    lv_label_set_text(ota_status, ota_busy ? "Downloading update..." : "Updates use published GitHub release builds");
    lv_obj_set_width(ota_status, 620);
    ota_timer = lv_timer_create(ota_tick, 250, NULL);
}

static void clock_clicked(lv_event_t *event)
{
    (void)event;
    show_clock();
}

static void show_clock(void)
{
    clear_content();
    clock_time = lv_label_create(content);
    lv_obj_set_style_text_font(clock_time, &lv_font_montserrat_48, 0);
    clock_date = lv_label_create(content);
    lv_obj_set_style_text_font(clock_date, &lv_font_montserrat_28, 0);
    clock_status = lv_label_create(content);
    clock_tick(NULL);
}

static void gpio_clicked(lv_event_t *event)
{
    (void)event;
    show_gpio();
}

static void show_gpio(void)
{
    clear_content();
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "GPIO");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_t *help = lv_label_create(content);
    lv_label_set_text(help, "Tap mode: INPUT (pull-up) -> LOW -> HIGH\n3.3V logic only. Do not connect GPIO directly to 5V.");
    lv_obj_set_width(help, 640);

    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_size(list, 650, 930);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 10, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    for (size_t i = 0; i < GPIO_CONTROL_COUNT; i++) {
        gpio_control_t *control = &gpio_controls[i];
        control->mode = 0;
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, 600, 66);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text_fmt(name, "%s  G%d", control->port, control->pin);
        lv_obj_set_width(name, 220);
        control->level_label = lv_label_create(row);
        lv_label_set_text(control->level_label, "1");
        lv_obj_t *mode = lv_button_create(row);
        lv_obj_set_size(mode, 180, 50);
        lv_obj_add_event_cb(mode, gpio_mode_clicked, LV_EVENT_CLICKED, control);
        control->mode_label = lv_label_create(mode);
        lv_label_set_text(control->mode_label, "SET");
        lv_obj_center(control->mode_label);
    }
    gpio_timer = lv_timer_create(gpio_tick, 200, NULL);
    gpio_tick(NULL);
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
    app_icon(content, LV_SYMBOL_EYE_OPEN, "Browser", 0x3F51B5, browser_clicked, 0, 2);
    app_icon(content, LV_SYMBOL_FILE, "Ebooks", 0x8D6E63, ebooks_clicked, 1, 2);
    app_icon(content, LV_SYMBOL_LOOP, "Clock", 0x009688, clock_clicked, 2, 2);
    app_icon(content, LV_SYMBOL_CHARGE, "GPIO", 0xE65100, gpio_clicked, 0, 3);
}

static void confirm_running_ota(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        ESP_LOGI("tab5-os", "OTA image validated");
    }
}

void app_main(void)
{
    ESP_LOGI("tab5-os", "Starting Tab5 OS");
    ESP_ERROR_CHECK(bsp_i2c_init());
    bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
    bsp_set_charge_qc_en(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    bsp_set_charge_en(true);
    battery_init(bsp_i2c_get_handle());
    clock_init(bsp_i2c_get_handle());
    vTaskDelay(pdMS_TO_TICKS(250));

    wifi_ready = start_wifi();
    load_chat_config();

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
    battery_label = lv_label_create(header);
    lv_obj_align(battery_label, LV_ALIGN_RIGHT_MID, -20, -18);
    time_label = lv_label_create(header);
    lv_obj_align(time_label, LV_ALIGN_RIGHT_MID, -20, 18);
    battery_tick(NULL);
    lv_timer_create(battery_tick, 5000, NULL);
    clock_tick(NULL);
    lv_timer_create(clock_tick, 1000, NULL);

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
    confirm_running_ota();
    mkdir(SD_PATH "/BOOKS", 0775);
    ebook_start_default_downloads();
}
