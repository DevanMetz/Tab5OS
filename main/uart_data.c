#include "uart_data.h"

#include <assert.h>
#include <string.h>

static const char *const endings[] = {"", "\n", "\r", "\r\n"};

static bool options_valid(uart_tx_mode_t mode, uart_tx_ending_t ending)
{
    return (mode == UART_TX_ASCII || mode == UART_TX_HEX) &&
           (unsigned)ending <= UART_TX_END_CRLF &&
           (mode != UART_TX_HEX || ending == UART_TX_END_NONE);
}

static bool whitespace(unsigned char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

static int hex_digit(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

uart_tx_status_t uart_tx_parse(const char *text, uart_tx_mode_t mode,
                               uart_tx_ending_t ending, uart_tx_data_t *result)
{
    if (!result) return UART_TX_BAD_OPTIONS;
    memset(result, 0, sizeof(*result));
    if (!text || !options_valid(mode, ending)) return UART_TX_BAD_OPTIONS;
    size_t length = 0;
    size_t limit = mode == UART_TX_HEX ? UART_TX_MAX_HEX_TEXT : UART_TX_MAX_BYTES;
    while (text[length]) if (++length > limit) return UART_TX_TOO_LONG;
    uart_tx_data_t parsed = {.mode = mode, .ending = ending};
    if (mode == UART_TX_ASCII) {
        size_t suffix = strlen(endings[ending]);
        if (length + suffix > UART_TX_MAX_BYTES) return UART_TX_TOO_LONG;
        for (size_t i = 0; i < length; i++)
            if ((unsigned char)text[i] > 0x7f) return UART_TX_BAD_ASCII;
        memcpy(parsed.bytes, text, length);
        memcpy(parsed.bytes + length, endings[ending], suffix);
        parsed.length = length + suffix;
    } else {
        size_t offset = 0;
        while (offset < length) {
            if (whitespace((unsigned char)text[offset])) { offset++; continue; }
            if (offset + 1 >= length) return UART_TX_BAD_HEX;
            int high = hex_digit((unsigned char)text[offset]);
            int low = hex_digit((unsigned char)text[offset + 1]);
            if (high < 0 || low < 0 || (offset + 2 < length && !whitespace((unsigned char)text[offset + 2])))
                return UART_TX_BAD_HEX;
            if (parsed.length == UART_TX_MAX_BYTES) return UART_TX_TOO_LONG;
            parsed.bytes[parsed.length++] = (uint8_t)((high << 4) | low);
            offset += 2;
        }
    }
    if (!parsed.length) return UART_TX_EMPTY;
    *result = parsed;
    return UART_TX_OK;
}

bool uart_tx_format_draft(const uart_tx_data_t *data, char *text, size_t capacity)
{
    if (!text || !capacity) return false;
    text[0] = '\0';
    if (!data || !options_valid(data->mode, data->ending) || !data->length || data->length > UART_TX_MAX_BYTES)
        return false;
    if (data->mode == UART_TX_ASCII) {
        size_t suffix = strlen(endings[data->ending]);
        if (suffix > data->length || memcmp(data->bytes + data->length - suffix, endings[data->ending], suffix)) return false;
        size_t length = data->length - suffix;
        if (capacity <= length) return false;
        for (size_t i = 0; i < length; i++) if (!data->bytes[i] || data->bytes[i] > 0x7f) return false;
        memcpy(text, data->bytes, length);
        text[length] = '\0';
    } else {
        if (capacity < data->length * 3) return false;
        static const char digits[] = "0123456789ABCDEF";
        size_t used = 0;
        for (size_t i = 0; i < data->length; i++) {
            if (i) text[used++] = ' ';
            text[used++] = digits[data->bytes[i] >> 4];
            text[used++] = digits[data->bytes[i] & 15];
        }
        text[used] = '\0';
    }
    return true;
}

const char *uart_tx_error(uart_tx_status_t status)
{
    switch (status) {
    case UART_TX_OK: return "Ready";
    case UART_TX_EMPTY: return "Enter bytes, or select an ASCII line ending to send an empty line.";
    case UART_TX_TOO_LONG: return "Limit: 256 transmitted bytes including line ending; 767 Hex characters. Nothing sent.";
    case UART_TX_BAD_ASCII: return "ASCII accepts 7-bit characters only. Use Hex for binary or encoded text. Nothing sent.";
    case UART_TX_BAD_HEX: return "Hex needs whitespace-separated byte pairs, such as 01 AF 7E. Nothing sent.";
    default: return "Invalid transmit format or line ending. Nothing sent.";
    }
}

void uart_data_self_test(void)
{
#ifndef NDEBUG
    uart_tx_data_t data;
    char draft[16];
    assert(uart_tx_parse("01 aF 7e", UART_TX_HEX, UART_TX_END_NONE, &data) == UART_TX_OK);
    assert(data.length == 3 && data.bytes[0] == 1 && data.bytes[1] == 0xaf && data.bytes[2] == 0x7e);
    assert(uart_tx_parse("AT", UART_TX_ASCII, UART_TX_END_CRLF, &data) == UART_TX_OK);
    assert(data.length == 4 && !memcmp(data.bytes, "AT\r\n", 4));
    assert(uart_tx_format_draft(&data, draft, sizeof(draft)) && !strcmp(draft, "AT"));
    assert(uart_tx_parse("0\n1", UART_TX_HEX, UART_TX_END_NONE, &data) == UART_TX_BAD_HEX);
    assert(!data.length);
#endif
}
