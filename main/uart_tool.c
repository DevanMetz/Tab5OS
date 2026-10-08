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
#include "uart_data.h"
#include "payload_clipboard.h"

#define UART_TOOL_PORT UART_NUM_1
#define UART_TOOL_TX GPIO_NUM_47
#define UART_TOOL_RX GPIO_NUM_48
#define RS485_TOOL_TX GPIO_NUM_20
#define RS485_TOOL_RX GPIO_NUM_21
#define RS485_TOOL_DIR GPIO_NUM_34
#define UART_TOOL_RX_BUFFER 2048
#define UART_TOOL_OUTPUT_CAPACITY 8192
#define UART_TOOL_HISTORY_COUNT 8
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
static lv_obj_t *keyboard;
static lv_obj_t *tx_mode_select;
static lv_obj_t *ending_select;
static lv_obj_t *tx_info_label;
static lv_obj_t *send_button;
static lv_obj_t *rx_capture_label;
static lv_obj_t *rx_copy_button;
static lv_obj_t *rx_info_label;
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
static uart_tx_data_t history[UART_TOOL_HISTORY_COUNT];
static unsigned history_count;
static unsigned history_next;
static unsigned history_offset;
static uart_tx_mode_t tx_mode;
static uart_tx_ending_t tx_ending;
/* LVGL counts Unicode characters. Retain enough bytes for the first excess
 * character too, so a rejected draft remains rejected after Home. */
static char ascii_draft[(UART_TX_MAX_BYTES + 1) * 4 + 1];
static char hex_draft[(UART_TX_MAX_HEX_TEXT + 1) * 4 + 1];
static bool loading_input;
static bool tx_dirty;

enum rx_capture_state { RX_CAPTURE_EMPTY, RX_CAPTURE_ACTIVE, RX_CAPTURE_FROZEN,
                        RX_CAPTURE_OVERFLOW, RX_CAPTURE_ERROR };
static enum rx_capture_state rx_capture_state;
static uint8_t rx_capture_bytes[PAYLOAD_CLIPBOARD_MAX_BYTES];
static size_t rx_capture_length;
static bool rx_capture_rs485;
static unsigned rx_capture_baud, rx_capture_parity, rx_capture_stop;
static bool rx_capture_dirty;

static const char *const hex_keys[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "\n",
    "8", "9", "A", "B", "C", "D", "E", "F", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t hex_controls[] = {
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    LV_BUTTONMATRIX_CTRL_CHECKED | 2, 6, LV_BUTTONMATRIX_CTRL_CHECKED | 2,
    LV_BUTTONMATRIX_CTRL_CHECKED | 3, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 3
};
_Static_assert(sizeof(hex_keys) / sizeof(hex_keys[0]) ==
               sizeof(hex_controls) / sizeof(hex_controls[0]) + 3,
               "Serial hex keyboard map and controls must match");

static void update_controls(void);
static void set_status(const char *format, ...);

static void render_rx_capture(void)
{
    rx_capture_dirty = false;
    if (!rx_info_label) return;
    if (rx_capture_label) lv_label_set_text(rx_capture_label,
        rx_capture_state == RX_CAPTURE_ACTIVE ? "FREEZE\nRX" : "CAPTURE\nRX");
    if (rx_copy_button) {
        if (rx_capture_state == RX_CAPTURE_FROZEN && rx_capture_length)
            lv_obj_remove_state(rx_copy_button, LV_STATE_DISABLED);
        else lv_obj_add_state(rx_copy_button, LV_STATE_DISABLED);
    }
    if (rx_capture_state == RX_CAPTURE_EMPTY) {
        lv_label_set_text(rx_info_label, "RX capture is empty. Start Serial, then CAPTURE RX before sending.\n"
                                       "Freeze to copy up to 128 bytes. Frame boundaries are not detected.");
        return;
    }
    const char *state = rx_capture_state == RX_CAPTURE_ACTIVE ? "Collecting" :
                        rx_capture_state == RX_CAPTURE_FROZEN ? "Frozen" :
                        rx_capture_state == RX_CAPTURE_OVERFLOW ? "Overflow" : "Read failed";
    const char *hint = rx_capture_state == RX_CAPTURE_ACTIVE ? "FREEZE RX ends this window. COPY RX is available after freezing." :
                       rx_capture_state == RX_CAPTURE_FROZEN ? "Raw bytes; verify that the window includes the entire intended reply." :
                       rx_capture_state == RX_CAPTURE_OVERFLOW ? "Window exceeded 128 bytes; no partial copy. Start a new capture." :
                       "RX could not be drained; no copy is available. Start a new capture.";
    static const char parity_letters[] = "NEO";
    lv_label_set_text_fmt(rx_info_label, "%s RX | %s %u 8%c%u | %u/128 bytes\n%s", state,
        rx_capture_rs485 ? "RS-485" : "UART", rx_capture_baud,
        parity_letters[rx_capture_parity], rx_capture_stop + 1,
        (unsigned)rx_capture_length, hint);
}

static void fail_rx_capture(enum rx_capture_state state)
{
    if (rx_capture_state != RX_CAPTURE_ACTIVE) return;
    memset(rx_capture_bytes, 0, sizeof(rx_capture_bytes));
    rx_capture_length = 0;
    rx_capture_state = state;
    rx_capture_dirty = true;
    set_status(state == RX_CAPTURE_OVERFLOW ?
        "RX capture exceeded 128 bytes and stopped. No partial window can be copied." :
        "RX capture stopped after a read/drain failure. Start a new capture to try again.");
}

static void capture_received(const uint8_t *bytes, size_t length)
{
    if (rx_capture_state != RX_CAPTURE_ACTIVE) return;
    if (length > sizeof(rx_capture_bytes) - rx_capture_length) {
        fail_rx_capture(RX_CAPTURE_OVERFLOW);
        return;
    }
    memcpy(rx_capture_bytes + rx_capture_length, bytes, length);
    rx_capture_length += length;
    rx_capture_dirty = true;
}

void uart_tool_self_test(void)
{
    uart_data_self_test();
    assert(strcmp(UART_TOOL_PATH, RS485_TOOL_PATH) != 0);
}

static uart_tx_status_t read_draft(uart_tx_data_t *data)
{
    return uart_tx_parse(lv_textarea_get_text(input_area), tx_mode,
                         tx_mode == UART_TX_HEX ? UART_TX_END_NONE : tx_ending, data);
}

static void render_tx_info(void)
{
    if (!input_area || !tx_info_label) return;
    tx_dirty = false;
    uart_tx_data_t data;
    uart_tx_status_t status = read_draft(&data);
    if (status == UART_TX_OK) {
        static const char *const suffix_names[] = {"no suffix", "LF appended", "CR appended", "CRLF appended"};
        lv_label_set_text_fmt(tx_info_label, "Ready: %u TX bytes | %s | %s. SEND transmits once.",
                              (unsigned)data.length, tx_mode == UART_TX_HEX ? "Hex" : "ASCII",
                              suffix_names[data.ending]);
    } else lv_label_set_text(tx_info_label, uart_tx_error(status));
    if (send_button) {
        if (driver_running && status == UART_TX_OK) lv_obj_remove_state(send_button, LV_STATE_DISABLED);
        else lv_obj_add_state(send_button, LV_STATE_DISABLED);
    }
}

static void keyboard_visible(bool visible)
{
    if (!keyboard || !output_area) return;
    if (visible) {
        lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(output_area, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(output_area, LV_OBJ_FLAG_HIDDEN);
    }
}

static void input_event(lv_event_t *event)
{
    if (loading_input) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED) {
        history_offset = 0;
        char *draft = tx_mode == UART_TX_HEX ? hex_draft : ascii_draft;
        size_t capacity = tx_mode == UART_TX_HEX ? sizeof(hex_draft) : sizeof(ascii_draft);
        snprintf(draft, capacity, "%s", lv_textarea_get_text(input_area));
        /* Bulk insertion emits one event per character. Validate at the next
         * existing display tick, avoiding hundreds of state animations. SEND
         * always validates the current text synchronously before writing. */
        tx_dirty = true;
    } else if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) keyboard_visible(true);
}

static void apply_tx_mode(void)
{
    loading_input = true;
    lv_textarea_set_max_length(input_area, tx_mode == UART_TX_HEX ? UART_TX_MAX_HEX_TEXT + 1 : UART_TX_MAX_BYTES + 1);
    lv_textarea_set_text(input_area, tx_mode == UART_TX_HEX ? hex_draft : ascii_draft);
    loading_input = false;
    lv_dropdown_set_selected(tx_mode_select, tx_mode);
    lv_dropdown_set_selected(ending_select, tx_mode == UART_TX_HEX ? UART_TX_END_NONE : tx_ending);
    if (tx_mode == UART_TX_HEX) lv_obj_add_state(ending_select, LV_STATE_DISABLED);
    else lv_obj_remove_state(ending_select, LV_STATE_DISABLED);
    lv_keyboard_set_mode(keyboard, tx_mode == UART_TX_HEX ? LV_KEYBOARD_MODE_USER_1 : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_text_font(keyboard, tx_mode == UART_TX_HEX ? &lv_font_montserrat_28 : &lv_font_montserrat_14, 0);
    render_tx_info();
}

static void tx_mode_changed(lv_event_t *event)
{
    (void)event;
    tx_mode = (uart_tx_mode_t)lv_dropdown_get_selected(tx_mode_select);
    apply_tx_mode();
}

static void ending_changed(lv_event_t *event)
{
    (void)event;
    if (tx_mode != UART_TX_ASCII) return;
    tx_ending = (uart_tx_ending_t)lv_dropdown_get_selected(ending_select);
    render_tx_info();
}

static void keyboard_done(lv_event_t *event)
{
    (void)event;
    keyboard_visible(false); /* Done never sends. */
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
    int error = 0;
    errno = 0;
    if (fprintf(log_file, "%lld,%s,", (long long)time(NULL), direction) < 0)
        error = errno ? errno : EIO;
    for (size_t i = 0; i < length && !error; i++) {
        errno = 0;
        if (fprintf(log_file, "%s%02X", i ? " " : "", bytes[i]) < 0)
            error = errno ? errno : EIO;
    }
    if (!error) {
        errno = 0;
        if (fputc('\n', log_file) == EOF) error = errno ? errno : EIO;
    }

    TickType_t now = xTaskGetTickCount();
    if (!error && now - log_last_flush_tick >= pdMS_TO_TICKS(UART_TOOL_FLUSH_MS)) {
        errno = 0;
        if (storage_sync_file(log_file) != 0) error = errno ? errno : EIO;
        else log_last_flush_tick = now;
    }
    if (!error) return;

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
    if (interface_label) lv_label_set_text(interface_label, rs485_interface ? "Link\nRS-485" : "Link\nUART");
    if (start_label) lv_label_set_text(start_label, driver_running ? "STOP" : "START");
    if (baud_label) lv_label_set_text_fmt(baud_label, "Baud\n%d", baud_choices[baud_index]);
    if (parity_label) lv_label_set_text_fmt(parity_label, "Parity\n%s", parity_names[parity_index]);
    if (stop_label) lv_label_set_text_fmt(stop_label, "Stop\n%s", stop_names[stop_index]);
    if (view_label) lv_label_set_text(view_label, hex_view ? "VIEW\nHEX" : "VIEW\nASCII");
    if (log_label) lv_label_set_text(log_label, log_file ? "STOP\nLOG" : "START\nLOG");
    render_tx_info();
    render_rx_capture();
}

static bool stop_log(void)
{
    if (!log_file) return true;
    errno = 0;
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
    int error = 0;
    errno = 0;
    if (mkdir(root, 0775) != 0 && errno != EEXIST) error = errno ? errno : EIO;
    if (!error) {
        errno = 0;
        if (mkdir(directory, 0775) != 0 && errno != EEXIST) error = errno ? errno : EIO;
    }
    if (error) {
        if (storage_error_cb) storage_error_cb(error);
        set_status("Could not create log folder: %s", strerror(error));
        return false;
    }

    error = EEXIST;
    for (unsigned suffix = 0; suffix < 10 && !log_file; suffix++) {
        char stem[9];
        snprintf(stem, sizeof(stem), "%c%02d%02d%02d%u", rs485_interface ? 'R' : 'U',
                 local.tm_hour, local.tm_min,
                 local.tm_sec, suffix);
        snprintf(log_temporary_path, sizeof(log_temporary_path), "%s/%s.TMP", directory, stem);
        snprintf(log_final_path, sizeof(log_final_path), "%s/%s.CSV", directory, stem);
        struct stat info;
        errno = 0;
        if (stat(log_final_path, &info) == 0) continue;
        if (errno != ENOENT) { error = errno ? errno : EIO; break; }
        errno = 0;
        int descriptor = open(log_temporary_path, O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (descriptor < 0) {
            error = errno ? errno : EIO;
            if (error == EEXIST) continue;
            break;
        }
        errno = 0;
        log_file = fdopen(descriptor, "wb");
        if (!log_file) {
            error = errno ? errno : EIO;
            close(descriptor);
            unlink(log_temporary_path);
            break;
        }
    }
    if (!log_file) {
        if (storage_error_cb) storage_error_cb(error);
        set_status("Could not start log: %s", strerror(error));
        return false;
    }
    errno = 0;
    if (fputs("unix_time,direction,data_hex\n", log_file) < 0) {
        error = errno ? errno : EIO;
        fclose(log_file);
        log_file = NULL;
        if (storage_error_cb) storage_error_cb(error);
        const char *name = strrchr(log_temporary_path, '/');
        set_status("Log header error: %s; retained %s", strerror(error), name ? name + 1 : log_temporary_path);
        return false;
    }
    log_last_flush_tick = xTaskGetTickCount();
    const char *name = strrchr(log_temporary_path, '/');
    set_status("Logging to %s", name ? name + 1 : log_temporary_path);
    update_controls();
    return true;
}

/* Drain a bounded amount for the display, capture boundary or Stop. Bytes
 * already queued before a new capture are shown/logged without being copied.
 * This is a read boundary, not a UART timestamp or protocol frame detector. */
static bool receive_available(void)
{
    uint8_t bytes[256];
    for (unsigned pass = 0; pass < 4; pass++) {
        size_t pending = 0;
        if (uart_get_buffered_data_len(UART_TOOL_PORT, &pending) != ESP_OK) {
            fail_rx_capture(RX_CAPTURE_ERROR);
            return false;
        }
        if (!pending) return true;
        if (pending > sizeof(bytes)) pending = sizeof(bytes);
        int read = uart_read_bytes(UART_TOOL_PORT, bytes, (uint32_t)pending, 0);
        if (read <= 0 || (size_t)read > pending) {
            fail_rx_capture(RX_CAPTURE_ERROR);
            return false;
        }
        capture_received(bytes, (size_t)read);
        show_bytes("RX", bytes, (size_t)read);
    }
    size_t remaining = 0;
    if (uart_get_buffered_data_len(UART_TOOL_PORT, &remaining) != ESP_OK) {
        fail_rx_capture(RX_CAPTURE_ERROR);
        return false;
    }
    return remaining == 0;
}

static void receive_tick(lv_timer_t *timer)
{
    (void)timer;
    if (tx_dirty) render_tx_info();
    if (driver_running) receive_available();
    if (rx_capture_dirty) render_rx_capture();
}

static void freeze_rx_capture(void)
{
    if (rx_capture_state != RX_CAPTURE_ACTIVE) return;
    if (driver_running && !receive_available()) fail_rx_capture(RX_CAPTURE_ERROR);
    if (rx_capture_state == RX_CAPTURE_ACTIVE) {
        rx_capture_state = RX_CAPTURE_FROZEN;
        if (rx_capture_length) set_status("RX window frozen at %u bytes. COPY RX sends it to the shared clipboard.",
                                          (unsigned)rx_capture_length);
        else set_status("RX window frozen empty. Start a new capture before the next reply.");
    }
    render_rx_capture();
}

static void capture_clicked(lv_event_t *event)
{
    (void)event;
    if (rx_capture_state == RX_CAPTURE_ACTIVE) {
        freeze_rx_capture();
        return;
    }
    if (!driver_running) {
        set_status("Start Serial before starting an RX capture. The previous capture is unchanged.");
        return;
    }
    if (!receive_available()) {
        set_status("RX backlog did not drain or a read failed. No new capture started; try again after it quiets.");
        return;
    }
    memset(rx_capture_bytes, 0, sizeof(rx_capture_bytes));
    rx_capture_length = 0;
    rx_capture_state = RX_CAPTURE_ACTIVE;
    rx_capture_rs485 = rs485_interface;
    rx_capture_baud = (unsigned)baud_choices[baud_index];
    rx_capture_parity = parity_index;
    rx_capture_stop = stop_index;
    render_rx_capture();
    set_status("RX capture started. Send your request, wait for its reply, then FREEZE RX and COPY RX.");
}

static void copy_rx_clicked(lv_event_t *event)
{
    (void)event;
    /* Revalidate even when a disabled button is invoked programmatically. */
    if (rx_capture_state != RX_CAPTURE_FROZEN || !rx_capture_length) {
        set_status("Freeze a nonempty RX window first. Overflow or failed reads cannot be copied.");
        return;
    }
    if (payload_clipboard_store(rx_capture_bytes, rx_capture_length))
        set_status("Copied %u RX bytes. Paste into Byte Lab or RTU Frames to inspect them.", (unsigned)rx_capture_length);
    else set_status("RX copy failed; the previous clipboard is unchanged.");
}

static void clear_rx_clicked(lv_event_t *event)
{
    (void)event;
    memset(rx_capture_bytes, 0, sizeof(rx_capture_bytes));
    rx_capture_length = 0;
    rx_capture_state = RX_CAPTURE_EMPTY;
    render_rx_capture();
    set_status("RX capture cleared. The transcript, shared clipboard and serial connection are unchanged.");
}

static bool stop_driver(void)
{
    if (!driver_running) return true;
    freeze_rx_capture();
    bool log_saved = stop_log();
    /* A write can return with bytes still in the FIFO. Bound the drain by one
     * maximum message at the actual framing/baud, plus a scheduler margin. */
    unsigned frame_bits = 1 + 8 + (parity_index ? 1 : 0) + (stop_index ? 2 : 1);
    unsigned baud = (unsigned)baud_choices[baud_index];
    unsigned drain_ms = (UART_TX_MAX_BYTES * frame_bits * 1000 + baud - 1) / baud + 20;
    esp_err_t drained = uart_wait_tx_done(UART_TOOL_PORT, pdMS_TO_TICKS(drain_ms));
    if (rs485_interface) gpio_set_level(RS485_TOOL_DIR, 0);
    uart_driver_delete(UART_TOOL_PORT);
    driver_running = false;
    release_pins();
    update_controls();
    if (drained != ESP_OK && log_saved)
        set_status("Serial stopped; TX drain failed (%s). The last message may be incomplete.", esp_err_to_name(drained));
    return log_saved && drained == ESP_OK;
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
    if (!receive_timer || !output_text) {
        set_status("Serial display resources unavailable. Return Home and reopen before starting.");
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
    set_status(hex_view ? "New RX/TX display lines use Hex. Transmit format is unchanged." :
                          "New RX/TX display lines use ASCII. Transmit format is unchanged.");
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

static void remember_message(const uart_tx_data_t *data)
{
    history[history_next] = *data;
    history_next = (history_next + 1) % UART_TOOL_HISTORY_COUNT;
    if (history_count < UART_TOOL_HISTORY_COUNT) history_count++;
    history_offset = 0;
}

static void previous_clicked(lv_event_t *event)
{
    (void)event;
    if (!history_count || !input_area) {
        set_status("Send history is empty");
        return;
    }
    unsigned index = (history_next + UART_TOOL_HISTORY_COUNT - 1 - history_offset) % UART_TOOL_HISTORY_COUNT;
    const uart_tx_data_t *data = &history[index];
    char *draft = data->mode == UART_TX_HEX ? hex_draft : ascii_draft;
    size_t capacity = data->mode == UART_TX_HEX ? sizeof(hex_draft) : sizeof(ascii_draft);
    if (!uart_tx_format_draft(data, draft, capacity)) return;
    tx_mode = data->mode;
    if (tx_mode == UART_TX_ASCII) tx_ending = data->ending;
    history_offset = (history_offset + 1) % history_count;
    apply_tx_mode();
    keyboard_visible(false);
    set_status("Restored %u transmitted bytes with their format/ending. Review before SEND.", (unsigned)data->length);
}

static void paste_clicked(lv_event_t *event)
{
    (void)event;
    const payload_clipboard_t *copy = payload_clipboard_peek();
    if (!copy || !copy->length) {
        set_status("Copy at least one byte in Byte Lab or UDP first. Serial draft unchanged.");
        return;
    }
    if (!payload_clipboard_hex(hex_draft, sizeof(hex_draft))) return;
    history_offset = 0;
    tx_mode = UART_TX_HEX;
    apply_tx_mode();
    keyboard_visible(false);
    set_status("Pasted %u bytes into Hex TX. No suffix, no transmission. Review, then SEND.", (unsigned)copy->length);
}

static void send_clicked(lv_event_t *event)
{
    (void)event;
    if (!driver_running) {
        set_status("Start the serial interface before sending");
        return;
    }
    uart_tx_data_t data;
    uart_tx_status_t status = read_draft(&data);
    if (status != UART_TX_OK) {
        set_status("%s", uart_tx_error(status));
        return;
    }
    int written = uart_write_bytes(UART_TOOL_PORT, data.bytes, data.length);
    bool logging = log_file != NULL;
    if (written != (int)data.length) {
        if (written > 0 && (size_t)written < data.length) {
            show_bytes("TX", data.bytes, (size_t)written);
            if (!logging || log_file)
                set_status("Only %d of %u bytes accepted. Draft kept; check the target before retrying.", written, (unsigned)data.length);
        } else set_status("Serial send failed. Draft kept; check the target before retrying.");
        return;
    }
    remember_message(&data);
    show_bytes("TX", data.bytes, data.length);
    lv_textarea_set_text(input_area, "");
    render_tx_info();
    keyboard_visible(false);
    if (!logging || log_file)
        set_status("Sent %u byte%s", (unsigned)data.length, data.length == 1 ? "" : "s");
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
    lv_obj_set_style_text_font(control, &lv_font_montserrat_28, 0);
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

static lv_obj_t *dropdown(lv_obj_t *parent, const char *caption, const char *options, lv_event_cb_t callback)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_size(block, 310, 82);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, 4, 0);
    lv_obj_t *label = lv_label_create(block);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_label_set_text(label, caption);
    lv_obj_t *select = lv_dropdown_create(block);
    lv_obj_set_size(select, 310, 60);
    lv_obj_set_style_text_font(select, &lv_font_montserrat_28, 0);
    lv_dropdown_set_options(select, options);
    lv_obj_t *list = lv_dropdown_get_list(select);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_28, LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(list, 20, LV_PART_MAIN);
    lv_obj_add_event_cb(select, callback, LV_EVENT_VALUE_CHANGED, NULL);
    return select;
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
    lv_obj_set_style_text_font(help_label, &lv_font_montserrat_14, 0);
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
    lv_obj_set_size(output_area, 640, 240);
    lv_obj_set_style_text_font(output_area, &lv_font_montserrat_14, 0);
    lv_textarea_set_text(output_area, "");
    lv_textarea_set_cursor_click_pos(output_area, false);

    lv_obj_t *capture_row = row(parent, 76);
    small_button(capture_row, "CAPTURE\nRX", 200, capture_clicked, NULL, &rx_capture_label);
    rx_copy_button = small_button(capture_row, "COPY RX", 200, copy_rx_clicked, NULL, NULL);
    small_button(capture_row, "CLEAR RX", 200, clear_rx_clicked, NULL, NULL);
    rx_info_label = lv_label_create(parent);
    lv_obj_set_size(rx_info_label, 640, 42);
    lv_obj_set_style_text_font(rx_info_label, &lv_font_montserrat_14, 0);

    lv_obj_t *tx_options = row(parent, 82);
    tx_mode_select = dropdown(tx_options, "Transmit format", "Literal ASCII\nHex byte pairs", tx_mode_changed);
    ending_select = dropdown(tx_options, "Append to ASCII", "None\nLF (0A)\nCR (0D)\nCRLF (0D 0A)", ending_changed);
    tx_info_label = lv_label_create(parent);
    lv_obj_set_size(tx_info_label, 640, 42);
    lv_obj_set_style_text_font(tx_info_label, &lv_font_montserrat_14, 0);

    input_area = lv_textarea_create(parent);
    lv_obj_set_size(input_area, 640, 96);
    lv_obj_set_style_text_font(input_area, &lv_font_montserrat_28, 0);
    lv_textarea_set_placeholder_text(input_area, "TX draft; line endings are explicit");
    lv_obj_add_event_cb(input_area, input_event, LV_EVENT_ALL, NULL);
    lv_obj_t *send_row = row(parent, 76);
    small_button(send_row, "PREV", 200, previous_clicked, NULL, NULL);
    small_button(send_row, "PASTE", 200, paste_clicked, NULL, NULL);
    send_button = small_button(send_row, "SEND", 200, send_clicked, NULL, NULL);

    keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(keyboard, 640, 240);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1, hex_keys, hex_controls);
    lv_keyboard_set_textarea(keyboard, input_area);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    status_label = lv_label_create(parent);
    lv_obj_set_size(status_label, 640, 42);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label, output_text ? "Ready; press START to connect" :
                                                "Output buffer allocation failed");
    receive_timer = lv_timer_create(receive_tick, 20, NULL);
    if (!receive_timer) lv_label_set_text(status_label, "Receive timer allocation failed. Return Home and reopen.");
    lv_obj_t *note = lv_label_create(parent);
    lv_obj_set_width(note, 640);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
    lv_label_set_text(note, "256 TX bytes including suffix; no escape expansion. VIEW changes new display lines only.\n"
                            "PREV restores sent bytes/format. PASTE replaces Hex TX. Drafts/history stay in RAM.");
    apply_tx_mode();
    update_controls();
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
    if (keyboard) lv_keyboard_set_textarea(keyboard, NULL);
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
    keyboard = tx_mode_select = ending_select = tx_info_label = send_button = NULL;
    rx_capture_label = rx_copy_button = rx_info_label = NULL;
    loading_input = false;
    tx_dirty = false;
    storage_error_cb = NULL;
}
