/*
 * Ender 3 robot controller.
 *
 * Drives a Creality Ender 3 mainboard (Marlin) over USB CDC-ACM from the
 * Tab5 USB-A host port. The X and Y steppers drive the two rear wheels;
 * tank-style controls translate into relative G1 moves: FWD/BACK run both
 * wheels together, LEFT/RIGHT spin them in opposite directions. SWAP and
 * INVERT adjust the mapping to match physical wiring.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ender3_tool.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/m5stack_tab5.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"

#define TAG "ender3"

#define E3_TICK_MS 50
#define E3_POLL_EVERY_TICKS (2000 / E3_TICK_MS)
#define E3_INIT_RETRY_TICKS (3000 / E3_TICK_MS)
#define E3_OPEN_TIMEOUT_MS 1000
#define E3_TX_TIMEOUT_MS 200
#define E3_MAX_SPEED_MMPS 15.0f
#define E3_MIN_SPEED_MMPS 3.0f
#define E3_RING_CAPACITY 2048
#define E3_LINE_CAP 192
#define E3_OUTPUT_CAPACITY 4096

typedef enum {
    E3_DIR_NONE = 0,
    E3_DIR_FWD,
    E3_DIR_BACK,
    E3_DIR_LEFT,
    E3_DIR_RIGHT,
} ender3_dir_t;

typedef struct {
    float x;
    float y;
    bool valid;
} ender3_position_t;

typedef struct {
    char line[E3_LINE_CAP];
    size_t len;
} ender3_splitter_t;

typedef void (*ender3_line_cb_t)(const char *line, void *ctx);

typedef struct {
    uint8_t data[E3_RING_CAPACITY];
    size_t head;
    size_t len;
} ring_t;

typedef struct {
    float x;
    float y;
    bool position_valid;
    unsigned ack_count;
    char last_response[128];
} rx_state_t;

static void ender3_split_lines(ender3_splitter_t *splitter, const uint8_t *data, size_t len,
                               ender3_line_cb_t callback, void *ctx);
static bool ender3_parse_position(const char *line, ender3_position_t *pos);
static void ender3_motion_deltas(ender3_dir_t dir, float speed_mmps, float dt, bool swap,
                                 bool invert_x, bool invert_y, float *dx, float *dy);

// --- shared runtime state -------------------------------------------------

static lv_obj_t *status_label;
static lv_obj_t *pos_label;
static lv_obj_t *response_label;
static lv_obj_t *speed_value_label;
static lv_obj_t *motor_label;
static lv_obj_t *swap_label;
static lv_obj_t *invert_x_label;
static lv_obj_t *invert_y_label;
static lv_obj_t *input_area;
static lv_obj_t *output_area;
static lv_timer_t *tick_timer;
static char *output_text;

static TaskHandle_t conn_task_handle;
static SemaphoreHandle_t rx_mutex;
static SemaphoreHandle_t dev_mutex;
static SemaphoreHandle_t conn_sem;
static SemaphoreHandle_t conn_done_sem;

static volatile bool stop_requested;
static volatile bool connected_flag;
static volatile bool init_pending;
static volatile bool host_started;
static volatile bool cdc_driver_ready;
static volatile bool conn_task_alive;
static volatile esp_err_t host_error;

static cdc_acm_dev_hdl_t cdc_hdl;
static ender3_dir_t move_dir;
static bool swap_axes;
static bool invert_x;
static bool invert_y;
static bool motors_enabled;
static float current_speed_mmps;
static unsigned poll_counter;
static volatile unsigned rx_line_count;
static unsigned retry_base_count;
static unsigned connected_tick;
static ring_t rx_ring;
static ender3_splitter_t rx_splitter;
static rx_state_t rx_state;

// --- pure helpers (covered by ender3_tool_self_test) ----------------------

static void ender3_motion_deltas(ender3_dir_t dir, float speed_mmps, float dt, bool swap,
                                 bool invert_x, bool invert_y, float *dx, float *dy)
{
    float left = 0.0f;   // X axis
    float right = 0.0f;  // Y axis
    switch (dir) {
    case E3_DIR_FWD:
        left = speed_mmps * dt;
        right = speed_mmps * dt;
        break;
    case E3_DIR_BACK:
        left = -speed_mmps * dt;
        right = -speed_mmps * dt;
        break;
    case E3_DIR_LEFT:
        left = -speed_mmps * dt;
        right = speed_mmps * dt;
        break;
    case E3_DIR_RIGHT:
        left = speed_mmps * dt;
        right = -speed_mmps * dt;
        break;
    default:
        break;
    }
    if (swap) {
        float swapped = left;
        left = right;
        right = swapped;
    }
    if (invert_x) left = -left;
    if (invert_y) right = -right;
    *dx = left;
    *dy = right;
}

static bool ender3_parse_position(const char *line, ender3_position_t *pos)
{
    const char *x = strstr(line, "X:");
    const char *y = strstr(line, "Y:");
    if (!x || !y) return false;
    char *end;
    float x_value = strtof(x + 2, &end);
    if (end == x + 2) return false;
    float y_value = strtof(y + 2, &end);
    if (end == y + 2) return false;
    pos->x = x_value;
    pos->y = y_value;
    pos->valid = true;
    return true;
}

static void ender3_split_lines(ender3_splitter_t *splitter, const uint8_t *data, size_t len,
                               ender3_line_cb_t callback, void *ctx)
{
    char line[E3_LINE_CAP];
    for (size_t i = 0; i < len; i++) {
        char character = (char)data[i];
        if (character == '\n') {
            size_t length = splitter->len;
            if (length >= sizeof(line)) length = sizeof(line) - 1;
            memcpy(line, splitter->line, length);
            line[length] = '\0';
            splitter->len = 0;
            callback(line, ctx);
        } else if (character != '\r') {
            if (splitter->len < sizeof(splitter->line) - 1) splitter->line[splitter->len++] = character;
        }
    }
}

static void ring_push(ring_t *ring, const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        ring->data[(ring->head + ring->len) % E3_RING_CAPACITY] = bytes[i];
        if (ring->len < E3_RING_CAPACITY) {
            ring->len++;
        } else {
            ring->head = (ring->head + 1) % E3_RING_CAPACITY;
        }
    }
}

static size_t ring_pull(ring_t *ring, uint8_t *out, size_t capacity)
{
    size_t count = ring->len < capacity ? ring->len : capacity;
    for (size_t i = 0; i < count; i++) out[i] = ring->data[(ring->head + i) % E3_RING_CAPACITY];
    ring->head = (ring->head + count) % E3_RING_CAPACITY;
    ring->len -= count;
    return count;
}

// --- CDC-ACM callbacks (run on the driver task; no LVGL here) -------------

static void rx_line_cb(const char *line, void *ctx)
{
    (void)ctx;
    rx_line_count++;
    ESP_LOGI(TAG, "RX: %.110s", line);
    if (!rx_mutex) return;
    xSemaphoreTake(rx_mutex, portMAX_DELAY);
    if (line[0] == 'o' && line[1] == 'k' && (line[2] == '\0' || line[2] == ' ')) {
        rx_state.ack_count++;
    } else {
        ender3_position_t pos;
        if (ender3_parse_position(line, &pos)) {
            rx_state.position_valid = true;
            rx_state.x = pos.x;
            rx_state.y = pos.y;
        } else {
            snprintf(rx_state.last_response, sizeof(rx_state.last_response), "%.*s",
                     (int)sizeof(rx_state.last_response) - 1, line);
        }
    }
    ring_push(&rx_ring, (const uint8_t *)line, strlen(line));
    ring_push(&rx_ring, (const uint8_t *)"\n", 1);
    xSemaphoreGive(rx_mutex);
}

static bool conn_rx_cb(const uint8_t *data, size_t data_len, void *user_arg)
{
    (void)user_arg;
    ender3_split_lines(&rx_splitter, data, data_len, rx_line_cb, NULL);
    return true;
}

static void conn_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "Ender 3 disconnected");
        xSemaphoreGive(conn_sem);
        break;
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error %d", event->data.error);
        break;
    default:
        break;
    }
}

// --- connection task -------------------------------------------------------

static volatile bool ctrl_done;
static volatile usb_transfer_status_t ctrl_result;

static void ctrl_cb(usb_transfer_t *transfer)
{
    ctrl_result = transfer->status;
    ctrl_done = true;
}

// The CDC-ACM host driver never configures vendor-class USB-UART bridges
// (CH340/CH341), which set their baud through WCH's vendor register protocol
// rather than the CDC SET_LINE_CODING request. Without this the bridge stays
// at its default 9600 baud while Marlin listens at 115200. Native-CDC boards
// (STM32) need no baud configuration and are only logged here.
static esp_err_t raw_request(usb_host_client_handle_t client, usb_device_handle_t dev,
                             uint8_t bm_request_type, uint8_t b_request, uint16_t w_value,
                             uint16_t w_index, uint8_t *data, size_t len)
{
    esp_err_t error = ESP_ERR_INVALID_RESPONSE;
    usb_transfer_t *transfer = NULL;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + len, 0, &transfer) != ESP_OK)
        return ESP_ERR_NO_MEM;
    uint8_t *buf = transfer->data_buffer;
    usb_setup_packet_t *setup = (usb_setup_packet_t *)buf;
    setup->bmRequestType = bm_request_type;
    setup->bRequest = b_request;
    setup->wValue = w_value;
    setup->wIndex = w_index;
    setup->wLength = (uint16_t)len;
    if (len && !(bm_request_type & 0x80) && data) {
        memcpy(buf + sizeof(usb_setup_packet_t), data, len);
    }
    transfer->num_bytes = (int)sizeof(usb_setup_packet_t) + (int)len;
    transfer->device_handle = dev;
    transfer->callback = ctrl_cb;
    ctrl_done = false;
    ctrl_result = USB_TRANSFER_STATUS_ERROR;
    if (usb_host_transfer_submit_control(client, transfer) != ESP_OK) {
        usb_host_transfer_free(transfer);
        return ESP_FAIL;
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (!ctrl_done && xTaskGetTickCount() < deadline) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(50));
    }
    if (ctrl_done && ctrl_result == USB_TRANSFER_STATUS_COMPLETED) {
        if (len && (bm_request_type & 0x80) && data) {
            memcpy(data, buf + sizeof(usb_setup_packet_t), len);
        }
        error = ESP_OK;
    }
    usb_host_transfer_free(transfer);
    return error;
}

static void configure_bridge_baud(void)
{
    uint8_t addr_list[8];
    int num_dev = 0;
    if (usb_host_device_addr_list_fill(sizeof(addr_list), addr_list, &num_dev) != ESP_OK ||
        num_dev < 1) {
        ESP_LOGW(TAG, "USB device list unavailable");
        return;
    }
    usb_host_client_config_t client_config = {
        .is_synchronous = true,
        .max_num_event_msg = 3,
    };
    usb_host_client_handle_t client = NULL;
    if (usb_host_client_register(&client_config, &client) != ESP_OK) return;

    usb_device_handle_t dev = NULL;
    if (usb_host_device_open(client, addr_list[0], &dev) != ESP_OK) {
        usb_host_client_deregister(client);
        return;
    }
    const usb_device_desc_t *dev_desc = NULL;
    if (usb_host_get_device_descriptor(dev, &dev_desc) == ESP_OK && dev_desc) {
        ESP_LOGI(TAG, "USB device VID 0x%04X PID 0x%04X", dev_desc->idVendor, dev_desc->idProduct);
    }
    if (dev_desc && dev_desc->idVendor == 0x1A86 && dev_desc->idProduct == 0x7523) {
        // WCH CH340G: baud is set with vendor register writes (see the Linux
        // ch341 driver). Divisor for 115200 = 0xCC03, plus bit 7 for chips
        // newer than version 0x27.
        uint8_t version_buf[2] = {0, 0};
        uint8_t version = 0;
        if (raw_request(client, dev, 0xC0, 0x5F, 0, 0, version_buf, 2) == ESP_OK) {
            version = version_buf[0];
        }
        ESP_LOGI(TAG, "CH340 version 0x%02X", version);
        raw_request(client, dev, 0x40, 0xA1, 0, 0, NULL, 0);  // serial init
        uint16_t divisor = 0xCC03;
        if (version > 0x27) divisor |= 0x0080;
        esp_err_t error = raw_request(client, dev, 0x40, 0x9A, 0x1312, divisor, NULL, 0);
        ESP_LOGI(TAG, "CH340 baud 115200: %s", error == ESP_OK ? "ok" : "failed");
        if (version >= 0x30) {
            raw_request(client, dev, 0x40, 0x9A, 0x2518, 0xC3, NULL, 0);  // 8N1, RX+TX on
        }
    }
    usb_host_device_close(client, dev);
    usb_host_client_deregister(client);
}

static void conn_task(void *arg)
{
    (void)arg;
    const cdc_acm_host_device_config_t config = {
        .connection_timeout_ms = E3_OPEN_TIMEOUT_MS,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .user_arg = NULL,
        .event_cb = conn_event_cb,
        .data_cb = conn_rx_cb,
    };
    while (1) {
        if (stop_requested) break;
        if (connected_flag) {
            xSemaphoreTake(conn_sem, portMAX_DELAY);
            xSemaphoreTake(dev_mutex, portMAX_DELAY);
            if (cdc_hdl) {
                cdc_acm_host_close(cdc_hdl);
                cdc_hdl = NULL;
            }
            xSemaphoreGive(dev_mutex);
            connected_flag = false;
            continue;
        }
        cdc_acm_dev_hdl_t handle = NULL;
        esp_err_t error = cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID, 0, &config, &handle);
        if (stop_requested) {
            if (error == ESP_OK) cdc_acm_host_close(handle);
            break;
        }
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "Ender 3 connected");
            configure_bridge_baud();
            xSemaphoreTake(dev_mutex, portMAX_DELAY);
            cdc_hdl = handle;
            xSemaphoreGive(dev_mutex);
            connected_flag = true;
            init_pending = true;
        } else {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    conn_task_alive = false;
    xSemaphoreGive(conn_done_sem);
    vTaskDelete(NULL);
}

// --- TX and UI (display task only) -----------------------------------------

static bool send_gcode(const char *format, ...)
{
    if (!connected_flag || !cdc_driver_ready) return false;
    char buffer[E3_LINE_CAP];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(buffer, sizeof(buffer) - 1, format, arguments);
    va_end(arguments);
    if (length < 0) return false;
    if (length > (int)sizeof(buffer) - 2) length = (int)sizeof(buffer) - 2;
    buffer[length++] = '\n';

    bool sent = false;
    xSemaphoreTake(dev_mutex, portMAX_DELAY);
    if (cdc_hdl) {
        esp_err_t tx_error = cdc_acm_host_data_tx_blocking(cdc_hdl, (const uint8_t *)buffer,
                                                           (size_t)length, E3_TX_TIMEOUT_MS);
        sent = tx_error == ESP_OK;
        if (tx_error != ESP_OK) {
            ESP_LOGW(TAG, "TX failed (%.*s): %s", length - 1, buffer, esp_err_to_name(tx_error));
        } else {
            ESP_LOGI(TAG, "TX: %.*s", length - 1, buffer);
        }
    }
    xSemaphoreGive(dev_mutex);
    if (sent && output_text) {
        size_t used = strlen(output_text);
        if (used > E3_OUTPUT_CAPACITY / 2) {
            char *keep = strchr(output_text + E3_OUTPUT_CAPACITY / 2, '\n');
            keep = keep ? keep + 1 : output_text + E3_OUTPUT_CAPACITY / 2;
            memmove(output_text, keep, strlen(keep) + 1);
            used = strlen(output_text);
        }
        int written = snprintf(output_text + used, E3_OUTPUT_CAPACITY - used, "> %.*s\n", length - 1, buffer);
        if (written > 0 && output_area) {
            lv_textarea_set_text(output_area, output_text);
            lv_textarea_set_cursor_pos(output_area, LV_TEXTAREA_CURSOR_LAST);
        }
    }
    return sent;
}

static void update_controls(void)
{
    if (motor_label)
        lv_label_set_text(motor_label, motors_enabled ? "MOTOR\nON" : "MOTOR\nOFF");
    if (swap_label) lv_label_set_text(swap_label, swap_axes ? "SWAP\nX/Y" : "SWAP\nOFF");
    if (invert_x_label) lv_label_set_text(invert_x_label, invert_x ? "INV X\nON" : "INV X\nOFF");
    if (invert_y_label) lv_label_set_text(invert_y_label, invert_y ? "INV Y\nON" : "INV Y\nOFF");
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

static void update_status(void)
{
    if (!status_label) return;
    if (connected_flag) {
        set_status(init_pending ? "Connected - sending setup commands..." :
                                  "Connected to Ender 3 (USB-A host port)");
    } else if (host_error != ESP_OK) {
        set_status("USB host error: %s", esp_err_to_name(host_error));
    } else {
        set_status("Searching for Ender 3 on the USB-A port...\n"
                   "Plug the printer USB cable (A to B) into the tablet.");
    }

    float x = 0.0f, y = 0.0f;
    bool valid = false;
    unsigned acks = 0;
    char response[128] = "";
    if (rx_mutex) {
        xSemaphoreTake(rx_mutex, portMAX_DELAY);
        valid = rx_state.position_valid;
        x = rx_state.x;
        y = rx_state.y;
        acks = rx_state.ack_count;
        snprintf(response, sizeof(response), "%s", rx_state.last_response);
        xSemaphoreGive(rx_mutex);
    }
    if (pos_label) {
        if (valid) {
            lv_label_set_text_fmt(pos_label, "X %+7.1f   Y %+7.1f   ok:%u", (double)x, (double)y, acks);
        } else {
            lv_label_set_text_fmt(pos_label, "X ?   Y ?   ok:%u", acks);
        }
    }
    if (response_label) {
        lv_label_set_text(response_label, response[0] ? response : "");
    }
}

static void drain_rx(void)
{
    uint8_t chunk[128];
    for (;;) {
        size_t count;
        xSemaphoreTake(rx_mutex, portMAX_DELAY);
        count = ring_pull(&rx_ring, chunk, sizeof(chunk));
        xSemaphoreGive(rx_mutex);
        if (!count || !output_text) break;
        size_t used = strlen(output_text);
        if (used > E3_OUTPUT_CAPACITY / 2) {
            char *keep = strchr(output_text + E3_OUTPUT_CAPACITY / 2, '\n');
            keep = keep ? keep + 1 : output_text + E3_OUTPUT_CAPACITY / 2;
            memmove(output_text, keep, strlen(keep) + 1);
            used = strlen(output_text);
        }
        size_t available = E3_OUTPUT_CAPACITY - used - 1;
        if (count > available) count = available;
        if (!count) break;
        memcpy(output_text + used, chunk, count);
        output_text[used + count] = '\0';
        if (output_area) {
            lv_textarea_set_text(output_area, output_text);
            lv_textarea_set_cursor_pos(output_area, LV_TEXTAREA_CURSOR_LAST);
        }
    }
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (stop_requested) return;

    if (init_pending) {
        init_pending = false;
        connected_tick = 0;
        retry_base_count = rx_line_count;
        send_gcode("M999");  // resume a halted Marlin (e.g. MINTEMP kill)
        send_gcode("M110 N0");
        send_gcode("G91");
        send_gcode("M17");
        send_gcode("M203 X3000 Y3000");  // runtime-only: raise the X/Y feedrate cap
        send_gcode("M201 X10 Y10");      // very gentle ramp: the robot is heavy
        send_gcode("M205 X1 Y1");        // near-zero jerk on starts and reversals
        send_gcode("M115");              // firmware banner; proves the link
        motors_enabled = true;
        update_controls();
        send_gcode("M114");
    } else if (connected_flag && connected_tick >= E3_INIT_RETRY_TICKS &&
               rx_line_count == retry_base_count) {
        // Marlin never answered (boot in progress, lost commands, or a
        // marginal link): re-send the setup until it responds.
        connected_tick = 0;
        retry_base_count = rx_line_count;
        send_gcode("M999");
        send_gcode("M110 N0");
        send_gcode("G91");
        send_gcode("M17");
        send_gcode("M203 X3000 Y3000");
        send_gcode("M201 X10 Y10");
        send_gcode("M205 X1 Y1");
        send_gcode("M115");
        motors_enabled = true;
        update_controls();
        send_gcode("M114");
    }

    if (connected_flag) connected_tick++;

    if (connected_flag && move_dir != E3_DIR_NONE) {
        float dx = 0.0f, dy = 0.0f;
        ender3_motion_deltas(move_dir, current_speed_mmps, (float)E3_TICK_MS / 1000.0f,
                             swap_axes, invert_x, invert_y, &dx, &dy);
        if (fabsf(dx) >= 0.02f || fabsf(dy) >= 0.02f) {
            send_gcode("G91");
            send_gcode("G1 X%.2f Y%.2f F%.0f", (double)dx, (double)dy,
                       (double)(current_speed_mmps * 60.0f));
        }
    }

    if (connected_flag && ++poll_counter >= E3_POLL_EVERY_TICKS) {
        poll_counter = 0;
        send_gcode("M114");
    }

    drain_rx();
    update_status();
}

// --- UI events -------------------------------------------------------------

static void dir_pressed(lv_event_t *event)
{
    move_dir = (ender3_dir_t)(uintptr_t)lv_event_get_user_data(event);
}

static void dir_released(lv_event_t *event)
{
    ender3_dir_t released = (ender3_dir_t)(uintptr_t)lv_event_get_user_data(event);
    if (move_dir == released) move_dir = E3_DIR_NONE;
}

static void stop_clicked(lv_event_t *event)
{
    (void)event;
    move_dir = E3_DIR_NONE;
    motors_enabled = false;
    update_controls();
    send_gcode("M410");
    send_gcode("M18");
    set_status("Stopped - motion halted and motors disabled");
}

static void motor_clicked(lv_event_t *event)
{
    (void)event;
    motors_enabled = !motors_enabled;
    send_gcode(motors_enabled ? "M17" : "M18");
    update_controls();
    set_status(motors_enabled ? "Steppers enabled" : "Steppers disabled");
}

static void swap_clicked(lv_event_t *event)
{
    (void)event;
    swap_axes = !swap_axes;
    update_controls();
    set_status(swap_axes ? "Left wheel = Y axis, right wheel = X axis" :
                           "Left wheel = X axis, right wheel = Y axis");
}

static void invert_x_clicked(lv_event_t *event)
{
    (void)event;
    invert_x = !invert_x;
    update_controls();
    set_status(invert_x ? "X axis (left wheel) inverted" : "X axis direction normal");
}

static void invert_y_clicked(lv_event_t *event)
{
    (void)event;
    invert_y = !invert_y;
    update_controls();
    set_status(invert_y ? "Y axis (right wheel) inverted" : "Y axis direction normal");
}

static void quick_clicked(lv_event_t *event)
{
    const char *command = (const char *)lv_event_get_user_data(event);
    if (send_gcode("%s", command)) {
        set_status("Sent %s", command);
    } else {
        set_status("Not connected - cannot send %s", command);
    }
}

static void send_clicked(lv_event_t *event)
{
    (void)event;
    if (!input_area) return;
    const char *text = lv_textarea_get_text(input_area);
    size_t length = strlen(text);
    while (length && (text[length - 1] == '\n' || text[length - 1] == '\r' || text[length - 1] == ' '))
        length--;
    if (!length) return;
    char line[E3_LINE_CAP];
    if (length >= sizeof(line)) length = sizeof(line) - 1;
    memcpy(line, text, length);
    line[length] = '\0';
    if (send_gcode("%s", line)) {
        lv_textarea_set_text(input_area, "");
        set_status("Sent %s", line);
    } else {
        set_status("Not connected - cannot send");
    }
}

static void clear_clicked(lv_event_t *event)
{
    (void)event;
    if (output_text) output_text[0] = '\0';
    if (output_area) lv_textarea_set_text(output_area, "");
}

static void speed_changed(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    int percent = (int)lv_slider_get_value(slider);
    current_speed_mmps = E3_MIN_SPEED_MMPS + (float)percent / 100.0f *
                        (E3_MAX_SPEED_MMPS - E3_MIN_SPEED_MMPS);
    if (speed_value_label) lv_label_set_text_fmt(speed_value_label, "%d%%", percent);
}

// --- UI construction -------------------------------------------------------

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

static lv_obj_t *pad_button(lv_obj_t *parent, const char *text, uint32_t color,
                            ender3_dir_t dir)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_size(control, 196, 196);
    lv_obj_set_style_radius(control, 16, 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(color), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(control, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(control, dir_pressed, LV_EVENT_PRESSED, (void *)(uintptr_t)dir);
    lv_obj_add_event_cb(control, dir_released, LV_EVENT_RELEASED, (void *)(uintptr_t)dir);
    lv_obj_add_event_cb(control, dir_released, LV_EVENT_PRESS_LOST, (void *)(uintptr_t)dir);
    lv_obj_t *label = lv_label_create(control);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_center(label);
    return control;
}

void ender3_tool_show(lv_obj_t *parent)
{
    stop_requested = false;
    connected_flag = false;
    init_pending = false;
    host_started = false;
    cdc_driver_ready = false;
    conn_task_alive = false;
    host_error = ESP_OK;
    cdc_hdl = NULL;
    move_dir = E3_DIR_NONE;
    swap_axes = false;
    invert_x = false;
    invert_y = false;
    motors_enabled = false;
    current_speed_mmps = E3_MIN_SPEED_MMPS + 0.20f * (E3_MAX_SPEED_MMPS - E3_MIN_SPEED_MMPS);
    poll_counter = 0;
    memset(&rx_ring, 0, sizeof(rx_ring));
    memset(&rx_splitter, 0, sizeof(rx_splitter));
    memset(&rx_state, 0, sizeof(rx_state));
    rx_line_count = 0;
    retry_base_count = 0;
    connected_tick = 0;

    rx_mutex = xSemaphoreCreateMutex();
    dev_mutex = xSemaphoreCreateMutex();
    conn_sem = xSemaphoreCreateBinary();
    conn_done_sem = xSemaphoreCreateBinary();
    output_text = heap_caps_calloc(1, E3_OUTPUT_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    lv_obj_set_style_pad_row(parent, 10, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Ender 3 Robot Controller");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *help = lv_label_create(parent);
    lv_obj_set_width(help, 640);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(help, "Hold FWD/BACK to drive, LEFT/RIGHT to spin in place. STOP halts motion.\n"
                            "X = left wheel, Y = right wheel. Use SWAP / INV X / INV Y to match your wiring.\n"
                            "Connect sets gentle acceleration (M201 X10 Y10) for the heavy chassis.");

    status_label = lv_label_create(parent);
    lv_obj_set_width(status_label, 640);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label, "Starting USB host...");

    if (!rx_mutex || !dev_mutex || !conn_sem || !conn_done_sem) {
        set_status("Failed to allocate control semaphores");
        return;
    }

    pos_label = lv_label_create(parent);
    lv_obj_set_width(pos_label, 640);
    lv_obj_set_style_text_align(pos_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(pos_label, "X ?   Y ?");

    response_label = lv_label_create(parent);
    lv_obj_set_width(response_label, 640);
    lv_obj_set_style_text_align(response_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(response_label, lv_color_hex(0x90A4AE), 0);

    // D-pad: FWD top, LEFT/STOP/RIGHT middle, BACK bottom.
    lv_obj_t *pad = lv_obj_create(parent);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 620, 620);
    lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(pad, 10, 0);
    pad_button(pad, LV_SYMBOL_UP "\nFWD", 0x2E7D32, E3_DIR_FWD);
    lv_obj_t *spacer = lv_obj_create(pad);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 196, 196);
    spacer = lv_obj_create(pad);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 196, 196);
    pad_button(pad, LV_SYMBOL_LEFT "\nLEFT", 0x1565C0, E3_DIR_LEFT);
    lv_obj_t *stop_button = pad_button(pad, "STOP", 0xC62828, E3_DIR_NONE);
    lv_obj_remove_event_cb(stop_button, dir_pressed);
    lv_obj_remove_event_cb(stop_button, dir_released);
    lv_obj_add_event_cb(stop_button, stop_clicked, LV_EVENT_CLICKED, NULL);
    pad_button(pad, LV_SYMBOL_RIGHT "\nRIGHT", 0x1565C0, E3_DIR_RIGHT);
    spacer = lv_obj_create(pad);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 196, 196);
    pad_button(pad, LV_SYMBOL_DOWN "\nBACK", 0x2E7D32, E3_DIR_BACK);
    spacer = lv_obj_create(pad);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 196, 196);

    // Speed slider.
    lv_obj_t *speed_row = row(parent, 80);
    lv_obj_t *speed_title = lv_label_create(speed_row);
    lv_label_set_text(speed_title, "Speed");
    lv_obj_t *slider = lv_slider_create(speed_row);
    lv_obj_set_size(slider, 340, 18);
    lv_slider_set_range(slider, 1, 100);
    lv_slider_set_value(slider, 20, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, speed_changed, LV_EVENT_VALUE_CHANGED, NULL);
    speed_value_label = lv_label_create(speed_row);
    lv_label_set_text(speed_value_label, "20%");

    // Toggles.
    lv_obj_t *toggle_row = row(parent, 76);
    small_button(toggle_row, "", 180, motor_clicked, NULL, &motor_label);
    small_button(toggle_row, "", 140, swap_clicked, NULL, &swap_label);
    small_button(toggle_row, "", 140, invert_x_clicked, NULL, &invert_x_label);
    small_button(toggle_row, "", 140, invert_y_clicked, NULL, &invert_y_label);

    // Quick commands.
    lv_obj_t *quick_row = row(parent, 76);
    small_button(quick_row, "ZERO", 200, quick_clicked, (void *)"G92 X0 Y0", NULL);
    small_button(quick_row, "M114", 200, quick_clicked, (void *)"M114", NULL);
    small_button(quick_row, "M119", 200, quick_clicked, (void *)"M119", NULL);

    // Raw G-code line.
    lv_obj_t *send_row = row(parent, 76);
    input_area = lv_textarea_create(send_row);
    lv_obj_set_size(input_area, 400, 72);
    lv_textarea_set_one_line(input_area, true);
    lv_textarea_set_max_length(input_area, E3_LINE_CAP - 1);
    lv_textarea_set_placeholder_text(input_area, "Raw G-code (e.g. M114)");
    small_button(send_row, "SEND", 120, send_clicked, NULL, NULL);
    small_button(send_row, "CLEAR", 100, clear_clicked, NULL, NULL);

    lv_obj_t *keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(keyboard, 640, 330);
    lv_keyboard_set_textarea(keyboard, input_area);

    output_area = lv_textarea_create(parent);
    lv_obj_set_size(output_area, 640, 220);
    lv_textarea_set_text(output_area, "");
    lv_textarea_set_cursor_click_pos(output_area, false);

    update_controls();
    update_status();
    tick_timer = lv_timer_create(tick_cb, E3_TICK_MS, NULL);

    // Power and start the USB host, then the CDC-ACM driver.
    bsp_set_usb_5v_en(true);
    host_error = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, false);
    if (host_error != ESP_OK) {
        ESP_LOGE(TAG, "USB host start failed: %s", esp_err_to_name(host_error));
        update_status();
        return;
    }
    host_started = true;
    host_error = cdc_acm_host_install(NULL);
    if (host_error != ESP_OK) {
        ESP_LOGE(TAG, "CDC-ACM install failed: %s", esp_err_to_name(host_error));
        update_status();
        return;
    }
    cdc_driver_ready = true;
    if (xTaskCreate(conn_task, "ender3-conn", 4096, NULL, 5, &conn_task_handle) != pdTRUE) {
        cdc_acm_host_uninstall();
        cdc_driver_ready = false;
        host_error = ESP_ERR_NO_MEM;
        update_status();
        return;
    }
    conn_task_alive = true;
    set_status("Searching for Ender 3 on the USB-A port...\n"
               "Plug the printer USB cable (A to B) into the tablet.");
}

bool ender3_tool_busy(void)
{
    return conn_task_alive || cdc_driver_ready || connected_flag || init_pending;
}

void ender3_tool_stop(void)
{
    if (tick_timer) {
        lv_timer_delete(tick_timer);
        tick_timer = NULL;
    }
    move_dir = E3_DIR_NONE;
    if (conn_task_alive || cdc_driver_ready || connected_flag) {
        stop_requested = true;
        if (conn_sem) xSemaphoreGive(conn_sem);
        if (conn_done_sem) xSemaphoreTake(conn_done_sem, pdMS_TO_TICKS(3000));
    }
    if (cdc_driver_ready) {
        cdc_acm_host_uninstall();
        cdc_driver_ready = false;
    }
    if (host_started) {
        bsp_usb_host_stop();
        host_started = false;
    }
    bsp_set_usb_5v_en(false);
    conn_task_alive = false;
    connected_flag = false;
    init_pending = false;
    stop_requested = false;
    if (rx_mutex) {
        vSemaphoreDelete(rx_mutex);
        rx_mutex = NULL;
    }
    if (dev_mutex) {
        vSemaphoreDelete(dev_mutex);
        dev_mutex = NULL;
    }
    if (conn_sem) {
        vSemaphoreDelete(conn_sem);
        conn_sem = NULL;
    }
    if (conn_done_sem) {
        vSemaphoreDelete(conn_done_sem);
        conn_done_sem = NULL;
    }
    heap_caps_free(output_text);
    output_text = NULL;
    status_label = NULL;
    pos_label = NULL;
    response_label = NULL;
    speed_value_label = NULL;
    motor_label = NULL;
    swap_label = NULL;
    invert_x_label = NULL;
    invert_y_label = NULL;
    input_area = NULL;
    output_area = NULL;
    conn_task_handle = NULL;
}

// --- self-test (runs at boot before BSP init) ------------------------------

typedef struct {
    char (*lines)[64];
    int count;
    int capacity;
} collector_t;

static void collect_line(const char *line, void *ctx)
{
    collector_t *collector = ctx;
    if (collector->count < collector->capacity) {
        snprintf(collector->lines[collector->count], 64, "%.*s", 63, line);
        collector->count++;
    }
}

void ender3_tool_self_test(void)
{
    float dx = 0.0f, dy = 0.0f;
    ender3_motion_deltas(E3_DIR_FWD, 10.0f, 0.1f, false, false, false, &dx, &dy);
    assert(fabsf(dx - 1.0f) < 1e-4f && fabsf(dy - 1.0f) < 1e-4f);
    ender3_motion_deltas(E3_DIR_BACK, 10.0f, 0.1f, false, false, false, &dx, &dy);
    assert(fabsf(dx + 1.0f) < 1e-4f && fabsf(dy + 1.0f) < 1e-4f);
    ender3_motion_deltas(E3_DIR_LEFT, 10.0f, 0.1f, false, false, false, &dx, &dy);
    assert(fabsf(dx + 1.0f) < 1e-4f && fabsf(dy - 1.0f) < 1e-4f);
    ender3_motion_deltas(E3_DIR_RIGHT, 10.0f, 0.1f, false, false, false, &dx, &dy);
    assert(fabsf(dx - 1.0f) < 1e-4f && fabsf(dy + 1.0f) < 1e-4f);
    // swap exchanges the wheels; invert_x/invert_y flip one axis each
    ender3_motion_deltas(E3_DIR_LEFT, 10.0f, 0.1f, true, false, false, &dx, &dy);
    assert(fabsf(dx - 1.0f) < 1e-4f && fabsf(dy + 1.0f) < 1e-4f);
    ender3_motion_deltas(E3_DIR_LEFT, 10.0f, 0.1f, false, true, false, &dx, &dy);
    assert(fabsf(dx - 1.0f) < 1e-4f && fabsf(dy - 1.0f) < 1e-4f);  // left wheel flipped to +1
    ender3_motion_deltas(E3_DIR_FWD, 10.0f, 0.1f, false, false, true, &dx, &dy);
    assert(fabsf(dx - 1.0f) < 1e-4f && fabsf(dy + 1.0f) < 1e-4f);  // right wheel flipped to -1
    ender3_motion_deltas(E3_DIR_FWD, 10.0f, 0.1f, false, true, true, &dx, &dy);
    assert(fabsf(dx + 1.0f) < 1e-4f && fabsf(dy + 1.0f) < 1e-4f);  // both flipped
    ender3_motion_deltas(E3_DIR_NONE, 10.0f, 0.1f, false, false, false, &dx, &dy);
    assert(dx == 0.0f && dy == 0.0f);

    ender3_position_t pos;
    assert(ender3_parse_position("X:12.50 Y:-3.25 Z:0.00 E:0.00 Count X:25 Y:-6 Z:0", &pos));
    assert(fabsf(pos.x - 12.5f) < 1e-3f && fabsf(pos.y + 3.25f) < 1e-3f);
    assert(ender3_parse_position("X:0.00 Y:0.00 Z:0.00 E:0.00", &pos));
    assert(fabsf(pos.x) < 1e-4f && fabsf(pos.y) < 1e-4f);
    assert(!ender3_parse_position("ok", &pos));
    assert(!ender3_parse_position("echo:busy: processing", &pos));
    assert(!ender3_parse_position("", &pos));

    ender3_splitter_t splitter;
    memset(&splitter, 0, sizeof(splitter));
    char lines[4][64];
    collector_t collector = {.lines = lines, .count = 0, .capacity = 4};
    ender3_split_lines(&splitter, (const uint8_t *)"ok\nX:1.00 Y:2.00\r\npartial", 25,
                       collect_line, &collector);
    assert(collector.count == 2);
    assert(strcmp(lines[0], "ok") == 0);
    assert(strcmp(lines[1], "X:1.00 Y:2.00") == 0);
    ender3_split_lines(&splitter, (const uint8_t *)"\n", 1, collect_line, &collector);
    assert(collector.count == 3);
    assert(strcmp(lines[2], "partial") == 0);
}
