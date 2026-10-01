#include "payload_clipboard.h"

#include <string.h>

static payload_clipboard_t clipboard;
static bool present;

bool payload_clipboard_store(const uint8_t *bytes, size_t length)
{
    if (length > sizeof(clipboard.bytes) || (!bytes && length)) return false;
    /* memmove also allows callers to copy a slice of the current clipboard. */
    if (length) memmove(clipboard.bytes, bytes, length);
    memset(clipboard.bytes + length, 0, sizeof(clipboard.bytes) - length);
    clipboard.length = length;
    present = true;
    return true;
}

const payload_clipboard_t *payload_clipboard_peek(void)
{
    return present ? &clipboard : NULL;
}

bool payload_clipboard_hex(char *text, size_t capacity)
{
    if (!text || !capacity) return false;
    text[0] = '\0';
    size_t required = clipboard.length ? clipboard.length * 3 : 1;
    if (!present || capacity < required) return false;
    static const char digits[] = "0123456789ABCDEF";
    size_t used = 0;
    for (size_t i = 0; i < clipboard.length; i++) {
        if (i) text[used++] = ' ';
        text[used++] = digits[clipboard.bytes[i] >> 4];
        text[used++] = digits[clipboard.bytes[i] & 15];
    }
    text[used] = '\0';
    return true;
}

void payload_clipboard_clear(void)
{
    memset(&clipboard, 0, sizeof(clipboard));
    present = false;
}
