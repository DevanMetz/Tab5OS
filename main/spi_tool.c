#include "spi_tool.h"

#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"

#define SPI_TOOL_HOST SPI2_HOST
#define SPI_TOOL_MOSI GPIO_NUM_18
#define SPI_TOOL_MISO GPIO_NUM_19
#define SPI_TOOL_SCLK GPIO_NUM_5
#define SPI_TOOL_CS GPIO_NUM_45
#define SPI_TOOL_MAX_BYTES 32

static const int speed_choices[] = {100000, 1000000, 5000000, 10000000};

static spi_device_handle_t device;
static bool bus_initialized;
static unsigned mode_index;
static unsigned speed_index = 1;
static lv_obj_t *mode_label;
static lv_obj_t *speed_label;
static lv_obj_t *start_label;
static lv_obj_t *input_area;
static lv_obj_t *result_area;
static lv_obj_t *status_label;

_Static_assert(SPI_TOOL_MOSI != SPI_TOOL_MISO && SPI_TOOL_MOSI != SPI_TOOL_SCLK &&
               SPI_TOOL_MOSI != SPI_TOOL_CS && SPI_TOOL_MISO != SPI_TOOL_SCLK &&
               SPI_TOOL_MISO != SPI_TOOL_CS && SPI_TOOL_SCLK != SPI_TOOL_CS,
               "SPI tool pins must be unique");

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
        if (high < 0 || low < 0 || (text[2] && !isspace((unsigned char)text[2])) ||
            count == capacity)
            return false;
        bytes[count++] = (uint8_t)((high << 4) | low);
        text += 2;
    }
    *length = count;
    return count > 0;
}

void spi_tool_self_test(void)
{
    uint8_t bytes[SPI_TOOL_MAX_BYTES];
    size_t length = 0;
    assert(parse_hex_bytes("00 ff A5", bytes, sizeof(bytes), &length));
    assert(length == 3 && bytes[0] == 0x00 && bytes[1] == 0xff && bytes[2] == 0xa5);
    assert(!parse_hex_bytes("", bytes, sizeof(bytes), &length));
    assert(!parse_hex_bytes("0", bytes, sizeof(bytes), &length));
    assert(!parse_hex_bytes("0011", bytes, sizeof(bytes), &length));
    assert(!parse_hex_bytes("gg", bytes, sizeof(bytes), &length));
    assert(parse_hex_bytes("00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f "
                           "10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f",
                           bytes, sizeof(bytes), &length));
    assert(length == SPI_TOOL_MAX_BYTES);
    assert(!parse_hex_bytes("00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f "
                            "10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f 20",
                            bytes, sizeof(bytes), &length));
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

static void release_pins(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << SPI_TOOL_MOSI) | (1ULL << SPI_TOOL_MISO) |
                        (1ULL << SPI_TOOL_SCLK) | (1ULL << SPI_TOOL_CS),
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);
}

bool spi_tool_busy(void)
{
    return bus_initialized || device != NULL;
}

static void update_controls(void)
{
    if (mode_label) lv_label_set_text_fmt(mode_label, "Mode\n%u", mode_index);
    if (speed_label) {
        int speed = speed_choices[speed_index];
        if (speed < 1000000)
            lv_label_set_text_fmt(speed_label, "Clock\n%d kHz", speed / 1000);
        else
            lv_label_set_text_fmt(speed_label, "Clock\n%d MHz", speed / 1000000);
    }
    if (start_label) lv_label_set_text(start_label, spi_tool_busy() ? "STOP" : "START");
}

static esp_err_t stop_bus(void)
{
    esp_err_t first_error = ESP_OK;
    if (device) {
        esp_err_t error = spi_bus_remove_device(device);
        if (error == ESP_OK)
            device = NULL;
        else
            first_error = error;
    }
    if (!device && bus_initialized) {
        esp_err_t error = spi_bus_free(SPI_TOOL_HOST);
        if (error == ESP_OK)
            bus_initialized = false;
        else if (first_error == ESP_OK)
            first_error = error;
    }
    if (!device && !bus_initialized) release_pins();
    update_controls();
    return first_error;
}

static esp_err_t start_bus(void)
{
    if (spi_tool_busy()) return ESP_ERR_INVALID_STATE;

    esp_err_t error = gpio_set_level(SPI_TOOL_CS, 1);
    if (error == ESP_OK) error = gpio_set_direction(SPI_TOOL_CS, GPIO_MODE_OUTPUT);
    if (error != ESP_OK) {
        release_pins();
        return error;
    }

    spi_bus_config_t bus_config = {
        .mosi_io_num = SPI_TOOL_MOSI,
        .miso_io_num = SPI_TOOL_MISO,
        .sclk_io_num = SPI_TOOL_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = SPI_TOOL_MAX_BYTES,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
    };
    error = spi_bus_initialize(SPI_TOOL_HOST, &bus_config, SPI_DMA_DISABLED);
    if (error != ESP_OK) {
        release_pins();
        return error;
    }
    bus_initialized = true;

    spi_device_interface_config_t device_config = {
        .mode = mode_index,
        .clock_source = SPI_CLK_SRC_DEFAULT,
        .clock_speed_hz = speed_choices[speed_index],
        .spics_io_num = SPI_TOOL_CS,
        .queue_size = 1,
    };
    error = spi_bus_add_device(SPI_TOOL_HOST, &device_config, &device);
    if (error != ESP_OK) {
        esp_err_t cleanup_error = stop_bus();
        if (cleanup_error != ESP_OK) return cleanup_error;
    }
    return error;
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

static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, 640, 76);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return container;
}

static void start_clicked(lv_event_t *event)
{
    (void)event;
    if (spi_tool_busy()) {
        esp_err_t error = stop_bus();
        if (error == ESP_OK)
            set_status("SPI stopped; G18/G19/G5/G45 released");
        else
            set_status("SPI stop failed: %s", esp_err_to_name(error));
        return;
    }
    esp_err_t error = start_bus();
    update_controls();
    if (error == ESP_OK)
        set_status("SPI active; CS G45 stays high between transfers");
    else
        set_status("SPI start failed: %s", esp_err_to_name(error));
}

static void setting_clicked(lv_event_t *event)
{
    if (spi_tool_busy()) {
        set_status("Stop SPI before changing mode or clock");
        return;
    }
    uintptr_t setting = (uintptr_t)lv_event_get_user_data(event);
    if (setting == 0)
        mode_index = (mode_index + 1) % 4;
    else
        speed_index = (speed_index + 1) % (sizeof(speed_choices) / sizeof(speed_choices[0]));
    update_controls();
    set_status("Settings selected; press START to apply");
}

static void clear_clicked(lv_event_t *event)
{
    (void)event;
    if (result_area) lv_textarea_set_text(result_area, "");
    set_status("Result cleared");
}

static void transfer_clicked(lv_event_t *event)
{
    (void)event;
    if (!device) {
        set_status("Press START before transferring");
        return;
    }

    uint8_t tx[SPI_TOOL_MAX_BYTES];
    uint8_t rx[SPI_TOOL_MAX_BYTES] = {0};
    size_t length = 0;
    if (!parse_hex_bytes(lv_textarea_get_text(input_area), tx, sizeof(tx), &length)) {
        set_status("Enter 1-32 whitespace-separated byte pairs, such as 9F 00 00 00");
        return;
    }

    spi_transaction_t transaction = {
        .length = length * 8,
        .rxlength = length * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t error = spi_device_transmit(device, &transaction);
    if (error != ESP_OK) {
        set_status("SPI transfer failed: %s", esp_err_to_name(error));
        return;
    }

    char result[256];
    size_t used = (size_t)snprintf(result, sizeof(result), "TX (%u):", (unsigned)length);
    for (size_t i = 0; i < length && used < sizeof(result); i++)
        used += (size_t)snprintf(result + used, sizeof(result) - used, " %02X", tx[i]);
    if (used < sizeof(result)) used += (size_t)snprintf(result + used, sizeof(result) - used, "\nRX (%u):", (unsigned)length);
    for (size_t i = 0; i < length && used < sizeof(result); i++)
        used += (size_t)snprintf(result + used, sizeof(result) - used, " %02X", rx[i]);
    lv_textarea_set_text(result_area, result);
    set_status("Transferred %u byte%s; CS returned high", (unsigned)length,
               length == 1 ? "" : "s");
}

void spi_tool_show(lv_obj_t *parent)
{
    lv_obj_set_style_pad_row(parent, 10, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "SPI Master Console");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *help = lv_label_create(parent);
    lv_obj_set_width(help, 640);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(help, "M5-Bus: MOSI G18, MISO G19, SCK G5, CS G45; 3.3 V logic only.\n"
                            "Share ground. Pins are high-impedance until START and after STOP/Home.\n"
                            "Each full-duplex transfer is limited to 32 bytes; verify target voltage and mode first.");

    lv_obj_t *settings = row(parent);
    small_button(settings, "", 200, setting_clicked, (void *)0, &mode_label);
    small_button(settings, "", 200, setting_clicked, (void *)1, &speed_label);
    small_button(settings, "", 200, start_clicked, NULL, &start_label);

    input_area = lv_textarea_create(parent);
    lv_obj_set_size(input_area, 640, 76);
    lv_textarea_set_one_line(input_area, true);
    lv_textarea_set_max_length(input_area, SPI_TOOL_MAX_BYTES * 3 - 1);
    lv_textarea_set_accepted_chars(input_area, "0123456789abcdefABCDEF ");
    lv_textarea_set_placeholder_text(input_area, "TX bytes, for example: 9F 00 00 00");

    lv_obj_t *actions = row(parent);
    small_button(actions, "TRANSFER", 310, transfer_clicked, NULL, NULL);
    small_button(actions, "CLEAR RESULT", 310, clear_clicked, NULL, NULL);

    result_area = lv_textarea_create(parent);
    lv_obj_set_size(result_area, 640, 220);
    lv_textarea_set_text(result_area, "");
    lv_textarea_set_cursor_click_pos(result_area, false);

    lv_obj_t *keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(keyboard, 640, 330);
    lv_keyboard_set_textarea(keyboard, input_area);

    status_label = lv_label_create(parent);
    lv_obj_set_width(status_label, 640);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label, "Ready; select mode/clock, then press START");
    update_controls();
}

void spi_tool_stop(void)
{
    esp_err_t error = stop_bus();
    if (error != ESP_OK) ESP_LOGE("spi-tool", "Could not release SPI bus: %s", esp_err_to_name(error));
    mode_label = NULL;
    speed_label = NULL;
    start_label = NULL;
    input_area = NULL;
    result_area = NULL;
    status_label = NULL;
}
