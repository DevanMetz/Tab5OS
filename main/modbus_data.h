#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MODBUS_MAX_VALUES 16
#define MODBUS_REQUEST_BYTES 12
#define MODBUS_MAX_RESPONSE_BYTES 41
#define MODBUS_RTU_REQUEST_BYTES 8
#define MODBUS_RTU_MAX_RESPONSE_BYTES 37
#define MODBUS_VALUES_TEXT_SIZE 768

typedef struct {
    uint16_t transaction_id;
    uint8_t unit_id;
    uint8_t function;
    uint16_t address;
    uint16_t quantity;
} modbus_request_t;

typedef struct {
    uint16_t values[MODBUS_MAX_VALUES];
    size_t count;
    uint8_t exception_code;
} modbus_result_t;

typedef enum {
    MODBUS_OK,
    MODBUS_EXCEPTION,
    MODBUS_INVALID_REQUEST,
    MODBUS_INVALID_RESPONSE,
    MODBUS_CRC_MISMATCH
} modbus_status_t;

typedef enum {
    MODBUS_VIEW_WORDS,
    MODBUS_VIEW_UINT32,
    MODBUS_VIEW_INT32,
    MODBUS_VIEW_FLOAT32
} modbus_value_view_t;

/* A/B are the high/low bytes of the first received register, C/D the second.
 * Each name specifies their order from most to least significant in the value. */
typedef enum {
    MODBUS_ORDER_ABCD,
    MODBUS_ORDER_CDAB,
    MODBUS_ORDER_BADC,
    MODBUS_ORDER_DCBA
} modbus_byte_order_t;

/* Read-only FC1/2/3/4, 1..16 items, zero-based address, no range wrap.
 * Caller supplies exactly 12 writable output bytes. Clears output on failure. */
bool modbus_build_request(const modbus_request_t *request, uint8_t out[MODBUS_REQUEST_BYTES]);

/* Call after receiving exactly six bytes of MBAP header. Validates the echoed
 * transaction, protocol and bounded length before receiving the remaining ADU.
 * total_bytes includes the six supplied bytes; cleared on failure. */
bool modbus_response_length(const modbus_request_t *request, const uint8_t header[6],
                            size_t *total_bytes);

/* Requires one complete ADU with no missing or trailing bytes. Registers are
 * unsigned big-endian values; bits are returned as 0/1 in address order.
 * Errors clear the result. MODBUS_EXCEPTION retains only exception_code. */
modbus_status_t modbus_parse_response(const modbus_request_t *request,
                                     const uint8_t *bytes, size_t length,
                                     modbus_result_t *result);
/* RTU read frames use unit 1..247; broadcasts and reserved units are rejected.
 * transaction_id is unused. Build clears all eight output bytes on failure.
 * Parse requires exactly one complete frame, including low-byte-first CRC.
 * It validates bytes only, not serial timing or provenance. The supplied start
 * address is not echoed in a read response and cannot be verified from it.
 * Result clearing and exception behavior match the TCP parser above. */
bool modbus_rtu_build_request(const modbus_request_t *request,
                              uint8_t out[MODBUS_RTU_REQUEST_BYTES]);
modbus_status_t modbus_rtu_parse_response(const modbus_request_t *request,
                                         const uint8_t *bytes, size_t length,
                                         modbus_result_t *result);
const char *modbus_data_error(modbus_status_t status);

/* One to five decimal digits, no whitespace or signs; cleared on failure. */
bool modbus_parse_decimal(const char *text, uint16_t maximum, uint16_t *value);

/* Format a validated reply without network activity. Pair views start at the
 * request address; an odd final register stays visible as unpaired. FC1/2
 * always show bits. Invalid input or insufficient capacity clears text and
 * returns false. MODBUS_VALUES_TEXT_SIZE holds every supported result. */
bool modbus_format_values(const modbus_request_t *request, const modbus_result_t *result,
                          modbus_value_view_t view, modbus_byte_order_t order,
                          char *text, size_t capacity);
void modbus_data_self_test(void);
