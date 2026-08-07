#include <dirent.h>
#include <assert.h>
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
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
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
#define SCOPE_RING_POINTS 1200
#define SCOPE_CHART_POINTS 300
#define ALARM_COUNT 3
#define WEATHER_HOURS 12
#define WEATHER_DAYS 7
#define SCREENSAVER_IDLE_MS (2 * 60 * 1000)
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
    bool input_only;
    uint8_t mode;
    lv_obj_t *mode_label;
    lv_obj_t *level_label;
} gpio_control_t;

typedef struct {
    gpio_num_t pin;
    adc_unit_t unit;
    adc_channel_t channel;
} scope_channel_t;

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t enabled;
} alarm_setting_t;

typedef struct {
    char time[17];
    float temperature;
    uint8_t precipitation;
    uint8_t code;
    float wind;
} weather_hour_t;

typedef struct {
    char date[11];
    char sunrise[17];
    char sunset[17];
    float high;
    float low;
    uint8_t precipitation;
    uint8_t code;
} weather_day_t;

typedef struct {
    char place[96];
    char updated[17];
    float temperature;
    float apparent;
    float precipitation;
    float pressure;
    float wind;
    float gust;
    uint16_t wind_direction;
    uint8_t humidity;
    uint8_t cloud;
    uint8_t code;
    uint8_t is_day;
    weather_hour_t hourly[WEATHER_HOURS];
    weather_day_t daily[WEATHER_DAYS];
    uint8_t hour_count;
    uint8_t day_count;
} weather_data_t;

static const ebook_default_t ebook_defaults[] = {
    {"ALICE.TXT", "https://www.gutenberg.org/cache/epub/11/pg11.txt"},
    {"FRANK.TXT", "https://www.gutenberg.org/cache/epub/84/pg84.txt"},
    {"HOLMES.TXT", "https://www.gutenberg.org/cache/epub/1661/pg1661.txt"},
};

#define GPIO_CONTROL(p, name) {.pin = (p), .port = (name)}
#define GPIO_INPUT(p, name) {.pin = (p), .port = (name), .input_only = true}
static gpio_control_t gpio_controls[] = {
    GPIO_CONTROL(GPIO_NUM_49, "EXT"), GPIO_CONTROL(GPIO_NUM_50, "EXT"), GPIO_CONTROL(GPIO_NUM_0, "EXT"),
    GPIO_CONTROL(GPIO_NUM_1, "EXT"), GPIO_CONTROL(GPIO_NUM_54, "EXT"), GPIO_CONTROL(GPIO_NUM_53, "EXT"),
    GPIO_CONTROL(GPIO_NUM_18, "M-BUS"), GPIO_CONTROL(GPIO_NUM_19, "M-BUS"), GPIO_CONTROL(GPIO_NUM_5, "M-BUS"),
    GPIO_CONTROL(GPIO_NUM_38, "M-BUS"), GPIO_CONTROL(GPIO_NUM_7, "M-BUS"), GPIO_CONTROL(GPIO_NUM_3, "M-BUS"),
    GPIO_CONTROL(GPIO_NUM_2, "M-BUS"), GPIO_CONTROL(GPIO_NUM_47, "M-BUS"), GPIO_CONTROL(GPIO_NUM_16, "M-BUS"),
    GPIO_INPUT(GPIO_NUM_17, "M-BUS PB_IN"), GPIO_CONTROL(GPIO_NUM_45, "M-BUS"), GPIO_INPUT(GPIO_NUM_52, "M-BUS PB_OUT"),
    GPIO_CONTROL(GPIO_NUM_37, "M-BUS"), GPIO_CONTROL(GPIO_NUM_6, "M-BUS"), GPIO_CONTROL(GPIO_NUM_4, "M-BUS"),
    GPIO_CONTROL(GPIO_NUM_48, "M-BUS"), GPIO_CONTROL(GPIO_NUM_35, "M-BUS"), GPIO_CONTROL(GPIO_NUM_51, "M-BUS"),
};
#undef GPIO_CONTROL
#undef GPIO_INPUT
#define GPIO_CONTROL_COUNT (sizeof(gpio_controls) / sizeof(gpio_controls[0]))
_Static_assert(GPIO_CONTROL_COUNT == 24, "Tab5 exposes 24 user GPIO pins");

static const scope_channel_t scope_channels[] = {
    {GPIO_NUM_16, ADC_UNIT_1, ADC_CHANNEL_0}, {GPIO_NUM_18, ADC_UNIT_1, ADC_CHANNEL_2},
    {GPIO_NUM_19, ADC_UNIT_1, ADC_CHANNEL_3}, {GPIO_NUM_49, ADC_UNIT_2, ADC_CHANNEL_0},
    {GPIO_NUM_50, ADC_UNIT_2, ADC_CHANNEL_1}, {GPIO_NUM_51, ADC_UNIT_2, ADC_CHANNEL_2},
    {GPIO_NUM_53, ADC_UNIT_2, ADC_CHANNEL_4}, {GPIO_NUM_54, ADC_UNIT_2, ADC_CHANNEL_5},
};
#define SCOPE_CHANNEL_COUNT (sizeof(scope_channels) / sizeof(scope_channels[0]))
_Static_assert(SCOPE_CHANNEL_COUNT == 8, "Tab5 exposes eight safe ADC inputs");
static const uint32_t scope_sample_rates[] = {1000, 5000, 20000, 80000};
static const uint16_t scope_ranges_mv[] = {3300, 2000, 1000, 500};

static lv_obj_t *content;
static lv_obj_t *header;
static lv_obj_t *battery_label;
static i2c_master_dev_handle_t battery_monitor;
static i2c_master_dev_handle_t rtc;
static lv_obj_t *time_label;
static lv_obj_t *clock_time;
static lv_obj_t *clock_date;
static lv_obj_t *clock_status;
static alarm_setting_t alarms[ALARM_COUNT];
static int alarm_last_day[ALARM_COUNT];
static lv_obj_t *alarm_time_labels[ALARM_COUNT];
static lv_obj_t *alarm_enabled_labels[ALARM_COUNT];
static lv_obj_t *alarm_modal;
static TaskHandle_t alarm_sound_task_handle;
static esp_codec_dev_handle_t alarm_speaker;
static volatile bool alarm_active;
static uint8_t alarm_active_index;
static time_t alarm_snooze_until;
static uint8_t alarm_snooze_index;
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
static lv_timer_t *scope_timer;
static lv_obj_t *scope_chart;
static lv_chart_series_t *scope_series;
static lv_obj_t *scope_stats;
static lv_obj_t *scope_channel_label;
static lv_obj_t *scope_run_label;
static lv_obj_t *scope_rate_label;
static lv_obj_t *scope_scale_label;
static lv_obj_t *scope_trigger_label;
static lv_obj_t *scope_level_label;
static int32_t *scope_chart_points;
static uint16_t *scope_ring;
static uint16_t *scope_snapshot;
static size_t scope_ring_head;
static size_t scope_ring_count;
static portMUX_TYPE scope_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t scope_task_handle;
static volatile bool scope_active;
static volatile bool scope_running = true;
static volatile bool scope_error;
static volatile uint8_t scope_channel_index;
static volatile uint8_t scope_rate_index = 1;
static uint8_t scope_range_index;
static uint8_t scope_trigger_mode;
static uint16_t scope_trigger_mv = 1650;
static TaskHandle_t ota_task_handle;
static volatile bool ota_busy;
static volatile bool ota_done;
static bool ota_ok;
static char ota_error[96];
static volatile int16_t remote_x;
static volatile int16_t remote_y;
static volatile bool remote_pressed;
static uint32_t remote_frame_number;
static lv_obj_t *weather_status;
static lv_obj_t *weather_location_area;
static lv_obj_t *weather_body;
static lv_obj_t *weather_keyboard;
static lv_obj_t *weather_keys_label;
static lv_timer_t *weather_timer;
static TaskHandle_t weather_task_handle;
static volatile bool weather_busy;
static volatile bool weather_done;
static bool weather_ok;
static bool weather_keyboard_visible;
static bool weather_has_data;
static char weather_location[64] = "Milwaukee, Wisconsin";
static char weather_pending_location[64];
static char weather_error[96];
static weather_data_t weather_data;
static time_t weather_fetched_at;
static lv_obj_t *screensaver;
static lv_obj_t *screensaver_panel;
static lv_obj_t *screensaver_time;
static lv_obj_t *screensaver_date;
static lv_obj_t *screensaver_weather;

static void show_launcher(void);
static void show_files(const char *path);
static void show_settings(void);
static void show_chat(void);
static void show_browser(void);
static void show_ebooks(void);
static void show_clock(void);
static void show_gpio(void);
static void show_scope(void);
static void show_weather(void);
static void weather_start(const char *location);
static void clear_content(void);
static void browser_link_clicked(lv_event_t *event);
static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback);

static void reset_content_scroll(void *object)
{
    lv_obj_scroll_to(object, 0, 0, LV_ANIM_OFF);
}

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
    control->mode = control->input_only ? 1 : control->mode == 3 ? 1 : control->mode + 1;
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

static bool scope_window_start(const uint16_t *samples, size_t count, uint8_t mode,
                               uint16_t level, size_t *start)
{
    if (count < SCOPE_CHART_POINTS) return false;
    *start = count - SCOPE_CHART_POINTS;
    bool found = false;
    for (size_t i = SCOPE_CHART_POINTS / 4; i + SCOPE_CHART_POINTS * 3 / 4 < count; i++) {
        bool crossing = mode == 2 ? samples[i - 1] > level && samples[i] <= level
                                  : samples[i - 1] < level && samples[i] >= level;
        if (crossing) {
            *start = i - SCOPE_CHART_POINTS / 4;
            found = true;
        }
    }
    return mode == 0 || found;
}

static void scope_self_test(void)
{
    uint16_t samples[600] = {0};
    for (size_t i = 200; i < 600; i++) samples[i] = 2000;
    size_t start = 0;
    assert(scope_window_start(samples, 600, 1, 1000, &start) && start == 125);
}

static adc_cali_handle_t scope_calibration(adc_unit_t unit, adc_channel_t channel)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_handle_t calibration = NULL;
    adc_cali_curve_fitting_config_t config = {
        .unit_id = unit, .chan = channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&config, &calibration) == ESP_OK) return calibration;
#endif
    return NULL;
}

static void scope_task(void *argument)
{
    (void)argument;
    uint8_t bytes[512];
    uint16_t millivolts[512 / SOC_ADC_DIGI_RESULT_BYTES];
    for (;;) {
        if (!scope_active || !scope_running) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        uint8_t channel_index = scope_channel_index;
        uint8_t rate_index = scope_rate_index;
        const scope_channel_t *input = &scope_channels[channel_index];
        adc_continuous_handle_t adc = NULL;
        adc_continuous_handle_cfg_t handle_config = {.max_store_buf_size = 2048, .conv_frame_size = sizeof(bytes)};
        adc_digi_pattern_config_t pattern = {
            .atten = ADC_ATTEN_DB_12, .channel = input->channel, .unit = input->unit, .bit_width = ADC_BITWIDTH_12,
        };
        adc_continuous_config_t config = {
            .pattern_num = 1,
            .adc_pattern = &pattern,
            .sample_freq_hz = scope_sample_rates[rate_index],
            .conv_mode = input->unit == ADC_UNIT_1 ? ADC_CONV_SINGLE_UNIT_1 : ADC_CONV_SINGLE_UNIT_2,
            .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
        };
        esp_err_t error = adc_continuous_new_handle(&handle_config, &adc);
        if (error == ESP_OK) error = adc_continuous_config(adc, &config);
        if (error == ESP_OK) error = adc_continuous_start(adc);
        if (error != ESP_OK) {
            scope_error = true;
            if (adc) adc_continuous_deinit(adc);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        adc_cali_handle_t calibration = scope_calibration(input->unit, input->channel);
        scope_error = false;
        portENTER_CRITICAL(&scope_lock);
        scope_ring_head = scope_ring_count = 0;
        portEXIT_CRITICAL(&scope_lock);

        while (scope_active && scope_running && channel_index == scope_channel_index && rate_index == scope_rate_index) {
            uint32_t bytes_read = 0;
            error = adc_continuous_read(adc, bytes, sizeof(bytes), &bytes_read, 100);
            if (error == ESP_ERR_TIMEOUT) continue;
            if (error != ESP_OK) {
                scope_error = true;
                break;
            }
            size_t count = 0;
            for (size_t i = 0; i < bytes_read; i += SOC_ADC_DIGI_RESULT_BYTES) {
                adc_digi_output_data_t *sample = (adc_digi_output_data_t *)&bytes[i];
                if (sample->type2.unit != input->unit || sample->type2.channel != input->channel) continue;
                int mv = sample->type2.data * 3300 / 4095;
                if (calibration) adc_cali_raw_to_voltage(calibration, sample->type2.data, &mv);
                millivolts[count++] = mv < 0 ? 0 : mv > 3300 ? 3300 : mv;
            }
            portENTER_CRITICAL(&scope_lock);
            for (size_t i = 0; i < count; i++) {
                scope_ring[scope_ring_head] = millivolts[i];
                scope_ring_head = (scope_ring_head + 1) % SCOPE_RING_POINTS;
                if (scope_ring_count < SCOPE_RING_POINTS) scope_ring_count++;
            }
            portEXIT_CRITICAL(&scope_lock);
        }
        adc_continuous_stop(adc);
        adc_continuous_deinit(adc);
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        if (calibration) adc_cali_delete_scheme_curve_fitting(calibration);
#endif
    }
}

static void scope_update_controls(void)
{
    const scope_channel_t *input = &scope_channels[scope_channel_index];
    lv_label_set_text_fmt(scope_channel_label, "G%d", input->pin);
    lv_label_set_text(scope_run_label, scope_running ? "HOLD" : "RUN");
    uint32_t us_per_div = 30000000 / scope_sample_rates[scope_rate_index];
    if (us_per_div >= 1000 && us_per_div % 1000)
        lv_label_set_text_fmt(scope_rate_label, "%lu.%lu ms/div", (unsigned long)(us_per_div / 1000),
                              (unsigned long)((us_per_div % 1000) / 100));
    else if (us_per_div >= 1000)
        lv_label_set_text_fmt(scope_rate_label, "%lu ms/div", (unsigned long)(us_per_div / 1000));
    else
        lv_label_set_text_fmt(scope_rate_label, "%lu us/div", (unsigned long)us_per_div);
    lv_label_set_text_fmt(scope_scale_label, "%u mV/div", scope_ranges_mv[scope_range_index] / 10);
    lv_label_set_text(scope_trigger_label, scope_trigger_mode == 0 ? "AUTO" : scope_trigger_mode == 1 ? "RISE" : "FALL");
    lv_label_set_text_fmt(scope_level_label, "Trigger %u mV", scope_trigger_mv);
}

static void scope_tick(lv_timer_t *timer)
{
    (void)timer;
    lv_obj_invalidate(header);
    size_t count;
    portENTER_CRITICAL(&scope_lock);
    count = scope_ring_count;
    size_t oldest = (scope_ring_head + SCOPE_RING_POINTS - count) % SCOPE_RING_POINTS;
    for (size_t i = 0; i < count; i++) scope_snapshot[i] = scope_ring[(oldest + i) % SCOPE_RING_POINTS];
    portEXIT_CRITICAL(&scope_lock);

    size_t start;
    if (!scope_window_start(scope_snapshot, count, scope_trigger_mode, scope_trigger_mv, &start)) {
        lv_label_set_text(scope_stats, scope_error ? "ADC error" : "Waiting for trigger...");
        return;
    }
    uint32_t sum = 0;
    uint16_t minimum = UINT16_MAX, maximum = 0;
    for (size_t i = 0; i < SCOPE_CHART_POINTS; i++) {
        uint16_t mv = scope_snapshot[start + i];
        scope_chart_points[i] = mv;
        sum += mv;
        if (mv < minimum) minimum = mv;
        if (mv > maximum) maximum = mv;
    }
    size_t first_crossing = 0, last_crossing = 0, crossings = 0;
    for (size_t i = 1; i < count; i++) {
        bool crossing = scope_trigger_mode == 2 ? scope_snapshot[i - 1] > scope_trigger_mv && scope_snapshot[i] <= scope_trigger_mv
                                                : scope_snapshot[i - 1] < scope_trigger_mv && scope_snapshot[i] >= scope_trigger_mv;
        if (crossing) {
            if (!crossings) first_crossing = i;
            last_crossing = i;
            crossings++;
        }
    }
    uint32_t hz = crossings > 1 ? (crossings - 1) * scope_sample_rates[scope_rate_index] /
                                  (last_crossing - first_crossing) : 0;
    uint16_t now = scope_chart_points[SCOPE_CHART_POINTS - 1];
    uint16_t average = sum / SCOPE_CHART_POINTS;
    uint16_t peak_to_peak = maximum - minimum;
    lv_label_set_text_fmt(scope_stats,
                          "Now %u.%03u V   Min %u.%03u   Max %u.%03u   Vpp %u.%03u\n"
                          "Avg %u.%03u V   Freq %lu Hz   %lu kS/s",
                          now / 1000, now % 1000, minimum / 1000, minimum % 1000,
                          maximum / 1000, maximum % 1000, peak_to_peak / 1000, peak_to_peak % 1000,
                          average / 1000, average % 1000, (unsigned long)hz,
                          (unsigned long)(scope_sample_rates[scope_rate_index] / 1000));
    lv_chart_refresh(scope_chart);
}

static void scope_channel_clicked(lv_event_t *event)
{
    (void)event;
    scope_channel_index = (scope_channel_index + 1) % SCOPE_CHANNEL_COUNT;
    scope_update_controls();
}

static void scope_run_clicked(lv_event_t *event)
{
    (void)event;
    scope_running = !scope_running;
    scope_update_controls();
}

static void scope_rate_clicked(lv_event_t *event)
{
    (void)event;
    scope_rate_index = (scope_rate_index + 1) % (sizeof(scope_sample_rates) / sizeof(scope_sample_rates[0]));
    scope_update_controls();
}

static void scope_scale_clicked(lv_event_t *event)
{
    (void)event;
    scope_range_index = (scope_range_index + 1) % (sizeof(scope_ranges_mv) / sizeof(scope_ranges_mv[0]));
    if (scope_trigger_mv > scope_ranges_mv[scope_range_index]) scope_trigger_mv = scope_ranges_mv[scope_range_index] / 2;
    lv_chart_set_range(scope_chart, LV_CHART_AXIS_PRIMARY_Y, 0, scope_ranges_mv[scope_range_index]);
    scope_update_controls();
}

static void scope_trigger_clicked(lv_event_t *event)
{
    (void)event;
    scope_trigger_mode = (scope_trigger_mode + 1) % 3;
    scope_update_controls();
}

static void scope_level_clicked(lv_event_t *event)
{
    int level = scope_trigger_mv + (int)(intptr_t)lv_event_get_user_data(event);
    scope_trigger_mv = level < 0 ? 0 : level > scope_ranges_mv[scope_range_index] ? scope_ranges_mv[scope_range_index] : level;
    scope_update_controls();
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

static uint8_t alarm_wrap(int value, int limit)
{
    value %= limit;
    return value < 0 ? value + limit : value;
}

static void alarm_self_test(void)
{
    assert(alarm_wrap(24, 24) == 0);
    assert(alarm_wrap(-1, 24) == 23);
    assert(alarm_wrap(60, 60) == 0);
}

static void load_alarms(void)
{
    for (size_t i = 0; i < ALARM_COUNT; i++) {
        alarms[i] = (alarm_setting_t){.hour = 7 + i, .minute = 0};
        alarm_last_day[i] = -1;
    }
    nvs_handle_t handle;
    if (nvs_open("tab5", NVS_READONLY, &handle) != ESP_OK) return;
    alarm_setting_t saved[ALARM_COUNT];
    size_t size = sizeof(saved);
    if (nvs_get_blob(handle, "alarms", saved, &size) == ESP_OK && size == sizeof(saved)) {
        bool valid = true;
        for (size_t i = 0; i < ALARM_COUNT; i++)
            valid &= saved[i].hour < 24 && saved[i].minute < 60 && saved[i].enabled <= 1;
        if (valid) memcpy(alarms, saved, sizeof(alarms));
    }
    nvs_close(handle);
}

static void save_alarms(void)
{
    nvs_handle_t handle;
    if (nvs_open("tab5", NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_blob(handle, "alarms", alarms, sizeof(alarms)) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

static void alarm_close(void)
{
    alarm_active = false;
    if (alarm_modal) {
        lv_obj_delete_async(alarm_modal);
        alarm_modal = NULL;
    }
}

static void alarm_dismiss_clicked(lv_event_t *event)
{
    (void)event;
    alarm_close();
}

static void alarm_snooze_clicked(lv_event_t *event)
{
    (void)event;
    alarm_snooze_until = time(NULL) + 9 * 60;
    alarm_snooze_index = alarm_active_index;
    alarm_close();
}

static void alarm_sound_task(void *argument)
{
    (void)argument;
    if (!voice_recording) {
        if (!alarm_speaker) alarm_speaker = bsp_audio_codec_speaker_init();
        esp_codec_dev_sample_info_t format = {.sample_rate = 16000, .channel = 1, .bits_per_sample = 16};
        if (alarm_speaker && esp_codec_dev_open(alarm_speaker, &format) == ESP_CODEC_DEV_OK) {
            esp_codec_dev_set_out_vol(alarm_speaker, 75);
            int16_t tone[160];
            for (size_t i = 0; i < sizeof(tone) / sizeof(tone[0]); i++) tone[i] = (i / 9) & 1 ? 9000 : -9000;
            while (alarm_active) {
                for (int i = 0; i < 40 && alarm_active; i++) esp_codec_dev_write(alarm_speaker, tone, sizeof(tone));
                vTaskDelay(pdMS_TO_TICKS(600));
            }
            esp_codec_dev_close(alarm_speaker);
        }
    }
    alarm_sound_task_handle = NULL;
    vTaskDelete(NULL);
}

static void alarm_trigger(uint8_t index)
{
    if (alarm_active) return;
    alarm_active = true;
    alarm_active_index = index;

    alarm_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(alarm_modal, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(alarm_modal, lv_color_hex(0x10141f), 0);
    lv_obj_set_style_bg_opa(alarm_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(alarm_modal, lv_color_white(), 0);
    lv_obj_set_flex_flow(alarm_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(alarm_modal, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(alarm_modal, 40, 0);

    lv_obj_t *title = lv_label_create(alarm_modal);
    lv_label_set_text(title, LV_SYMBOL_BELL "  ALARM");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_t *time_label = lv_label_create(alarm_modal);
    uint8_t hour = alarms[index].hour % 12;
    lv_label_set_text_fmt(time_label, "%u:%02u %s", hour ? hour : 12, alarms[index].minute,
                          alarms[index].hour < 12 ? "AM" : "PM");
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_48, 0);
    lv_obj_t *message = lv_label_create(alarm_modal);
    lv_label_set_text(message, "Daily alarm");
    lv_obj_t *row = lv_obj_create(alarm_modal);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 600, 120);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *snooze = button(row, "Snooze 9m", alarm_snooze_clicked);
    lv_obj_set_size(snooze, 260, 100);
    lv_obj_t *dismiss = button(row, "Dismiss", alarm_dismiss_clicked);
    lv_obj_set_size(dismiss, 260, 100);

    if (!alarm_sound_task_handle) xTaskCreate(alarm_sound_task, "alarm-sound", 3072, NULL, 5, &alarm_sound_task_handle);
}

static void alarm_check(time_t now, const struct tm *local)
{
    if (!alarm_active && alarm_snooze_until && now >= alarm_snooze_until) {
        alarm_snooze_until = 0;
        alarm_trigger(alarm_snooze_index);
        return;
    }
    int day = (local->tm_year + 1900) * 400 + local->tm_yday;
    for (size_t i = 0; i < ALARM_COUNT; i++) {
        if (alarms[i].enabled && alarms[i].hour == local->tm_hour && alarms[i].minute == local->tm_min &&
            alarm_last_day[i] != day) {
            alarm_last_day[i] = day;
            alarm_trigger(i);
            break;
        }
    }
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
    if (valid) alarm_check(now, &local);
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

static bool weather_url_encode(const char *input, char *output, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p; p++) {
        bool plain = isalnum(*p) || strchr("-_.~", *p);
        size_t needed = plain ? 1 : 3;
        if (used + needed >= capacity) return false;
        if (plain) output[used++] = *p;
        else {
            output[used++] = '%';
            output[used++] = hex[*p >> 4];
            output[used++] = hex[*p & 15];
        }
    }
    output[used] = '\0';
    return true;
}

static void weather_ascii(char *output, size_t capacity, const char *input)
{
    // ponytail: ASCII avoids missing LVGL glyph boxes; enable a Unicode font for native place spelling.
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p && used + 1 < capacity; p++)
        if (*p >= 32 && *p < 127) output[used++] = *p;
    output[used] = '\0';
}

static void weather_self_test(void)
{
    char encoded[32];
    assert(weather_url_encode("St. Paul, MN", encoded, sizeof(encoded)));
    assert(strcmp(encoded, "St.%20Paul%2C%20MN") == 0);
}

static bool weather_http_get(const char *url, char *response, size_t capacity)
{
    http_buffer_t buffer = {.data = response, .capacity = capacity};
    response[0] = '\0';
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = chat_http_event,
        .user_data = &buffer,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "Tab5OS/1.0");
    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (error != ESP_OK || status != 200) {
        snprintf(weather_error, sizeof(weather_error), "Weather failed: %s (%d)", esp_err_to_name(error), status);
        ESP_LOGE("weather", "%s", weather_error);
        return false;
    }
    return true;
}

static bool weather_number(cJSON *object, const char *key, float *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) return false;
    *value = (float)item->valuedouble;
    return true;
}

static bool weather_array_number(cJSON *array, int index, float *value)
{
    cJSON *item = cJSON_GetArrayItem(array, index);
    if (!cJSON_IsNumber(item)) return false;
    *value = (float)item->valuedouble;
    return true;
}

static bool weather_array_text(cJSON *array, int index, char *output, size_t capacity)
{
    cJSON *item = cJSON_GetArrayItem(array, index);
    if (!cJSON_IsString(item)) return false;
    snprintf(output, capacity, "%s", item->valuestring);
    return true;
}

static bool weather_parse_forecast(const char *json, weather_data_t *data)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *current = root ? cJSON_GetObjectItemCaseSensitive(root, "current") : NULL;
    cJSON *hourly = root ? cJSON_GetObjectItemCaseSensitive(root, "hourly") : NULL;
    cJSON *daily = root ? cJSON_GetObjectItemCaseSensitive(root, "daily") : NULL;
    float value;
    cJSON *updated = current ? cJSON_GetObjectItemCaseSensitive(current, "time") : NULL;
    bool valid = cJSON_IsObject(current) && cJSON_IsObject(hourly) && cJSON_IsObject(daily) &&
        cJSON_IsString(updated) &&
        weather_number(current, "temperature_2m", &data->temperature) &&
        weather_number(current, "apparent_temperature", &data->apparent) &&
        weather_number(current, "relative_humidity_2m", &value);
    if (!valid) {
        cJSON_Delete(root);
        return false;
    }
    data->humidity = value;
    valid = weather_number(current, "precipitation", &data->precipitation) &&
        weather_number(current, "surface_pressure", &data->pressure) &&
        weather_number(current, "wind_speed_10m", &data->wind) &&
        weather_number(current, "wind_gusts_10m", &data->gust) &&
        weather_number(current, "wind_direction_10m", &value);
    data->wind_direction = value;
    valid = valid && weather_number(current, "cloud_cover", &value);
    data->cloud = value;
    valid = valid && weather_number(current, "weather_code", &value);
    data->code = value;
    valid = valid && weather_number(current, "is_day", &value);
    data->is_day = value;
    snprintf(data->updated, sizeof(data->updated), "%s", updated->valuestring);

    cJSON *times = cJSON_GetObjectItemCaseSensitive(hourly, "time");
    cJSON *temperatures = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
    cJSON *rain = cJSON_GetObjectItemCaseSensitive(hourly, "precipitation_probability");
    cJSON *codes = cJSON_GetObjectItemCaseSensitive(hourly, "weather_code");
    cJSON *winds = cJSON_GetObjectItemCaseSensitive(hourly, "wind_speed_10m");
    int start = 0, count = cJSON_GetArraySize(times);
    for (int i = 0; i < count; i++) {
        cJSON *time_item = cJSON_GetArrayItem(times, i);
        if (cJSON_IsString(time_item) && strncmp(time_item->valuestring, data->updated, 13) == 0) {
            start = i;
            break;
        }
    }
    data->hour_count = 0;
    for (int i = start; valid && i < count && data->hour_count < WEATHER_HOURS; i++) {
        weather_hour_t *hour = &data->hourly[data->hour_count];
        valid = weather_array_text(times, i, hour->time, sizeof(hour->time)) &&
            weather_array_number(temperatures, i, &hour->temperature) &&
            weather_array_number(rain, i, &value);
        hour->precipitation = value;
        valid = valid && weather_array_number(codes, i, &value);
        hour->code = value;
        valid = valid && weather_array_number(winds, i, &hour->wind);
        if (valid) data->hour_count++;
    }

    cJSON *dates = cJSON_GetObjectItemCaseSensitive(daily, "time");
    cJSON *highs = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    cJSON *lows = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
    cJSON *daily_rain = cJSON_GetObjectItemCaseSensitive(daily, "precipitation_probability_max");
    cJSON *daily_codes = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
    cJSON *sunrises = cJSON_GetObjectItemCaseSensitive(daily, "sunrise");
    cJSON *sunsets = cJSON_GetObjectItemCaseSensitive(daily, "sunset");
    count = cJSON_GetArraySize(dates);
    data->day_count = 0;
    for (int i = 0; valid && i < count && data->day_count < WEATHER_DAYS; i++) {
        weather_day_t *day = &data->daily[data->day_count];
        valid = weather_array_text(dates, i, day->date, sizeof(day->date)) &&
            weather_array_text(sunrises, i, day->sunrise, sizeof(day->sunrise)) &&
            weather_array_text(sunsets, i, day->sunset, sizeof(day->sunset)) &&
            weather_array_number(highs, i, &day->high) && weather_array_number(lows, i, &day->low) &&
            weather_array_number(daily_rain, i, &value);
        day->precipitation = value;
        valid = valid && weather_array_number(daily_codes, i, &value);
        day->code = value;
        if (valid) data->day_count++;
    }
    cJSON_Delete(root);
    return valid && data->hour_count && data->day_count;
}

static void load_weather_location(void)
{
    nvs_handle_t handle;
    if (nvs_open("tab5", NVS_READWRITE, &handle) != ESP_OK) return;
    size_t size = sizeof(weather_location);
    nvs_get_str(handle, "weather_loc", weather_location, &size);
    uint8_t migrated = 0;
    if (nvs_get_u8(handle, "weather_mke", &migrated) != ESP_OK) {
        if (strcmp(weather_location, "Chicago") == 0)
            snprintf(weather_location, sizeof(weather_location), "Milwaukee, Wisconsin");
        nvs_set_str(handle, "weather_loc", weather_location);
        nvs_set_u8(handle, "weather_mke", 1);
        nvs_commit(handle);
    }
    nvs_close(handle);
}

static void save_weather_location(void)
{
    nvs_handle_t handle;
    if (nvs_open("tab5", NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_str(handle, "weather_loc", weather_location) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

static void weather_task(void *argument)
{
    (void)argument;
    char *response = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char url[1024], encoded[192];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        weather_data_t next = {0};
        weather_ok = false;
        if (!response || !weather_url_encode(weather_pending_location, encoded, sizeof(encoded))) {
            snprintf(weather_error, sizeof(weather_error), "Location is too long");
            goto done;
        }
        snprintf(url, sizeof(url),
            "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json", encoded);
        if (!weather_http_get(url, response, 32768)) goto done;
        cJSON *root = cJSON_Parse(response);
        cJSON *results = root ? cJSON_GetObjectItemCaseSensitive(root, "results") : NULL;
        cJSON *result = cJSON_IsArray(results) ? cJSON_GetArrayItem(results, 0) : NULL;
        cJSON *name = result ? cJSON_GetObjectItemCaseSensitive(result, "name") : NULL;
        cJSON *admin = result ? cJSON_GetObjectItemCaseSensitive(result, "admin1") : NULL;
        cJSON *country = result ? cJSON_GetObjectItemCaseSensitive(result, "country") : NULL;
        cJSON *latitude = result ? cJSON_GetObjectItemCaseSensitive(result, "latitude") : NULL;
        cJSON *longitude = result ? cJSON_GetObjectItemCaseSensitive(result, "longitude") : NULL;
        if (!cJSON_IsString(name) || !cJSON_IsNumber(latitude) || !cJSON_IsNumber(longitude)) {
            cJSON_Delete(root);
            snprintf(weather_error, sizeof(weather_error), "Location not found");
            goto done;
        }
        char raw_place[160];
        snprintf(raw_place, sizeof(raw_place), "%s%s%s%s%s", name->valuestring,
            cJSON_IsString(admin) ? ", " : "", cJSON_IsString(admin) ? admin->valuestring : "",
            cJSON_IsString(country) ? ", " : "", cJSON_IsString(country) ? country->valuestring : "");
        weather_ascii(next.place, sizeof(next.place), raw_place);
        double lat = latitude->valuedouble, lon = longitude->valuedouble;
        cJSON_Delete(root);

        snprintf(url, sizeof(url), "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f"
            "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,weather_code,cloud_cover,surface_pressure,wind_speed_10m,wind_direction_10m,wind_gusts_10m"
            "&hourly=temperature_2m,precipitation_probability,weather_code,wind_speed_10m"
            "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
            "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch&timezone=auto&forecast_days=7&forecast_hours=12",
            lat, lon);
        if (!weather_http_get(url, response, 32768)) goto done;
        if (!weather_parse_forecast(response, &next)) {
            snprintf(weather_error, sizeof(weather_error), "Invalid weather response");
            goto done;
        }
        weather_data = next;
        weather_has_data = true;
        weather_fetched_at = time(NULL);
        weather_ok = true;
        snprintf(weather_location, sizeof(weather_location), "%s", weather_pending_location);
        save_weather_location();
done:
        weather_busy = false;
        weather_done = true;
    }
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
    if (scope_timer) {
        lv_timer_delete(scope_timer);
        scope_timer = NULL;
    }
    if (weather_timer) {
        lv_timer_delete(weather_timer);
        weather_timer = NULL;
    }
    scope_active = false;
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
    for (size_t i = 0; i < ALARM_COUNT; i++) {
        alarm_time_labels[i] = NULL;
        alarm_enabled_labels[i] = NULL;
    }
    scope_chart = NULL;
    scope_series = NULL;
    scope_stats = NULL;
    scope_channel_label = NULL;
    scope_run_label = NULL;
    scope_rate_label = NULL;
    scope_scale_label = NULL;
    scope_trigger_label = NULL;
    scope_level_label = NULL;
    weather_status = NULL;
    weather_location_area = NULL;
    weather_body = NULL;
    weather_keyboard = NULL;
    weather_keys_label = NULL;
    lv_obj_clean(content);
    lv_obj_scroll_to(content, 0, 0, LV_ANIM_OFF);
    lv_async_call(reset_content_scroll, content);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
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

static void alarm_update_row(size_t index)
{
    if (!alarm_time_labels[index]) return;
    uint8_t hour = alarms[index].hour % 12;
    lv_label_set_text_fmt(alarm_time_labels[index], "%u:%02u %s", hour ? hour : 12, alarms[index].minute,
                          alarms[index].hour < 12 ? "AM" : "PM");
    lv_label_set_text(alarm_enabled_labels[index], alarms[index].enabled ? "ON" : "OFF");
}

static void alarm_edit_clicked(lv_event_t *event)
{
    unsigned action = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    size_t index = action >> 4;
    switch (action & 0x0f) {
        case 0: alarms[index].hour = alarm_wrap(alarms[index].hour - 1, 24); break;
        case 1: alarms[index].hour = alarm_wrap(alarms[index].hour + 1, 24); break;
        case 2: alarms[index].minute = alarm_wrap(alarms[index].minute - 1, 60); break;
        case 3: alarms[index].minute = alarm_wrap(alarms[index].minute + 1, 60); break;
        default: alarms[index].enabled = !alarms[index].enabled; break;
    }
    alarm_last_day[index] = -1;
    alarm_update_row(index);
    save_alarms();
}

static lv_obj_t *alarm_control(lv_obj_t *parent, const char *text, int width, unsigned action, lv_obj_t **label_out)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_size(control, width, 64);
    lv_obj_add_event_cb(control, alarm_edit_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)action);
    lv_obj_t *label = lv_label_create(control);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (label_out) *label_out = label;
    return control;
}

static void show_clock(void)
{
    clear_content();
    clock_time = lv_label_create(content);
    lv_obj_set_style_text_font(clock_time, &lv_font_montserrat_48, 0);
    clock_date = lv_label_create(content);
    lv_obj_set_style_text_font(clock_date, &lv_font_montserrat_28, 0);
    clock_status = lv_label_create(content);
    lv_obj_t *heading = lv_label_create(content);
    lv_label_set_text(heading, "Daily alarms");
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_28, 0);
    for (size_t i = 0; i < ALARM_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(content);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 650, 76);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        alarm_time_labels[i] = lv_label_create(row);
        lv_obj_set_width(alarm_time_labels[i], 120);
        alarm_control(row, "H-", 68, i << 4, NULL);
        alarm_control(row, "H+", 68, (i << 4) | 1, NULL);
        alarm_control(row, "M-", 68, (i << 4) | 2, NULL);
        alarm_control(row, "M+", 68, (i << 4) | 3, NULL);
        alarm_control(row, "OFF", 100, (i << 4) | 4, &alarm_enabled_labels[i]);
        alarm_update_row(i);
    }
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
        lv_label_set_text(control->mode_label, control->input_only ? "READ" : "SET");
        lv_obj_center(control->mode_label);
    }
    gpio_timer = lv_timer_create(gpio_tick, 200, NULL);
    gpio_tick(NULL);
}

static lv_obj_t *scope_control(lv_obj_t *parent, const char *text, int width,
                               lv_event_cb_t callback, void *user_data, lv_obj_t **label_out)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_size(control, width, 60);
    lv_obj_add_event_cb(control, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = lv_label_create(control);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (label_out) *label_out = label;
    return control;
}

static lv_obj_t *scope_row(void)
{
    lv_obj_t *row = lv_obj_create(content);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 650, 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static void scope_clicked(lv_event_t *event)
{
    (void)event;
    show_scope();
}

static void show_scope(void)
{
    clear_content();
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    if (!scope_ring) {
        scope_ring = heap_caps_calloc(SCOPE_RING_POINTS, sizeof(*scope_ring), MALLOC_CAP_SPIRAM);
        scope_snapshot = heap_caps_calloc(SCOPE_RING_POINTS, sizeof(*scope_snapshot), MALLOC_CAP_SPIRAM);
        scope_chart_points = heap_caps_calloc(SCOPE_CHART_POINTS, sizeof(*scope_chart_points), MALLOC_CAP_SPIRAM);
        if (!scope_ring || !scope_snapshot || !scope_chart_points) {
            heap_caps_free(scope_ring);
            heap_caps_free(scope_snapshot);
            heap_caps_free(scope_chart_points);
            scope_ring = scope_snapshot = NULL;
            scope_chart_points = NULL;
            lv_obj_t *error = lv_label_create(content);
            lv_label_set_text(error, "Not enough memory for oscilloscope buffers.");
            return;
        }
    }
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "ADC Oscilloscope");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    scope_stats = lv_label_create(content);
    lv_obj_set_width(scope_stats, 650);
    lv_obj_set_style_text_align(scope_stats, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(scope_stats, "Starting ADC...");

    scope_chart = lv_chart_create(content);
    lv_obj_set_size(scope_chart, 650, 500);
    lv_chart_set_type(scope_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(scope_chart, SCOPE_CHART_POINTS);
    lv_chart_set_range(scope_chart, LV_CHART_AXIS_PRIMARY_Y, 0, scope_ranges_mv[scope_range_index]);
    lv_chart_set_div_line_count(scope_chart, 9, 11);
    scope_series = lv_chart_add_series(scope_chart, lv_color_hex(0x00E676), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_series_ext_y_array(scope_chart, scope_series, scope_chart_points);

    lv_obj_t *row = scope_row();
    scope_control(row, "G16", 130, scope_channel_clicked, NULL, &scope_channel_label);
    scope_control(row, "HOLD", 130, scope_run_clicked, NULL, &scope_run_label);
    scope_control(row, "6 ms/div", 250, scope_rate_clicked, NULL, &scope_rate_label);

    row = scope_row();
    scope_control(row, "330 mV/div", 250, scope_scale_clicked, NULL, &scope_scale_label);
    scope_control(row, "AUTO", 250, scope_trigger_clicked, NULL, &scope_trigger_label);

    row = scope_row();
    scope_control(row, LV_SYMBOL_MINUS, 100, scope_level_clicked, (void *)(intptr_t)-100, NULL);
    scope_level_label = lv_label_create(row);
    lv_obj_set_width(scope_level_label, 260);
    lv_obj_set_style_text_align(scope_level_label, LV_TEXT_ALIGN_CENTER, 0);
    scope_control(row, LV_SYMBOL_PLUS, 100, scope_level_clicked, (void *)(intptr_t)100, NULL);

    lv_obj_t *warning = lv_label_create(content);
    lv_label_set_text(warning, "Inputs: G16 G18 G19 G49 G50 G51 G53 G54   |   0-3.3V only");
    lv_obj_set_width(warning, 650);
    lv_obj_set_style_text_align(warning, LV_TEXT_ALIGN_CENTER, 0);

    scope_running = true;
    scope_error = false;
    scope_update_controls();
    scope_active = true;
    if (!scope_task_handle) xTaskCreate(scope_task, "adc-scope", 4096, NULL, 5, &scope_task_handle);
    scope_timer = lv_timer_create(scope_tick, 150, NULL);
}

static const char *weather_condition(uint8_t code)
{
    if (code == 0) return "Clear";
    if (code <= 2) return "Partly cloudy";
    if (code == 3) return "Overcast";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if (code >= 61 && code <= 67) return "Rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code >= 80 && code <= 82) return "Showers";
    if (code == 85 || code == 86) return "Snow showers";
    if (code >= 95) return "Thunderstorm";
    return "Mixed weather";
}

static const char *weather_wind_direction(uint16_t degrees)
{
    static const char *directions[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return directions[((degrees + 22) / 45) % 8];
}

static int weather_round(float value)
{
    return (int)(value + (value < 0 ? -0.5f : 0.5f));
}

static void weather_short_time(const char *iso, char output[12])
{
    int hour = 0, minute = 0;
    if (strlen(iso) >= 16) sscanf(iso + 11, "%d:%d", &hour, &minute);
    snprintf(output, 12, "%d:%02d %s", hour % 12 ? hour % 12 : 12, minute, hour < 12 ? "AM" : "PM");
}

static void weather_render(void)
{
    if (!weather_body || !weather_has_data) return;
    lv_obj_clean(weather_body);
    lv_obj_t *place = lv_label_create(weather_body);
    lv_label_set_text(place, weather_data.place);
    lv_obj_set_width(place, 600);
    lv_obj_set_style_text_font(place, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(place, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *current = lv_obj_create(weather_body);
    lv_obj_set_size(current, 610, 245);
    lv_obj_clear_flag(current, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(current, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(current, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(current, 12, 0);
    lv_obj_set_style_pad_row(current, 5, 0);
    lv_obj_t *temperature = lv_label_create(current);
    lv_label_set_text_fmt(temperature, "%d F", weather_round(weather_data.temperature));
    lv_obj_set_style_text_font(temperature, &lv_font_montserrat_48, 0);
    lv_obj_t *condition = lv_label_create(current);
    lv_label_set_text(condition, weather_condition(weather_data.code));
    lv_obj_set_style_text_font(condition, &lv_font_montserrat_28, 0);
    lv_obj_t *details = lv_label_create(current);
    lv_label_set_text_fmt(details,
        "Feels %d F  |  Humidity %u%%  |  Clouds %u%%\n"
        "Wind %s %d mph, gusts %d  |  Pressure %d hPa\n"
        "Precipitation %d.%02d in",
        weather_round(weather_data.apparent), weather_data.humidity, weather_data.cloud,
        weather_wind_direction(weather_data.wind_direction), weather_round(weather_data.wind),
        weather_round(weather_data.gust), weather_round(weather_data.pressure),
        (int)(weather_data.precipitation * 100) / 100, (int)(weather_data.precipitation * 100) % 100);
    lv_obj_set_width(details, 570);
    lv_obj_set_style_text_align(details, LV_TEXT_ALIGN_CENTER, 0);
    if (weather_data.day_count) {
        char sunrise[12], sunset[12];
        weather_short_time(weather_data.daily[0].sunrise, sunrise);
        weather_short_time(weather_data.daily[0].sunset, sunset);
        lv_obj_t *sun = lv_label_create(current);
        lv_label_set_text_fmt(sun, "Sunrise %s  |  Sunset %s", sunrise, sunset);
    }

    lv_obj_t *hourly_title = lv_label_create(weather_body);
    lv_label_set_text(hourly_title, "Next 12 hours");
    lv_obj_set_style_text_font(hourly_title, &lv_font_montserrat_28, 0);
    lv_obj_t *hourly = lv_obj_create(weather_body);
    lv_obj_set_size(hourly, 610, 170);
    lv_obj_set_flex_flow(hourly, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hourly, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(hourly, 8, 0);
    lv_obj_set_style_pad_column(hourly, 8, 0);
    for (uint8_t i = 0; i < weather_data.hour_count; i++) {
        weather_hour_t *hour = &weather_data.hourly[i];
        lv_obj_t *card = lv_obj_create(hourly);
        lv_obj_set_size(card, 130, 135);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(card, 5, 0);
        lv_obj_set_style_pad_row(card, 3, 0);
        char time[12];
        weather_short_time(hour->time, time);
        lv_obj_t *label = lv_label_create(card);
        lv_label_set_text_fmt(label, "%s\n%d F\n%u%% rain\n%s", time, weather_round(hour->temperature),
                              (unsigned)hour->precipitation, weather_condition(hour->code));
        lv_obj_set_width(label, 115);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t *daily_title = lv_label_create(weather_body);
    lv_label_set_text(daily_title, "7-day forecast");
    lv_obj_set_style_text_font(daily_title, &lv_font_montserrat_28, 0);
    for (uint8_t i = 0; i < weather_data.day_count; i++) {
        weather_day_t *day = &weather_data.daily[i];
        lv_obj_t *row = lv_obj_create(weather_body);
        lv_obj_set_size(row, 610, 70);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text_fmt(label, "%c%c/%c%c  %s  %u%% rain  %d / %d F",
            day->date[5], day->date[6], day->date[8], day->date[9], weather_condition(day->code),
            (unsigned)day->precipitation, weather_round(day->high), weather_round(day->low));
        lv_obj_set_width(label, 570);
    }
    lv_obj_t *source = lv_label_create(weather_body);
    lv_label_set_text(source, "Weather data: Open-Meteo");
}

static void weather_start(const char *location)
{
    if (weather_busy) return;
    if (!wifi_connected) {
        if (weather_status) lv_label_set_text(weather_status, "Connect to Wi-Fi first");
        return;
    }
    while (*location && isspace((unsigned char)*location)) location++;
    if (strlen(location) < 2) {
        if (weather_status) lv_label_set_text(weather_status, "Enter a city or postal code");
        return;
    }
    snprintf(weather_pending_location, sizeof(weather_pending_location), "%.63s", location);
    weather_busy = true;
    weather_done = false;
    if (weather_status) lv_label_set_text(weather_status, "Updating forecast...");
    if (weather_task_handle) xTaskNotifyGive(weather_task_handle);
    else if (xTaskCreate(weather_task, "weather", 7168, NULL, 4, &weather_task_handle) == pdPASS)
        xTaskNotifyGive(weather_task_handle);
    else {
        weather_busy = false;
        if (weather_status) lv_label_set_text(weather_status, "Could not start weather service");
    }
}

static void weather_search_clicked(lv_event_t *event)
{
    (void)event;
    weather_start(lv_textarea_get_text(weather_location_area));
}

static void weather_refresh_clicked(lv_event_t *event)
{
    (void)event;
    weather_start(weather_location);
}

static void weather_keys_clicked(lv_event_t *event)
{
    (void)event;
    weather_keyboard_visible = !weather_keyboard_visible;
    if (weather_keyboard_visible) {
        lv_obj_clear_flag(weather_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_view_recursive(weather_keyboard, LV_ANIM_ON);
    } else {
        lv_obj_add_flag(weather_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_y(content, 0, LV_ANIM_ON);
    }
    lv_label_set_text(weather_keys_label, weather_keyboard_visible ? "Hide" : "Keys");
}

static void weather_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!weather_done || !weather_status) return;
    weather_done = false;
    lv_label_set_text(weather_status, weather_ok ? "Forecast updated" : weather_error);
    if (weather_ok) {
        lv_textarea_set_text(weather_location_area, weather_location);
        weather_render();
    }
}

static void show_weather(void)
{
    clear_content();
    lv_obj_set_style_pad_row(content, 10, 0);
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "Weather");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *search = lv_obj_create(content);
    lv_obj_set_size(search, 650, 76);
    lv_obj_clear_flag(search, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(search, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(search, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(search, 4, 0);
    weather_location_area = lv_textarea_create(search);
    lv_obj_set_size(weather_location_area, 385, 62);
    lv_textarea_set_one_line(weather_location_area, true);
    lv_textarea_set_max_length(weather_location_area, sizeof(weather_location) - 1);
    lv_textarea_set_text(weather_location_area, weather_location);
    lv_obj_add_event_cb(weather_location_area, weather_search_clicked, LV_EVENT_READY, NULL);
    lv_obj_t *find = button(search, "Find", weather_search_clicked);
    lv_obj_set_size(find, 110, 62);
    lv_obj_t *keys = button(search, weather_keyboard_visible ? "Hide" : "Keys", weather_keys_clicked);
    weather_keys_label = lv_obj_get_child(keys, 0);
    lv_obj_set_size(keys, 125, 62);

    lv_obj_t *tools = lv_obj_create(content);
    lv_obj_set_size(tools, 650, 58);
    lv_obj_clear_flag(tools, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tools, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tools, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(tools, 3, 0);
    weather_status = lv_label_create(tools);
    lv_obj_set_width(weather_status, 450);
    lv_label_set_text(weather_status, weather_busy ? "Updating forecast..." : weather_has_data ? "Saved forecast" : "Ready");
    lv_obj_t *refresh = button(tools, "Refresh", weather_refresh_clicked);
    lv_obj_set_size(refresh, 150, 52);

    weather_body = lv_obj_create(content);
    lv_obj_set_width(weather_body, 650);
    lv_obj_set_height(weather_body, LV_SIZE_CONTENT);
    lv_obj_clear_flag(weather_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(weather_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(weather_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(weather_body, 8, 0);
    lv_obj_set_style_pad_row(weather_body, 10, 0);
    if (weather_has_data) weather_render();
    else {
        lv_obj_t *message = lv_label_create(weather_body);
        lv_label_set_text(message, "Search for a city or postal code to load weather.");
        lv_obj_set_width(message, 600);
        lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    }

    weather_keyboard = lv_keyboard_create(content);
    lv_obj_set_size(weather_keyboard, 640, 330);
    lv_keyboard_set_textarea(weather_keyboard, weather_location_area);
    lv_obj_move_to_index(weather_keyboard, 3);
    if (!weather_keyboard_visible) lv_obj_add_flag(weather_keyboard, LV_OBJ_FLAG_HIDDEN);
    weather_timer = lv_timer_create(weather_tick, 200, NULL);
    if (!weather_has_data && !weather_busy && wifi_connected) weather_start(weather_location);
}

static void weather_clicked(lv_event_t *event)
{
    (void)event;
    show_weather();
}

static void screensaver_close(void)
{
    if (!screensaver) return;
    lv_obj_delete_async(screensaver);
    screensaver = screensaver_panel = screensaver_time = screensaver_date = screensaver_weather = NULL;
}

static void screensaver_touched(lv_event_t *event)
{
    (void)event;
    screensaver_close();
}

static void screensaver_update(void)
{
    if (!screensaver) return;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char text[48];
    strftime(text, sizeof(text), "%I:%M %p", &local);
    if (text[0] == '0') memmove(text, text + 1, strlen(text));
    lv_label_set_text(screensaver_time, text);
    strftime(text, sizeof(text), "%A, %B %d, %Y", &local);
    lv_label_set_text(screensaver_date, text);
    if (weather_has_data) {
        lv_label_set_text_fmt(screensaver_weather, "%d F\n%s\n%s\n\nBAT %d%%",
            weather_round(weather_data.temperature), weather_condition(weather_data.code),
            weather_data.place, battery_percent);
    } else {
        lv_label_set_text(screensaver_weather, wifi_connected ? "Loading Milwaukee weather..." :
            "Milwaukee weather needs Wi-Fi");
    }
    static const int16_t offsets[][2] = {{-45, -70}, {45, -35}, {-30, 15}, {35, 60}};
    size_t position = local.tm_min % (sizeof(offsets) / sizeof(offsets[0]));
    lv_obj_align(screensaver_panel, LV_ALIGN_CENTER, offsets[position][0], offsets[position][1]);
}

static void screensaver_show(void)
{
    if (screensaver) return;
    screensaver = lv_obj_create(lv_layer_top());
    lv_obj_set_size(screensaver, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(screensaver, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(screensaver, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screensaver, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(screensaver, lv_color_hex(0x080b12), 0);
    lv_obj_set_style_bg_opa(screensaver, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screensaver, 0, 0);
    lv_obj_set_style_radius(screensaver, 0, 0);
    lv_obj_set_style_text_color(screensaver, lv_color_white(), 0);
    lv_obj_add_event_cb(screensaver, screensaver_touched, LV_EVENT_PRESSED, NULL);

    screensaver_panel = lv_obj_create(screensaver);
    lv_obj_remove_style_all(screensaver_panel);
    lv_obj_set_size(screensaver_panel, 620, 650);
    lv_obj_set_flex_flow(screensaver_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screensaver_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screensaver_panel, 28, 0);
    screensaver_time = lv_label_create(screensaver_panel);
    lv_obj_set_style_text_font(screensaver_time, &lv_font_montserrat_48, 0);
    screensaver_date = lv_label_create(screensaver_panel);
    lv_obj_set_style_text_font(screensaver_date, &lv_font_montserrat_28, 0);
    screensaver_weather = lv_label_create(screensaver_panel);
    lv_obj_set_width(screensaver_weather, 600);
    lv_obj_set_style_text_font(screensaver_weather, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(screensaver_weather, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *source = lv_label_create(screensaver_panel);
    lv_label_set_text(source, "Weather data: Open-Meteo  |  Touch to wake");
    screensaver_update();
    if (wifi_connected && (!weather_has_data || time(NULL) - weather_fetched_at > 15 * 60))
        weather_start(weather_location);
}

static void screensaver_tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t inactive = lv_display_get_inactive_time(NULL);
    if (screensaver) {
        if (inactive < 1000 || alarm_active) screensaver_close();
        else screensaver_update();
    } else if (!alarm_active && inactive >= SCREENSAVER_IDLE_MS) {
        screensaver_show();
    }
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
    app_icon(content, LV_SYMBOL_CHARGE, "GPIO", 0xE65100, gpio_clicked, 0, 1);
    app_icon(content, LV_SYMBOL_WIFI, "Settings", 0x0288D1, settings_clicked, 1, 1);
    app_icon(content, LV_SYMBOL_ENVELOPE, "AI Chat", 0xE91E63, chat_clicked, 2, 1);
    app_icon(content, LV_SYMBOL_EYE_OPEN, "Browser", 0x3F51B5, browser_clicked, 0, 2);
    app_icon(content, LV_SYMBOL_FILE, "Ebooks", 0x8D6E63, ebooks_clicked, 1, 2);
    app_icon(content, LV_SYMBOL_LOOP, "Clock", 0x009688, clock_clicked, 2, 2);
    app_icon(content, LV_SYMBOL_SETTINGS, "System", 0x7C4DFF, system_clicked, 0, 3);
    app_icon(content, LV_SYMBOL_BARS, "Scope", 0x00897B, scope_clicked, 1, 3);
    app_icon(content, LV_SYMBOL_REFRESH, "Weather", 0x039BE5, weather_clicked, 2, 3);
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
    scope_self_test();
    alarm_self_test();
    weather_self_test();
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
    load_alarms();
    load_weather_location();
    load_chat_config();

    internal_ready = mount_internal();
    sd_ready = bsp_sdcard_init(SD_PATH, 5) == ESP_OK;

    lv_display_t *display = bsp_display_start();
    bsp_display_lock(0);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x10141f), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    header = lv_obj_create(screen);
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
    lv_timer_create(screensaver_tick, 1000, NULL);

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
