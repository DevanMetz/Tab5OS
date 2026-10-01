#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "payload_clipboard.h"

int main(void)
{
    char hex[PAYLOAD_CLIPBOARD_HEX_SIZE];
    assert(!payload_clipboard_peek());
    assert(!payload_clipboard_hex(hex, sizeof(hex)) && !hex[0]);
    assert(payload_clipboard_store(NULL, 0));
    assert(payload_clipboard_peek() && !payload_clipboard_peek()->length);
    assert(payload_clipboard_hex(hex, 1) && !hex[0]);
    uint8_t bytes[PAYLOAD_CLIPBOARD_MAX_BYTES + 1];
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)(i * 37);
    assert(payload_clipboard_store(bytes, PAYLOAD_CLIPBOARD_MAX_BYTES));
    const payload_clipboard_t *copy = payload_clipboard_peek();
    assert(copy->length == PAYLOAD_CLIPBOARD_MAX_BYTES);
    assert(!memcmp(copy->bytes, bytes, copy->length));
    assert(!payload_clipboard_store(bytes, sizeof(bytes)));
    assert(!payload_clipboard_store(NULL, 1));
    assert(copy->length == PAYLOAD_CLIPBOARD_MAX_BYTES);
    assert(!memcmp(copy->bytes, bytes, copy->length));
    assert(payload_clipboard_hex(hex, sizeof(hex)));
    assert(strlen(hex) == sizeof(hex) - 1);
    assert(!strncmp(hex, "00 25 4A 6F", 11));
    for (size_t i = 0; i < copy->length; i++) {
        unsigned decoded;
        assert(sscanf(hex + 3 * i, "%2x", &decoded) == 1 && decoded == bytes[i]);
    }
    assert(!payload_clipboard_hex(hex, sizeof(hex) - 1) && !hex[0]);
    assert(!payload_clipboard_hex(NULL, sizeof(hex)));
    assert(!payload_clipboard_hex(hex, 0));
    assert(payload_clipboard_store(copy->bytes + 1, 2));
    assert(copy->length == 2 && copy->bytes[0] == 37 && copy->bytes[1] == 74);
    for (size_t i = 2; i < sizeof(copy->bytes); i++) assert(copy->bytes[i] == 0);
    assert(payload_clipboard_hex(hex, 6) && !strcmp(hex, "25 4A"));
    assert(!payload_clipboard_hex(hex, 5) && !hex[0]);
    payload_clipboard_clear();
    assert(!payload_clipboard_peek() && !copy->length);
    for (size_t i = 0; i < sizeof(copy->bytes); i++) assert(copy->bytes[i] == 0);
    payload_clipboard_clear();
    puts("Clipboard empty/max/oversize/null/alias/format bounds and clearing PASS");
    return 0;
}
