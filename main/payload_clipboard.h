#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAYLOAD_CLIPBOARD_MAX_BYTES 128
#define PAYLOAD_CLIPBOARD_HEX_SIZE (PAYLOAD_CLIPBOARD_MAX_BYTES * 3)

typedef struct {
    size_t length;
    uint8_t bytes[PAYLOAD_CLIPBOARD_MAX_BYTES];
} payload_clipboard_t;

/* UI-thread-only, RAM-only byte clipboard shared by the bench apps. No worker
 * may read it. A failed copy preserves the previous value. An empty copy is
 * distinct from no copy; peek's pointer is valid until the next store/clear. */
bool payload_clipboard_store(const uint8_t *bytes, size_t length);
const payload_clipboard_t *payload_clipboard_peek(void);
bool payload_clipboard_hex(char *text, size_t capacity);
void payload_clipboard_clear(void);
