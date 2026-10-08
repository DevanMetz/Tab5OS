#include "uart_data.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void invalid(const char *text, uart_tx_mode_t mode, uart_tx_ending_t ending, uart_tx_status_t expected)
{
    uart_tx_data_t data;
    memset(&data, 0xff, sizeof(data));
    assert(uart_tx_parse(text, mode, ending, &data) == expected);
    for (size_t i = 0; i < sizeof(data); i++) assert(((const unsigned char *)&data)[i] == 0);
}

int main(void)
{
    uart_data_self_test();
    uart_tx_data_t data, restored;
    char draft[UART_TX_MAX_HEX_TEXT + 2];
    const char *const suffixes[] = {"", "\n", "\r", "\r\n"};
    for (unsigned ending = 0; ending < 4; ending++) {
        for (size_t length = 0; length <= 257; length++) {
            char ascii[258];
            memset(ascii, 'A', length); ascii[length] = 0;
            size_t total = length + strlen(suffixes[ending]);
            if (total == 0 || total > 256) {
                invalid(ascii, UART_TX_ASCII, (uart_tx_ending_t)ending, total ? UART_TX_TOO_LONG : UART_TX_EMPTY);
                continue;
            }
            assert(uart_tx_parse(ascii, UART_TX_ASCII, (uart_tx_ending_t)ending, &data) == UART_TX_OK);
            assert(data.length == total && !memcmp(data.bytes, ascii, length));
            assert(!memcmp(data.bytes + length, suffixes[ending], total - length));
            assert(uart_tx_format_draft(&data, draft, length + 1));
            assert(!strcmp(draft, ascii));
            assert(uart_tx_parse(draft, data.mode, data.ending, &restored) == UART_TX_OK);
            assert(restored.length == data.length && !memcmp(restored.bytes, data.bytes, data.length));
            if (length) assert(!uart_tx_format_draft(&data, draft, length) && !draft[0]);
        }
    }
    for (size_t length = 1; length <= UART_TX_MAX_BYTES; length++) {
        for (size_t i = 0; i < length; i++) {
            if (i) draft[i * 3 - 1] = ' ';
            snprintf(draft + i * 3, 3, "%02X", (unsigned)(uint8_t)i);
        }
        assert(uart_tx_parse(draft, UART_TX_HEX, UART_TX_END_NONE, &data) == UART_TX_OK);
        assert(data.length == length);
        for (size_t i = 0; i < length; i++) assert(data.bytes[i] == (uint8_t)i);
        assert(uart_tx_format_draft(&data, draft, length * 3));
        assert(uart_tx_parse(draft, UART_TX_HEX, UART_TX_END_NONE, &restored) == UART_TX_OK);
        assert(restored.length == data.length && !memcmp(data.bytes, restored.bytes, length));
        assert(!uart_tx_format_draft(&data, draft, length * 3 - 1) && !draft[0]);
    }
    invalid("", UART_TX_ASCII, UART_TX_END_NONE, UART_TX_EMPTY);
    invalid(" \t\r\n", UART_TX_HEX, UART_TX_END_NONE, UART_TX_EMPTY);
    const char *bad_hex[] = {"0", "0 0", "0011", "0xFF", "FF,01", "1\n2", "GG", ("FF\xc2\xa0" "01")};
    for (size_t i = 0; i < sizeof(bad_hex) / sizeof(bad_hex[0]); i++)
        invalid(bad_hex[i], UART_TX_HEX, UART_TX_END_NONE, UART_TX_BAD_HEX);
    assert(uart_tx_parse("\r\nA5\t5a\v\f00 ", UART_TX_HEX, UART_TX_END_NONE, &data) == UART_TX_OK);
    assert(data.length == 3 && data.bytes[0] == 0xa5 && data.bytes[1] == 0x5a && !data.bytes[2]);
    assert(uart_tx_parse("Hi\n\\n", UART_TX_ASCII, UART_TX_END_CRLF, &data) == UART_TX_OK);
    assert(data.length == 7 && !memcmp(data.bytes, "Hi\n\\n\r\n", 7));
    for (unsigned ch = 128; ch <= 255; ch++) {
        char text[] = {'A', (char)ch, 0};
        invalid(text, UART_TX_ASCII, UART_TX_END_NONE, UART_TX_BAD_ASCII);
    }
    memset(draft, ' ', UART_TX_MAX_HEX_TEXT + 1); draft[UART_TX_MAX_HEX_TEXT + 1] = 0;
    invalid(draft, UART_TX_HEX, UART_TX_END_NONE, UART_TX_TOO_LONG);
    invalid(NULL, UART_TX_ASCII, UART_TX_END_NONE, UART_TX_BAD_OPTIONS);
    invalid("AA", (uart_tx_mode_t)-1, UART_TX_END_NONE, UART_TX_BAD_OPTIONS);
    invalid("AA", UART_TX_ASCII, (uart_tx_ending_t)4, UART_TX_BAD_OPTIONS);
    invalid("AA", UART_TX_HEX, UART_TX_END_LF, UART_TX_BAD_OPTIONS);
    assert(uart_tx_parse("A", UART_TX_ASCII, UART_TX_END_NONE, NULL) == UART_TX_BAD_OPTIONS);
    assert(!uart_tx_format_draft(NULL, draft, sizeof(draft)) && !draft[0]);
    assert(uart_tx_parse("AT", UART_TX_ASCII, UART_TX_END_CRLF, &data) == UART_TX_OK);
    data.bytes[data.length - 1] = 'x';
    assert(!uart_tx_format_draft(&data, draft, sizeof(draft)) && !draft[0]);
    data.length = UART_TX_MAX_BYTES + 1;
    assert(!uart_tx_format_draft(&data, draft, sizeof(draft)) && !draft[0]);
    puts("Serial TX: all lengths/endings, 256-byte Hex, exact history replay, invalid input and output bounds PASS");
    return 0;
}
