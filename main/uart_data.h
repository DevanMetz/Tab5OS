#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UART_TX_MAX_BYTES 256
#define UART_TX_MAX_HEX_TEXT (UART_TX_MAX_BYTES * 3 - 1)

typedef enum { UART_TX_ASCII, UART_TX_HEX } uart_tx_mode_t;
typedef enum { UART_TX_END_NONE, UART_TX_END_LF, UART_TX_END_CR, UART_TX_END_CRLF } uart_tx_ending_t;
typedef enum { UART_TX_OK, UART_TX_EMPTY, UART_TX_TOO_LONG, UART_TX_BAD_ASCII, UART_TX_BAD_HEX, UART_TX_BAD_OPTIONS } uart_tx_status_t;

typedef struct {
    uint8_t bytes[UART_TX_MAX_BYTES];
    size_t length;
    uart_tx_mode_t mode;
    uart_tx_ending_t ending;
} uart_tx_data_t;

/* The limit includes the selected ASCII suffix. Hex never adds a suffix.
 * Failure clears the entire result; no valid truncated prefix is returned. */
uart_tx_status_t uart_tx_parse(const char *text, uart_tx_mode_t mode,
                               uart_tx_ending_t ending, uart_tx_data_t *result);
/* Reconstructs a sent message's draft and removes its selected ASCII suffix,
 * so PREV followed by SEND reproduces the same bytes without adding it twice. */
bool uart_tx_format_draft(const uart_tx_data_t *data, char *text, size_t capacity);
const char *uart_tx_error(uart_tx_status_t status);
void uart_data_self_test(void);
