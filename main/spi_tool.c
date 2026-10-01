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
#include "payload_clipboard.h"

#define SPI_TOOL_HOST SPI2_HOST
#define SPI_TOOL_MOSI GPIO_NUM_18
#define SPI_TOOL_MISO GPIO_NUM_19
#define SPI_TOOL_SCLK GPIO_NUM_5
#define SPI_TOOL_CS GPIO_NUM_45
#define SPI_TOOL_MAX_BYTES 32
#define SPI_TOOL_MAX_TEXT (SPI_TOOL_MAX_BYTES * 3 - 1)

static const int speed_choices[] = {100000, 1000000, 5000000, 10000000};

static spi_device_handle_t device;
static bool bus_initialized;
static unsigned mode_index;
static unsigned speed_index = 1;
/* LVGL limits Unicode characters, not bytes. Keep the first excess character
 * too, so invalid or overlong input stays invalid after Home. */
static char saved_input[(SPI_TOOL_MAX_TEXT + 1) * 4 + 1];
static lv_obj_t *mode_label;
static lv_obj_t *speed_label;
static lv_obj_t *start_label;
static lv_obj_t *input_area;
static lv_obj_t *result_area;
static lv_obj_t *status_label;
static lv_obj_t *keyboard;
static lv_obj_t *copy_rx_button;
static lv_obj_t *identity_label;
/* The result owns its settings and bytes independently of the next draft.
 * length zero means there is no completed transfer available to copy. */
static struct {
    uint8_t tx[SPI_TOOL_MAX_BYTES], rx[SPI_TOOL_MAX_BYTES];
    size_t length;
    unsigned mode;
    int clock_hz;
} saved_transfer;

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
               "SPI hex keyboard map and controls must match");

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
    if (strlen(text) > SPI_TOOL_MAX_TEXT) return false;
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
#ifndef NDEBUG
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
#endif
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

static void keyboard_visible(bool visible)
{
    if (!keyboard) return;
    lv_keyboard_set_textarea(keyboard, visible ? input_area : NULL);
    if (visible) lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(input_area, LV_STATE_FOCUSED);
    }
}

static void input_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED) {
        snprintf(saved_input, sizeof(saved_input), "%s", lv_textarea_get_text(input_area));
    } else if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        keyboard_visible(true);
    }
}

static void keyboard_done(lv_event_t *event)
{
    (void)event;
    keyboard_visible(false); /* Done or Cancel only closes the editor. */
}

static void clear_tx_clicked(lv_event_t *event)
{
    (void)event;
    lv_textarea_set_text(input_area, "");
    keyboard_visible(false);
    set_status("TX draft cleared. The saved transfer and byte clipboard are unchanged.");
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
    lv_obj_set_style_text_font(control, &lv_font_montserrat_28, 0);
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

static void render_transfer(void)
{
    if (!result_area || !identity_label || !copy_rx_button) return;
    if (!saved_transfer.length) {
        lv_textarea_set_text(result_area, "");
        lv_label_set_text(identity_label, "No completed transfer saved.\nCOPY RX copies received bytes to Byte Lab.");
        lv_obj_add_state(copy_rx_button, LV_STATE_DISABLED);
        return;
    }
    lv_label_set_text_fmt(identity_label, "Saved: mode %u | %d Hz | %u bytes\nCS G45; RX has one byte per transmitted byte.",
        saved_transfer.mode, saved_transfer.clock_hz, (unsigned)saved_transfer.length);
    lv_obj_remove_state(copy_rx_button, LV_STATE_DISABLED);
    char result[256];
    size_t used = (size_t)snprintf(result, sizeof(result), "TX (%u):", (unsigned)saved_transfer.length);
    for (size_t i = 0; i < saved_transfer.length; i++)
        used += (size_t)snprintf(result + used, sizeof(result) - used, " %02X", saved_transfer.tx[i]);
    used += (size_t)snprintf(result + used, sizeof(result) - used, "\nRX (%u):", (unsigned)saved_transfer.length);
    for (size_t i = 0; i < saved_transfer.length; i++)
        used += (size_t)snprintf(result + used, sizeof(result) - used, " %02X", saved_transfer.rx[i]);
    lv_textarea_set_text(result_area, result);
}

static void clear_clicked(lv_event_t *event)
{
    (void)event;
    memset(&saved_transfer, 0, sizeof(saved_transfer));
    render_transfer();
    set_status("Saved result cleared. The byte clipboard and transmit draft are unchanged.");
}

static void copy_rx_clicked(lv_event_t *event)
{
    (void)event;
    if (!saved_transfer.length || saved_transfer.length > SPI_TOOL_MAX_BYTES) {
        set_status("Complete a transfer before copying RX bytes. The previous clipboard is unchanged.");
        return;
    }
    if (payload_clipboard_store(saved_transfer.rx, saved_transfer.length))
        set_status("Copied %u received bytes from the saved transfer. Paste in Byte Lab to inspect them.",
                   (unsigned)saved_transfer.length);
    else set_status("RX copy failed; the previous clipboard is unchanged.");
}

static void paste_clicked(lv_event_t *event)
{
    (void)event;
    const payload_clipboard_t *copy = payload_clipboard_peek();
    if (!copy) {
        set_status("Byte clipboard is empty. Copy bytes in Byte Lab or a UDP reply first.");
        return;
    }
    if (!copy->length || copy->length > SPI_TOOL_MAX_BYTES) {
        set_status("Clipboard has %u bytes. SPI needs 1-32; draft unchanged.", (unsigned)copy->length);
        return;
    }
    char hex[SPI_TOOL_MAX_TEXT + 1];
    if (!payload_clipboard_hex(hex, sizeof(hex))) return;
    lv_textarea_set_text(input_area, hex);
    keyboard_visible(false);
    set_status("Pasted %u bytes. Review the draft, then TRANSFER. Nothing sent.", (unsigned)copy->length);
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
        set_status("Enter 1-32 whitespace-separated byte pairs (95 characters max), such as 9F 00 00 00");
        return;
    }
    keyboard_visible(false);

    spi_transaction_t transaction = {
        .length = length * 8,
        .rxlength = length * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    memset(&saved_transfer, 0, sizeof(saved_transfer));
    esp_err_t error = spi_device_transmit(device, &transaction);
    if (error != ESP_OK) {
        render_transfer();
        set_status("SPI transfer failed: %s. Saved result cleared; the clipboard is unchanged.", esp_err_to_name(error));
        return;
    }
    saved_transfer.length = length;
    saved_transfer.mode = mode_index;
    saved_transfer.clock_hz = speed_choices[speed_index];
    memcpy(saved_transfer.tx, tx, length);
    memcpy(saved_transfer.rx, rx, length);
    render_transfer();
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
    lv_obj_set_style_text_font(help, &lv_font_montserrat_14, 0);
    lv_label_set_text(help, "M5-Bus: MOSI G18, MISO G19, SCK G5, CS G45; 3.3 V logic only.\n"
                            "Share ground. Pins are high-impedance until START and after STOP/Home.\n"
                            "Each full-duplex transfer is limited to 32 bytes; verify target voltage and mode first.");

    lv_obj_t *settings = row(parent);
    small_button(settings, "", 200, setting_clicked, (void *)0, &mode_label);
    small_button(settings, "", 200, setting_clicked, (void *)1, &speed_label);
    small_button(settings, "", 200, start_clicked, NULL, &start_label);

    input_area = lv_textarea_create(parent);
    lv_obj_set_size(input_area, 640, 128);
    lv_obj_set_style_text_font(input_area, &lv_font_montserrat_28, 0);
    /* Retain invalid characters/newlines and one excess character. Reject an
     * overlong draft instead of silently sending a valid truncated prefix. */
    lv_textarea_set_max_length(input_area, SPI_TOOL_MAX_TEXT + 1);
    lv_textarea_set_placeholder_text(input_area, "TX hex pairs: 9F 00 00 00");
    /* Restore before attaching the callback: LVGL emits per-character events
     * during set_text, which must not overwrite the source being restored. */
    lv_textarea_set_text(input_area, saved_input);
    lv_obj_add_event_cb(input_area, input_event, LV_EVENT_ALL, NULL);

    lv_obj_t *actions = row(parent);
    small_button(actions, "TRANSFER", 200, transfer_clicked, NULL, NULL);
    small_button(actions, "PASTE\nBYTES", 200, paste_clicked, NULL, NULL);
    small_button(actions, "CLEAR TX", 200, clear_tx_clicked, NULL, NULL);

    lv_obj_t *result_controls = row(parent);
    copy_rx_button = small_button(result_controls, "COPY RX", 310, copy_rx_clicked, NULL, NULL);
    small_button(result_controls, "CLEAR RESULT", 310, clear_clicked, NULL, NULL);
    identity_label = lv_label_create(parent);
    lv_obj_set_size(identity_label, 640, 34);
    lv_obj_set_style_text_font(identity_label, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(identity_label, LV_LABEL_LONG_WRAP);

    result_area = lv_textarea_create(parent);
    lv_obj_set_size(result_area, 640, 220);
    lv_obj_set_style_text_font(result_area, &lv_font_montserrat_28, 0);
    lv_textarea_set_text(result_area, "");
    lv_textarea_set_cursor_click_pos(result_area, false);

    keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(keyboard, 640, 240);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1, hex_keys, hex_controls);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_CANCEL, NULL);

    status_label = lv_label_create(parent);
    lv_obj_set_width(status_label, 640);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(status_label, "Tap TX for hex keys. Select mode/clock, then START.\n"
                                    "Done only hides the keyboard; TRANSFER sends the draft.");
    update_controls();
    render_transfer();
}

void spi_tool_stop(void)
{
    esp_err_t error = stop_bus();
    if (error != ESP_OK) ESP_LOGE("spi-tool", "Could not release SPI bus: %s", esp_err_to_name(error));
    if (keyboard) lv_keyboard_set_textarea(keyboard, NULL);
    keyboard = NULL;
    mode_label = NULL;
    speed_label = NULL;
    start_label = NULL;
    input_area = NULL;
    result_area = NULL;
    status_label = NULL;
    copy_rx_button = identity_label = NULL;
}
