#include "uart_tool.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage_io.h"

#define UART_TOOL_PORT UART_NUM_1
#define UART_TOOL_TX GPIO_NUM_47
#define UART_TOOL_RX GPIO_NUM_48
#define RS485_TOOL_TX GPIO_NUM_20
#define RS485_TOOL_RX GPIO_NUM_21
#define RS485_TOOL_DIR GPIO_NUM_34
#define UART_TOOL_RX_BUFFER 2048
#define UART_TOOL_OUTPUT_CAPACITY 8192
#define UART_TOOL_HISTORY_COUNT 8
#define UART_TOOL_MESSAGE_CAPACITY 256
#define UART_TOOL_FLUSH_MS 10000
#define UART_TOOL_PATH "/sdcard/UART"
#define RS485_TOOL_PATH "/sdcard/RS485"

static const int baud_choices[] = {9600, 38400, 115200, 230400, 921600};
static const uart_parity_t parity_choices[] = {
    UART_PARITY_DISABLE, UART_PARITY_EVEN, UART_PARITY_ODD,
};
static const uart_stop_bits_t stop_choices[] = {UART_STOP_BITS_1, UART_STOP_BITS_2};
static const char *const parity_names[] = {"None", "Even", "Odd"};
static const char *const stop_names[] = {"1", "2"};

static lv_obj_t *output_area;
static lv_obj_t *input_area;
static lv_obj_t *status_label;
static lv_obj_t *title_label;
static lv_obj_t *help_label;
static lv_obj_t *interface_label;
static lv_obj_t *start_label;
static lv_obj_t *baud_label;
static lv_obj_t *parity_label;
static lv_obj_t *stop_label;
static lv_obj_t *view_label;
static lv_obj_t *log_label;
static lv_timer_t *receive_timer;
static char *output_text;
static bool driver_running;
static bool rs485_interface;
static bool hex_view;
static bool sd_available;
static unsigned baud_index = 2;
static unsigned parity_index;
static unsigned stop_index;
static FILE *log_file;
static char log_temporary_path[96];
static char log_final_path[96];
static TickType_t log_last_flush_tick;
static uart_tool_storage_error_cb_t storage_error_cb;
static char history[UART_TOOL_HISTORY_COUNT][UART_TOOL_MESSAGE_CAPACITY];
static unsigned history_count;
static unsigned history_next;
static unsigned history_cursor;

static void update_controls(void);

static int hex_digit(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    character = (char)tolower((unsigned char)character);
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

static bool parse_hex_bytes(const char *text, uint8_t *bytes, size_t capacity, size_t *length)
{
    size_t count = 0;
    while (*text) {
        while (isspace((unsigned char)*text)) text++;
        if (!*text) break;
        int high = hex_digit(text[0]);
        int low = text[1] ? hex_digit(text[1]) : -1;
        if (high < 0 || low < 0 || (text[2] && !isspace((unsigned char)text[2])) || count == capacity)
            return false;
        bytes[count++] = (uint8_t)((high << 4) | low);
        text += 2;
    }
    *length = count;
    return count > 0;
}

void uart_tool_self_test(void)
{
    uint8_t bytes[4];
    size_t length = 0;
    assert(parse_hex_bytes("01 aF 7e", bytes, sizeof(bytes), &length));
    assert(length == 3 && bytes[0] == 0x01 && bytes[1] == 0xaf && bytes[2] == 0x7e);
    assert(!parse_hex_bytes("1", bytes, sizeof(bytes), &length));
    assert(!parse_hex_bytes("001", bytes, sizeof(bytes), &length));
    assert(!parse_hex_bytes("zz", bytes, sizeof(bytes), &length));
    assert(!parse_hex_bytes("01 02 03 04 05", bytes, sizeof(bytes), &length));
    assert(!parse_hex_bytes("   ", bytes, sizeof(bytes), &length));
    assert(strcmp(UART_TOOL_PATH, RS485_TOOL_PATH) != 0);
}

static void set_status(const char *format, ...)
{
    if (!status_label) return;
    char text[160];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    lv_label_set_text(status_label, text);
}

static void append_output(const char *format, ...)
{
    if (!output_area || !output_text) return;
    size_t used = strlen(output_text);
    if (used > UART_TOOL_OUTPUT_CAPACITY / 2) {
        char *keep = strchr(output_text + UART_TOOL_OUTPUT_CAPACITY / 2, '\n');
        keep = keep ? keep + 1 : output_text + UART_TOOL_OUTPUT_CAPACITY / 2;
        memmove(output_text, keep, strlen(keep) + 1);
        used = strlen(output_text);
    }
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(output_text + used, UART_TOOL_OUTPUT_CAPACITY - used, format, arguments);
    va_end(arguments);
    lv_textarea_set_text(output_area, output_text);
    lv_textarea_set_cursor_pos(output_area, LV_TEXTAREA_CURSOR_LAST);
}

static void timestamp(char text[16])
{
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    snprintf(text, 16, "%02d:%02d:%02d.%03lld", local.tm_hour, local.tm_min, local.tm_sec,
             (long long)(esp_timer_get_time() / 1000) % 1000);
}

static void format_bytes(const uint8_t *bytes, size_t length, bool hex, char *text, size_t capacity)
{
    size_t used = 0;
    for (size_t i = 0; i < length && used + 4 < capacity; i++) {
        int written;
        if (hex) {
            written = snprintf(text + used, capacity - used, "%s%02X", i ? " " : "", bytes[i]);
        } else if (bytes[i] == '\r') {
            written = snprintf(text + used, capacity - used, "\\r");
        } else if (bytes[i] == '\n') {
            written = snprintf(text + used, capacity - used, "\\n");
        } else {
            text[used] = isprint(bytes[i]) ? (char)bytes[i] : '.';
            text[used + 1] = '\0';
            written = 1;
        }
        if (written < 0 || (size_t)written >= capacity - used) break;
        used += (size_t)written;
    }
}

static void log_bytes(const char *direction, const uint8_t *bytes, size_t length)
{
    if (!log_file) return;
    bool failed = fprintf(log_file, "%lld,%s,", (long long)time(NULL), direction) < 0;
    for (size_t i = 0; i < length && !failed; i++)
        failed = fprintf(log_file, "%s%02X", i ? " " : "", bytes[i]) < 0;
    if (!failed) failed = fputc('\n', log_file) == EOF;

    TickType_t now = xTaskGetTickCount();
    if (!failed && now - log_last_flush_tick >= pdMS_TO_TICKS(UART_TOOL_FLUSH_MS)) {
        failed = storage_sync_file(log_file) != 0;
        if (!failed) log_last_flush_tick = now;
    }
    if (!failed) return;

    int error = errno ? errno : EIO;
    fclose(log_file);
    log_file = NULL;
    if (storage_error_cb) storage_error_cb(error);
    const char *name = strrchr(log_temporary_path, '/');
    set_status("Log error: %s; retained %s", strerror(error), name ? name + 1 : log_temporary_path);
    update_controls();
}

static void show_bytes(const char *direction, const uint8_t *bytes, size_t length)
{
    char formatted[1024] = "";
    char stamp[16];
    timestamp(stamp);
    format_bytes(bytes, length, hex_view, formatted, sizeof(formatted));
    append_output("[%s] %s %s\n", stamp, direction, formatted);
    log_bytes(direction, bytes, length);
}

static void release_pins(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << UART_TOOL_TX) | (1ULL << UART_TOOL_RX) |
                        (1ULL << RS485_TOOL_TX) | (1ULL << RS485_TOOL_RX) |
                        (1ULL << RS485_TOOL_DIR),
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);
}

static esp_err_t start_driver(bool route_pins)
{
    uart_config_t config = {
        .baud_rate = baud_choices[baud_index],
        .data_bits = UART_DATA_8_BITS,
        .parity = parity_choices[parity_index],
        .stop_bits = stop_choices[stop_index],
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t error = uart_param_config(UART_TOOL_PORT, &config);
    bool installed = false;
    if (error == ESP_OK) {
        error = uart_driver_install(UART_TOOL_PORT, UART_TOOL_RX_BUFFER, 0, 0, NULL, 0);
        installed = error == ESP_OK;
    }
    if (error == ESP_OK)
        error = uart_set_mode(UART_TOOL_PORT, route_pins && rs485_interface ?
                             UART_MODE_RS485_HALF_DUPLEX : UART_MODE_UART);
    if (error == ESP_OK && route_pins && rs485_interface) {
        error = gpio_set_level(RS485_TOOL_DIR, 0);
        if (error == ESP_OK) error = gpio_set_direction(RS485_TOOL_DIR, GPIO_MODE_OUTPUT);
        if (error == ESP_OK)
            error = uart_set_pin(UART_TOOL_PORT, RS485_TOOL_TX, RS485_TOOL_RX,
                                 RS485_TOOL_DIR, UART_PIN_NO_CHANGE);
    } else if (error == ESP_OK && route_pins) {
        error = uart_set_pin(UART_TOOL_PORT, UART_TOOL_TX, UART_TOOL_RX,
                             UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (error != ESP_OK) {
        if (route_pins && rs485_interface) gpio_set_level(RS485_TOOL_DIR, 0);
        if (installed) uart_driver_delete(UART_TOOL_PORT);
        release_pins();
    }
    return error;
}

static void update_controls(void)
{
    if (title_label) lv_label_set_text(title_label, rs485_interface ? "Onboard RS-485 Terminal" :
                                                                       "UART1 Terminal");
    if (help_label) {
        if (rs485_interface) {
            lv_label_set_text(help_label, "J7: pin 1 GND, pin 3 A, pin 4 B. Leave VIN pin 2 disconnected on USB.\n"
                                          "The 120-ohm terminator is the physical switch; enable it only at a bus end.\n"
                                          "SIT3088 direction is automatic. Stop returns to receive-safe/high-impedance GPIO.");
        } else {
            lv_label_set_text(help_label, "M5-Bus TX G47 / RX G48, 3.3 V logic only. Share ground.\n"
                                          "Pins are high-impedance while stopped. Settings are 8 data bits, no flow control.");
        }
    }
    if (interface_label) lv_label_set_text(interface_label, rs485_interface ? "Interface\nRS-485" :
                                                                                 "Interface\nUART");
    if (start_label) lv_label_set_text(start_label, driver_running ? "STOP" : "START");
    if (baud_label) lv_label_set_text_fmt(baud_label, "Baud\n%d", baud_choices[baud_index]);
    if (parity_label) lv_label_set_text_fmt(parity_label, "Parity\n%s", parity_names[parity_index]);
    if (stop_label) lv_label_set_text_fmt(stop_label, "Stop\n%s", stop_names[stop_index]);
    if (view_label) lv_label_set_text(view_label, hex_view ? "VIEW\nHEX" : "VIEW\nASCII");
    if (log_label) lv_label_set_text(log_label, log_file ? "STOP\nLOG" : "START\nLOG");
}

static bool stop_log(void)
{
    if (!log_file) return true;
    int result = storage_commit_new_file(&log_file, log_temporary_path, log_final_path);
    int error = errno;
    const char *name = strrchr(result == 0 ? log_final_path : log_temporary_path, '/');
    if (result == 0) {
        set_status("Saved %s", name ? name + 1 : log_final_path);
    } else {
        if (storage_error_cb) storage_error_cb(error ? error : EIO);
        set_status("Publish failed: %s retained", name ? name + 1 : log_temporary_path);
    }
    update_controls();
    return result == 0;
}

static bool start_log(void)
{
    if (!sd_available) {
        set_status("SD card unavailable");
        return false;
    }
    struct tm local;
    time_t now = time(NULL);
    localtime_r(&now, &local);
    const char *root = rs485_interface ? RS485_TOOL_PATH : UART_TOOL_PATH;
    char directory[64];
    snprintf(directory, sizeof(directory), "%s/%02d%02d%02d", root,
             (local.tm_year + 1900) % 100, local.tm_mon + 1, local.tm_mday);
    if ((mkdir(root, 0775) != 0 && errno != EEXIST) ||
        (mkdir(directory, 0775) != 0 && errno != EEXIST)) {
        int error = errno;
        if (storage_error_cb) storage_error_cb(error);
        set_status("Could not create log folder: %s", strerror(error));
        return false;
    }

    for (unsigned suffix = 0; suffix < 10 && !log_file; suffix++) {
        char stem[9];
        snprintf(stem, sizeof(stem), "%c%02d%02d%02d%u", rs485_interface ? 'R' : 'U',
                 local.tm_hour, local.tm_min,
                 local.tm_sec, suffix);
        snprintf(log_temporary_path, sizeof(log_temporary_path), "%s/%s.TMP", directory, stem);
        snprintf(log_final_path, sizeof(log_final_path), "%s/%s.CSV", directory, stem);
        struct stat info;
        if (stat(log_final_path, &info) == 0 || errno != ENOENT) continue;
        int descriptor = open(log_temporary_path, O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (descriptor < 0) continue;
        log_file = fdopen(descriptor, "wb");
        if (!log_file) {
            int error = errno;
            close(descriptor);
            unlink(log_temporary_path);
            errno = error;
        }
    }
    if (!log_file) {
        int error = errno ? errno : EEXIST;
        if (storage_error_cb) storage_error_cb(error);
        set_status("Could not start log: %s", strerror(error));
        return false;
    }
    if (fputs("unix_time,direction,data_hex\n", log_file) < 0) {
        int error = errno ? errno : EIO;
        fclose(log_file);
        log_file = NULL;
        unlink(log_temporary_path);
        if (storage_error_cb) storage_error_cb(error);
        set_status("Could not write log header: %s", strerror(error));
        return false;
    }
    log_last_flush_tick = xTaskGetTickCount();
    const char *name = strrchr(log_temporary_path, '/');
    set_status("Logging to %s", name ? name + 1 : log_temporary_path);
    update_controls();
    return true;
}

static void receive_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!driver_running) return;
    uint8_t bytes[256];
    for (unsigned pass = 0; pass < 4; pass++) {
        size_t pending = 0;
        if (uart_get_buffered_data_len(UART_TOOL_PORT, &pending) != ESP_OK || !pending) break;
        if (pending > sizeof(bytes)) pending = sizeof(bytes);
        int read = uart_read_bytes(UART_TOOL_PORT, bytes, (uint32_t)pending, 0);
        if (read > 0) show_bytes("RX", bytes, (size_t)read);
    }
}

static bool stop_driver(void)
{
    if (!driver_running) return true;
    bool log_saved = stop_log();
    uart_wait_tx_done(UART_TOOL_PORT, pdMS_TO_TICKS(100));
    if (rs485_interface) gpio_set_level(RS485_TOOL_DIR, 0);
    uart_driver_delete(UART_TOOL_PORT);
    driver_running = false;
    release_pins();
    update_controls();
    return log_saved;
}

static void start_clicked(lv_event_t *event)
{
    (void)event;
    if (driver_running) {
        bool was_rs485 = rs485_interface;
        if (stop_driver())
            set_status(was_rs485 ? "RS-485 stopped; transceiver is receive-safe" :
                                   "UART stopped; G47/G48 released");
        return;
    }
    esp_err_t error = start_driver(true);
    if (error != ESP_OK) {
        set_status("Serial start failed: %s", esp_err_to_name(error));
        return;
    }
    driver_running = true;
    update_controls();
    set_status(rs485_interface ? "RS-485 listening on J7 A/B; direction changes automatically on send" :
                                 "UART1 active on TX G47 / RX G48 (3.3 V only)");
}

static void interface_clicked(lv_event_t *event)
{
    (void)event;
    if (driver_running) {
        set_status("Stop the serial interface before changing modes");
        return;
    }
    rs485_interface = !rs485_interface;
    update_controls();
    set_status(rs485_interface ? "RS-485 selected; check A/B and the physical termination switch" :
                                 "UART selected; use 3.3 V logic on G47/G48");
}

static void setting_clicked(lv_event_t *event)
{
    if (driver_running) {
        set_status("Stop the serial interface before changing line settings");
        return;
    }
    uintptr_t setting = (uintptr_t)lv_event_get_user_data(event);
    if (setting == 0) baud_index = (baud_index + 1) % (sizeof(baud_choices) / sizeof(baud_choices[0]));
    if (setting == 1) parity_index = (parity_index + 1) % (sizeof(parity_choices) / sizeof(parity_choices[0]));
    if (setting == 2) stop_index = (stop_index + 1) % (sizeof(stop_choices) / sizeof(stop_choices[0]));
    update_controls();
}

static void view_clicked(lv_event_t *event)
{
    (void)event;
    hex_view = !hex_view;
    update_controls();
    set_status(hex_view ? "Hex view: send whitespace-separated byte pairs" :
                          "ASCII view: send text exactly as entered");
}

static void clear_clicked(lv_event_t *event)
{
    (void)event;
    if (output_text) output_text[0] = '\0';
    if (output_area) lv_textarea_set_text(output_area, "");
}

static void log_clicked(lv_event_t *event)
{
    (void)event;
    if (!driver_running) {
        set_status("Start the serial interface before starting a log");
    } else if (log_file) {
        stop_log();
    } else {
        start_log();
    }
}

static void remember_message(const char *message)
{
    if (!message[0]) return;
    snprintf(history[history_next], sizeof(history[history_next]), "%s", message);
    history_next = (history_next + 1) % UART_TOOL_HISTORY_COUNT;
    if (history_count < UART_TOOL_HISTORY_COUNT) history_count++;
    history_cursor = history_next;
}

static void previous_clicked(lv_event_t *event)
{
    (void)event;
    if (!history_count || !input_area) {
        set_status("Send history is empty");
        return;
    }
    history_cursor = history_cursor == 0 ? UART_TOOL_HISTORY_COUNT - 1 : history_cursor - 1;
    lv_textarea_set_text(input_area, history[history_cursor]);
}

static void send_clicked(lv_event_t *event)
{
    (void)event;
    if (!driver_running) {
        set_status("Start the serial interface before sending");
        return;
    }
    const char *message = lv_textarea_get_text(input_area);
    uint8_t bytes[UART_TOOL_MESSAGE_CAPACITY];
    size_t length;
    if (hex_view) {
        if (!parse_hex_bytes(message, bytes, sizeof(bytes), &length)) {
            set_status("Hex input must be byte pairs such as 01 AF 7E");
            return;
        }
    } else {
        length = strlen(message);
        if (!length) {
            set_status("Enter text to send");
            return;
        }
        if (length > sizeof(bytes)) length = sizeof(bytes);
        memcpy(bytes, message, length);
    }
    int written = uart_write_bytes(UART_TOOL_PORT, bytes, length);
    if (written != (int)length) {
        set_status("Serial send failed");
        return;
    }
    remember_message(message);
    bool logging = log_file != NULL;
    show_bytes("TX", bytes, length);
    lv_textarea_set_text(input_area, "");
    if (!logging || log_file)
        set_status("Sent %u byte%s", (unsigned)length, length == 1 ? "" : "s");
}

static void loopback_clicked(lv_event_t *event)
{
    (void)event;
    if (driver_running) {
        set_status("Stop the serial interface before running a test");
        return;
    }
    if (rs485_interface) {
        set_status("RS-485 needs an external A/B peer or loop fixture; no data was sent");
        return;
    }
    const uint8_t expected[] = {0x55, 0xaa, 0x00, 0xff};
    uint8_t received[sizeof(expected)] = {0};
    esp_err_t error = start_driver(false);
    bool installed = error == ESP_OK;
    if (error == ESP_OK) error = uart_set_loop_back(UART_TOOL_PORT, true);
    if (error == ESP_OK && uart_write_bytes(UART_TOOL_PORT, expected, sizeof(expected)) != sizeof(expected))
        error = ESP_FAIL;
    if (error == ESP_OK) error = uart_wait_tx_done(UART_TOOL_PORT, pdMS_TO_TICKS(100));
    int count = error == ESP_OK ? uart_read_bytes(UART_TOOL_PORT, received, sizeof(received),
                                                 pdMS_TO_TICKS(100)) : 0;
    if (installed) {
        uart_set_loop_back(UART_TOOL_PORT, false);
        uart_driver_delete(UART_TOOL_PORT);
    }
    release_pins();
    if (error == ESP_OK && count == sizeof(received) && memcmp(expected, received, sizeof(expected)) == 0) {
        set_status("Internal loopback passed; no external pins were routed");
    } else {
        set_status("Internal loopback failed%s%s", error == ESP_OK ? "" : ": ",
                   error == ESP_OK ? "" : esp_err_to_name(error));
    }
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, int width,
                              lv_event_cb_t callback, void *user_data, lv_obj_t **label_out)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_size(control, width, 72);
    lv_obj_add_event_cb(control, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = lv_label_create(control);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    if (label_out) *label_out = label;
    return control;
}

static lv_obj_t *row(lv_obj_t *parent, int height)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, 640, height);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return container;
}

void uart_tool_show(lv_obj_t *parent, bool available,
                    uart_tool_storage_error_cb_t error_callback)
{
    sd_available = available;
    storage_error_cb = error_callback;
    output_text = heap_caps_calloc(1, UART_TOOL_OUTPUT_CAPACITY,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    lv_obj_set_style_pad_row(parent, 10, 0);
    title_label = lv_label_create(parent);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_28, 0);

    help_label = lv_label_create(parent);
    lv_obj_set_width(help_label, 640);
    lv_obj_set_style_text_align(help_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *settings = row(parent, 76);
    small_button(settings, "", 148, interface_clicked, NULL, &interface_label);
    small_button(settings, "", 148, setting_clicked, (void *)0, &baud_label);
    small_button(settings, "", 148, setting_clicked, (void *)1, &parity_label);
    small_button(settings, "", 148, setting_clicked, (void *)2, &stop_label);

    lv_obj_t *actions = row(parent, 76);
    small_button(actions, "", 118, start_clicked, NULL, &start_label);
    small_button(actions, "LINE\nTEST", 118, loopback_clicked, NULL, NULL);
    small_button(actions, "CLEAR", 118, clear_clicked, NULL, NULL);
    small_button(actions, "", 118, view_clicked, NULL, &view_label);
    small_button(actions, "", 118, log_clicked, NULL, &log_label);

    output_area = lv_textarea_create(parent);
    lv_obj_set_size(output_area, 640, 310);
    lv_obj_set_style_min_height(output_area, 310, 0);
    lv_textarea_set_text(output_area, "");
    lv_textarea_set_cursor_click_pos(output_area, false);

    lv_obj_t *send_row = row(parent, 76);
    input_area = lv_textarea_create(send_row);
    lv_obj_set_size(input_area, 400, 72);
    lv_textarea_set_one_line(input_area, true);
    lv_textarea_set_max_length(input_area, UART_TOOL_MESSAGE_CAPACITY);
    lv_textarea_set_placeholder_text(input_area, "Text, or byte pairs in hex view");
    small_button(send_row, "PREV", 100, previous_clicked, NULL, NULL);
    small_button(send_row, "SEND", 120, send_clicked, NULL, NULL);

    lv_obj_t *keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(keyboard, 640, 330);
    lv_keyboard_set_textarea(keyboard, input_area);

    status_label = lv_label_create(parent);
    lv_obj_set_width(status_label, 640);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label, output_text ? "Ready; press START to connect" :
                                                "Output buffer allocation failed");
    update_controls();
    receive_timer = lv_timer_create(receive_tick, 20, NULL);
}

bool uart_tool_busy(void)
{
    return driver_running || log_file;
}

void uart_tool_stop(void)
{
    if (receive_timer) {
        lv_timer_delete(receive_timer);
        receive_timer = NULL;
    }
    stop_driver();
    if (log_file) stop_log();
    release_pins();
    heap_caps_free(output_text);
    output_text = NULL;
    output_area = NULL;
    input_area = NULL;
    status_label = NULL;
    title_label = NULL;
    help_label = NULL;
    interface_label = NULL;
    start_label = NULL;
    baud_label = NULL;
    parity_label = NULL;
    stop_label = NULL;
    view_label = NULL;
    log_label = NULL;
    storage_error_cb = NULL;
}
