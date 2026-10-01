#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef enum { UART_PARITY_DISABLE, UART_PARITY_EVEN, UART_PARITY_ODD } uart_parity_t;
typedef enum { UART_STOP_BITS_1, UART_STOP_BITS_2 } uart_stop_bits_t;
enum { UART_NUM_1 = 1, UART_DATA_8_BITS, UART_HW_FLOWCTRL_DISABLE, UART_SCLK_DEFAULT, UART_MODE_UART, UART_MODE_RS485_HALF_DUPLEX };
#define UART_PIN_NO_CHANGE (-1)
typedef struct {
    int baud_rate, data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    int flow_ctrl, source_clk;
} uart_config_t;
esp_err_t uart_param_config(int port, const uart_config_t *config);
esp_err_t uart_driver_install(int port, int rx_size, int tx_size, int queue_size, void *queue, int flags);
esp_err_t uart_set_mode(int port, int mode);
esp_err_t uart_set_pin(int port, int tx, int rx, int rts, int cts);
esp_err_t uart_driver_delete(int port);
esp_err_t uart_get_buffered_data_len(int port, size_t *length);
int uart_read_bytes(int port, void *buffer, uint32_t length, TickType_t ticks);
int uart_write_bytes(int port, const void *bytes, size_t length);
esp_err_t uart_wait_tx_done(int port, TickType_t ticks);
esp_err_t uart_set_loop_back(int port, bool enabled);
