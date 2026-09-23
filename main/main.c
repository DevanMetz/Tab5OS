#include <dirent.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "storage_io.h"
#include "capture_viewer.h"
#include "serial_log_viewer.h"
#include "ble_tool.h"
#include "http_tool.h"
#include "mqtt_tool.h"
#include "network_tool.h"
#include "ota_manifest.h"
#include "signal_tool.h"
#include "spi_tool.h"
#include "uart_tool.h"
#include "ender3_tool.h"
#include "esp_app_desc.h"
#include "esp_cache.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
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
#include "driver/ledc.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "lvgl.h"
#include "cJSON.h"
#include "sdkconfig.h"

#if !defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE) || \
    !defined(CONFIG_ESP_HOSTED_RESET_GPIO_ACTIVE_LOW) || \
    CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH != 4 || \
    CONFIG_ESP_HOSTED_SDIO_PIN_CLK != 12 || \
    CONFIG_ESP_HOSTED_SDIO_PIN_CMD != 13 || \
    CONFIG_ESP_HOSTED_SDIO_PIN_D0 != 11 || \
    CONFIG_ESP_HOSTED_SDIO_PIN_D1 != 10 || \
    CONFIG_ESP_HOSTED_SDIO_PIN_D2 != 9 || \
    CONFIG_ESP_HOSTED_SDIO_PIN_D3 != 8 || \
    CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE != 15
#error "Tab5 OS requires ESP-Hosted SDIO CLK12 CMD13 D0-D3=11,10,9,8 RESET15 active-low; delete sdkconfig and rebuild"
#endif

#if !defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) || \
    !defined(CONFIG_ESP_CONSOLE_SECONDARY_NONE)
#error "Tab5 OS reserves hardware UARTs for tools; use USB Serial/JTAG as the sole console"
#endif

#ifdef CHAT_HAS_SECRETS
#include "chat_secrets.h"
#else
#define CHAT_RELAY_URL ""
#define CHAT_DEVICE_TOKEN ""
#endif

#define INTERNAL_PATH BSP_SPIFFS_MOUNT_POINT
#define SD_PATH "/sdcard"
#define HEALTH_PATH SD_PATH "/HEALTH"
#define HEART_RATE_LOG HEALTH_PATH "/HR.CSV"
#define SCOPE_PATH SD_PATH "/SCOPE"
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
#define OTA_MANIFEST_URL "https://github.com/DevanMetz/Tab5OS/releases/latest/download/tab5_os.json"
#define BATTERY_EMPTY_MV 6000
#define BATTERY_FULL_MV 8230
#define BATTERY_HISTORY_POINTS 60
#define SCOPE_RING_POINTS 1200
#define SCOPE_CHART_POINTS 300
#define ALARM_COUNT 3
#define WEATHER_HOURS 12
#define WEATHER_DAYS 7
#define SCREENSAVER_IDLE_MS (2 * 60 * 1000)
#define SCREENSAVER_FORECAST_ITEMS 5
#define OTA_HEALTH_WINDOW_MS (30 * 1000)
#define I2C_STANDARD_SPEED_HZ 100000U
#define I2C_FAST_SPEED_HZ 400000U
#define I2C_WRITE_CONFIRM_MS 5000U
#define I2C_CAPTURE_FLUSH_MS (10 * 1000)
#define TIME_ZONE "CST6CDT,M3.2.0,M11.1.0"
#define GOVEE_TEMP_OFFSET_C 0.0f
#define GOVEE_HUMIDITY_OFFSET 0.0f
#define SERVO_PIN GPIO_NUM_0
#define TOY_LED_PIN GPIO_NUM_54
#define SERVO_LEDC_TIMER LEDC_TIMER_1
#define SERVO_LEDC_CHANNEL LEDC_CHANNEL_2
#define SERVO_TIMEOUT_MS (5 * 60 * 1000)
#define RING_HR_TIMEOUT_MS (60 * 1000)
#define RING_SYNC_TIMEOUT_MS (15 * 1000)
#define RING_HR_HISTORY_POINTS 60
#define RING_HR_SYNC_DAYS 7
#define RIDE_CHART_POINTS 120
#define RIDE_FLUSH_MS (10 * 1000)

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

typedef enum {
    DISPLAY_AWAKE,
    DISPLAY_DIMMED,
    DISPLAY_OFF,
} display_power_state_t;

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
    uint32_t frequency_hz;
    uint16_t duty_permille;
} scope_measurement_t;

typedef struct {
    const char *symbol;
    const char *name;
    uint32_t color;
    lv_event_cb_t enter;
    void (*leave)(void);
    uint8_t column;
    uint8_t row;
} app_definition_t;

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

typedef struct {
    float temperature_c;
    float humidity;
    uint8_t battery;
} govee_reading_t;

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
static const int16_t scope_offset_choices_mv[] = {-200, -100, -50, -20, -10, 0, 10, 20, 50, 100, 200};
static const uint16_t scope_gain_choices_permille[] = {900, 950, 975, 1000, 1025, 1050, 1100};

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
static lv_obj_t *storage_status;
static lv_obj_t *storage_format_label;
static bool storage_format_armed;
static uint8_t battery_history[BATTERY_HISTORY_POINTS];
static uint8_t battery_history_count;
static uint8_t battery_history_head;
static int battery_millivolts;
static int battery_milliamps;
static int battery_percent;
static lv_obj_t *note_area;
static lv_obj_t *counter_label;
static bool internal_ready;
static esp_err_t storage_init_error = ESP_OK;
static volatile bool sd_ready;
static portMUX_TYPE sd_error_lock = portMUX_INITIALIZER_UNLOCKED;
static int sd_last_errno;
static int counter;
static char current_directory[256];
static char file_paths[64][256];
static size_t file_path_count;
static bool wifi_ready;
static esp_err_t nvs_init_error = ESP_OK;
static volatile bool wifi_connecting;
static volatile bool wifi_connected;
static volatile bool wifi_scan_busy;
static volatile bool wifi_scan_done;
static esp_err_t wifi_scan_error;
static unsigned wifi_retries;
static bool wifi_should_connect;
static bool wifi_forget_armed;
static uint32_t wifi_forget_armed_at;
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
static volatile bool voice_mic_open;
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
static lv_obj_t *i2c_status;
static lv_obj_t *i2c_devices;
static lv_obj_t *i2c_address_label;
static lv_obj_t *i2c_register_label;
static lv_obj_t *i2c_read_result;
static lv_obj_t *i2c_speed_label;
static lv_obj_t *i2c_watch_label;
static lv_obj_t *i2c_value_label;
static lv_obj_t *i2c_write_label;
static lv_obj_t *i2c_capture_label;
static lv_obj_t *i2c_capture_status;
static lv_timer_t *i2c_timer;
static FILE *i2c_capture_file;
static char i2c_capture_temporary_path[96];
static char i2c_capture_final_path[96];
static char i2c_capture_notice[128];
static TickType_t i2c_capture_last_flush_tick;
static uint8_t i2c_selected_address = 0x08;
static uint8_t i2c_selected_register;
static uint8_t i2c_write_value;
static uint32_t i2c_bus_speed_hz = I2C_STANDARD_SPEED_HZ;
static uint32_t i2c_write_armed_at_ms;
static bool i2c_watch_enabled;
static bool i2c_write_armed;
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
static lv_obj_t *scope_offset_label;
static lv_obj_t *scope_gain_label;
static lv_obj_t *scope_capture_status;
static int32_t *scope_chart_points;
static uint16_t *scope_ring;
static uint16_t *scope_snapshot;
static size_t scope_ring_head;
static size_t scope_ring_count;
static portMUX_TYPE scope_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t scope_task_handle;
static volatile bool scope_active;
static volatile bool scope_sampling;
static volatile bool scope_running = true;
static volatile bool scope_error;
static volatile uint8_t scope_channel_index;
static volatile uint8_t scope_rate_index = 1;
static uint8_t scope_range_index;
static uint8_t scope_trigger_mode;
static uint16_t scope_trigger_mv = 1650;
static int16_t scope_offsets_mv[SCOPE_CHANNEL_COUNT];
static uint16_t scope_gains_permille[SCOPE_CHANNEL_COUNT];
static bool scope_chart_ready;
static char scope_capture_notice[128];
static void (*active_app_leave)(void);
static TaskHandle_t ota_task_handle;
static volatile bool ota_busy;
static volatile bool ota_done;
static bool ota_ok;
static bool ota_health_window_elapsed;
static char ota_error[96];
static char ota_last_result[64] = "none recorded";
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
static portMUX_TYPE govee_lock = portMUX_INITIALIZER_UNLOCKED;
static govee_reading_t govee_reading;
static char govee_name[24] = "Govee H5075";
static char govee_address[18];
static int8_t govee_rssi;
static time_t govee_updated_at;
static bool govee_ready;
static bool govee_enabled;
static bool govee_scanning;
static lv_obj_t *govee_status;
static lv_obj_t *govee_toggle_label;
static lv_obj_t *govee_temperature;
static lv_obj_t *govee_humidity;
static lv_obj_t *govee_details;
static lv_timer_t *govee_timer;
static portMUX_TYPE ring_lock = portMUX_INITIALIZER_UNLOCKED;
static char ring_name[24] = "COLMI R12";
static char ring_address[18];
static int8_t ring_rssi;
static int ring_battery = -1;
static time_t ring_updated_at;
static bool ring_found;
static bool ring_enabled;
static bool ring_connecting;
static bool ring_connected;
static bool ring_stopping;
static bool ring_charging;
static uint16_t ring_conn_handle;
static int ring_heart_rate = -1;
static int ring_hr_error;
static time_t ring_hr_updated_at;
static bool ring_hr_active;
static TickType_t ring_hr_deadline;
static uint8_t ring_hr_history[RING_HR_HISTORY_POINTS];
static uint8_t ring_hr_history_count;
static uint8_t ring_hr_history_head;
typedef struct { time_t timestamp; uint8_t bpm; } ring_hr_sample_t;
static QueueHandle_t ring_hr_samples;
static StaticQueue_t ring_hr_queue_control;
static uint8_t *ring_hr_queue_storage;
static time_t ring_hr_last_saved;
static bool ring_hr_last_saved_loaded;
static char ring_storage_error[96];
static int ring_sync_days_ago = RING_HR_SYNC_DAYS;
static int ring_sync_packets_total;
static time_t ring_sync_day_start;
static bool ring_sync_active;
static bool ring_sync_pending;
static TickType_t ring_sync_deadline;
static lv_obj_t *ring_status;
static lv_obj_t *ring_toggle_label;
static lv_obj_t *ring_battery_label;
static lv_obj_t *ring_hr_label;
static lv_obj_t *ring_hr_status;
static lv_obj_t *ring_hr_button_label;
static lv_obj_t *ring_hr_chart;
static lv_chart_series_t *ring_hr_series;
static lv_obj_t *ring_details;
static lv_timer_t *ring_timer;
typedef struct {
    float speed_kmh;
    float cadence_rpm;
    int power_w;
    int resistance;
    bool has_speed, has_cadence, has_power, has_resistance;
} kickr_data_t;
static portMUX_TYPE kickr_lock = portMUX_INITIALIZER_UNLOCKED;
static char kickr_name[32] = "Wahoo KICKR";
static char kickr_address[18];
static bool kickr_enabled, kickr_found, kickr_connecting, kickr_connected, kickr_subscribed, kickr_stopping;
static uint16_t kickr_conn_handle, kickr_service_start, kickr_service_end;
static uint16_t kickr_data_handle, kickr_cccd_handle;
static kickr_data_t kickr_data;
static time_t kickr_updated_at;
static bool ride_recording;
static time_t ride_started_at;
static TickType_t ride_started_tick, ride_last_log_tick, ride_last_flush_tick;
static FILE *ride_file;
static char ride_temporary_path[128], ride_final_path[128], ride_notice[96];
static float ride_distance_km, ride_work_kj;
static int64_t ride_power_sum, ride_hr_sum;
static uint32_t ride_power_samples, ride_hr_samples_count;
static int ride_max_power, ride_max_hr;
static TickType_t ride_next_hr_measure;
static lv_obj_t *ride_status, *ride_power_label, *ride_cadence_label, *ride_hr_label;
static lv_obj_t *ride_toggle_label, *ride_stats, *ride_button_label, *ride_history;
static lv_obj_t *ride_chart;
static lv_chart_series_t *ride_power_series, *ride_hr_series;
static lv_obj_t *servo_status;
static lv_obj_t *servo_position;
static lv_obj_t *servo_mode_label;
static lv_obj_t *servo_range_label;
static lv_obj_t *servo_rate_label;
static lv_timer_t *servo_timer;
static bool servo_running;
static bool servo_pwm_ready;
static bool servo_sine_mode = true;
static const uint16_t servo_ranges_us[] = {250, 375, 500};
static const uint8_t servo_frequencies_cHz[] = {5, 10, 20, 25, 50, 75, 100};
static uint8_t servo_range_index = 1;
static uint8_t servo_speed_index = 1;
static uint8_t servo_frequency_index = 3;
static uint16_t servo_pulse_us = 1500;
static uint16_t servo_target_us = 1500;
static uint32_t servo_phase;
static uint32_t servo_updated_ms;
static TickType_t servo_next_target;
static TickType_t servo_deadline;
static lv_obj_t *screensaver;
static lv_obj_t *screensaver_panel;
static lv_obj_t *screensaver_time;
static lv_obj_t *screensaver_date;
static lv_obj_t *screensaver_weather;
static lv_obj_t *screensaver_hour_labels[SCREENSAVER_FORECAST_ITEMS];
static lv_obj_t *screensaver_day_labels[SCREENSAVER_FORECAST_ITEMS];
static uint8_t display_brightness = 100;
static uint16_t screen_timeout_seconds = 300;
static display_power_state_t display_power_state = DISPLAY_OFF;
static const uint8_t display_brightness_choices[] = {100, 75, 50, 25};
static const uint16_t screen_timeout_choices[] = {300, 600, 1800, 0};

static void show_launcher(void);
static void show_files(const char *path);
static void show_settings(void);
static void show_chat(void);
static void show_browser(void);
static void show_ebooks(void);
static void show_clock(void);
static void show_gpio(void);
static void show_i2c(void);
static bool i2c_capture_stop(void);
static void show_scope(void);
static void show_weather(void);
static void show_govee(void);
static void show_ring(void);
static void show_cycling(void);
static void show_servo(void);
static void servo_stop(void);
static void weather_start(const char *location);
static int weather_round(float value);
static void clear_content(void);
static void browser_link_clicked(lv_event_t *event);
static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback);
static void settings_leave(void);
static void validate_running_ota(void);
static void screensaver_close(void);

static bool govee_decode(const uint8_t *manufacturer, size_t length, govee_reading_t *reading)
{
    if (length < 7 || manufacturer[0] != 0x88 || manufacturer[1] != 0xec || manufacturer[2] != 0x00) return false;
    uint32_t packed = ((uint32_t)manufacturer[3] << 16) |
                      ((uint32_t)manufacturer[4] << 8) | manufacturer[5];
    uint32_t magnitude = packed & 0x7fffff;
    reading->temperature_c = (float)(magnitude / 1000) / 10.0f;
    if (packed & 0x800000) reading->temperature_c = -reading->temperature_c;
    reading->temperature_c += GOVEE_TEMP_OFFSET_C;
    reading->humidity = (float)(magnitude % 1000) / 10.0f + GOVEE_HUMIDITY_OFFSET;
    reading->battery = manufacturer[6] & 0x7f;
    return !(manufacturer[6] & 0x80) && reading->temperature_c >= -40.0f &&
           reading->temperature_c <= 70.0f && reading->humidity >= 0.0f && reading->humidity <= 100.0f;
}

static void govee_self_test(void)
{
    const uint8_t sample[] = {0x88, 0xec, 0x00, 0x03, 0x4d, 0xb2, 0x64, 0x00};
    govee_reading_t reading;
    assert(govee_decode(sample, sizeof(sample), &reading));
    assert(reading.temperature_c > 21.5f && reading.temperature_c < 21.7f);
    assert(reading.humidity > 49.7f && reading.humidity < 49.9f && reading.battery == 100);
}

#define RING_WRITE_HANDLE 16
#define RING_NOTIFY_HANDLE 18
#define RING_NOTIFY_CCCD_HANDLE 19

static void ring_battery_packet(uint8_t packet[16])
{
    memset(packet, 0, 16);
    packet[0] = 0x03;
    packet[15] = 0x03;
}

static void ring_manual_hr_packet(uint8_t packet[16])
{
    memset(packet, 0, 16);
    packet[0] = 0x69;
    packet[1] = 0x01;
    packet[15] = 0x6a;
}

static void ring_auto_hr_packet(uint8_t packet[16])
{
    memset(packet, 0, 16);
    packet[0] = 0x16;
    packet[1] = 0x02;
    packet[2] = 0x01;
    packet[3] = 5;
    packet[15] = 0x1e;
}

static void ring_history_packet(uint8_t packet[16], time_t day)
{
    memset(packet, 0, 16);
    packet[0] = 0x15;
    struct tm utc;
    gmtime_r(&day, &utc);
    uint32_t ring_time = (uint32_t)(day + day - mktime(&utc));
    memcpy(packet + 1, &ring_time, sizeof(ring_time));
    for (int i = 0; i < 15; i++) packet[15] += packet[i];
}

static bool ring_packet_valid(const uint8_t *packet, size_t length)
{
    if (length != 16) return false;
    uint8_t checksum = 0;
    for (size_t i = 0; i < 15; i++) checksum += packet[i];
    return checksum == packet[15];
}

static bool ring_decode_battery(const uint8_t *packet, size_t length, int *battery, bool *charging)
{
    if (!ring_packet_valid(packet, length) || packet[0] != 0x03 || packet[1] > 100) return false;
    *battery = packet[1];
    *charging = packet[2] == 1;
    return true;
}

static bool ring_decode_manual_heart_rate(const uint8_t *packet, size_t length, int *error, int *heart_rate)
{
    if (!ring_packet_valid(packet, length) || packet[0] != 0x69 || packet[2] > 2 || packet[3] > 250) return false;
    *error = packet[2];
    *heart_rate = packet[3];
    return true;
}

static bool ring_should_connect(bool enabled, bool is_ring, bool connecting, bool connected)
{
    return enabled && is_ring && !connecting && !connected;
}

static bool ring_connection_active(uint16_t connection)
{
    portENTER_CRITICAL(&ring_lock);
    bool active = ring_enabled && ring_connected && ring_conn_handle == connection;
    portEXIT_CRITICAL(&ring_lock);
    return active;
}

static bool kickr_connection_active(uint16_t connection)
{
    portENTER_CRITICAL(&kickr_lock);
    bool active = kickr_enabled && kickr_connected && kickr_conn_handle == connection;
    portEXIT_CRITICAL(&kickr_lock);
    return active;
}

static bool ble_should_scan(bool govee, bool ring, bool kickr)
{
    return govee || ring || kickr;
}

static bool ble_products_idle(void)
{
    portENTER_CRITICAL(&govee_lock);
    bool govee = govee_enabled;
    portEXIT_CRITICAL(&govee_lock);
    portENTER_CRITICAL(&ring_lock);
    bool ring = ring_enabled || ring_connecting || ring_connected || ring_stopping;
    portEXIT_CRITICAL(&ring_lock);
    portENTER_CRITICAL(&kickr_lock);
    bool kickr = kickr_enabled || kickr_connecting || kickr_connected || kickr_stopping;
    portEXIT_CRITICAL(&kickr_lock);
    return !govee && !ring && !kickr;
}

static bool sd_media_lost(int error)
{
    return error == EIO || error == ENODEV || error == ENXIO || error == EBADF;
}

static bool sd_storage_error(int error)
{
    return sd_media_lost(error) || error == ENOSPC || error == EROFS;
}

static void sd_record_error(int error)
{
    if (!sd_storage_error(error)) return;
    portENTER_CRITICAL(&sd_error_lock);
    if (sd_last_errno == 0 || (sd_media_lost(error) && !sd_media_lost(sd_last_errno)))
        sd_last_errno = error;
    if (sd_media_lost(error)) sd_ready = false;
    portEXIT_CRITICAL(&sd_error_lock);
    ESP_LOGE("storage", "SD card error: %s", strerror(error));
}

static int sd_error_snapshot(void)
{
    portENTER_CRITICAL(&sd_error_lock);
    int error = sd_last_errno;
    portEXIT_CRITICAL(&sd_error_lock);
    return error;
}

static void sd_self_test(void)
{
    assert(sd_media_lost(EIO) && sd_media_lost(ENODEV));
    assert(!sd_media_lost(ENOSPC));
    assert(sd_storage_error(ENOSPC) && sd_storage_error(EROFS));
    assert(!sd_storage_error(EEXIST));
}

static void ring_self_test(void)
{
    uint8_t packet[16];
    ring_battery_packet(packet);
    assert(packet[0] == 3 && packet[15] == 3);
    const uint8_t response[16] = {3, 99, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 103};
    int battery;
    bool charging;
    assert(ring_decode_battery(response, sizeof(response), &battery, &charging));
    assert(battery == 99 && charging);
    ring_manual_hr_packet(packet);
    assert(packet[0] == 0x69 && packet[1] == 1 && packet[15] == 0x6a);
    const uint8_t manual_response[16] = {0x69, 1, 0, 72, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 178};
    int error;
    assert(ring_decode_manual_heart_rate(manual_response, sizeof(manual_response), &error, &battery));
    assert(error == 0 && battery == 72);
    ring_auto_hr_packet(packet);
    assert(packet[0] == 0x16 && packet[3] == 5 && ring_packet_valid(packet, sizeof(packet)));
    assert(!ring_should_connect(false, true, false, false));
    assert(ring_should_connect(true, true, false, false));
}

static bool kickr_read_field(const uint8_t *packet, size_t length, size_t *offset,
                             size_t bytes, uint32_t *value)
{
    if (*offset + bytes > length) return false;
    *value = 0;
    for (size_t i = 0; i < bytes; i++) *value |= (uint32_t)packet[*offset + i] << (i * 8);
    *offset += bytes;
    return true;
}

static bool kickr_decode(const uint8_t *packet, size_t length, kickr_data_t *data)
{
    if (length < 2) return false;
    uint16_t flags = packet[0] | ((uint16_t)packet[1] << 8);
    size_t offset = 2;
    uint32_t value;
    memset(data, 0, sizeof(*data));
#define KICKR_FIELD(bit, bytes, body) do { if (flags & (1U << (bit))) { \
    if (!kickr_read_field(packet, length, &offset, bytes, &value)) { return false; } body; } } while (0)
    if (!(flags & 1)) {
        if (!kickr_read_field(packet, length, &offset, 2, &value)) return false;
        data->speed_kmh = value / 100.0f;
        data->has_speed = true;
    }
    KICKR_FIELD(1, 2, (void)0);
    KICKR_FIELD(2, 2, data->cadence_rpm = value / 2.0f; data->has_cadence = true);
    KICKR_FIELD(3, 2, (void)0);
    KICKR_FIELD(4, 3, (void)0);
    KICKR_FIELD(5, 2, data->resistance = (int16_t)value; data->has_resistance = true);
    KICKR_FIELD(6, 2, data->power_w = (int16_t)value; data->has_power = true);
    KICKR_FIELD(7, 2, (void)0);
    KICKR_FIELD(8, 5, (void)0);
    KICKR_FIELD(9, 1, (void)0);
    KICKR_FIELD(10, 1, (void)0);
    KICKR_FIELD(11, 2, (void)0);
    KICKR_FIELD(12, 2, (void)0);
#undef KICKR_FIELD
    return offset == length;
}

static void kickr_self_test(void)
{
    const uint8_t packet[] = {0x74, 0x00, 0xb2, 0x0c, 0xb4, 0x00, 0x39, 0x30, 0x00,
                              0xc8, 0x00, 0xfa, 0x00};
    kickr_data_t data;
    assert(kickr_decode(packet, sizeof(packet), &data));
    assert(data.has_speed && data.speed_kmh == 32.5f);
    assert(data.has_cadence && data.cadence_rpm == 90.0f);
    assert(data.has_power && data.power_w == 250 && data.resistance == 200);
    assert(!ble_should_scan(false, false, false));
    assert(ble_should_scan(true, false, false));
    assert(ble_should_scan(false, true, false));
    assert(ble_should_scan(false, false, true));
}

static void ble_scan(void);
static int govee_gap_event(struct ble_gap_event *event, void *argument);
static int kickr_gap_event(struct ble_gap_event *event, void *argument);

static int ring_write_done(uint16_t conn_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attribute, void *argument)
{
    (void)conn_handle;
    (void)attribute;
    (void)argument;
    if (error->status) ESP_LOGW("ring", "GATT write failed: %d", error->status);
    return 0;
}

static int ring_history_requested(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attribute, void *argument)
{
    ring_write_done(conn_handle, error, attribute, argument);
    if (!ring_connection_active(conn_handle)) return 0;
    if (error->status) {
        portENTER_CRITICAL(&ring_lock);
        ring_sync_active = false;
        ring_sync_pending = ring_enabled;
        portEXIT_CRITICAL(&ring_lock);
    }
    return 0;
}

static void ring_schedule_history_sync(void)
{
    portENTER_CRITICAL(&ring_lock);
    if (ring_enabled) {
        ring_sync_days_ago = RING_HR_SYNC_DAYS;
        ring_sync_active = false;
        ring_sync_pending = true;
    }
    portEXIT_CRITICAL(&ring_lock);
}

static int ring_battery_requested(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attribute, void *argument)
{
    ring_write_done(conn_handle, error, attribute, argument);
    if (!error->status && ring_connection_active(conn_handle)) ring_schedule_history_sync();
    return 0;
}

static int ring_auto_hr_set(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attribute, void *argument)
{
    ring_write_done(conn_handle, error, attribute, argument);
    if (error->status || !ring_connection_active(conn_handle)) return 0;
    uint8_t packet[16];
    ring_battery_packet(packet);
    int rc = ble_gattc_write_flat(conn_handle, RING_WRITE_HANDLE, packet, sizeof(packet),
                                  ring_battery_requested, NULL);
    if (rc) ESP_LOGW("ring", "Battery request failed: %d", rc);
    return 0;
}

static int ring_send_manual_hr(void)
{
    uint16_t connection;
    portENTER_CRITICAL(&ring_lock);
    bool enabled = ring_enabled;
    bool connected = ring_connected;
    connection = ring_conn_handle;
    portEXIT_CRITICAL(&ring_lock);
    if (!enabled || !connected) return -1;
    uint8_t packet[16];
    ring_manual_hr_packet(packet);
    return ble_gattc_write_flat(connection, RING_WRITE_HANDLE, packet, sizeof(packet), ring_write_done, NULL);
}

static bool ring_hr_append_log(time_t sample_time, int heart_rate)
{
    if (!sd_ready) {
        int error = sd_error_snapshot();
        errno = error ? error : ENODEV;
        return false;
    }
    mkdir(HEALTH_PATH, 0775);
    if (storage_repair_csv_tail(HEART_RATE_LOG) != 0) {
        int error = errno ? errno : EIO;
        sd_record_error(error);
        errno = error;
        return false;
    }
    FILE *file = fopen(HEART_RATE_LOG, "ab+");
    if (!file) {
        int error = errno ? errno : EIO;
        sd_record_error(error);
        errno = error;
        return false;
    }
    bool ok = fseek(file, 0, SEEK_END) == 0;
    long size = ok ? ftell(file) : -1;
    if (size < 0) ok = false;
    if (ok && size == 0) ok = fputs("unix_time,local_time,bpm\n", file) >= 0;
    struct tm local;
    char timestamp[24];
    localtime_r(&sample_time, &local);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
    if (ok) ok = fprintf(file, "%lld,%s,%d\n", (long long)sample_time, timestamp, heart_rate) >= 0;
    int write_error = 0;
    if (ok && storage_sync_file(file) != 0) {
        write_error = errno ? errno : EIO;
        ok = false;
    }
    if (fclose(file) != 0) {
        if (!write_error) write_error = errno ? errno : EIO;
        ok = false;
    }
    if (!ok) {
        if (!write_error) write_error = errno ? errno : EIO;
        storage_repair_csv_tail(HEART_RATE_LOG);
        sd_record_error(write_error);
        errno = write_error;
    }
    return ok;
}

static void ring_hr_load_last_saved(void)
{
    if (ring_hr_last_saved_loaded) return;
    if (storage_repair_csv_tail(HEART_RATE_LOG) != 0) {
        sd_record_error(errno ? errno : EIO);
        return;
    }
    ring_hr_last_saved_loaded = true;
    FILE *file = fopen(HEART_RATE_LOG, "rb");
    if (!file) return;
    char tail[513];
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    long end = ftell(file);
    long offset = end > (long)sizeof(tail) - 1 ? end - ((long)sizeof(tail) - 1) : 0;
    if (end < 0 || fseek(file, offset, SEEK_SET) != 0) {
        fclose(file);
        return;
    }
    size_t length = fread(tail, 1, sizeof(tail) - 1, file);
    tail[length] = '\0';
    fclose(file);
    char *line = tail;
    if (offset) {
        line = strchr(line, '\n');
        if (!line) return;
        line++;
    }
    long long timestamp;
    while (*line) {
        if (sscanf(line, "%lld,", &timestamp) == 1 && timestamp > ring_hr_last_saved)
            ring_hr_last_saved = (time_t)timestamp;
        line = strchr(line, '\n');
        if (!line) break;
        line++;
    }
}

static int ring_request_history(void)
{
    uint16_t connection;
    int days_ago;
    portENTER_CRITICAL(&ring_lock);
    if (!ring_enabled || !ring_connected || ring_hr_active || ring_sync_active || !ring_sync_pending) {
        portEXIT_CRITICAL(&ring_lock);
        return -1;
    }
    connection = ring_conn_handle;
    days_ago = ring_sync_days_ago;
    portEXIT_CRITICAL(&ring_lock);

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    local.tm_mday -= days_ago;
    time_t day = mktime(&local);
    uint8_t packet[16];
    ring_history_packet(packet, day);
    int rc = ble_gattc_write_flat(connection, RING_WRITE_HANDLE, packet, sizeof(packet),
                                  ring_history_requested, NULL);
    if (rc) return rc;
    portENTER_CRITICAL(&ring_lock);
    ring_sync_day_start = day;
    ring_sync_packets_total = 0;
    ring_sync_pending = false;
    ring_sync_active = true;
    ring_sync_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RING_SYNC_TIMEOUT_MS);
    portEXIT_CRITICAL(&ring_lock);
    ESP_LOGI("ring", "Syncing heart-rate history from %d day(s) ago", days_ago);
    return 0;
}

static bool ring_hr_begin(void)
{
    portENTER_CRITICAL(&ring_lock);
    bool ready = ring_enabled && ring_connected && !ring_hr_active && !ring_sync_active;
    portEXIT_CRITICAL(&ring_lock);
    if (!ready || ring_send_manual_hr() != 0) return false;
    portENTER_CRITICAL(&ring_lock);
    ring_hr_active = true;
    ring_hr_error = 0;
    ring_hr_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RING_HR_TIMEOUT_MS);
    portEXIT_CRITICAL(&ring_lock);
    return true;
}

static void ring_health_tick(lv_timer_t *timer)
{
    (void)timer;
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&ring_lock);
    if (ring_hr_active && (int32_t)(now - ring_hr_deadline) >= 0) ring_hr_active = false;
    if (ring_sync_active && (int32_t)(now - ring_sync_deadline) >= 0) {
        ring_sync_active = false;
        ring_sync_pending = true;
    }
    portEXIT_CRITICAL(&ring_lock);

    ring_hr_load_last_saved();
    ring_hr_sample_t sample;
    while (ring_hr_samples && xQueuePeek(ring_hr_samples, &sample, 0) == pdTRUE) {
        if (sample.timestamp > ring_hr_last_saved) {
            if (ring_hr_append_log(sample.timestamp, sample.bpm)) {
                ring_hr_last_saved = sample.timestamp;
                ring_storage_error[0] = '\0';
            } else {
                snprintf(ring_storage_error, sizeof(ring_storage_error),
                         "Heart-rate log not saved: %s", strerror(errno));
                break;
            }
        }
        if (xQueueReceive(ring_hr_samples, &sample, 0) != pdTRUE) break;
        portENTER_CRITICAL(&ring_lock);
        ring_heart_rate = sample.bpm;
        ring_hr_updated_at = sample.timestamp;
        ring_hr_history[ring_hr_history_head] = sample.bpm;
        ring_hr_history_head = (ring_hr_history_head + 1) % RING_HR_HISTORY_POINTS;
        if (ring_hr_history_count < RING_HR_HISTORY_POINTS) ring_hr_history_count++;
        portEXIT_CRITICAL(&ring_lock);
    }
    if (!ring_hr_samples || uxQueueMessagesWaiting(ring_hr_samples) == 0) ring_request_history();
}

static int ring_subscribed(uint16_t conn_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attribute, void *argument)
{
    (void)attribute;
    (void)argument;
    if (error->status) {
        ESP_LOGW("ring", "Notification setup failed: %d", error->status);
        return 0;
    }
    if (!ring_connection_active(conn_handle)) return 0;
    uint8_t packet[16];
    ring_auto_hr_packet(packet);
    int rc = ble_gattc_write_flat(conn_handle, RING_WRITE_HANDLE, packet, sizeof(packet), ring_auto_hr_set, NULL);
    if (rc) ESP_LOGW("ring", "Automatic HR setup failed: %d", rc);
    return 0;
}

static void ring_history_finished(void)
{
    portENTER_CRITICAL(&ring_lock);
    ring_sync_active = false;
    if (!ring_enabled) {
        ring_sync_pending = false;
    } else if (ring_sync_days_ago > 0) {
        ring_sync_days_ago--;
        ring_sync_pending = true;
    }
    portEXIT_CRITICAL(&ring_lock);
}

static bool ring_handle_history(const uint8_t packet[16])
{
    if (!ring_packet_valid(packet, 16) || packet[0] != 0x15) return false;
    portENTER_CRITICAL(&ring_lock);
    bool enabled = ring_enabled;
    if (enabled) ring_sync_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RING_SYNC_TIMEOUT_MS);
    portEXIT_CRITICAL(&ring_lock);
    if (!enabled) return true;
    int packet_number = packet[1];
    if (packet_number == 0xff) {
        ring_history_finished();
        return true;
    }
    if (packet_number == 0) {
        portENTER_CRITICAL(&ring_lock);
        ring_sync_packets_total = packet[2];
        portEXIT_CRITICAL(&ring_lock);
        if (packet[2] <= 1) ring_history_finished();
        return true;
    }

    portENTER_CRITICAL(&ring_lock);
    int packets_total = ring_sync_packets_total;
    time_t day_start = ring_sync_day_start;
    portEXIT_CRITICAL(&ring_lock);
    int start = packet_number == 1 ? 6 : 2;
    int previous_minutes = packet_number == 1 ? 0 : 45 + (packet_number - 2) * 65;
    for (int i = start; i < 15; i++) {
        if (!packet[i]) continue;
        struct tm local;
        localtime_r(&day_start, &local);
        int minute = previous_minutes + (i - start) * 5;
        local.tm_hour = minute / 60;
        local.tm_min = minute % 60;
        local.tm_sec = 0;
        ring_hr_sample_t sample = {.timestamp = mktime(&local), .bpm = packet[i]};
        if (!ring_hr_samples || xQueueSend(ring_hr_samples, &sample, 0) != pdTRUE)
            ESP_LOGW("ring", "Heart-rate history queue full");
    }
    if (packets_total > 0 && packet_number == packets_total - 1) ring_history_finished();
    return true;
}

static int kickr_subscribed_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attribute, void *argument)
{
    (void)attribute; (void)argument;
    portENTER_CRITICAL(&kickr_lock);
    bool active = kickr_enabled && kickr_connected && kickr_conn_handle == conn_handle;
    kickr_subscribed = active && error->status == 0;
    portEXIT_CRITICAL(&kickr_lock);
    if (!active) return 0;
    if (error->status) ESP_LOGW("kickr", "Indoor Bike subscription failed: %d", error->status);
    else ESP_LOGI("kickr", "Receiving Indoor Bike Data");
    return 0;
}

static int kickr_descriptor_found(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  uint16_t chr_val_handle, const struct ble_gatt_dsc *descriptor,
                                  void *argument)
{
    (void)argument;
    if (!kickr_connection_active(conn_handle)) return 0;
    if (!error->status && chr_val_handle == kickr_data_handle &&
        ble_uuid_cmp(&descriptor->uuid.u, BLE_UUID16_DECLARE(0x2902)) == 0)
        kickr_cccd_handle = descriptor->handle;
    if (error->status == BLE_HS_EDONE) {
        if (!kickr_cccd_handle) {
            ESP_LOGW("kickr", "Indoor Bike CCCD not found");
            return 0;
        }
        const uint8_t notify[] = {1, 0};
        int rc = ble_gattc_write_flat(conn_handle, kickr_cccd_handle, notify, sizeof(notify),
                                      kickr_subscribed_cb, NULL);
        if (rc) ESP_LOGW("kickr", "Indoor Bike subscription failed: %d", rc);
    }
    return 0;
}

static int kickr_characteristic_found(uint16_t conn_handle, const struct ble_gatt_error *error,
                                      const struct ble_gatt_chr *characteristic, void *argument)
{
    (void)argument;
    if (!kickr_connection_active(conn_handle)) return 0;
    if (!error->status && characteristic) kickr_data_handle = characteristic->val_handle;
    if (error->status == BLE_HS_EDONE) {
        if (!kickr_data_handle) {
            ESP_LOGW("kickr", "Indoor Bike Data characteristic not found");
            return 0;
        }
        int rc = ble_gattc_disc_all_dscs(conn_handle, kickr_data_handle, kickr_service_end,
                                         kickr_descriptor_found, NULL);
        if (rc) ESP_LOGW("kickr", "Descriptor discovery failed: %d", rc);
    }
    return 0;
}

static int kickr_service_found(uint16_t conn_handle, const struct ble_gatt_error *error,
                               const struct ble_gatt_svc *service, void *argument)
{
    (void)argument;
    if (!kickr_connection_active(conn_handle)) return 0;
    if (!error->status && service) {
        kickr_service_start = service->start_handle;
        kickr_service_end = service->end_handle;
    }
    if (error->status == BLE_HS_EDONE) {
        if (!kickr_service_start) {
            ESP_LOGW("kickr", "Fitness Machine service not found");
            return 0;
        }
        int rc = ble_gattc_disc_chrs_by_uuid(conn_handle, kickr_service_start, kickr_service_end,
                                             BLE_UUID16_DECLARE(0x2ad2), kickr_characteristic_found, NULL);
        if (rc) ESP_LOGW("kickr", "Indoor Bike discovery failed: %d", rc);
    }
    return 0;
}

static int kickr_gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        portENTER_CRITICAL(&kickr_lock);
        bool enabled = kickr_enabled;
        kickr_connecting = false;
        kickr_connected = enabled && event->connect.status == 0;
        kickr_stopping = !enabled && event->connect.status == 0;
        kickr_conn_handle = event->connect.conn_handle;
        kickr_service_start = kickr_service_end = kickr_data_handle = kickr_cccd_handle = 0;
        portEXIT_CRITICAL(&kickr_lock);
        if (!event->connect.status && !enabled) {
            int rc = ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            if (rc) ESP_LOGW("kickr", "Disconnect request failed: %d", rc);
        } else if (!event->connect.status) {
            ESP_LOGI("kickr", "Connected to %s", kickr_name);
            int rc = ble_gattc_disc_svc_by_uuid(kickr_conn_handle, BLE_UUID16_DECLARE(0x1826),
                                                kickr_service_found, NULL);
            if (rc) ESP_LOGW("kickr", "Fitness Machine discovery failed: %d", rc);
        } else ESP_LOGW("kickr", "Connection failed: %d", event->connect.status);
        ble_scan();
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        portENTER_CRITICAL(&kickr_lock);
        kickr_connected = kickr_connecting = kickr_subscribed = false;
        kickr_stopping = false;
        portEXIT_CRITICAL(&kickr_lock);
        ble_scan();
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_NOTIFY_RX && event->notify_rx.attr_handle == kickr_data_handle &&
        kickr_connection_active(event->notify_rx.conn_handle)) {
        size_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
        uint8_t packet[32];
        kickr_data_t data;
        if (length <= sizeof(packet) && os_mbuf_copydata(event->notify_rx.om, 0, length, packet) == 0 &&
            kickr_decode(packet, length, &data)) {
            portENTER_CRITICAL(&kickr_lock);
            kickr_data = data;
            kickr_updated_at = time(NULL);
            portEXIT_CRITICAL(&kickr_lock);
        }
    }
    return 0;
}

static int govee_gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        portENTER_CRITICAL(&ring_lock);
        bool enabled = ring_enabled;
        ring_connecting = false;
        ring_connected = enabled && event->connect.status == 0;
        ring_stopping = !enabled && event->connect.status == 0;
        ring_conn_handle = event->connect.conn_handle;
        portEXIT_CRITICAL(&ring_lock);
        if (event->connect.status == 0 && !enabled) {
            int rc = ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            if (rc) ESP_LOGW("ring", "Disconnect request failed: %d", rc);
        } else if (event->connect.status == 0) {
            const uint8_t notify[] = {1, 0};
            ESP_LOGI("ring", "Connected to %s", ring_name);
            int rc = ble_gattc_write_flat(ring_conn_handle, RING_NOTIFY_CCCD_HANDLE,
                                          notify, sizeof(notify), ring_subscribed, NULL);
            if (rc) ESP_LOGW("ring", "Notification setup failed: %d", rc);
        } else {
            ESP_LOGW("ring", "Connection failed: %d", event->connect.status);
        }
        ble_scan();
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        portENTER_CRITICAL(&ring_lock);
        ring_connected = false;
        ring_connecting = false;
        ring_stopping = false;
        ring_hr_active = false;
        ring_sync_active = false;
        ring_sync_pending = false;
        portEXIT_CRITICAL(&ring_lock);
        ble_scan();
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_NOTIFY_RX && event->notify_rx.attr_handle == RING_NOTIFY_HANDLE) {
        if (!ring_connection_active(event->notify_rx.conn_handle)) return 0;
        uint8_t packet[16];
        int battery, heart_rate, manual_error;
        bool charging;
        if (OS_MBUF_PKTLEN(event->notify_rx.om) != sizeof(packet) ||
            os_mbuf_copydata(event->notify_rx.om, 0, sizeof(packet), packet) != 0) return 0;
        if (ring_handle_history(packet)) {
            return 0;
        } else if (ring_decode_battery(packet, sizeof(packet), &battery, &charging)) {
            portENTER_CRITICAL(&ring_lock);
            ring_battery = battery;
            ring_charging = charging;
            ring_updated_at = time(NULL);
            portEXIT_CRITICAL(&ring_lock);
            ESP_LOGI("ring", "Battery %d%%, charging=%d", battery, charging);
        } else if (ring_decode_manual_heart_rate(packet, sizeof(packet), &manual_error, &heart_rate)) {
            portENTER_CRITICAL(&ring_lock);
            ring_hr_error = manual_error ? manual_error : heart_rate > 0 ? 0 : 2;
            ring_hr_active = false;
            portEXIT_CRITICAL(&ring_lock);
            if (!manual_error && heart_rate > 0) {
                ring_hr_sample_t sample = {.timestamp = time(NULL), .bpm = heart_rate};
                if (ring_hr_samples) xQueueSend(ring_hr_samples, &sample, 0);
            }
            if (manual_error) ESP_LOGW("ring", "Heart-rate measurement error %d", manual_error);
            else if (heart_rate > 0) ESP_LOGI("ring", "Heart rate %d bpm", heart_rate);
        }
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) return 0;
    ble_tool_observe_advertisement(&event->disc.addr, event->disc.rssi,
                                   fields.name, fields.name_len);
    bool is_kickr = fields.name && fields.name_len >= 5 && !memcmp(fields.name, "KICKR", 5);
    for (uint8_t i = 0; !is_kickr && i < fields.num_uuids16; i++) is_kickr = fields.uuids16[i].value == 0x1826;
    portENTER_CRITICAL(&kickr_lock);
    bool connect_kickr = kickr_enabled && is_kickr && !kickr_connecting && !kickr_connected;
    if (connect_kickr) {
        kickr_found = true;
        kickr_connecting = true;
        if (fields.name && fields.name_len) snprintf(kickr_name, sizeof(kickr_name), "%.*s", fields.name_len, fields.name);
        snprintf(kickr_address, sizeof(kickr_address), "%02X:%02X:%02X:%02X:%02X:%02X",
                 event->disc.addr.val[5], event->disc.addr.val[4], event->disc.addr.val[3],
                 event->disc.addr.val[2], event->disc.addr.val[1], event->disc.addr.val[0]);
    }
    portEXIT_CRITICAL(&kickr_lock);
    if (connect_kickr) {
        uint8_t address_type;
        ble_gap_disc_cancel();
        if (ble_hs_id_infer_auto(0, &address_type) != 0 ||
            ble_gap_connect(address_type, &event->disc.addr, 30000, NULL, kickr_gap_event, NULL) != 0) {
            portENTER_CRITICAL(&kickr_lock);
            kickr_connecting = false;
            portEXIT_CRITICAL(&kickr_lock);
            ble_scan();
        }
        return 0;
    }
    bool is_ring = fields.name && fields.name_len >= 10 && !memcmp(fields.name, "COLMI R12_", 10);
    for (uint8_t i = 0; !is_ring && i < fields.num_uuids16; i++) is_ring = fields.uuids16[i].value == 0xfee7;
    if (!is_ring && fields.svc_data_uuid16_len >= 2)
        is_ring = fields.svc_data_uuid16[0] == 0xe7 && fields.svc_data_uuid16[1] == 0xfe;
    portENTER_CRITICAL(&ring_lock);
    bool connect_ring = ring_should_connect(ring_enabled, is_ring, ring_connecting, ring_connected);
    if (connect_ring) {
        ring_found = true;
        ring_connecting = true;
        ring_rssi = event->disc.rssi;
        if (fields.name && fields.name_len) snprintf(ring_name, sizeof(ring_name), "%.*s", fields.name_len, fields.name);
        snprintf(ring_address, sizeof(ring_address), "%02X:%02X:%02X:%02X:%02X:%02X",
                 event->disc.addr.val[5], event->disc.addr.val[4], event->disc.addr.val[3],
                 event->disc.addr.val[2], event->disc.addr.val[1], event->disc.addr.val[0]);
    }
    portEXIT_CRITICAL(&ring_lock);
    if (connect_ring) {
        uint8_t address_type;
        ble_gap_disc_cancel();
        if (ble_hs_id_infer_auto(0, &address_type) != 0 ||
            ble_gap_connect(address_type, &event->disc.addr, 30000, NULL, govee_gap_event, NULL) != 0) {
            portENTER_CRITICAL(&ring_lock);
            ring_connecting = false;
            portEXIT_CRITICAL(&ring_lock);
            ble_scan();
        }
        return 0;
    }
    portENTER_CRITICAL(&govee_lock);
    bool monitor_govee = govee_enabled;
    portEXIT_CRITICAL(&govee_lock);
    if (!monitor_govee || !fields.mfg_data) return 0;
    govee_reading_t reading;
    if (!govee_decode(fields.mfg_data, fields.mfg_data_len, &reading)) return 0;
    char name[sizeof(govee_name)] = "Govee H5075";
    if (fields.name && fields.name_len) snprintf(name, sizeof(name), "%.*s", fields.name_len, fields.name);
    portENTER_CRITICAL(&govee_lock);
    bool first = !govee_ready;
    govee_reading = reading;
    snprintf(govee_name, sizeof(govee_name), "%s", name);
    snprintf(govee_address, sizeof(govee_address), "%02X:%02X:%02X:%02X:%02X:%02X",
             event->disc.addr.val[5], event->disc.addr.val[4], event->disc.addr.val[3],
             event->disc.addr.val[2], event->disc.addr.val[1], event->disc.addr.val[0]);
    govee_rssi = event->disc.rssi;
    govee_updated_at = time(NULL);
    govee_ready = true;
    portEXIT_CRITICAL(&govee_lock);
    if (first) ESP_LOGI("govee", "Found H5075: %.1f C, %.1f%% RH, battery %u%%",
                        reading.temperature_c, reading.humidity, reading.battery);
    return 0;
}

static void ble_scan(void)
{
    portENTER_CRITICAL(&govee_lock);
    bool govee = govee_enabled;
    portEXIT_CRITICAL(&govee_lock);
    portENTER_CRITICAL(&ring_lock);
    bool ring = ring_enabled;
    portEXIT_CRITICAL(&ring_lock);
    portENTER_CRITICAL(&kickr_lock);
    bool kickr = kickr_enabled;
    portEXIT_CRITICAL(&kickr_lock);
    bool generic = ble_tool_scan_requested();
    if (!ble_should_scan(govee, ring, kickr) && !generic) {
        if (ble_gap_disc_active()) ble_gap_disc_cancel();
        portENTER_CRITICAL(&govee_lock);
        govee_scanning = false;
        portEXIT_CRITICAL(&govee_lock);
        return;
    }
    if (ble_gap_disc_active()) return;
    uint8_t address_type;
    struct ble_gap_disc_params scan = {.passive = 0, .filter_duplicates = 0};
    if (ble_hs_util_ensure_addr(0) == 0 && ble_hs_id_infer_auto(0, &address_type) == 0 &&
        ble_gap_disc(address_type, BLE_HS_FOREVER, &scan, govee_gap_event, NULL) == 0) {
        portENTER_CRITICAL(&govee_lock);
        govee_scanning = true;
        portEXIT_CRITICAL(&govee_lock);
        ESP_LOGI("ble", "Scanning for enabled Bluetooth tools");
    }
}

static void govee_sync(void)
{
    ble_scan();
}

static void govee_host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void govee_start(void)
{
    esp_err_t error = nimble_port_init();
    if (error != ESP_OK) {
        ESP_LOGE("govee", "BLE init failed: %s", esp_err_to_name(error));
        return;
    }
    ble_hs_cfg.sync_cb = govee_sync;
    nimble_port_freertos_init(govee_host_task);
}

static uint32_t servo_duty(uint16_t pulse_us)
{
    return (uint32_t)pulse_us * ((1U << 14) - 1) / 20000;
}

static uint32_t servo_sine_advance(uint32_t phase, uint32_t elapsed_ms, uint8_t frequency_cHz)
{
    /* One cycle is 100000 units: milliseconds times hundredths of a hertz. */
    return (phase + (uint64_t)elapsed_ms * frequency_cHz) % 100000U;
}

static uint16_t servo_sine_pulse(uint32_t phase, uint16_t amplitude_us)
{
    return (uint16_t)lroundf(1500.0f + amplitude_us * sinf(phase * (6.28318530718f / 100000.0f)));
}

static void servo_self_test(void)
{
    assert(SERVO_LEDC_TIMER != LEDC_TIMER_0);
    assert(SERVO_LEDC_CHANNEL != LEDC_CHANNEL_0 && SERVO_LEDC_CHANNEL != LEDC_CHANNEL_1);
    assert(servo_duty(1000) == 819);
    assert(servo_duty(1500) == 1228);
    assert(servo_duty(2000) == 1638);
    assert(servo_sine_pulse(0, 500) == 1500);
    assert(servo_sine_pulse(25000, 500) == 2000);
    assert(servo_sine_pulse(50000, 500) == 1500);
    assert(servo_sine_pulse(75000, 500) == 1000);
    assert(servo_sine_pulse(25000, 250) == 1750);
    assert(servo_sine_pulse(75000, 375) == 1125);
    assert(servo_sine_advance(0, 1000, 25) == 25000);
    assert(servo_sine_advance(0, 4000, 25) == 0);
    assert(servo_sine_advance(0, 20000, 5) == 0);
    assert(servo_sine_advance(0, 1000, 100) == 0);
    assert(servo_sine_advance(servo_sine_advance(0, 137, 25), 863, 25) == 25000);
    assert(servo_sine_advance(25000, 1000, 50) == 75000);
}

static esp_err_t servo_start_pwm(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = SERVO_LEDC_TIMER,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = SERVO_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = SERVO_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = SERVO_LEDC_TIMER,
        .duty = servo_duty(servo_pulse_us),
        .hpoint = 0,
    };
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << TOY_LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&led);
    if (error == ESP_OK) error = gpio_set_level(TOY_LED_PIN, 0);
    if (error == ESP_OK) error = ledc_timer_config(&timer);
    if (error == ESP_OK) error = ledc_channel_config(&channel);
    servo_pwm_ready = error == ESP_OK;
    if (error != ESP_OK) servo_stop();
    return error;
}

static void servo_set_pulse(uint16_t pulse_us)
{
    servo_pulse_us = pulse_us;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_LEDC_CHANNEL, servo_duty(pulse_us));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_LEDC_CHANNEL);
}

static void servo_stop(void)
{
    servo_running = false;
    gpio_set_level(TOY_LED_PIN, 0);
    if (servo_pwm_ready) {
        ledc_stop(LEDC_LOW_SPEED_MODE, SERVO_LEDC_CHANNEL, 0);
        servo_pwm_ready = false;
    }
    const gpio_config_t released = {
        .pin_bit_mask = (1ULL << SERVO_PIN) | (1ULL << TOY_LED_PIN),
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&released));
    if (servo_status) lv_label_set_text(servo_status, "Stopped - outputs off");
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

static bool scope_measure(const uint16_t *samples, size_t count, uint16_t level,
                          uint32_t sample_rate_hz, scope_measurement_t *measurement)
{
    size_t first = 0, last = 0, rising_edges = 0;
    for (size_t i = 1; i < count; i++) {
        if (samples[i - 1] < level && samples[i] >= level) {
            if (!rising_edges) first = i;
            last = i;
            rising_edges++;
        }
    }
    if (rising_edges < 2 || last == first) return false;

    size_t high_samples = 0;
    for (size_t i = first; i < last; i++) high_samples += samples[i] >= level;
    size_t span = last - first;
    measurement->frequency_hz = (uint32_t)(((uint64_t)(rising_edges - 1) * sample_rate_hz + span / 2) / span);
    measurement->duty_permille = (uint16_t)((high_samples * 1000U + span / 2) / span);
    return true;
}

static bool scope_offset_valid(int16_t offset_mv)
{
    for (size_t i = 0; i < sizeof(scope_offset_choices_mv) / sizeof(scope_offset_choices_mv[0]); i++)
        if (scope_offset_choices_mv[i] == offset_mv) return true;
    return false;
}

static bool scope_gain_valid(uint16_t gain_permille)
{
    for (size_t i = 0; i < sizeof(scope_gain_choices_permille) / sizeof(scope_gain_choices_permille[0]); i++)
        if (scope_gain_choices_permille[i] == gain_permille) return true;
    return false;
}

static int16_t scope_next_offset(int16_t offset_mv)
{
    for (size_t i = 0; i < sizeof(scope_offset_choices_mv) / sizeof(scope_offset_choices_mv[0]); i++)
        if (scope_offset_choices_mv[i] == offset_mv)
            return scope_offset_choices_mv[(i + 1) % (sizeof(scope_offset_choices_mv) / sizeof(scope_offset_choices_mv[0]))];
    return 0;
}

static uint16_t scope_next_gain(uint16_t gain_permille)
{
    for (size_t i = 0; i < sizeof(scope_gain_choices_permille) / sizeof(scope_gain_choices_permille[0]); i++)
        if (scope_gain_choices_permille[i] == gain_permille)
            return scope_gain_choices_permille[(i + 1) % (sizeof(scope_gain_choices_permille) / sizeof(scope_gain_choices_permille[0]))];
    return 1000;
}

static uint16_t scope_apply_calibration(int millivolts, uint16_t gain_permille, int16_t offset_mv)
{
    int calibrated = (millivolts * gain_permille + 500) / 1000 + offset_mv;
    return calibrated < 0 ? 0 : calibrated > 3300 ? 3300 : (uint16_t)calibrated;
}

static void scope_calibration_defaults(void)
{
    memset(scope_offsets_mv, 0, sizeof(scope_offsets_mv));
    for (size_t i = 0; i < SCOPE_CHANNEL_COUNT; i++) scope_gains_permille[i] = 1000;
}

static void load_scope_calibration(void)
{
    scope_calibration_defaults();
    if (nvs_init_error != ESP_OK) return;

    nvs_handle_t handle;
    if (nvs_open("tab5", NVS_READONLY, &handle) != ESP_OK) return;
    int16_t offsets[SCOPE_CHANNEL_COUNT];
    uint16_t gains[SCOPE_CHANNEL_COUNT];
    size_t offset_size = sizeof(offsets), gain_size = sizeof(gains);
    bool valid = nvs_get_blob(handle, "scope_offset", offsets, &offset_size) == ESP_OK &&
                 offset_size == sizeof(offsets) &&
                 nvs_get_blob(handle, "scope_gain", gains, &gain_size) == ESP_OK &&
                 gain_size == sizeof(gains);
    for (size_t i = 0; valid && i < SCOPE_CHANNEL_COUNT; i++)
        valid = scope_offset_valid(offsets[i]) && scope_gain_valid(gains[i]);
    if (valid) {
        memcpy(scope_offsets_mv, offsets, sizeof(offsets));
        memcpy(scope_gains_permille, gains, sizeof(gains));
    }
    nvs_close(handle);
}

static esp_err_t save_scope_calibration(void)
{
    if (nvs_init_error != ESP_OK) return nvs_init_error;
    int16_t offsets[SCOPE_CHANNEL_COUNT];
    uint16_t gains[SCOPE_CHANNEL_COUNT];
    portENTER_CRITICAL(&scope_lock);
    memcpy(offsets, scope_offsets_mv, sizeof(offsets));
    memcpy(gains, scope_gains_permille, sizeof(gains));
    portEXIT_CRITICAL(&scope_lock);

    nvs_handle_t handle;
    esp_err_t error = nvs_open("tab5", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, "scope_offset", offsets, sizeof(offsets));
        if (error == ESP_OK) error = nvs_set_blob(handle, "scope_gain", gains, sizeof(gains));
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    return error;
}

static void scope_self_test(void)
{
    uint16_t samples[600] = {0};
    for (size_t i = 200; i < 600; i++) samples[i] = 2000;
    size_t start = 0;
    assert(scope_window_start(samples, 600, 1, 1000, &start) && start == 125);
    uint16_t square[100];
    for (size_t i = 0; i < sizeof(square) / sizeof(square[0]); i++) square[i] = i % 10 < 3 ? 2000 : 0;
    scope_measurement_t measurement;
    assert(scope_measure(square, sizeof(square) / sizeof(square[0]), 1000, 1000, &measurement));
    assert(measurement.frequency_hz == 100 && measurement.duty_permille == 300);
    assert(!scope_measure(samples, 100, 1000, 1000, &measurement));
    assert(scope_apply_calibration(1000, 1050, -50) == 1000);
    assert(scope_apply_calibration(50, 900, -200) == 0);
    assert(scope_apply_calibration(3200, 1100, 200) == 3300);
    assert(scope_next_offset(200) == -200 && scope_next_gain(1100) == 900);
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
        portENTER_CRITICAL(&scope_lock);
        bool should_sample = scope_active && scope_running;
        if (should_sample) scope_sampling = true;
        portEXIT_CRITICAL(&scope_lock);
        if (!should_sample) {
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
            portENTER_CRITICAL(&scope_lock);
            scope_sampling = false;
            portEXIT_CRITICAL(&scope_lock);
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
            int16_t offset_mv;
            uint16_t gain_permille;
            portENTER_CRITICAL(&scope_lock);
            offset_mv = scope_offsets_mv[channel_index];
            gain_permille = scope_gains_permille[channel_index];
            portEXIT_CRITICAL(&scope_lock);
            size_t count = 0;
            for (size_t i = 0; i < bytes_read; i += SOC_ADC_DIGI_RESULT_BYTES) {
                adc_digi_output_data_t *sample = (adc_digi_output_data_t *)&bytes[i];
                if (sample->type2.unit != input->unit || sample->type2.channel != input->channel) continue;
                int mv = sample->type2.data * 3300 / 4095;
                if (calibration) adc_cali_raw_to_voltage(calibration, sample->type2.data, &mv);
                millivolts[count++] = scope_apply_calibration(mv, gain_permille, offset_mv);
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
        portENTER_CRITICAL(&scope_lock);
        scope_sampling = false;
        portEXIT_CRITICAL(&scope_lock);
    }
}

static void scope_release(void)
{
    portENTER_CRITICAL(&scope_lock);
    scope_active = false;
    portEXIT_CRITICAL(&scope_lock);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(250);
    bool warned = false;
    while (scope_sampling) {
        if (!warned && (int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGW("scope", "Waiting for ADC teardown before app switch");
            warned = true;
        }
        vTaskDelay(1);
    }
}

static void scope_update_controls(void)
{
    const scope_channel_t *input = &scope_channels[scope_channel_index];
    int16_t offset_mv;
    uint16_t gain_permille;
    portENTER_CRITICAL(&scope_lock);
    offset_mv = scope_offsets_mv[scope_channel_index];
    gain_permille = scope_gains_permille[scope_channel_index];
    portEXIT_CRITICAL(&scope_lock);
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
    if (scope_offset_label) lv_label_set_text_fmt(scope_offset_label, "Offset\n%d mV", (int)offset_mv);
    if (scope_gain_label)
        lv_label_set_text_fmt(scope_gain_label, "Scale\n%u.%u%%",
                              (unsigned)(gain_permille / 10), (unsigned)(gain_permille % 10));
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
    scope_measurement_t measurement;
    bool measured = scope_measure(scope_snapshot, count, scope_trigger_mv,
                                  scope_sample_rates[scope_rate_index], &measurement);
    uint16_t now = scope_chart_points[SCOPE_CHART_POINTS - 1];
    uint16_t average = sum / SCOPE_CHART_POINTS;
    uint16_t peak_to_peak = maximum - minimum;
    char timing[64];
    if (measured)
        snprintf(timing, sizeof(timing), "Freq %lu Hz   Duty %u.%u%%",
                 (unsigned long)measurement.frequency_hz,
                 measurement.duty_permille / 10, measurement.duty_permille % 10);
    else
        snprintf(timing, sizeof(timing), "Freq --   Duty --");
    lv_label_set_text_fmt(scope_stats,
                          "Now %u.%03u V   Min %u.%03u   Max %u.%03u   Vpp %u.%03u\n"
                          "Avg %u.%03u V   %s   %lu kS/s",
                          now / 1000, now % 1000, minimum / 1000, minimum % 1000,
                          maximum / 1000, maximum % 1000, peak_to_peak / 1000, peak_to_peak % 1000,
                          average / 1000, average % 1000, timing,
                          (unsigned long)(scope_sample_rates[scope_rate_index] / 1000));
    scope_chart_ready = true;
    lv_chart_refresh(scope_chart);
}

static void scope_channel_clicked(lv_event_t *event)
{
    (void)event;
    scope_channel_index = (scope_channel_index + 1) % SCOPE_CHANNEL_COUNT;
    scope_chart_ready = false;
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
    scope_chart_ready = false;
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

static void scope_calibration_saved(void)
{
    portENTER_CRITICAL(&scope_lock);
    scope_ring_head = scope_ring_count = 0;
    portEXIT_CRITICAL(&scope_lock);
    scope_chart_ready = false;
    esp_err_t error = save_scope_calibration();
    const scope_channel_t *input = &scope_channels[scope_channel_index];
    if (error == ESP_OK)
        snprintf(scope_capture_notice, sizeof(scope_capture_notice), "G%d calibration saved", input->pin);
    else
        snprintf(scope_capture_notice, sizeof(scope_capture_notice), "Calibration is temporary: %s",
                 esp_err_to_name(error));
    if (scope_capture_status) lv_label_set_text(scope_capture_status, scope_capture_notice);
    scope_update_controls();
}

static void scope_offset_clicked(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&scope_lock);
    scope_offsets_mv[scope_channel_index] = scope_next_offset(scope_offsets_mv[scope_channel_index]);
    portEXIT_CRITICAL(&scope_lock);
    scope_calibration_saved();
}

static void scope_gain_clicked(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&scope_lock);
    scope_gains_permille[scope_channel_index] = scope_next_gain(scope_gains_permille[scope_channel_index]);
    portEXIT_CRITICAL(&scope_lock);
    scope_calibration_saved();
}

static void scope_calibration_reset_clicked(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&scope_lock);
    scope_offsets_mv[scope_channel_index] = 0;
    scope_gains_permille[scope_channel_index] = 1000;
    portEXIT_CRITICAL(&scope_lock);
    scope_calibration_saved();
}

static void scope_capture_clicked(lv_event_t *event)
{
    (void)event;
    if (!scope_chart_ready) {
        lv_label_set_text(scope_capture_status, "Wait for a complete chart before saving");
        return;
    }
    if (!sd_ready) {
        int error = sd_error_snapshot();
        lv_label_set_text_fmt(scope_capture_status, "SD unavailable: %s", strerror(error ? error : ENODEV));
        return;
    }
    if (mkdir(SCOPE_PATH, 0775) != 0 && errno != EEXIST) {
        int error = errno ? errno : EIO;
        sd_record_error(error);
        lv_label_set_text_fmt(scope_capture_status, "Could not create Scope folder: %s", strerror(error));
        return;
    }

    time_t captured_at = time(NULL);
    struct tm local;
    char date[7], clock[7], directory[64], temporary_path[96] = "", final_path[96] = "";
    localtime_r(&captured_at, &local);
    strftime(date, sizeof(date), "%y%m%d", &local);
    strftime(clock, sizeof(clock), "%H%M%S", &local);
    snprintf(directory, sizeof(directory), SCOPE_PATH "/%s", date);
    if (mkdir(directory, 0775) != 0 && errno != EEXIST) {
        int error = errno ? errno : EIO;
        sd_record_error(error);
        lv_label_set_text_fmt(scope_capture_status, "Could not create capture folder: %s", strerror(error));
        return;
    }

    FILE *file = NULL;
    int create_error = EEXIST;
    for (unsigned suffix = 0; suffix < 100 && !file; suffix++) {
        char stem[9];
        snprintf(stem, sizeof(stem), "%s%02u", clock, suffix);
        snprintf(temporary_path, sizeof(temporary_path), "%s/%s.TMP", directory, stem);
        snprintf(final_path, sizeof(final_path), "%s/%s.CSV", directory, stem);
        struct stat info;
        if (stat(final_path, &info) == 0) continue;
        if (errno != ENOENT) {
            create_error = errno ? errno : EIO;
            break;
        }
        int descriptor = open(temporary_path, O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (descriptor < 0) {
            if (errno == EEXIST) continue;
            create_error = errno ? errno : EIO;
            break;
        }
        file = fdopen(descriptor, "wb");
        if (!file) {
            create_error = errno ? errno : EIO;
            close(descriptor);
            remove(temporary_path);
        }
    }
    if (!file) {
        sd_record_error(create_error);
        lv_label_set_text_fmt(scope_capture_status, "Could not create capture: %s", strerror(create_error));
        return;
    }

    uint8_t channel_index = scope_channel_index;
    uint8_t rate_index = scope_rate_index;
    int16_t offset_mv;
    uint16_t gain_permille;
    portENTER_CRITICAL(&scope_lock);
    offset_mv = scope_offsets_mv[channel_index];
    gain_permille = scope_gains_permille[channel_index];
    portEXIT_CRITICAL(&scope_lock);
    uint32_t rate = scope_sample_rates[rate_index];
    bool write_ok = fputs("unix_time,elapsed_us,gpio,millivolts,sample_rate_hz,offset_mv,scale_permille\n", file) >= 0;
    for (size_t i = 0; write_ok && i < SCOPE_CHART_POINTS; i++) {
        uint32_t elapsed_us = (uint32_t)((uint64_t)i * 1000000U / rate);
        write_ok = fprintf(file, "%lld,%lu,%d,%ld,%lu,%d,%u\n",
                           (long long)captured_at, (unsigned long)elapsed_us,
                           (int)scope_channels[channel_index].pin, (long)scope_chart_points[i],
                           (unsigned long)rate, (int)offset_mv, (unsigned)gain_permille) >= 0;
    }

    bool saved = write_ok && storage_commit_new_file(&file, temporary_path, final_path) == 0;
    if (!saved && file) {
        errno = errno ? errno : EIO;
        storage_commit_new_file(&file, temporary_path, final_path);
    }
    if (saved) {
        const char *name = strrchr(final_path, '/');
        snprintf(scope_capture_notice, sizeof(scope_capture_notice), "Saved %s",
                 name ? name + 1 : "Scope CSV");
    } else {
        int error = errno ? errno : EIO;
        sd_record_error(error);
        struct stat info;
        bool retained = stat(temporary_path, &info) == 0;
        const char *name = strrchr(temporary_path, '/');
        if (retained)
            snprintf(scope_capture_notice, sizeof(scope_capture_notice),
                     "Capture not published; %s retained (%s)",
                     name ? name + 1 : "TMP", strerror(error));
        else
            snprintf(scope_capture_notice, sizeof(scope_capture_notice),
                     "Capture failed: %s", strerror(error));
    }
    lv_label_set_text(scope_capture_status, scope_capture_notice);
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

static bool display_brightness_valid(uint8_t value)
{
    for (size_t i = 0; i < sizeof(display_brightness_choices) / sizeof(display_brightness_choices[0]); i++)
        if (display_brightness_choices[i] == value) return true;
    return false;
}

static bool display_timeout_valid(uint16_t value)
{
    for (size_t i = 0; i < sizeof(screen_timeout_choices) / sizeof(screen_timeout_choices[0]); i++)
        if (screen_timeout_choices[i] == value) return true;
    return false;
}

static uint8_t display_next_brightness(uint8_t value)
{
    for (size_t i = 0; i < sizeof(display_brightness_choices) / sizeof(display_brightness_choices[0]); i++)
        if (display_brightness_choices[i] == value)
            return display_brightness_choices[(i + 1) % (sizeof(display_brightness_choices) /
                                                         sizeof(display_brightness_choices[0]))];
    return display_brightness_choices[0];
}

static uint16_t display_next_timeout(uint16_t value)
{
    for (size_t i = 0; i < sizeof(screen_timeout_choices) / sizeof(screen_timeout_choices[0]); i++)
        if (screen_timeout_choices[i] == value)
            return screen_timeout_choices[(i + 1) % (sizeof(screen_timeout_choices) /
                                                      sizeof(screen_timeout_choices[0]))];
    return screen_timeout_choices[0];
}

static bool display_timeout_elapsed(uint32_t inactive_ms, uint16_t timeout_seconds, bool inhibited)
{
    return !inhibited && timeout_seconds != 0 && inactive_ms >= (uint32_t)timeout_seconds * 1000U;
}

static bool display_set_power_state(display_power_state_t state)
{
    int brightness = state == DISPLAY_AWAKE ? display_brightness :
                     state == DISPLAY_DIMMED ? (display_brightness < 20 ? display_brightness : 20) : 0;
    esp_err_t error = bsp_display_brightness_set(brightness);
    if (error == ESP_OK) {
        display_power_state = state;
        return true;
    }
    ESP_LOGE("display", "Could not set backlight to %d%%: %s", brightness, esp_err_to_name(error));
    return false;
}

static void load_display_settings(void)
{
    if (nvs_init_error != ESP_OK) return;
    nvs_handle_t handle;
    if (nvs_open("tab5", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t brightness;
    uint16_t timeout;
    if (nvs_get_u8(handle, "brightness", &brightness) == ESP_OK && display_brightness_valid(brightness))
        display_brightness = brightness;
    if (nvs_get_u16(handle, "screen_timeout", &timeout) == ESP_OK && display_timeout_valid(timeout))
        screen_timeout_seconds = timeout;
    nvs_close(handle);
}

static void save_display_settings(void)
{
    if (nvs_init_error != ESP_OK) return;
    nvs_handle_t handle;
    esp_err_t error = nvs_open("tab5", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_u8(handle, "brightness", display_brightness);
        if (error == ESP_OK) error = nvs_set_u16(handle, "screen_timeout", screen_timeout_seconds);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error != ESP_OK) ESP_LOGW("display", "Could not save display settings: %s", esp_err_to_name(error));
}

static void display_self_test(void)
{
    assert(display_brightness_valid(100) && display_brightness_valid(25));
    assert(!display_brightness_valid(0) && !display_brightness_valid(74));
    assert(display_timeout_valid(0) && display_timeout_valid(300));
    assert(!display_timeout_valid(301));
    assert(display_next_brightness(25) == 100);
    assert(display_next_timeout(0) == 300);
    assert(!display_timeout_elapsed(299999, 300, false));
    assert(display_timeout_elapsed(300000, 300, false));
    assert(!display_timeout_elapsed(UINT32_MAX, 0, false));
    assert(!display_timeout_elapsed(UINT32_MAX, 300, true));
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
    lv_display_trigger_activity(NULL);
    screensaver_close();
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
    if (ota_busy) return;
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
    nvs_init_error = nvs_flash_init();
    if (nvs_init_error != ESP_OK) {
        ESP_LOGE("tab5-os", "NVS unavailable; settings preserved: %s", esp_err_to_name(nvs_init_error));
        return false;
    }
    if (esp_netif_init() != ESP_OK ||
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
    esp_err_t error = esp_wifi_start();
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
    uint8_t *pixels = NULL;
    uint8_t *encoded = NULL;
    uint8_t *framebuffer = NULL;
    esp_err_t frame_error = esp_lcd_dpi_panel_get_frame_buffer(
        bsp_display_get_panel_handle(), 1, (void **)&framebuffer);
    if (frame_error != ESP_OK) {
        ESP_LOGE("tab5-os", "Remote desktop framebuffer unavailable");
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
            if (!pixels) {
                pixels = heap_caps_malloc(pixel_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                encoded = heap_caps_malloc(pixel_bytes * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!pixels || !encoded) {
                    heap_caps_free(pixels);
                    heap_caps_free(encoded);
                    pixels = encoded = NULL;
                    ESP_LOGE("tab5-os", "Remote desktop buffer allocation failed");
                    continue;
                }
            }
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

static bool bytes_are_erased(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; i++) if (data[i] != 0xff) return false;
    return true;
}

static bool storage_partition_is_erased(void)
{
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                 ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!partition) return false;
    uint8_t data[256];
    for (size_t offset = 0; offset < partition->size; offset += sizeof(data)) {
        size_t length = partition->size - offset < sizeof(data) ? partition->size - offset : sizeof(data);
        if (esp_partition_read(partition, offset, data, length) != ESP_OK || !bytes_are_erased(data, length))
            return false;
    }
    return true;
}

static bool mount_internal(void)
{
    esp_vfs_spiffs_conf_t config = {
        .base_path = INTERNAL_PATH,
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    storage_init_error = esp_vfs_spiffs_register(&config);
    if (storage_init_error != ESP_OK && storage_partition_is_erased()) {
        ESP_LOGI("tab5-os", "Initializing blank internal storage");
        config.format_if_mount_failed = true;
        storage_init_error = esp_vfs_spiffs_register(&config);
    }
    if (storage_init_error != ESP_OK)
        ESP_LOGW("tab5-os", "Internal storage unavailable: %s", esp_err_to_name(storage_init_error));
    return storage_init_error == ESP_OK;
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
                     uint32_t color, lv_event_cb_t callback, void *user_data,
                     int column, int row)
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
    lv_obj_add_event_cb(tile, callback, LV_EVENT_CLICKED, user_data);
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
    if (link[0] == '?') {
        const char *end = strpbrk(base, "?#");
        size_t prefix = (end ? end : base + strlen(base)) - base;
        size_t length = strlen(link);
        if (prefix + length >= capacity) return false;
        memcpy(output, base, prefix);
        memcpy(output + prefix, link, length + 1);
        return true;
    }
    if (link[0] == '/' && link[1] == '/') {
        size_t prefix = scheme_end - base;
        size_t length = strlen(link);
        if (prefix + 1 + length >= capacity) return false;
        memcpy(output, base, prefix);
        output[prefix] = ':';
        memcpy(output + prefix + 1, link, length + 1);
        return true;
    }
    const char *host_end = strpbrk(scheme_end + 3, "/?#");
    if (!host_end) host_end = base + strlen(base);
    if (link[0] == '/') {
        size_t prefix = host_end - base;
        size_t length = strlen(link);
        if (prefix + length >= capacity) return false;
        memcpy(output, base, prefix);
        memcpy(output + prefix, link, length + 1);
        return true;
    }
    const char *base_end = strpbrk(host_end, "?#");
    if (!base_end) base_end = base + strlen(base);
    const char *path_end = host_end;
    for (const char *p = host_end; p < base_end; p++) if (*p == '/') path_end = p;
    size_t root = host_end - base + 1;
    size_t used = path_end - base + 1;
    if (used >= capacity) return false;
    if (host_end == base_end) {
        memcpy(output, base, used - 1);
        output[used - 1] = '/';
    } else {
        memcpy(output, base, used);
    }
    while (browser_prefix(link, "./")) link += 2;
    while (browser_prefix(link, "../")) {
        link += 3;
        if (used > root) {
            used--;
            while (used > root && output[used - 1] != '/') used--;
        }
    }
    size_t length = strlen(link);
    if (used + length >= capacity) return false;
    memcpy(output + used, link, length + 1);
    return true;
}

static void browser_self_test(void)
{
    char url[128];
    assert(browser_resolve_url("https://example.com/a/page.html", "?p=2", url, sizeof(url)) &&
           strcmp(url, "https://example.com/a/page.html?p=2") == 0);
    assert(browser_resolve_url("https://example.com/a/page.html", "../next", url, sizeof(url)) &&
           strcmp(url, "https://example.com/next") == 0);
    assert(browser_resolve_url("https://example.com/a/page.html", "./next", url, sizeof(url)) &&
           strcmp(url, "https://example.com/a/next") == 0);
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
    void (*leave)(void) = active_app_leave;
    active_app_leave = NULL;
    if (leave) leave();
    capture_viewer_stop();
    serial_log_viewer_stop();
    signal_tool_stop();
    spi_tool_stop();
    uart_tool_stop();
    ender3_tool_stop();
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
    if (i2c_timer) {
        lv_timer_delete(i2c_timer);
        i2c_timer = NULL;
    }
    i2c_capture_stop();
    i2c_watch_enabled = false;
    i2c_write_armed = false;
    i2c_write_armed_at_ms = 0;
    if (scope_timer) {
        lv_timer_delete(scope_timer);
        scope_timer = NULL;
    }
    if (weather_timer) {
        lv_timer_delete(weather_timer);
        weather_timer = NULL;
    }
    if (govee_timer) {
        lv_timer_delete(govee_timer);
        govee_timer = NULL;
    }
    if (ring_timer) {
        lv_timer_delete(ring_timer);
        ring_timer = NULL;
    }
    if (servo_timer) {
        lv_timer_delete(servo_timer);
        servo_timer = NULL;
    }
    servo_stop();
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
    i2c_status = NULL;
    i2c_devices = NULL;
    i2c_address_label = NULL;
    i2c_register_label = NULL;
    i2c_read_result = NULL;
    i2c_speed_label = NULL;
    i2c_watch_label = NULL;
    i2c_value_label = NULL;
    i2c_write_label = NULL;
    i2c_capture_label = NULL;
    i2c_capture_status = NULL;
    battery_metrics = NULL;
    battery_chart = NULL;
    battery_series = NULL;
    storage_status = NULL;
    storage_format_label = NULL;
    storage_format_armed = false;
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
    scope_offset_label = NULL;
    scope_gain_label = NULL;
    scope_capture_status = NULL;
    weather_status = NULL;
    weather_location_area = NULL;
    weather_body = NULL;
    weather_keyboard = NULL;
    weather_keys_label = NULL;
    govee_status = NULL;
    govee_toggle_label = NULL;
    govee_temperature = NULL;
    govee_humidity = NULL;
    govee_details = NULL;
    ring_status = NULL;
    ring_toggle_label = NULL;
    ring_battery_label = NULL;
    ring_hr_label = NULL;
    ring_hr_status = NULL;
    ring_hr_button_label = NULL;
    ring_hr_chart = NULL;
    ring_hr_series = NULL;
    ring_details = NULL;
    ride_status = NULL;
    ride_toggle_label = NULL;
    ride_power_label = NULL;
    ride_cadence_label = NULL;
    ride_hr_label = NULL;
    ride_stats = NULL;
    ride_button_label = NULL;
    ride_history = NULL;
    ride_chart = NULL;
    ride_power_series = NULL;
    ride_hr_series = NULL;
    servo_status = NULL;
    servo_position = NULL;
    servo_mode_label = NULL;
    servo_range_label = NULL;
    servo_rate_label = NULL;
    lv_obj_clean(content);
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

static void home_clicked(lv_event_t *event)
{
    (void)event;
    if (ota_busy) return;
    show_launcher();
}

static void file_back_clicked(lv_event_t *event)
{
    (void)event;
    char directory[sizeof(current_directory)];
    snprintf(directory, sizeof(directory), "%s", current_directory);
    show_files(directory);
}

static void open_file(const char *path)
{
    clear_content();
    lv_obj_t *back = button(content, "Back to files", file_back_clicked);
    lv_obj_set_size(back, 640, 68);
    if (capture_viewer_show(content, path)) return;
    if (serial_log_viewer_show(content, path)) return;
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
    lv_textarea_set_cursor_pos(viewer, 0);
    lv_obj_scroll_to_y(viewer, 0, LV_ANIM_OFF);
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
    if (remove(temporary) != 0 && errno != ENOENT) {
        int remove_error = errno ? errno : EIO;
        sd_record_error(remove_error);
        ESP_LOGE("tab5-os", "Could not clear %s: %s", temporary, strerror(remove_error));
        return false;
    }
    FILE *file = fopen(temporary, "wb");
    if (!file) {
        int open_error = errno ? errno : EIO;
        sd_record_error(open_error);
        ESP_LOGE("tab5-os", "Could not create %s: %s", temporary, strerror(open_error));
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
    bool downloaded = error == ESP_OK && status >= 200 && status < 300;
    bool saved = false;
    if (downloaded) {
        saved = storage_commit_new_file(&file, temporary, path) == 0;
        if (!saved) sd_record_error(errno ? errno : EIO);
    } else {
        int stream_error = ferror(file) ? (errno ? errno : EIO) : 0;
        if (fclose(file) != 0 && !stream_error) stream_error = errno ? errno : EIO;
        file = NULL;
        remove(temporary);
        if (stream_error) sd_record_error(stream_error);
    }
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
        const char *temporary = SD_PATH "/BOOKS/WELCOME.TMP";
        const char *final = SD_PATH "/BOOKS/WELCOME.TXT";
        FILE *welcome = fopen(temporary, "wb");
        if (welcome) {
            if (fputs("Welcome to Tab5 Books!\n\nCopy .txt ebooks into the BOOKS folder on the SD card. Use Next and Prev to move through the book, and Text to change the reading size.\n", welcome) < 0) {
                int error = errno ? errno : EIO;
                fclose(welcome);
                remove(temporary);
                sd_record_error(error);
            } else if (storage_commit_new_file(&welcome, temporary, final) != 0) {
                sd_record_error(errno ? errno : EIO);
            }
        } else {
            sd_record_error(errno ? errno : EIO);
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
    mkdir(SD_PATH "/DOCS", 0775);
    const char *temporary_path = SD_PATH "/DOCS/NOTE.TMP";
    const char *final_path = SD_PATH "/DOCS/NOTE.TXT";
    const char *backup_path = SD_PATH "/DOCS/NOTE.BAK";
    remove(temporary_path);
    FILE *file = fopen(temporary_path, "wb");
    if (!file) {
        int error = errno ? errno : EIO;
        sd_record_error(error);
        lv_label_set_text_fmt(status, "Save failed: %s", strerror(error));
        return;
    }
    if (fputs(lv_textarea_get_text(note_area), file) < 0) {
        int write_error = errno ? errno : EIO;
        fclose(file);
        file = NULL;
        remove(temporary_path);
        sd_record_error(write_error);
        lv_label_set_text_fmt(status, "Save failed: %s", strerror(write_error));
        return;
    }
    if (storage_commit_replace_file(&file, temporary_path, final_path, backup_path) != 0) {
        int save_error = errno;
        sd_record_error(save_error);
        struct stat info;
        if (stat(temporary_path, &info) == 0)
            lv_label_set_text(status, "Save not published; NOTE.TMP was retained");
        else
            lv_label_set_text_fmt(status, "Save failed: %s", strerror(save_error));
        return;
    }
    lv_label_set_text(status, "Saved safely to /sdcard/DOCS/NOTE.TXT");
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
    storage_recover_replace(SD_PATH "/DOCS/NOTE.TXT", SD_PATH "/DOCS/NOTE.BAK");
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
    active_app_leave = settings_leave;
}

static bool wifi_forget_confirmation_valid(bool armed, uint32_t armed_at, uint32_t now)
{
    return armed && (uint32_t)(now - armed_at) <= 5000U;
}

static void wifi_forget_clicked(lv_event_t *event)
{
    (void)event;
    if (!wifi_ready || !wifi_ssid[0]) return;
    uint32_t now = lv_tick_get();
    if (!wifi_forget_confirmation_valid(wifi_forget_armed, wifi_forget_armed_at, now)) {
        wifi_forget_armed = true;
        wifi_forget_armed_at = now;
        lv_label_set_text(wifi_status, "Tap Forget again within 5 seconds to erase the saved network");
        return;
    }
    wifi_forget_armed = false;
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
    if (wifi_forget_confirmation_valid(wifi_forget_armed, wifi_forget_armed_at, lv_tick_get())) {
        lv_label_set_text(wifi_status, "Tap Forget again within 5 seconds to erase the saved network");
    } else if (!wifi_ready) lv_label_set_text(wifi_status, "Wi-Fi hardware unavailable");
    else if (wifi_connected) lv_label_set_text_fmt(wifi_status, "Connected: %s\nIP: %s", wifi_ssid, wifi_ip);
    else if (wifi_connecting) lv_label_set_text_fmt(wifi_status, "Connecting to %s...", wifi_ssid);
    else if (wifi_ssid[0]) lv_label_set_text_fmt(wifi_status, "Not connected: %s", wifi_ssid);
    else lv_label_set_text(wifi_status, "Not connected");
    if (wifi_forget_armed &&
        !wifi_forget_confirmation_valid(true, wifi_forget_armed_at, lv_tick_get()))
        wifi_forget_armed = false;

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
        snprintf(label, sizeof(label), "%s   ch %u   %d dBm%s", wifi_aps[i].ssid,
                 (unsigned)wifi_aps[i].primary, wifi_aps[i].rssi,
                 wifi_aps[i].authmode == WIFI_AUTH_OPEN ? "" : "   locked");
        lv_obj_t *network = lv_list_add_button(wifi_list, LV_SYMBOL_WIFI, label);
        lv_obj_add_event_cb(network, wifi_network_clicked, LV_EVENT_CLICKED, &wifi_aps[i]);
    }
}

static void display_timeout_label_update(lv_obj_t *label)
{
    if (screen_timeout_seconds == 0) lv_label_set_text(label, "Screen off\nNever");
    else lv_label_set_text_fmt(label, "Screen off\n%u min", screen_timeout_seconds / 60);
}

static void display_brightness_clicked(lv_event_t *event)
{
    display_brightness = display_next_brightness(display_brightness);
    lv_display_trigger_activity(NULL);
    display_set_power_state(DISPLAY_AWAKE);
    lv_label_set_text_fmt(lv_obj_get_child(lv_event_get_target(event), 0),
                          "Brightness\n%u%%", display_brightness);
    save_display_settings();
}

static void screen_timeout_clicked(lv_event_t *event)
{
    screen_timeout_seconds = display_next_timeout(screen_timeout_seconds);
    lv_display_trigger_activity(NULL);
    display_set_power_state(DISPLAY_AWAKE);
    display_timeout_label_update(lv_obj_get_child(lv_event_get_target(event), 0));
    save_display_settings();
}

static void settings_leave(void)
{
    wifi_forget_armed = false;
    if (wifi_timer) {
        lv_timer_delete(wifi_timer);
        wifi_timer = NULL;
    }
    network_tool_stop();
}

static void network_tools_clicked(lv_event_t *event)
{
    (void)event;
    clear_content();
    network_tool_show(content, wifi_connected);
    active_app_leave = settings_leave;
}

static void show_settings(void)
{
    clear_content();
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *display_row = lv_obj_create(content);
    lv_obj_set_size(display_row, 640, 100);
    lv_obj_clear_flag(display_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(display_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(display_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *brightness = button(display_row, "", display_brightness_clicked);
    lv_obj_set_size(brightness, 290, 82);
    lv_label_set_text_fmt(lv_obj_get_child(brightness, 0), "Brightness\n%u%%", display_brightness);
    lv_obj_t *timeout = button(display_row, "", screen_timeout_clicked);
    lv_obj_set_size(timeout, 290, 82);
    display_timeout_label_update(lv_obj_get_child(timeout, 0));

    wifi_status = lv_label_create(content);
    lv_obj_set_size(wifi_status, 640, 75);

    lv_obj_t *actions = lv_obj_create(content);
    lv_obj_set_size(actions, 640, 100);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *scan = button(actions, "Scan", wifi_scan_clicked);
    lv_obj_t *forget = button(actions, "Forget", wifi_forget_clicked);
    lv_obj_t *tools = button(actions, "Tools", network_tools_clicked);
    lv_obj_set_size(scan, 180, 82);
    lv_obj_set_size(forget, 180, 82);
    lv_obj_set_size(tools, 180, 82);

    wifi_list = lv_list_create(content);
    lv_obj_set_size(wifi_list, 640, 680);
    lv_list_add_text(wifi_list, wifi_ready ? "Tap Scan to find networks" : "Wi-Fi hardware unavailable");
    if (!wifi_ready) {
        lv_obj_add_state(scan, LV_STATE_DISABLED);
        lv_obj_add_state(forget, LV_STATE_DISABLED);
        lv_obj_add_state(tools, LV_STATE_DISABLED);
    }
    if (!wifi_ssid[0]) lv_obj_add_state(forget, LV_STATE_DISABLED);
    wifi_timer = lv_timer_create(wifi_tick, 250, NULL);
    wifi_tick(wifi_timer);
    active_app_leave = settings_leave;
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

static void ota_record_pending(const char *version)
{
    if (nvs_init_error != ESP_OK) return;
    nvs_handle_t handle;
    esp_err_t error = nvs_open("tab5", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_str(handle, "ota_pending", version);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error != ESP_OK)
        ESP_LOGW("tab5-os", "Could not record pending OTA version: %s", esp_err_to_name(error));
}

static void ota_record_result(const char *result)
{
    snprintf(ota_last_result, sizeof(ota_last_result), "%s", result);
    if (nvs_init_error != ESP_OK) return;
    nvs_handle_t handle;
    esp_err_t error = nvs_open("tab5", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_str(handle, "ota_result", ota_last_result);
        if (error == ESP_OK) {
            esp_err_t erase_error = nvs_erase_key(handle, "ota_pending");
            if (erase_error != ESP_OK && erase_error != ESP_ERR_NVS_NOT_FOUND) error = erase_error;
        }
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error != ESP_OK)
        ESP_LOGW("tab5-os", "Could not record OTA result: %s", esp_err_to_name(error));
}

static void ota_load_result(void)
{
    if (nvs_init_error != ESP_OK) return;
    nvs_handle_t handle;
    if (nvs_open("tab5", NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = sizeof(ota_last_result);
    nvs_get_str(handle, "ota_result", ota_last_result, &size);
    char pending[32] = "";
    size = sizeof(pending);
    esp_err_t pending_error = nvs_get_str(handle, "ota_pending", pending, &size);
    nvs_close(handle);
    if (pending_error != ESP_OK || !pending[0]) return;

    const esp_app_desc_t *running_description = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    bool running_pending = strcmp(running_description->version, pending) == 0 &&
                           esp_ota_get_state_partition(running, &state) == ESP_OK &&
                           (state == ESP_OTA_IMG_NEW || state == ESP_OTA_IMG_PENDING_VERIFY);
    if (running_pending) {
        snprintf(ota_last_result, sizeof(ota_last_result), "Installing %s; health check pending", pending);
        return;
    }

    char result[64];
    if (strcmp(running_description->version, pending) == 0) {
        snprintf(result, sizeof(result), "Installed %s", pending);
    } else {
        const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
        esp_app_desc_t invalid_description;
        bool rolled_back = invalid &&
                           esp_ota_get_partition_description(invalid, &invalid_description) == ESP_OK &&
                           strcmp(invalid_description.version, pending) == 0;
        snprintf(result, sizeof(result), rolled_back ? "Rolled back %s" : "Update to %s did not activate", pending);
    }
    ota_record_result(result);
}

static const char *restart_blocker(void)
{
    if (ebook_download_busy) return "Wait for book downloads to finish";
    if (ride_recording || ride_file) return "Stop and save the active ride first";
    if (i2c_capture_file) return "Stop and save the I2C capture first";
    if (signal_tool_busy()) return "Stop the PWM output first";
    if (spi_tool_busy()) return "Stop the SPI interface first";
    if (uart_tool_busy()) return "Stop and save the serial session first";
    if (ender3_tool_busy()) return "Stop the Ender 3 connection first";
    if (voice_recording || voice_mic_open) return "Finish the voice recording first";
    if (scope_sampling) return "Wait for the scope to release its input";
    if (servo_running) return "Stop the Servo Toy output first";
    if (weather_busy) return "Wait for weather settings to finish saving";
    if (chat_busy || browser_busy || wifi_scan_busy || network_tool_busy() ||
        http_tool_busy() || mqtt_tool_busy())
        return "Wait for the active network task to finish";
    if (ble_tool_busy()) return "Disconnect the BLE GATT Explorer first";
    if (alarm_active) return "Dismiss the active alarm first";
    if (ring_hr_samples && uxQueueMessagesWaiting(ring_hr_samples)) return "Wait for ring data to finish saving";

    portENTER_CRITICAL(&govee_lock);
    bool govee = govee_enabled;
    portEXIT_CRITICAL(&govee_lock);
    if (govee) return "Turn Govee Bluetooth off first";

    portENTER_CRITICAL(&ring_lock);
    bool ring = ring_enabled || ring_connecting || ring_connected || ring_stopping || ring_hr_active ||
                ring_sync_active || ring_sync_pending;
    portEXIT_CRITICAL(&ring_lock);
    if (ring) return "Turn Ring Bluetooth off first";

    portENTER_CRITICAL(&kickr_lock);
    bool kickr = kickr_enabled || kickr_connecting || kickr_connected || kickr_stopping;
    portEXIT_CRITICAL(&kickr_lock);
    if (kickr) return "Turn KICKR Bluetooth off first";
    return NULL;
}

static void ota_update_task(void *argument)
{
    (void)argument;
    for (;;) {
        ota_manifest_t manifest;
        ota_error[0] = '\0';
        ESP_LOGI("tab5-os", "OTA manifest check starting");
        esp_err_t error = ota_manifest_fetch(OTA_MANIFEST_URL, &manifest,
                                             ota_error, sizeof(ota_error));
        if (error == ESP_OK)
            error = ota_manifest_check(&manifest, esp_app_get_description()->version,
                                       ota_error, sizeof(ota_error));
        if (error == ESP_OK) {
            ESP_LOGI("tab5-os", "Installing verified OTA manifest version %s", manifest.version);
            error = ota_manifest_install(&manifest, ota_error, sizeof(ota_error));
        }
        ota_ok = error == ESP_OK;
        if (!ota_ok && !ota_error[0])
            snprintf(ota_error, sizeof(ota_error), "Update failed: %s", esp_err_to_name(error));
        if (!ota_ok) ota_busy = false;
        ota_done = true;
        if (ota_ok) {
            const esp_partition_t *installed = esp_ota_get_boot_partition();
            esp_app_desc_t installed_description;
            if (installed && esp_ota_get_partition_description(installed, &installed_description) == ESP_OK)
                ota_record_pending(installed_description.version);
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
    const char *blocker = restart_blocker();
    if (blocker) {
        lv_label_set_text(ota_status, blocker);
        return;
    }
    lv_display_trigger_activity(NULL);
    screensaver_close();
    ota_busy = true;
    ota_done = false;
    ota_ok = false;
    lv_label_set_text(ota_status, "Checking stable release manifest...");
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
    if (!ota_ok) {
        lv_display_trigger_activity(NULL);
        screensaver_close();
        if (ota_button) lv_obj_remove_state(ota_button, LV_STATE_DISABLED);
    }
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON: return "power on";
        case ESP_RST_EXT: return "external pin";
        case ESP_RST_SW: return "software restart";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_USB: return "USB";
        case ESP_RST_JTAG: return "JTAG";
        case ESP_RST_EFUSE: return "eFuse error";
        case ESP_RST_PWR_GLITCH: return "power glitch";
        case ESP_RST_CPU_LOCKUP: return "CPU lockup";
        default: return "unknown";
    }
}

static const char *ota_state_name(esp_ota_img_states_t state)
{
    switch (state) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "health check pending";
        case ESP_OTA_IMG_VALID: return "validated";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "rolled back";
        default: return "not tracked";
    }
}

static void system_self_test(void)
{
    uint8_t erased[] = {0xff, 0xff, 0xff};
    assert(strcmp(reset_reason_name(ESP_RST_POWERON), "power on") == 0);
    assert(strcmp(reset_reason_name(ESP_RST_TASK_WDT), "task watchdog") == 0);
    assert(strcmp(ota_state_name(ESP_OTA_IMG_PENDING_VERIFY), "health check pending") == 0);
    assert(bytes_are_erased(erased, sizeof(erased)));
    erased[1] = 0;
    assert(!bytes_are_erased(erased, sizeof(erased)));
    assert(wifi_forget_confirmation_valid(true, 100, 5100));
    assert(!wifi_forget_confirmation_valid(true, 100, 5101));
    assert(!wifi_forget_confirmation_valid(false, 100, 100));
}

static void storage_format_clicked(lv_event_t *event)
{
    if (internal_ready || ota_busy) return;
    if (!storage_format_armed) {
        storage_format_armed = true;
        lv_label_set_text(storage_format_label, "Erase and initialize");
        lv_label_set_text(storage_status, "This erases damaged internal storage. Tap again to confirm.");
        return;
    }
    storage_format_armed = false;
    lv_label_set_text(storage_status, "Initializing internal storage...");
    lv_refr_now(NULL);
    storage_init_error = esp_spiffs_format("storage");
    internal_ready = storage_init_error == ESP_OK && mount_internal();
    if (internal_ready) {
        lv_label_set_text(storage_status, "Internal storage is ready");
        lv_label_set_text(storage_format_label, "Internal storage ready");
        lv_obj_add_state(lv_event_get_target(event), LV_STATE_DISABLED);
        if (ota_health_window_elapsed) validate_running_ota();
    } else {
        lv_label_set_text_fmt(storage_status, "Initialization failed: %s", esp_err_to_name(storage_init_error));
        lv_label_set_text(storage_format_label, "Try initialization again");
    }
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
    const esp_app_desc_t *app = esp_app_get_description();
    uint64_t uptime = esp_timer_get_time() / 1000000;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    const char *ota_state_text = esp_ota_get_state_partition(running, &ota_state) == ESP_OK
                                 ? ota_state_name(ota_state) : "not tracked";
    char last_rollback[40] = "none recorded";
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    if (invalid) {
        esp_app_desc_t description;
        if (esp_ota_get_partition_description(invalid, &description) == ESP_OK)
            snprintf(last_rollback, sizeof(last_rollback), "%s", description.version);
    }
    char internal_text[64] = "unavailable";
    size_t internal_total = 0, internal_used = 0;
    if (internal_ready && esp_spiffs_info("storage", &internal_total, &internal_used) == ESP_OK)
        snprintf(internal_text, sizeof(internal_text), "mounted, %u KB free",
                 (unsigned)((internal_total - internal_used) / 1024));
    char sd_text[64] = "not inserted";
    uint64_t sd_total = 0, sd_free = 0;
    int sd_error = sd_error_snapshot();
    if (sd_media_lost(sd_error)) {
        snprintf(sd_text, sizeof(sd_text), "removed/unresponsive; reinsert and reboot");
    } else if (sd_error == ENOSPC) {
        snprintf(sd_text, sizeof(sd_text), "full; reads remain available");
    } else if (sd_error == EROFS) {
        snprintf(sd_text, sizeof(sd_text), "read-only/write-protected");
    } else if (sd_ready) {
        if (esp_vfs_fat_info(SD_PATH, &sd_total, &sd_free) == ESP_OK)
            snprintf(sd_text, sizeof(sd_text), "mounted, %llu MB free",
                     (unsigned long long)(sd_free / (1024 * 1024)));
        else
            snprintf(sd_text, sizeof(sd_text), "mounted but not responding");
    }
    lv_obj_t *info = lv_label_create(content);
    lv_label_set_text_fmt(info,
        "Tab5 OS %s\nBuilt %s %s with %s\n\n"
        "ESP32-P4 rev %d.%d  |  %d cores\nPanel: %s 720 x 1280\n"
        "Reset: %s\nUptime: %lu d %02lu:%02lu\n"
        "Internal heap: %lu KB free / %lu KB minimum\nPSRAM: %lu KB free / %lu KB total\n\n"
        "Settings: %s\nInternal: %s\nSD card: %s\nWi-Fi: %s\n"
        "OTA image: %s\nLast OTA: %s\nInvalid OTA image: %s",
        app->version, app->date, app->time, app->idf_ver,
        chip.revision / 100, chip.revision % 100, chip.cores,
        bsp_display_get_panel_ic(), reset_reason_name(esp_reset_reason()),
        (unsigned long)(uptime / 86400), (unsigned long)(uptime / 3600 % 24),
        (unsigned long)(uptime / 60 % 60),
        (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
        (unsigned long)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
        (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
        nvs_init_error == ESP_OK ? "ready" : esp_err_to_name(nvs_init_error),
        internal_text, sd_text,
        wifi_connected ? wifi_ip : wifi_ready ? "disconnected" : "unavailable",
        ota_state_text, ota_last_result, last_rollback);
    lv_obj_set_style_text_line_space(info, 8, 0);
    storage_format_armed = false;
    if (!internal_ready) {
        storage_status = lv_label_create(content);
        lv_obj_set_width(storage_status, 620);
        lv_label_set_text(storage_status, "Internal storage could not be mounted. Initialization erases it.");
        lv_obj_t *format = button(content, "Initialize internal storage", storage_format_clicked);
        storage_format_label = lv_obj_get_child(format, 0);
        lv_obj_set_size(format, 520, 82);
    }
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
    ota_button = button(content, "Install stable", ota_clicked);
    lv_obj_set_size(ota_button, 320, 82);
    if (ota_busy) lv_obj_add_state(ota_button, LV_STATE_DISABLED);
    ota_status = lv_label_create(content);
    lv_label_set_text(ota_status, ota_busy ? "Checking stable release manifest..." :
                                            "Manifest version, hardware, size, URL, and SHA-256 are verified before activation");
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

static void i2c_address_text(char text[5], uint8_t address)
{
    snprintf(text, 5, "0x%02X", address);
}

static uint8_t i2c_step_value(uint8_t value, int step, uint8_t minimum, uint8_t maximum)
{
    int next = value + step;
    if (next < minimum) return minimum;
    if (next > maximum) return maximum;
    return (uint8_t)next;
}

static uint32_t i2c_next_speed(uint32_t speed_hz)
{
    return speed_hz == I2C_STANDARD_SPEED_HZ ? I2C_FAST_SPEED_HZ : I2C_STANDARD_SPEED_HZ;
}

static bool i2c_write_confirmation_valid(bool armed, uint32_t armed_at_ms, uint32_t now_ms)
{
    return armed && (uint32_t)(now_ms - armed_at_ms) < I2C_WRITE_CONFIRM_MS;
}

static void i2c_self_test(void)
{
    char text[5];
    i2c_address_text(text, 0x3c);
    assert(strcmp(text, "0x3C") == 0);
    assert(i2c_step_value(0x08, -1, 0x08, 0x77) == 0x08);
    assert(i2c_step_value(0x70, 0x10, 0x08, 0x77) == 0x77);
    assert(i2c_step_value(0x00, -0x10, 0x00, 0xff) == 0x00);
    assert(i2c_step_value(0xf8, 0x10, 0x00, 0xff) == 0xff);
    assert(i2c_next_speed(I2C_STANDARD_SPEED_HZ) == I2C_FAST_SPEED_HZ);
    assert(i2c_next_speed(I2C_FAST_SPEED_HZ) == I2C_STANDARD_SPEED_HZ);
    assert(i2c_next_speed(0) == I2C_STANDARD_SPEED_HZ);
    assert(!i2c_write_confirmation_valid(false, 100, 101));
    assert(i2c_write_confirmation_valid(true, 100, 100 + I2C_WRITE_CONFIRM_MS - 1));
    assert(!i2c_write_confirmation_valid(true, 100, 100 + I2C_WRITE_CONFIRM_MS));
    assert(i2c_write_confirmation_valid(true, UINT32_MAX - 1000, 1000));
}

static void i2c_disarm_write(void)
{
    i2c_write_armed = false;
    i2c_write_armed_at_ms = 0;
}

static void i2c_update_controls(void)
{
    if (i2c_address_label)
        lv_label_set_text_fmt(i2c_address_label, "Address: 0x%02X", i2c_selected_address);
    if (i2c_register_label)
        lv_label_set_text_fmt(i2c_register_label, "Register: 0x%02X", i2c_selected_register);
    if (i2c_speed_label)
        lv_label_set_text_fmt(i2c_speed_label, "Register speed: %lu kHz",
                              (unsigned long)(i2c_bus_speed_hz / 1000));
    if (i2c_watch_label)
        lv_label_set_text(i2c_watch_label, i2c_watch_enabled ? "WATCH 1 Hz: ON" : "WATCH 1 Hz: OFF");
    if (i2c_value_label)
        lv_label_set_text_fmt(i2c_value_label, "Write value: 0x%02X", i2c_write_value);
    if (i2c_write_label)
        lv_label_set_text(i2c_write_label, i2c_write_armed ? "CONFIRM WRITE" : "ARM WRITE BYTE");
    if (i2c_capture_label)
        lv_label_set_text(i2c_capture_label, i2c_capture_file ? "STOP & SAVE CSV" : "START CSV CAPTURE");
}

static bool i2c_capture_stop(void)
{
    if (!i2c_capture_file) return true;

    const char *temporary_name = strrchr(i2c_capture_temporary_path, '/');
    const char *final_name = strrchr(i2c_capture_final_path, '/');
    bool saved = storage_commit_new_file(&i2c_capture_file, i2c_capture_temporary_path,
                                         i2c_capture_final_path) == 0;
    if (saved) {
        snprintf(i2c_capture_notice, sizeof(i2c_capture_notice), "Saved %s",
                 final_name ? final_name + 1 : "I2C CSV");
    } else {
        int save_error = errno ? errno : EIO;
        sd_record_error(save_error);
        snprintf(i2c_capture_notice, sizeof(i2c_capture_notice),
                 "Capture not published; %s retained (%s)",
                 temporary_name ? temporary_name + 1 : "TMP", strerror(save_error));
    }
    i2c_capture_temporary_path[0] = '\0';
    i2c_capture_final_path[0] = '\0';
    i2c_update_controls();
    if (i2c_capture_status) lv_label_set_text(i2c_capture_status, i2c_capture_notice);
    return saved;
}

static bool i2c_capture_start(void)
{
    i2c_capture_notice[0] = '\0';
    if (!sd_ready) {
        int error = sd_error_snapshot();
        snprintf(i2c_capture_notice, sizeof(i2c_capture_notice), "SD unavailable: %s",
                 strerror(error ? error : ENODEV));
        if (i2c_capture_status) lv_label_set_text(i2c_capture_status, i2c_capture_notice);
        return false;
    }

    if (mkdir(SD_PATH "/I2C", 0775) != 0 && errno != EEXIST) {
        int error = errno ? errno : EIO;
        sd_record_error(error);
        snprintf(i2c_capture_notice, sizeof(i2c_capture_notice),
                 "Could not create I2C folder: %s", strerror(error));
        if (i2c_capture_status) lv_label_set_text(i2c_capture_status, i2c_capture_notice);
        return false;
    }

    time_t now = time(NULL);
    struct tm local;
    char date[7], clock[7], directory[64];
    localtime_r(&now, &local);
    strftime(date, sizeof(date), "%y%m%d", &local);
    strftime(clock, sizeof(clock), "%H%M%S", &local);
    snprintf(directory, sizeof(directory), SD_PATH "/I2C/%s", date);
    if (mkdir(directory, 0775) != 0 && errno != EEXIST) {
        int error = errno ? errno : EIO;
        sd_record_error(error);
        snprintf(i2c_capture_notice, sizeof(i2c_capture_notice),
                 "Could not create capture folder: %s", strerror(error));
        if (i2c_capture_status) lv_label_set_text(i2c_capture_status, i2c_capture_notice);
        return false;
    }

    int create_error = EEXIST;
    bool temporary_created = false;
    for (unsigned suffix = 0; suffix < 100 && !i2c_capture_file; suffix++) {
        char stem[9];
        snprintf(stem, sizeof(stem), "%s%02u", clock, suffix);
        snprintf(i2c_capture_temporary_path, sizeof(i2c_capture_temporary_path),
                 "%s/%s.TMP", directory, stem);
        snprintf(i2c_capture_final_path, sizeof(i2c_capture_final_path),
                 "%s/%s.CSV", directory, stem);
        struct stat info;
        if (stat(i2c_capture_final_path, &info) == 0) continue;
        if (errno != ENOENT) {
            create_error = errno ? errno : EIO;
            break;
        }
        int descriptor = open(i2c_capture_temporary_path, O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (descriptor < 0) {
            if (errno == EEXIST) continue;
            create_error = errno ? errno : EIO;
            break;
        }
        temporary_created = true;
        i2c_capture_file = fdopen(descriptor, "wb");
        if (!i2c_capture_file) {
            create_error = errno ? errno : EIO;
            close(descriptor);
            break;
        }
    }
    if (!i2c_capture_file) {
        sd_record_error(create_error);
        const char *name = strrchr(i2c_capture_temporary_path, '/');
        if (temporary_created)
            snprintf(i2c_capture_notice, sizeof(i2c_capture_notice),
                     "Capture not started; %s retained (%s)",
                     name ? name + 1 : "TMP", strerror(create_error));
        else
            snprintf(i2c_capture_notice, sizeof(i2c_capture_notice),
                     "Could not create capture: %s", strerror(create_error));
        i2c_capture_temporary_path[0] = '\0';
        i2c_capture_final_path[0] = '\0';
        if (i2c_capture_status) lv_label_set_text(i2c_capture_status, i2c_capture_notice);
        return false;
    }

    if (fputs("unix_time,address,register,value,status,speed_khz\n", i2c_capture_file) < 0) {
        int error = errno ? errno : EIO;
        fclose(i2c_capture_file);
        i2c_capture_file = NULL;
        sd_record_error(error);
        const char *name = strrchr(i2c_capture_temporary_path, '/');
        snprintf(i2c_capture_notice, sizeof(i2c_capture_notice),
                 "Capture not started; %s retained (%s)",
                 name ? name + 1 : "TMP", strerror(error));
        i2c_capture_temporary_path[0] = '\0';
        i2c_capture_final_path[0] = '\0';
        if (i2c_capture_status) lv_label_set_text(i2c_capture_status, i2c_capture_notice);
        return false;
    }

    const char *name = strrchr(i2c_capture_temporary_path, '/');
    snprintf(i2c_capture_notice, sizeof(i2c_capture_notice), "Logging to %s",
             name ? name + 1 : "I2C TMP");
    i2c_capture_last_flush_tick = xTaskGetTickCount();
    i2c_watch_enabled = true;
    i2c_update_controls();
    if (i2c_capture_status) lv_label_set_text(i2c_capture_status, i2c_capture_notice);
    return true;
}

static void i2c_capture_log(esp_err_t transaction_error, uint8_t value)
{
    if (!i2c_capture_file) return;

    char value_text[5] = "";
    if (transaction_error == ESP_OK) snprintf(value_text, sizeof(value_text), "0x%02X", value);
    bool failed = fprintf(i2c_capture_file, "%lld,0x%02X,0x%02X,%s,%s,%lu\n",
                          (long long)time(NULL), i2c_selected_address, i2c_selected_register,
                          value_text, esp_err_to_name(transaction_error),
                          (unsigned long)(i2c_bus_speed_hz / 1000)) < 0;
    TickType_t now = xTaskGetTickCount();
    if (!failed && now - i2c_capture_last_flush_tick >= pdMS_TO_TICKS(I2C_CAPTURE_FLUSH_MS)) {
        failed = storage_sync_file(i2c_capture_file) != 0;
        if (!failed) i2c_capture_last_flush_tick = now;
    }
    if (failed) {
        int error = errno ? errno : EIO;
        sd_record_error(error);
        errno = error;
        i2c_capture_stop();
    }
}

static void i2c_address_step_clicked(lv_event_t *event)
{
    int step = (int)(intptr_t)lv_event_get_user_data(event);
    i2c_disarm_write();
    i2c_selected_address = i2c_step_value(i2c_selected_address, step, 0x08, 0x77);
    i2c_update_controls();
}

static void i2c_register_step_clicked(lv_event_t *event)
{
    int step = (int)(intptr_t)lv_event_get_user_data(event);
    i2c_disarm_write();
    i2c_selected_register = i2c_step_value(i2c_selected_register, step, 0x00, 0xff);
    i2c_update_controls();
}

static void i2c_value_step_clicked(lv_event_t *event)
{
    int step = (int)(intptr_t)lv_event_get_user_data(event);
    i2c_disarm_write();
    i2c_write_value = i2c_step_value(i2c_write_value, step, 0x00, 0xff);
    i2c_update_controls();
}

static void i2c_speed_clicked(lv_event_t *event)
{
    (void)event;
    i2c_disarm_write();
    i2c_bus_speed_hz = i2c_next_speed(i2c_bus_speed_hz);
    i2c_update_controls();
}

static lv_obj_t *i2c_step_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, int step)
{
    lv_obj_t *control = button(parent, text, NULL);
    lv_obj_set_size(control, 125, 60);
    lv_obj_add_event_cb(control, callback, LV_EVENT_CLICKED, (void *)(intptr_t)step);
    return control;
}

static lv_obj_t *i2c_step_row(lv_event_cb_t callback)
{
    lv_obj_t *row = lv_obj_create(content);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 620, 62);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    i2c_step_button(row, "-0x10", callback, -0x10);
    i2c_step_button(row, "-1", callback, -1);
    i2c_step_button(row, "+1", callback, 1);
    i2c_step_button(row, "+0x10", callback, 0x10);
    return row;
}

static void i2c_scan_clicked(lv_event_t *event)
{
    (void)event;
    esp_err_t error = bsp_ext_i2c_init();
    if (error != ESP_OK) {
        lv_label_set_text_fmt(i2c_status, "Could not start I2C: %s", esp_err_to_name(error));
        return;
    }

    lv_label_set_text(i2c_status, "Scanning 0x08-0x77...");
    lv_label_set_text(i2c_devices, "");
    lv_refr_now(NULL);

    char found[768] = "";
    size_t length = 0;
    unsigned count = 0;
    uint8_t first_address = 0;
    for (uint8_t address = 0x08; address <= 0x77; address++) {
        error = i2c_master_probe(bsp_ext_i2c_get_handle(), address, 10);
        if (error == ESP_ERR_NOT_FOUND) continue;
        if (error != ESP_OK) break;
        if (count == 0) first_address = address;
        char text[5];
        i2c_address_text(text, address);
        length += snprintf(found + length, sizeof(found) - length, "%s%s",
                           count ? count % 8 ? "  " : "\n" : "", text);
        count++;
    }
    esp_err_t deinit_error = bsp_ext_i2c_deinit();

    if (deinit_error != ESP_OK) {
        ESP_LOGE("i2c", "Could not release external I2C after scan: %s", esp_err_to_name(deinit_error));
        lv_label_set_text_fmt(i2c_status, "I2C cleanup failed: %s", esp_err_to_name(deinit_error));
    } else if (error == ESP_ERR_TIMEOUT) {
        lv_label_set_text(i2c_status, "Bus timeout - check SDA, SCL, and pull-ups");
    } else if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) {
        lv_label_set_text_fmt(i2c_status, "Scan stopped: %s", esp_err_to_name(error));
    } else {
        lv_label_set_text_fmt(i2c_status, "%u device%s found", count, count == 1 ? "" : "s");
    }
    lv_label_set_text(i2c_devices, count ? found : "No devices responded");
    if (count) {
        i2c_disarm_write();
        i2c_selected_address = first_address;
        i2c_update_controls();
    }
}

static esp_err_t i2c_register_byte_transaction(uint8_t address, uint8_t register_address,
                                               uint8_t *value, bool write)
{
    esp_err_t error = bsp_ext_i2c_init();
    if (error != ESP_OK) return error;

    i2c_master_dev_handle_t device = NULL;
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = i2c_bus_speed_hz,
    };
    error = i2c_master_bus_add_device(bsp_ext_i2c_get_handle(), &config, &device);
    if (error == ESP_OK) {
        uint8_t data[] = {register_address, *value};
        error = write ? i2c_master_transmit(device, data, sizeof(data), 50) :
                        i2c_master_transmit_receive(device, data, 1, value, 1, 50);
    }
    esp_err_t device_cleanup_error = device ? i2c_master_bus_rm_device(device) : ESP_OK;
    esp_err_t bus_cleanup_error = bsp_ext_i2c_deinit();
    if (device_cleanup_error != ESP_OK)
        ESP_LOGE("i2c", "Could not remove external I2C device: %s", esp_err_to_name(device_cleanup_error));
    if (bus_cleanup_error != ESP_OK)
        ESP_LOGE("i2c", "Could not release external I2C bus: %s", esp_err_to_name(bus_cleanup_error));
    if (device_cleanup_error != ESP_OK) return device_cleanup_error;
    if (bus_cleanup_error != ESP_OK) return bus_cleanup_error;
    return error;
}

static void i2c_read_once(bool watching)
{
    if (!i2c_read_result) return;
    uint8_t value = 0;
    esp_err_t error = i2c_register_byte_transaction(i2c_selected_address, i2c_selected_register,
                                                    &value, false);
    if (watching) i2c_capture_log(error, value);
    if (error == ESP_OK) {
        if (watching)
            lv_label_set_text_fmt(i2c_read_result, "WATCH  0x%02X = 0x%02X  (%u)",
                                  i2c_selected_register, value, value);
        else
            lv_label_set_text_fmt(i2c_read_result, "0x%02X = 0x%02X  (%u)",
                                  i2c_selected_register, value, value);
    }
    else if (error == ESP_ERR_TIMEOUT)
        lv_label_set_text(i2c_read_result, "Read timed out - check wiring and pull-ups");
    else
        lv_label_set_text_fmt(i2c_read_result, "Read failed: %s", esp_err_to_name(error));
}

static void i2c_read_clicked(lv_event_t *event)
{
    (void)event;
    lv_label_set_text_fmt(i2c_read_result, "Reading 0x%02X register 0x%02X...",
                          i2c_selected_address, i2c_selected_register);
    lv_refr_now(NULL);
    i2c_read_once(false);
}

static void i2c_watch_clicked(lv_event_t *event)
{
    (void)event;
    i2c_disarm_write();
    i2c_watch_enabled = !i2c_watch_enabled;
    if (!i2c_watch_enabled && i2c_capture_file) i2c_capture_stop();
    i2c_update_controls();
    if (i2c_watch_enabled) i2c_read_once(true);
    else lv_label_set_text(i2c_read_result, "1 Hz read watch stopped");
}

static void i2c_capture_clicked(lv_event_t *event)
{
    (void)event;
    if (i2c_capture_file) {
        i2c_capture_stop();
    } else if (i2c_capture_start()) {
        i2c_read_once(true);
    }
}

static void i2c_write_clicked(lv_event_t *event)
{
    (void)event;
    uint32_t now = lv_tick_get();
    if (!i2c_write_confirmation_valid(i2c_write_armed, i2c_write_armed_at_ms, now)) {
        i2c_watch_enabled = false;
        if (i2c_capture_file) i2c_capture_stop();
        i2c_write_armed = true;
        i2c_write_armed_at_ms = now;
        i2c_update_controls();
        lv_label_set_text_fmt(i2c_read_result,
                              "Armed: write 0x%02X to 0x%02X register 0x%02X. Tap again within 5s.",
                              i2c_write_value, i2c_selected_address, i2c_selected_register);
        return;
    }

    i2c_disarm_write();
    i2c_update_controls();
    lv_label_set_text_fmt(i2c_read_result, "Writing 0x%02X to register 0x%02X...",
                          i2c_write_value, i2c_selected_register);
    lv_refr_now(NULL);
    uint8_t value = i2c_write_value;
    esp_err_t error = i2c_register_byte_transaction(i2c_selected_address, i2c_selected_register,
                                                    &value, true);
    if (error == ESP_OK)
        lv_label_set_text_fmt(i2c_read_result, "Wrote 0x%02X to register 0x%02X",
                              value, i2c_selected_register);
    else if (error == ESP_ERR_TIMEOUT)
        lv_label_set_text(i2c_read_result, "Write timed out - check wiring and pull-ups");
    else
        lv_label_set_text_fmt(i2c_read_result, "Write failed: %s", esp_err_to_name(error));
}

static void i2c_tick(lv_timer_t *timer)
{
    (void)timer;
    if (i2c_write_armed &&
        !i2c_write_confirmation_valid(true, i2c_write_armed_at_ms, lv_tick_get())) {
        i2c_disarm_write();
        i2c_update_controls();
        if (i2c_read_result) lv_label_set_text(i2c_read_result, "Write confirmation expired");
    }
    if (i2c_watch_enabled) i2c_read_once(true);
}

static void i2c_clicked(lv_event_t *event)
{
    (void)event;
    show_i2c();
}

static void uart_clicked(lv_event_t *event)
{
    (void)event;
    clear_content();
    uart_tool_show(content, sd_ready, sd_record_error);
}

static void ender3_clicked(lv_event_t *event)
{
    (void)event;
    clear_content();
    ender3_tool_show(content);
}

static void spi_clicked(lv_event_t *event)
{
    (void)event;
    clear_content();
    spi_tool_show(content);
}

static void signal_clicked(lv_event_t *event)
{
    (void)event;
    clear_content();
    signal_tool_show(content);
}

static void http_clicked(lv_event_t *event)
{
    (void)event;
    clear_content();
    http_tool_show(content, wifi_connected, sd_ready, sd_record_error);
}

static void mqtt_clicked(lv_event_t *event)
{
    (void)event;
    clear_content();
    mqtt_tool_show(content, wifi_connected, sd_ready, sd_record_error);
}

static void ble_clicked(lv_event_t *event)
{
    (void)event;
    clear_content();
    ble_tool_show(content, wifi_ready, sd_ready, ble_scan, ble_products_idle, sd_record_error);
}

static void show_i2c(void)
{
    clear_content();
    lv_obj_set_style_pad_row(content, 12, 0);
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "External I2C Inspector");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *help = lv_label_create(content);
    lv_obj_set_width(help, 620);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(help, "Grove/EXT: SDA G53, SCL G54; external 5V stays off.\n"
                            "Share ground. Use 3.3V pull-ups or level-shift a 5V bus.\n"
                            "One-byte reads can have side effects; check the device datasheet.\n"
                            "Raw writes can reconfigure hardware; verify the register and value first.");

    lv_obj_t *scan = button(content, LV_SYMBOL_REFRESH "  SCAN", i2c_scan_clicked);
    lv_obj_set_size(scan, 620, 90);
    i2c_status = lv_label_create(content);
    lv_obj_set_width(i2c_status, 620);
    lv_obj_set_style_text_align(i2c_status, LV_TEXT_ALIGN_CENTER, 0);
    i2c_devices = lv_label_create(content);
    lv_obj_set_width(i2c_devices, 620);
    lv_obj_set_style_text_align(i2c_devices, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *speed = button(content, "", i2c_speed_clicked);
    lv_obj_set_size(speed, 620, 70);
    i2c_speed_label = lv_obj_get_child(speed, 0);
    i2c_address_label = lv_label_create(content);
    lv_obj_set_style_text_font(i2c_address_label, &lv_font_montserrat_28, 0);
    i2c_step_row(i2c_address_step_clicked);
    i2c_register_label = lv_label_create(content);
    lv_obj_set_style_text_font(i2c_register_label, &lv_font_montserrat_28, 0);
    i2c_step_row(i2c_register_step_clicked);
    lv_obj_t *read_row = lv_obj_create(content);
    lv_obj_remove_style_all(read_row);
    lv_obj_set_size(read_row, 620, 72);
    lv_obj_set_flex_flow(read_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(read_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *read = button(read_row, "READ BYTE", i2c_read_clicked);
    lv_obj_set_size(read, 300, 70);
    lv_obj_t *watch = button(read_row, "", i2c_watch_clicked);
    lv_obj_set_size(watch, 300, 70);
    i2c_watch_label = lv_obj_get_child(watch, 0);
    i2c_read_result = lv_label_create(content);
    lv_obj_set_width(i2c_read_result, 620);
    lv_obj_set_style_text_align(i2c_read_result, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(i2c_read_result, "Select an address and register, then read once");
    lv_obj_t *capture = button(content, "", i2c_capture_clicked);
    lv_obj_set_size(capture, 620, 70);
    i2c_capture_label = lv_obj_get_child(capture, 0);
    i2c_capture_status = lv_label_create(content);
    lv_obj_set_width(i2c_capture_status, 620);
    lv_obj_set_style_text_align(i2c_capture_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(i2c_capture_status, i2c_capture_notice[0] ? i2c_capture_notice :
                      "Optional 1 Hz CSV logging to the SD card");
    i2c_value_label = lv_label_create(content);
    lv_obj_set_style_text_font(i2c_value_label, &lv_font_montserrat_28, 0);
    i2c_step_row(i2c_value_step_clicked);
    lv_obj_t *write = button(content, "", i2c_write_clicked);
    lv_obj_set_size(write, 620, 80);
    i2c_write_label = lv_obj_get_child(write, 0);
    i2c_update_controls();
    i2c_scan_clicked(NULL);
    i2c_timer = lv_timer_create(i2c_tick, 1000, NULL);
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
    lv_obj_set_size(scope_chart, 650, 420);
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

    row = scope_row();
    scope_control(row, "Offset\n0 mV", 180, scope_offset_clicked, NULL, &scope_offset_label);
    scope_control(row, "Scale\n100.0%", 180, scope_gain_clicked, NULL, &scope_gain_label);
    scope_control(row, "RESET CAL", 180, scope_calibration_reset_clicked, NULL, NULL);

    row = scope_row();
    scope_control(row, "SAVE CSV", 180, scope_capture_clicked, NULL, NULL);
    scope_capture_status = lv_label_create(row);
    lv_obj_set_width(scope_capture_status, 420);
    lv_obj_set_style_text_align(scope_capture_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(scope_capture_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(scope_capture_status, scope_capture_notice[0] ? scope_capture_notice :
                      "Save the visible 300-point chart to SD");

    lv_obj_t *warning = lv_label_create(content);
    lv_label_set_text(warning, "Inputs: G16 G18 G19 G49 G50 G51 G53 G54   |   0-3.3V only");
    lv_obj_set_width(warning, 650);
    lv_obj_set_style_text_align(warning, LV_TEXT_ALIGN_CENTER, 0);

    scope_running = true;
    scope_error = false;
    scope_chart_ready = false;
    scope_update_controls();
    portENTER_CRITICAL(&scope_lock);
    scope_active = true;
    portEXIT_CRITICAL(&scope_lock);
    if (!scope_task_handle) xTaskCreate(scope_task, "adc-scope", 4096, NULL, 5, &scope_task_handle);
    scope_timer = lv_timer_create(scope_tick, 150, NULL);
}

static bool govee_snapshot(govee_reading_t *reading, char *name, size_t name_size,
                           char *address, size_t address_size, int8_t *rssi, time_t *updated)
{
    portENTER_CRITICAL(&govee_lock);
    bool ready = govee_ready;
    if (ready) {
        *reading = govee_reading;
        snprintf(name, name_size, "%s", govee_name);
        snprintf(address, address_size, "%s", govee_address);
        *rssi = govee_rssi;
        *updated = govee_updated_at;
    }
    portEXIT_CRITICAL(&govee_lock);
    return ready;
}

static void govee_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!govee_status) return;
    portENTER_CRITICAL(&govee_lock);
    bool enabled = govee_enabled;
    bool scanning = govee_scanning;
    portEXIT_CRITICAL(&govee_lock);
    if (govee_toggle_label)
        lv_label_set_text(govee_toggle_label, enabled ? "Turn Govee Bluetooth off" : "Turn Govee Bluetooth on");
    if (!enabled) {
        lv_label_set_text(govee_status, "Govee Bluetooth is off");
        lv_label_set_text(govee_temperature, "-- F");
        lv_label_set_text(govee_humidity, "--% RH");
        lv_label_set_text(govee_details, "Turn on Bluetooth to monitor broadcasts");
        return;
    }
    govee_reading_t reading;
    char name[sizeof(govee_name)], address[sizeof(govee_address)];
    int8_t rssi;
    time_t updated;
    if (!govee_snapshot(&reading, name, sizeof(name), address, sizeof(address), &rssi, &updated)) {
        lv_label_set_text(govee_status, scanning ? "Scanning for a Govee H5075..." : "Bluetooth is unavailable");
        return;
    }
    int age = (int)(time(NULL) - updated);
    int c_tenths = weather_round(reading.temperature_c * 10.0f);
    lv_label_set_text_fmt(govee_status, "%s  |  updated %d sec ago", name, age < 0 ? 0 : age);
    lv_label_set_text_fmt(govee_temperature, "%d F", weather_round(reading.temperature_c * 9.0f / 5.0f + 32.0f));
    lv_label_set_text_fmt(govee_humidity, "%d%% RH", weather_round(reading.humidity));
    lv_label_set_text_fmt(govee_details, "%s%d.%d C\nSensor battery %u%%\nSignal %d dBm\n%s",
                          c_tenths < 0 ? "-" : "", abs(c_tenths) / 10, abs(c_tenths) % 10,
                          reading.battery, rssi, address);
}

static void govee_toggle_clicked(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&govee_lock);
    bool enabled = govee_enabled = !govee_enabled;
    if (!enabled) govee_ready = false;
    portEXIT_CRITICAL(&govee_lock);
    ble_scan();
    govee_tick(NULL);
}

static void show_govee(void)
{
    clear_content();
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "Govee H5075");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    govee_status = lv_label_create(content);
    lv_obj_set_width(govee_status, 620);
    lv_obj_set_style_text_align(govee_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *toggle = button(content, "Turn Govee Bluetooth on", govee_toggle_clicked);
    govee_toggle_label = lv_obj_get_child(toggle, 0);
    lv_obj_set_size(toggle, 520, 82);

    lv_obj_t *card = lv_obj_create(content);
    lv_obj_set_size(card, 620, 610);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 32, 0);
    govee_temperature = lv_label_create(card);
    lv_obj_set_style_text_font(govee_temperature, &lv_font_montserrat_48, 0);
    govee_humidity = lv_label_create(card);
    lv_obj_set_style_text_font(govee_humidity, &lv_font_montserrat_48, 0);
    govee_details = lv_label_create(card);
    lv_obj_set_width(govee_details, 560);
    lv_obj_set_style_text_align(govee_details, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(govee_temperature, "-- F");
    lv_label_set_text(govee_humidity, "--% RH");
    lv_label_set_text(govee_details, "Waiting for a broadcast...");
    govee_tick(NULL);
    govee_timer = lv_timer_create(govee_tick, 1000, NULL);
}

static void govee_clicked(lv_event_t *event)
{
    (void)event;
    show_govee();
}

static void ride_load_history(void)
{
    if (!ride_history) return;
    storage_recover_replace(SD_PATH "/RIDES/SUMMARY.CSV", SD_PATH "/RIDES/SUMMARY.BAK");
    FILE *file = fopen(SD_PATH "/RIDES/SUMMARY.CSV", "rb");
    int rides = 0, best_power = 0;
    long long start;
    unsigned duration;
    float distance, work;
    int average_power, maximum_power, average_hr, maximum_hr;
    uint64_t total_seconds = 0;
    float total_distance = 0;
    char line[160];
    if (file) {
        while (fgets(line, sizeof(line), file)) {
            if (sscanf(line, "%lld,%u,%f,%f,%d,%d,%d,%d", &start, &duration, &distance,
                       &work, &average_power, &maximum_power, &average_hr, &maximum_hr) != 8) continue;
            rides++;
            total_seconds += duration;
            total_distance += distance;
            if (maximum_power > best_power) best_power = maximum_power;
        }
        fclose(file);
    }
    lv_label_set_text_fmt(ride_history, "History: %d rides  |  %.1f mi  |  %.1f hr  |  best %d W",
                          rides, total_distance * 0.621371f, total_seconds / 3600.0f, best_power);
}

static bool ride_append_summary(unsigned duration)
{
    mkdir(SD_PATH "/RIDES", 0775);
    const char *temporary_path = SD_PATH "/RIDES/SUMMARY.TMP";
    const char *final_path = SD_PATH "/RIDES/SUMMARY.CSV";
    const char *backup_path = SD_PATH "/RIDES/SUMMARY.BAK";
    if (storage_recover_replace(final_path, backup_path) != 0) return false;
    remove(temporary_path);
    FILE *source = fopen(final_path, "rb");
    if (!source && errno != ENOENT) return false;
    FILE *file = fopen(temporary_path, "wb");
    if (!file) {
        if (source) fclose(source);
        return false;
    }
    bool ok = true;
    bool empty = true;
    int last = '\n';
    char buffer[512];
    while (source && ok) {
        size_t length = fread(buffer, 1, sizeof(buffer), source);
        if (length) {
            empty = false;
            last = (unsigned char)buffer[length - 1];
            ok = fwrite(buffer, 1, length, file) == length;
        }
        if (length < sizeof(buffer)) {
            if (ferror(source)) ok = false;
            break;
        }
    }
    if (source && fclose(source) != 0) ok = false;
    if (ok && !empty && last != '\n') ok = fputc('\n', file) != EOF;
    if (ok && empty)
        ok = fputs("start_unix,duration_s,distance_km,work_kj,avg_power_w,max_power_w,avg_hr,max_hr\n", file) >= 0;
    if (ok)
        ok = fprintf(file, "%lld,%u,%.3f,%.1f,%d,%d,%d,%d\n", (long long)ride_started_at, duration,
                     ride_distance_km, ride_work_kj,
                     ride_power_samples ? (int)(ride_power_sum / ride_power_samples) : 0, ride_max_power,
                     ride_hr_samples_count ? (int)(ride_hr_sum / ride_hr_samples_count) : 0, ride_max_hr) >= 0;
    if (!ok) {
        fclose(file);
        remove(temporary_path);
        return false;
    }
    return storage_commit_replace_file(&file, temporary_path, final_path, backup_path) == 0;
}

static void ride_clear_paths(void)
{
    ride_temporary_path[0] = '\0';
    ride_final_path[0] = '\0';
}

static void ride_abort(const char *message, bool retain_temporary)
{
    ride_recording = false;
    if (ride_file) {
        fclose(ride_file);
        ride_file = NULL;
    }
    const char *name = strrchr(ride_temporary_path, '/');
    if (retain_temporary && ride_temporary_path[0])
        snprintf(ride_notice, sizeof(ride_notice), "%s; %s retained", message, name ? name + 1 : "TMP");
    else {
        if (ride_temporary_path[0]) remove(ride_temporary_path);
        snprintf(ride_notice, sizeof(ride_notice), "%s", message);
    }
    ride_clear_paths();
}

static bool ride_stop(void)
{
    if (!ride_recording) return false;
    ride_recording = false;
    unsigned duration = pdTICKS_TO_MS(xTaskGetTickCount() - ride_started_tick) / 1000;
    if (storage_commit_new_file(&ride_file, ride_temporary_path, ride_final_path) != 0) {
        int save_error = errno;
        sd_record_error(save_error);
        const char *name = strrchr(ride_temporary_path, '/');
        snprintf(ride_notice, sizeof(ride_notice), "Ride not published; %s retained (%s)",
                 name ? name + 1 : "TMP", strerror(save_error));
        ride_clear_paths();
        return false;
    }
    bool summary_ok = ride_append_summary(duration);
    if (!summary_ok) sd_record_error(errno ? errno : EIO);
    snprintf(ride_notice, sizeof(ride_notice), "%s",
             summary_ok ? "Ride saved safely" : "Ride saved; history index update failed");
    ride_clear_paths();
    ride_load_history();
    return true;
}

static bool ride_start(void)
{
    ride_notice[0] = '\0';
    portENTER_CRITICAL(&kickr_lock);
    bool subscribed = kickr_subscribed;
    portEXIT_CRITICAL(&kickr_lock);
    if (!sd_ready || !subscribed) return false;
    mkdir(SD_PATH "/RIDES", 0775);
    time_t now = time(NULL);
    struct tm local;
    char date[7], clock[7], directory[64];
    localtime_r(&now, &local);
    strftime(date, sizeof(date), "%y%m%d", &local);
    strftime(clock, sizeof(clock), "%H%M%S", &local);
    snprintf(directory, sizeof(directory), SD_PATH "/RIDES/%s", date);
    if (mkdir(directory, 0775) != 0 && errno != EEXIST) {
        int error = errno ? errno : EIO;
        sd_record_error(error);
        snprintf(ride_notice, sizeof(ride_notice), "Could not create ride folder: %s", strerror(error));
        return false;
    }
    for (unsigned suffix = 0; suffix < 100 && !ride_file; suffix++) {
        char stem[9];
        snprintf(stem, sizeof(stem), "%s%02u", clock, suffix);
        snprintf(ride_temporary_path, sizeof(ride_temporary_path), "%s/%s.TMP", directory, stem);
        snprintf(ride_final_path, sizeof(ride_final_path), "%s/%s.CSV", directory, stem);
        struct stat info;
        if (stat(ride_final_path, &info) == 0) continue;
        if (errno != ENOENT) break;
        int descriptor = open(ride_temporary_path, O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (descriptor < 0) {
            if (errno == EEXIST) continue;
            break;
        }
        ride_file = fdopen(descriptor, "wb");
        if (!ride_file) {
            close(descriptor);
            remove(ride_temporary_path);
        }
    }
    if (!ride_file) {
        sd_record_error(errno ? errno : EIO);
        ride_clear_paths();
        snprintf(ride_notice, sizeof(ride_notice), "Could not create a new ride file");
        return false;
    }
    if (fputs("unix_time,elapsed_s,power_w,cadence_rpm,speed_kmh,heart_rate,resistance,distance_km,work_kj\n",
              ride_file) < 0) {
        sd_record_error(errno ? errno : EIO);
        ride_abort("Could not write the ride header", false);
        return false;
    }
    ride_started_at = now;
    ride_started_tick = ride_last_log_tick = ride_last_flush_tick = xTaskGetTickCount();
    ride_distance_km = ride_work_kj = 0;
    ride_power_sum = ride_hr_sum = 0;
    ride_power_samples = ride_hr_samples_count = 0;
    ride_max_power = ride_max_hr = 0;
    ride_next_hr_measure = 0;
    ride_recording = true;
    return true;
}

static void ride_button_clicked(lv_event_t *event)
{
    (void)event;
    if (ride_recording) ride_stop();
    else if (!ride_start() && ride_status && !ride_notice[0])
        lv_label_set_text(ride_status, !sd_ready ? "SD card is unavailable" : "Wake and connect the KICKR first");
    if (ride_status && ride_notice[0]) lv_label_set_text(ride_status, ride_notice);
}

static void cycling_tick(lv_timer_t *timer)
{
    (void)timer;
    kickr_data_t data;
    bool enabled, found, connecting, connected, subscribed;
    time_t updated;
    portENTER_CRITICAL(&kickr_lock);
    data = kickr_data;
    enabled = kickr_enabled;
    found = kickr_found;
    connecting = kickr_connecting;
    connected = kickr_connected;
    subscribed = kickr_subscribed;
    updated = kickr_updated_at;
    portEXIT_CRITICAL(&kickr_lock);
    portENTER_CRITICAL(&ring_lock);
    int heart_rate = ring_heart_rate;
    time_t heart_rate_time = ring_hr_updated_at;
    portEXIT_CRITICAL(&ring_lock);

    time_t now = time(NULL);
    bool fresh = updated && now - updated <= 3;
    bool fresh_hr = heart_rate_time && now - heart_rate_time <= 90;
    if (!enabled) fresh = false;
    if (!fresh) memset(&data, 0, sizeof(data));
    if (!fresh_hr) heart_rate = -1;

    TickType_t ticks = xTaskGetTickCount();
    if (ride_recording && (ride_next_hr_measure == 0 || (int32_t)(ticks - ride_next_hr_measure) >= 0) &&
        ring_hr_begin())
        ride_next_hr_measure = ticks + pdMS_TO_TICKS(60000);

    if (ride_recording && ticks - ride_last_log_tick >= pdMS_TO_TICKS(1000)) {
        float seconds = (ticks - ride_last_log_tick) / (float)configTICK_RATE_HZ;
        unsigned elapsed = pdTICKS_TO_MS(ticks - ride_started_tick) / 1000;
        ride_last_log_tick = ticks;
        ride_distance_km += data.speed_kmh * seconds / 3600.0f;
        if (data.has_power) {
            ride_work_kj += data.power_w * seconds / 1000.0f;
            ride_power_sum += data.power_w;
            ride_power_samples++;
            if (data.power_w > ride_max_power) ride_max_power = data.power_w;
        }
        if (heart_rate > 0) {
            ride_hr_sum += heart_rate;
            ride_hr_samples_count++;
            if (heart_rate > ride_max_hr) ride_max_hr = heart_rate;
        }
        bool write_failed = !ride_file || fprintf(ride_file, "%lld,%lld,%d,%.1f,%.2f,%d,%d,%.3f,%.1f\n",
                (long long)now, (long long)elapsed, data.has_power ? data.power_w : 0,
                data.has_cadence ? data.cadence_rpm : 0, data.has_speed ? data.speed_kmh : 0,
                heart_rate, data.has_resistance ? data.resistance : 0, ride_distance_km, ride_work_kj) < 0;
        if (!write_failed && ticks - ride_last_flush_tick >= pdMS_TO_TICKS(RIDE_FLUSH_MS)) {
            write_failed = storage_sync_file(ride_file) != 0;
            if (!write_failed) ride_last_flush_tick = ticks;
        }
        if (write_failed) {
            int error = errno ? errno : EIO;
            sd_record_error(error);
            ride_abort("SD write failed", true);
        }
    }

    if (!ride_status) return;
    if (!ride_recording && ride_notice[0]) lv_label_set_text(ride_status, ride_notice);
    else lv_label_set_text(ride_status, !enabled ? "KICKR Bluetooth is off" :
                           subscribed ? (fresh ? "KICKR connected" : "KICKR connected - start pedaling") :
                           connecting ? "Connecting to KICKR..." : connected ? "Reading KICKR services..." :
                           found ? "KICKR disconnected - scanning..." : "Wake the KICKR by pedaling");
    if (ride_toggle_label)
        lv_label_set_text(ride_toggle_label, enabled ? "Turn KICKR Bluetooth off" : "Turn KICKR Bluetooth on");
    lv_label_set_text_fmt(ride_power_label, data.has_power ? "%d W" : "-- W", data.power_w);
    lv_label_set_text_fmt(ride_cadence_label, data.has_cadence ? "%.0f RPM" : "-- RPM", data.cadence_rpm);
    lv_label_set_text_fmt(ride_hr_label, heart_rate > 0 ? "%d BPM" : "-- BPM", heart_rate);
    unsigned elapsed = ride_recording ? pdTICKS_TO_MS(ticks - ride_started_tick) / 1000 : 0;
    lv_label_set_text_fmt(ride_stats, "%.1f mph  |  %.2f mi  |  %.1f kJ  |  %02u:%02u:%02u",
                          data.speed_kmh * 0.621371f, ride_distance_km * 0.621371f, ride_work_kj,
                          elapsed / 3600, elapsed / 60 % 60, elapsed % 60);
    lv_label_set_text(ride_button_label, ride_recording ? "Stop & save" : "Start ride");
    if (ride_chart && fresh) {
        lv_chart_set_next_value(ride_chart, ride_power_series, data.has_power ? data.power_w : 0);
        lv_chart_set_next_value(ride_chart, ride_hr_series, heart_rate > 0 ? heart_rate : LV_CHART_POINT_NONE);
    }
}

static void kickr_toggle_clicked(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&kickr_lock);
    bool enabled = kickr_enabled = !kickr_enabled;
    bool cancel = !enabled && kickr_connecting;
    bool disconnect = !enabled && kickr_connected;
    uint16_t connection = kickr_conn_handle;
    if (!enabled) {
        kickr_stopping = cancel || disconnect;
        kickr_found = false;
        kickr_subscribed = false;
        kickr_updated_at = 0;
        memset(&kickr_data, 0, sizeof(kickr_data));
    }
    portEXIT_CRITICAL(&kickr_lock);
    if (!enabled && ride_recording) ride_stop();
    if (cancel) {
        int rc = ble_gap_conn_cancel();
        if (rc) ESP_LOGW("kickr", "Connection cancel failed: %d", rc);
    }
    if (disconnect) {
        int rc = ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
        if (rc) ESP_LOGW("kickr", "Disconnect request failed: %d", rc);
    }
    ble_scan();
    cycling_tick(NULL);
}

static void cycling_clicked(lv_event_t *event)
{
    (void)event;
    show_cycling();
}

static void show_cycling(void)
{
    clear_content();
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "Cycling");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    ride_status = lv_label_create(content);
    lv_obj_set_width(ride_status, 640);
    lv_obj_set_style_text_align(ride_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *toggle = button(content, "Turn KICKR Bluetooth on", kickr_toggle_clicked);
    ride_toggle_label = lv_obj_get_child(toggle, 0);
    lv_obj_set_size(toggle, 520, 82);

    lv_obj_t *metrics = lv_obj_create(content);
    lv_obj_set_size(metrics, 640, 180);
    lv_obj_clear_flag(metrics, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(metrics, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ride_power_label = lv_label_create(metrics);
    ride_cadence_label = lv_label_create(metrics);
    ride_hr_label = lv_label_create(metrics);
    lv_obj_set_style_text_font(ride_power_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_font(ride_cadence_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_font(ride_hr_label, &lv_font_montserrat_28, 0);

    ride_stats = lv_label_create(content);
    lv_obj_set_width(ride_stats, 640);
    lv_obj_set_style_text_align(ride_stats, LV_TEXT_ALIGN_CENTER, 0);
    ride_chart = lv_chart_create(content);
    lv_obj_set_size(ride_chart, 640, 350);
    lv_chart_set_type(ride_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ride_chart, RIDE_CHART_POINTS);
    lv_chart_set_range(ride_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_set_range(ride_chart, LV_CHART_AXIS_SECONDARY_Y, 40, 220);
    lv_chart_set_div_line_count(ride_chart, 5, 7);
    ride_power_series = lv_chart_add_series(ride_chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
    ride_hr_series = lv_chart_add_series(ride_chart, lv_palette_main(LV_PALETTE_RED), LV_CHART_AXIS_SECONDARY_Y);

    lv_obj_t *start = button(content, ride_recording ? "Stop & save" : "Start ride", ride_button_clicked);
    lv_obj_set_size(start, 520, 82);
    ride_button_label = lv_obj_get_child(start, 0);
    ride_history = lv_label_create(content);
    lv_obj_set_width(ride_history, 640);
    lv_obj_set_style_text_align(ride_history, LV_TEXT_ALIGN_CENTER, 0);
    ride_load_history();
    cycling_tick(NULL);
}

static void ring_chart_render(void)
{
    if (!ring_hr_chart || !ring_hr_series) return;
    uint8_t values[RING_HR_HISTORY_POINTS], count, first;
    portENTER_CRITICAL(&ring_lock);
    count = ring_hr_history_count;
    first = (ring_hr_history_head + RING_HR_HISTORY_POINTS - count) % RING_HR_HISTORY_POINTS;
    for (uint8_t i = 0; i < count; i++) values[i] = ring_hr_history[(first + i) % RING_HR_HISTORY_POINTS];
    portEXIT_CRITICAL(&ring_lock);
    lv_chart_set_all_value(ring_hr_chart, ring_hr_series, LV_CHART_POINT_NONE);
    for (uint8_t i = 0; i < count; i++) lv_chart_set_next_value(ring_hr_chart, ring_hr_series, values[i]);
}

static void ring_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!ring_status) return;
    char name[sizeof(ring_name)], address[sizeof(ring_address)];
    int battery;
    int8_t rssi;
    time_t updated;
    int heart_rate;
    bool enabled, found, connecting, connected, charging, hr_active, sync_active;
    int hr_error;
    portENTER_CRITICAL(&ring_lock);
    snprintf(name, sizeof(name), "%s", ring_name);
    snprintf(address, sizeof(address), "%s", ring_address);
    battery = ring_battery;
    rssi = ring_rssi;
    updated = ring_updated_at;
    enabled = ring_enabled;
    found = ring_found;
    connecting = ring_connecting;
    connected = ring_connected;
    charging = ring_charging;
    heart_rate = ring_heart_rate;
    hr_active = ring_hr_active;
    sync_active = ring_sync_active || ring_sync_pending;
    hr_error = ring_hr_error;
    portEXIT_CRITICAL(&ring_lock);
    lv_label_set_text(ring_status, !enabled ? "Ring Bluetooth is off" : connected ? "Connected" : connecting ? "Connecting..." : found ? "Reconnecting..." : "Scanning...");
    if (ring_toggle_label) lv_label_set_text(ring_toggle_label, enabled ? "Turn Ring Bluetooth off" : "Turn Ring Bluetooth on");
    lv_label_set_text_fmt(ring_battery_label, battery < 0 ? "--%%" : "%d%%", battery);
    if (ring_hr_label) lv_label_set_text_fmt(ring_hr_label, heart_rate < 0 ? "-- BPM" : "%d BPM", heart_rate);
    if (ring_hr_button_label) lv_label_set_text(ring_hr_button_label, hr_active ? "Measuring..." : "Measure now");
    if (ring_hr_status) {
        if (ring_storage_error[0]) lv_label_set_text(ring_hr_status, ring_storage_error);
        else if (!enabled) lv_label_set_text(ring_hr_status, "Turn on Ring Bluetooth to connect");
        else if (!connected) lv_label_set_text(ring_hr_status, "Ring is disconnected");
        else if (hr_active) lv_label_set_text(ring_hr_status, "Measuring - keep your hand still");
        else if (sync_active) lv_label_set_text(ring_hr_status, "Syncing stored heart-rate history...");
        else if (hr_error == 1) lv_label_set_text(ring_hr_status, "Adjust the ring for better skin contact");
        else if (hr_error == 2) lv_label_set_text(ring_hr_status, "No reading yet - keep your hand still");
        else lv_label_set_text(ring_hr_status, "Ring records every 5 min, even while away");
    }
    ring_chart_render();
    if (!enabled) {
        lv_label_set_text(ring_details, "Ring Bluetooth is off");
    } else if (!found) {
        lv_label_set_text(ring_details, "Looking for COLMI R12...");
    } else {
        int age = updated ? (int)(time(NULL) - updated) : 0;
        if (updated)
            lv_label_set_text_fmt(ring_details, "%s\n%s\n%s  |  signal %d dBm\nUpdated %d sec ago",
                                  name, address, charging ? "Charging" : "On battery", rssi, age < 0 ? 0 : age);
        else
            lv_label_set_text_fmt(ring_details, "%s\n%s\n%s  |  signal %d dBm",
                                  name, address, charging ? "Charging" : "On battery", rssi);
    }
}

static void ring_hr_clicked(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&ring_lock);
    bool enabled = ring_enabled;
    bool active = ring_hr_active;
    bool connected = ring_connected;
    portEXIT_CRITICAL(&ring_lock);
    if (active) return;
    if (!enabled) {
        lv_label_set_text(ring_hr_status, "Turn on Ring Bluetooth to connect");
        return;
    }
    if (!connected) {
        lv_label_set_text(ring_hr_status, "Ring is disconnected");
        return;
    }
    if (!ring_hr_begin()) {
        lv_label_set_text(ring_hr_status, "Could not start measurement");
        return;
    }
    ring_tick(NULL);
}

static void ring_toggle_clicked(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&ring_lock);
    bool enabled = ring_enabled = !ring_enabled;
    bool cancel = !enabled && ring_connecting;
    bool disconnect = !enabled && ring_connected;
    uint16_t connection = ring_conn_handle;
    if (!enabled) {
        ring_stopping = cancel || disconnect;
        ring_found = false;
        ring_hr_active = false;
        ring_sync_active = false;
        ring_sync_pending = false;
    }
    portEXIT_CRITICAL(&ring_lock);
    if (cancel) {
        int rc = ble_gap_conn_cancel();
        if (rc) ESP_LOGW("ring", "Connection cancel failed: %d", rc);
    }
    if (disconnect) {
        int rc = ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
        if (rc) ESP_LOGW("ring", "Disconnect request failed: %d", rc);
    }
    ble_scan();
    ring_tick(NULL);
}

static void show_ring(void)
{
    clear_content();
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "COLMI R12 Ring");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    ring_status = lv_label_create(content);
    lv_obj_set_width(ring_status, 620);
    lv_obj_set_style_text_align(ring_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *toggle = button(content, "Turn Ring Bluetooth on", ring_toggle_clicked);
    ring_toggle_label = lv_obj_get_child(toggle, 0);
    lv_obj_set_size(toggle, 520, 82);
    lv_obj_t *card = lv_obj_create(content);
    lv_obj_set_size(card, 620, 500);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 20, 0);
    ring_battery_label = lv_label_create(card);
    lv_obj_set_style_text_font(ring_battery_label, &lv_font_montserrat_28, 0);
    ring_hr_label = lv_label_create(card);
    lv_label_set_text(ring_hr_label, "-- BPM");
    lv_obj_set_style_text_font(ring_hr_label, &lv_font_montserrat_48, 0);
    ring_hr_status = lv_label_create(card);
    lv_obj_set_width(ring_hr_status, 560);
    lv_obj_set_style_text_align(ring_hr_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *measure = button(card, "Measure now", ring_hr_clicked);
    ring_hr_button_label = lv_obj_get_child(measure, 0);
    lv_obj_set_size(measure, 520, 82);
    ring_details = lv_label_create(card);
    lv_obj_set_width(ring_details, 560);
    lv_obj_set_style_text_align(ring_details, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *disclaimer = lv_label_create(card);
    lv_label_set_text(disclaimer, "Wellness estimate - not a medical measurement");
    lv_obj_t *chart_title = lv_label_create(content);
    lv_label_set_text(chart_title, "Heart rate - last 5 hours");
    ring_hr_chart = lv_chart_create(content);
    lv_obj_set_size(ring_hr_chart, 620, 260);
    lv_chart_set_type(ring_hr_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ring_hr_chart, RING_HR_HISTORY_POINTS);
    lv_chart_set_range(ring_hr_chart, LV_CHART_AXIS_PRIMARY_Y, 40, 200);
    lv_chart_set_div_line_count(ring_hr_chart, 5, 6);
    ring_hr_series = lv_chart_add_series(ring_hr_chart, lv_palette_main(LV_PALETTE_RED), LV_CHART_AXIS_PRIMARY_Y);
    ring_tick(NULL);
    ring_timer = lv_timer_create(ring_tick, 1000, NULL);
}

static void ring_clicked(lv_event_t *event)
{
    (void)event;
    show_ring();
}

static void servo_update_labels(void)
{
    static const char *ranges[] = {"Range: Narrow", "Range: Medium", "Range: Wide"};
    static const char *speeds[] = {"Speed: Slow", "Speed: Medium", "Speed: Fast"};
    if (servo_mode_label) lv_label_set_text(servo_mode_label, servo_sine_mode ? "Motion: Sine" : "Motion: Random");
    if (servo_range_label) lv_label_set_text(servo_range_label, ranges[servo_range_index]);
    if (servo_rate_label) {
        if (servo_sine_mode) {
            unsigned frequency = servo_frequencies_cHz[servo_frequency_index];
            lv_label_set_text_fmt(servo_rate_label, "Freq: %u.%02u Hz", frequency / 100, frequency % 100);
        } else {
            lv_label_set_text(servo_rate_label, speeds[servo_speed_index]);
        }
    }
}

static void servo_update_sine(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    servo_phase = servo_sine_advance(servo_phase, now_ms - servo_updated_ms,
                                   servo_frequencies_cHz[servo_frequency_index]);
    servo_updated_ms = now_ms;
    servo_set_pulse(servo_sine_pulse(servo_phase, servo_ranges_us[servo_range_index]));
}

static void servo_tick(lv_timer_t *timer)
{
    (void)timer;
    TickType_t now = xTaskGetTickCount();
    if (servo_running && (int32_t)(now - servo_deadline) >= 0) servo_stop();
    if (servo_running && servo_sine_mode) {
        servo_update_sine();
    } else if (servo_running) {
        static const uint8_t steps[] = {3, 7, 12};
        if ((int32_t)(now - servo_next_target) >= 0) {
            uint16_t width = servo_ranges_us[servo_range_index] * 2;
            servo_target_us = 1500 - servo_ranges_us[servo_range_index] + esp_random() % (width + 1);
            servo_next_target = now + pdMS_TO_TICKS(700 + esp_random() % 1000);
        }
        uint8_t step = steps[servo_speed_index];
        if (servo_pulse_us < servo_target_us)
            servo_set_pulse(servo_pulse_us + step > servo_target_us ? servo_target_us : servo_pulse_us + step);
        else if (servo_pulse_us > servo_target_us)
            servo_set_pulse(servo_pulse_us - step < servo_target_us ? servo_target_us : servo_pulse_us - step);
    }
    if (servo_position)
        lv_label_set_text_fmt(servo_position, "%d deg", ((int)servo_pulse_us - 1000) * 180 / 1000);
}

static void servo_start_clicked(lv_event_t *event)
{
    (void)event;
    if (servo_running) return;
    servo_pulse_us = servo_target_us = 1500;
    esp_err_t error = servo_start_pwm();
    if (error != ESP_OK) {
        lv_label_set_text_fmt(servo_status, "Could not start: %s", esp_err_to_name(error));
        return;
    }
    gpio_set_level(TOY_LED_PIN, 1);
    TickType_t now = xTaskGetTickCount();
    servo_phase = 0;
    servo_updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
    servo_next_target = now;
    servo_deadline = now + pdMS_TO_TICKS(SERVO_TIMEOUT_MS);
    servo_running = true;
    lv_label_set_text(servo_status, "Running - auto-stops in 5 minutes");
}

static void servo_stop_clicked(lv_event_t *event)
{
    (void)event;
    servo_stop();
}

static void servo_range_clicked(lv_event_t *event)
{
    (void)event;
    servo_range_index = (servo_range_index + 1) % 3;
    servo_update_labels();
}

static void servo_mode_clicked(lv_event_t *event)
{
    (void)event;
    servo_stop();
    servo_sine_mode = !servo_sine_mode;
    if (servo_timer) lv_timer_set_period(servo_timer, servo_sine_mode ? 20 : 50);
    servo_update_labels();
}

static void servo_rate_clicked(lv_event_t *event)
{
    (void)event;
    if (servo_sine_mode) {
        /* Account for time at the old frequency before changing it, without resetting phase. */
        if (servo_running) servo_update_sine();
        servo_frequency_index = (servo_frequency_index + 1) %
                                (sizeof(servo_frequencies_cHz) / sizeof(servo_frequencies_cHz[0]));
    } else {
        servo_speed_index = (servo_speed_index + 1) % 3;
    }
    servo_update_labels();
}

static void show_servo(void)
{
    clear_content();
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "Servo Toy");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    servo_status = lv_label_create(content);
    lv_label_set_text(servo_status, "Stopped - outputs off");
    servo_position = lv_label_create(content);
    lv_label_set_text(servo_position, "90 deg");
    lv_obj_set_style_text_font(servo_position, &lv_font_montserrat_48, 0);

    lv_obj_t *mode = button(content, "Motion: Sine", servo_mode_clicked);
    servo_mode_label = lv_obj_get_child(mode, 0);
    lv_obj_set_size(mode, 620, 72);

    lv_obj_t *controls = lv_obj_create(content);
    lv_obj_set_size(controls, 620, 100);
    lv_obj_remove_flag(controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *range = button(controls, "Range: Medium", servo_range_clicked);
    servo_range_label = lv_obj_get_child(range, 0);
    lv_obj_set_size(range, 270, 72);
    lv_obj_t *rate = button(controls, "Freq: 0.25 Hz", servo_rate_clicked);
    servo_rate_label = lv_obj_get_child(rate, 0);
    lv_obj_set_size(rate, 270, 72);

    lv_obj_t *hint = lv_label_create(content);
    lv_obj_set_width(hint, 620);
    lv_label_set_text(hint, "Sine frequency is full back-and-forth cycles per second.\nTap Motion to switch modes; switching stops the servo.");

    lv_obj_t *start = button(content, "START", servo_start_clicked);
    lv_obj_set_size(start, 620, 110);
    lv_obj_t *stop = button(content, "STOP", servo_stop_clicked);
    lv_obj_set_size(stop, 620, 150);
    lv_obj_set_style_bg_color(stop, lv_color_hex(0xC62828), 0);

    lv_obj_t *wiring = lv_label_create(content);
    lv_obj_set_width(wiring, 620);
    lv_label_set_long_mode(wiring, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(wiring, "G%d: servo signal\nG%d: LED driver signal\nUse external 5V servo power and common ground. Use a resistor and transistor/MOSFET for the LED. Leaving this app turns both outputs off.", SERVO_PIN, TOY_LED_PIN);
    servo_update_labels();
    servo_timer = lv_timer_create(servo_tick, servo_sine_mode ? 20 : 50, NULL);
}

static void servo_clicked(lv_event_t *event)
{
    (void)event;
    show_servo();
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
    if (weather_busy || ota_busy) return;
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
    if (!display_set_power_state(DISPLAY_AWAKE)) return;
    if (!screensaver) return;
    lv_obj_delete_async(screensaver);
    screensaver = screensaver_panel = screensaver_time = screensaver_date = screensaver_weather = NULL;
    memset(screensaver_hour_labels, 0, sizeof(screensaver_hour_labels));
    memset(screensaver_day_labels, 0, sizeof(screensaver_day_labels));
}

static void screensaver_touched(lv_event_t *event)
{
    (void)event;
    lv_display_trigger_activity(NULL);
    screensaver_close();
}

static const char *screensaver_symbol(uint8_t code)
{
    if (code <= 1) return "\\ | /\n--O--\n/ | \\";
    if (code <= 3 || code == 45 || code == 48) return " .--.\n(___)\n     ";
    return " .--.\n(___)\n /// ";
}

static void screensaver_disable_child_hits(lv_obj_t *parent)
{
    uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        lv_obj_remove_flag(child, LV_OBJ_FLAG_CLICKABLE);
        screensaver_disable_child_hits(child);
    }
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
        govee_reading_t indoor;
        char name[sizeof(govee_name)], address[sizeof(govee_address)];
        int8_t rssi;
        time_t updated;
        if (govee_snapshot(&indoor, name, sizeof(name), address, sizeof(address), &rssi, &updated) && now - updated < 300)
            lv_label_set_text_fmt(screensaver_weather, "%d F  |  %s\n%s\nIndoor %d F  %d%% RH  |  BAT %d%%",
                weather_round(weather_data.temperature), weather_condition(weather_data.code), weather_data.place,
                weather_round(indoor.temperature_c * 9.0f / 5.0f + 32.0f), weather_round(indoor.humidity), battery_percent);
        else
            lv_label_set_text_fmt(screensaver_weather, "%d F  |  %s\n%s\nBAT %d%%",
                weather_round(weather_data.temperature), weather_condition(weather_data.code),
                weather_data.place, battery_percent);
        for (size_t i = 0; i < SCREENSAVER_FORECAST_ITEMS; i++) {
            char hour[12];
            weather_short_time(weather_data.hourly[i].time, hour);
            lv_label_set_text_fmt(screensaver_hour_labels[i], "%s\n%s\n%d F\n%u%% rain", hour,
                screensaver_symbol(weather_data.hourly[i].code), weather_round(weather_data.hourly[i].temperature),
                (unsigned)weather_data.hourly[i].precipitation);
            weather_day_t *day = &weather_data.daily[i];
            lv_label_set_text_fmt(screensaver_day_labels[i], "%c%c/%c%c\n%s\n%d/%d F\n%u%% rain",
                day->date[5], day->date[6], day->date[8], day->date[9], screensaver_symbol(day->code),
                weather_round(day->high), weather_round(day->low), (unsigned)day->precipitation);
        }
    } else {
        lv_label_set_text(screensaver_weather, wifi_connected ? "Loading Milwaukee weather..." :
            "Milwaukee weather needs Wi-Fi");
        for (size_t i = 0; i < SCREENSAVER_FORECAST_ITEMS; i++) {
            lv_label_set_text(screensaver_hour_labels[i], "--\n\n-- F");
            lv_label_set_text(screensaver_day_labels[i], "--/--\n\n--/-- F");
        }
    }
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
    lv_obj_set_size(screensaver_panel, 690, 1040);
    lv_obj_align(screensaver_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(screensaver_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screensaver_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screensaver_panel, 10, 0);
    screensaver_time = lv_label_create(screensaver_panel);
    lv_obj_set_style_text_font(screensaver_time, &lv_font_montserrat_48, 0);
    screensaver_date = lv_label_create(screensaver_panel);
    lv_obj_set_style_text_font(screensaver_date, &lv_font_montserrat_28, 0);
    screensaver_weather = lv_label_create(screensaver_panel);
    lv_obj_set_width(screensaver_weather, 660);
    lv_obj_set_style_text_font(screensaver_weather, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(screensaver_weather, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *heading = lv_label_create(screensaver_panel);
    lv_label_set_text(heading, "HOURLY");
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_28, 0);
    lv_obj_t *row = lv_obj_create(screensaver_panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 680, 205);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (size_t i = 0; i < SCREENSAVER_FORECAST_ITEMS; i++) {
        lv_obj_t *card = lv_obj_create(row);
        lv_obj_set_size(card, 128, 200);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(card, 4, 0);
        screensaver_hour_labels[i] = lv_label_create(card);
        lv_obj_set_width(screensaver_hour_labels[i], 116);
        lv_obj_set_style_text_align(screensaver_hour_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(screensaver_hour_labels[i]);
    }

    heading = lv_label_create(screensaver_panel);
    lv_label_set_text(heading, "DAILY");
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_28, 0);
    row = lv_obj_create(screensaver_panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 680, 205);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (size_t i = 0; i < SCREENSAVER_FORECAST_ITEMS; i++) {
        lv_obj_t *card = lv_obj_create(row);
        lv_obj_set_size(card, 128, 200);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(card, 4, 0);
        screensaver_day_labels[i] = lv_label_create(card);
        lv_obj_set_width(screensaver_day_labels[i], 116);
        lv_obj_set_style_text_align(screensaver_day_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(screensaver_day_labels[i]);
    }
    lv_obj_t *source = lv_label_create(screensaver_panel);
    lv_label_set_text(source, "Weather data: Open-Meteo  |  Touch to wake");
    screensaver_disable_child_hits(screensaver);
    screensaver_update();
    display_set_power_state(DISPLAY_DIMMED);
    if (wifi_connected && (!weather_has_data || time(NULL) - weather_fetched_at > 15 * 60))
        weather_start(weather_location);
}

static void screensaver_tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t inactive = lv_display_get_inactive_time(NULL);
    bool inhibited = alarm_active || ota_busy || ota_done;
    if (inhibited) {
        if (screensaver || display_power_state != DISPLAY_AWAKE) {
            lv_display_trigger_activity(NULL);
            screensaver_close();
        }
        return;
    }
    if (screensaver) {
        if (inactive < 1000) {
            screensaver_close();
        } else if (display_timeout_elapsed(inactive, screen_timeout_seconds, false)) {
            if (display_power_state != DISPLAY_OFF) display_set_power_state(DISPLAY_OFF);
        } else {
            if (display_power_state != DISPLAY_DIMMED) display_set_power_state(DISPLAY_DIMMED);
            screensaver_update();
        }
    } else if (inactive >= SCREENSAVER_IDLE_MS) {
        screensaver_show();
    }
}

static const app_definition_t launcher_apps[] = {
    {LV_SYMBOL_DIRECTORY, "Files", 0x2196F3, files_clicked, NULL, 0, 0},
    {LV_SYMBOL_EDIT, "Notes", 0x00A896, notes_clicked, NULL, 1, 0},
    {LV_SYMBOL_PLUS, "Counter", 0xF59E0B, counter_clicked, NULL, 2, 0},
    {LV_SYMBOL_CHARGE, "GPIO", 0xE65100, gpio_clicked, NULL, 3, 0},
    {LV_SYMBOL_WIFI, "Settings", 0x0288D1, settings_clicked, settings_leave, 0, 1},
    {LV_SYMBOL_ENVELOPE, "AI Chat", 0xE91E63, chat_clicked, NULL, 1, 1},
    {LV_SYMBOL_EYE_OPEN, "Browser", 0x3F51B5, browser_clicked, NULL, 2, 1},
    {LV_SYMBOL_FILE, "Ebooks", 0x8D6E63, ebooks_clicked, NULL, 3, 1},
    {LV_SYMBOL_LOOP, "Clock", 0x009688, clock_clicked, NULL, 0, 2},
    {LV_SYMBOL_SETTINGS, "System", 0x7C4DFF, system_clicked, NULL, 1, 2},
    {LV_SYMBOL_BARS, "Scope", 0x00897B, scope_clicked, scope_release, 2, 2},
    {LV_SYMBOL_REFRESH, "Weather", 0x039BE5, weather_clicked, NULL, 3, 2},
    {LV_SYMBOL_BLUETOOTH, "Govee", 0x26A69A, govee_clicked, NULL, 0, 3},
    {LV_SYMBOL_BLUETOOTH, "Ring", 0x7E57C2, ring_clicked, NULL, 1, 3},
    {LV_SYMBOL_PLAY, "Servo Toy", 0xEF6C00, servo_clicked, NULL, 2, 3},
    {LV_SYMBOL_CHARGE, "Cycling", 0x1565C0, cycling_clicked, NULL, 3, 3},
    {LV_SYMBOL_LIST, "I2C Tool", 0x00838F, i2c_clicked, NULL, 0, 4},
    {LV_SYMBOL_CALL, "Serial", 0x5E35B1, uart_clicked, NULL, 1, 4},
    {LV_SYMBOL_SHUFFLE, "SPI Master", 0xAD4B00, spi_clicked, NULL, 2, 4},
    {LV_SYMBOL_TINT, "Signal Gen", 0xC62828, signal_clicked, NULL, 3, 4},
    {LV_SYMBOL_DOWNLOAD, "HTTP Tool", 0x00695C, http_clicked, http_tool_stop, 0, 5},
    {LV_SYMBOL_WIFI, "MQTT", 0x455A64, mqtt_clicked, mqtt_tool_stop, 1, 5},
    {LV_SYMBOL_BLUETOOTH, "BLE GATT", 0x6A4C93, ble_clicked, ble_tool_stop, 2, 5},
    {LV_SYMBOL_DRIVE, "Ender 3", 0x1B5E20, ender3_clicked, ender3_tool_stop, 0, 6},
};

static void launcher_app_clicked(lv_event_t *event)
{
    const app_definition_t *app = lv_event_get_user_data(event);
    app->enter(event);
    active_app_leave = app->leave;
}

static void show_launcher(void)
{
    static int32_t columns[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t rows[] = {
        215, 215, 215, 215, 215, 215, 215,
        LV_GRID_TEMPLATE_LAST};
    clear_content();
    lv_obj_set_grid_dsc_array(content, columns, rows);
    lv_obj_set_style_pad_row(content, 12, 0);
    lv_obj_set_style_pad_column(content, 12, 0);
    for (size_t i = 0; i < sizeof(launcher_apps) / sizeof(launcher_apps[0]); i++) {
        const app_definition_t *app = &launcher_apps[i];
        app_icon(content, app->symbol, app->name, app->color, launcher_app_clicked,
                 (void *)app, app->column, app->row);
    }
}

static void validate_running_ota(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) return;
    if (nvs_init_error != ESP_OK) {
        ESP_LOGE("tab5-os", "OTA image not validated because NVS failed: %s", esp_err_to_name(nvs_init_error));
        return;
    }
    if (!internal_ready) {
        ESP_LOGE("tab5-os", "OTA image not validated because internal storage failed: %s",
                 esp_err_to_name(storage_init_error));
        return;
    }
    esp_err_t error = esp_ota_mark_app_valid_cancel_rollback();
    if (error == ESP_OK) {
        char result[64];
        snprintf(result, sizeof(result), "Installed %s", esp_app_get_description()->version);
        ota_record_result(result);
        ESP_LOGI("tab5-os", "OTA image validated after health window");
    }
    else ESP_LOGE("tab5-os", "OTA validation failed: %s", esp_err_to_name(error));
}

static void confirm_running_ota(lv_timer_t *timer)
{
    lv_timer_delete(timer);
    ota_health_window_elapsed = true;
    validate_running_ota();
}

void app_main(void)
{
    scope_self_test();
    i2c_self_test();
    alarm_self_test();
    weather_self_test();
    browser_self_test();
    govee_self_test();
    sd_self_test();
    ring_self_test();
    kickr_self_test();
    servo_self_test();
    system_self_test();
    display_self_test();
    uart_tool_self_test();
    ender3_tool_self_test();
    spi_tool_self_test();
    signal_tool_self_test();
    network_tool_self_test();
    http_tool_self_test();
    mqtt_tool_self_test();
    ble_tool_self_test();
    ota_manifest_self_test();
    ESP_LOGI("tab5-os", "Starting Tab5 OS");
    ESP_ERROR_CHECK(bsp_i2c_init());
    bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
    bsp_set_charge_qc_en(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    bsp_set_charge_en(true);
    battery_init(bsp_i2c_get_handle());
    clock_init(bsp_i2c_get_handle());
    vTaskDelay(pdMS_TO_TICKS(250));

    lv_display_t *display = bsp_display_start();
    if (!display) {
        ESP_LOGE("tab5-os", "Display initialization failed");
        return;
    }
    wifi_ready = start_wifi();
    load_display_settings();
    load_scope_calibration();
    ota_load_result();
    load_alarms();
    load_weather_location();
    load_chat_config();

    internal_ready = mount_internal();
    sd_ready = bsp_sdcard_init(SD_PATH, 5) == ESP_OK;

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
    ring_hr_queue_storage = heap_caps_malloc(320 * sizeof(ring_hr_sample_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ring_hr_queue_storage)
        ring_hr_samples = xQueueCreateStatic(320, sizeof(ring_hr_sample_t), ring_hr_queue_storage,
                                             &ring_hr_queue_control);
    if (!ring_hr_samples) ESP_LOGE("ring", "Could not create heart-rate history queue");
    if (wifi_ready) govee_start();
    lv_timer_create(ring_health_tick, 1000, NULL);
    lv_timer_create(cycling_tick, 1000, NULL);
    lv_timer_create(confirm_running_ota, OTA_HEALTH_WINDOW_MS, NULL);

    bsp_display_unlock();
    display_set_power_state(DISPLAY_AWAKE);
    mkdir(SD_PATH "/BOOKS", 0775);
    ebook_start_default_downloads();
}
