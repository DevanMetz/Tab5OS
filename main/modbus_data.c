#include "modbus_data.h"
#include "byte_data.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Wire format: Modbus Application Protocol V1.1b3, sections 6.1-6.4 and 7;
 * Modbus Messaging Implementation Guide V1.0b, MBAP header.
 * https://www.modbus.org/file/secure/modbusprotocolspecification.pdf
 * https://www.modbus.org/file/secure/messagingimplementationguide.pdf
 * RTU framing: Modbus over Serial Line V1.02, sections 2.2 and 2.5.1.
 * https://www.modbus.org/file/secure/modbusoverserial.pdf */

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static bool valid_request(const modbus_request_t *request)
{
    return request && request->function >= 1 && request->function <= 4 &&
           request->quantity >= 1 && request->quantity <= MODBUS_MAX_VALUES &&
           (uint32_t)request->address + request->quantity <= UINT32_C(65536);
}

static size_t response_data_bytes(const modbus_request_t *request)
{
    return request->function <= 2 ? (request->quantity + 7u) / 8u : request->quantity * 2u;
}

static void build_read_pdu(const modbus_request_t *request, uint8_t pdu[5])
{
    pdu[0] = request->function;
    write_u16(pdu + 1, request->address);
    write_u16(pdu + 3, request->quantity);
}

bool modbus_build_request(const modbus_request_t *request, uint8_t out[MODBUS_REQUEST_BYTES])
{
    if (!out) return false;
    memset(out, 0, MODBUS_REQUEST_BYTES);
    if (!valid_request(request)) return false;
    write_u16(out, request->transaction_id);
    write_u16(out + 4, 6); /* Unit id plus five-byte read PDU. */
    out[6] = request->unit_id;
    build_read_pdu(request, out + 7);
    return true;
}

bool modbus_response_length(const modbus_request_t *request, const uint8_t header[6],
                            size_t *total_bytes)
{
    if (!total_bytes) return false;
    *total_bytes = 0;
    if (!valid_request(request) || !header) return false;
    if (read_u16(header) != request->transaction_id || read_u16(header + 2) != 0) return false;
    size_t total = (size_t)read_u16(header + 4) + 6;
    if (total != 9 && total != 9 + response_data_bytes(request)) return false;
    *total_bytes = total;
    return true;
}

/* Wrappers validate their envelope and clear result before calling this. */
static modbus_status_t parse_read_pdu(const modbus_request_t *request,
                                      const uint8_t *pdu, size_t length,
                                      modbus_result_t *result)
{
    if (length < 2) return MODBUS_INVALID_RESPONSE;
    if (pdu[0] == (uint8_t)(request->function | 0x80)) {
        if (length != 2 || pdu[1] == 0) return MODBUS_INVALID_RESPONSE;
        result->exception_code = pdu[1];
        return MODBUS_EXCEPTION;
    }
    size_t data_bytes = response_data_bytes(request);
    if (pdu[0] != request->function || pdu[1] != data_bytes || length != 2 + data_bytes) {
        return MODBUS_INVALID_RESPONSE;
    }
    if (request->function <= 2 && request->quantity % 8) {
        /* The standard requires unused high bits in the final byte to be zero. */
        uint8_t mask = (uint8_t)(0xffu << (request->quantity % 8));
        if (pdu[length - 1] & mask) return MODBUS_INVALID_RESPONSE;
    }
    for (size_t i = 0; i < request->quantity; i++) {
        result->values[i] = request->function <= 2 ?
            (uint16_t)((pdu[2 + i / 8] >> (i % 8)) & 1) : read_u16(pdu + 2 + 2 * i);
    }
    result->count = request->quantity;
    return MODBUS_OK;
}

modbus_status_t modbus_parse_response(const modbus_request_t *request,
                                     const uint8_t *bytes, size_t length,
                                     modbus_result_t *result)
{
    if (!result) return MODBUS_INVALID_RESPONSE;
    memset(result, 0, sizeof(*result));
    if (!valid_request(request)) return MODBUS_INVALID_REQUEST;
    if (!bytes || length < 9 || length > MODBUS_MAX_RESPONSE_BYTES) return MODBUS_INVALID_RESPONSE;
    size_t expected_length;
    if (!modbus_response_length(request, bytes, &expected_length) || length != expected_length ||
        bytes[6] != request->unit_id) return MODBUS_INVALID_RESPONSE;
    return parse_read_pdu(request, bytes + 7, length - 7, result);
}

static bool valid_rtu_request(const modbus_request_t *request)
{
    return valid_request(request) && request->unit_id >= 1 && request->unit_id <= 247;
}

bool modbus_rtu_build_request(const modbus_request_t *request,
                              uint8_t out[MODBUS_RTU_REQUEST_BYTES])
{
    if (!out) return false;
    memset(out, 0, MODBUS_RTU_REQUEST_BYTES);
    if (!valid_rtu_request(request)) return false;
    out[0] = request->unit_id;
    build_read_pdu(request, out + 1);
    uint16_t crc = byte_data_crc16_modbus(out, 6);
    out[6] = (uint8_t)crc;
    out[7] = (uint8_t)(crc >> 8);
    return true;
}

modbus_status_t modbus_rtu_parse_response(const modbus_request_t *request,
                                         const uint8_t *bytes, size_t length,
                                         modbus_result_t *result)
{
    if (!result) return MODBUS_INVALID_RESPONSE;
    memset(result, 0, sizeof(*result));
    if (!valid_rtu_request(request)) return MODBUS_INVALID_REQUEST;
    if (!bytes || length < 5 || length > MODBUS_RTU_MAX_RESPONSE_BYTES ||
        bytes[0] != request->unit_id) return MODBUS_INVALID_RESPONSE;
    uint16_t wire_crc = (uint16_t)(bytes[length - 2] | ((uint16_t)bytes[length - 1] << 8));
    if (byte_data_crc16_modbus(bytes, length - 2) != wire_crc) return MODBUS_CRC_MISMATCH;
    return parse_read_pdu(request, bytes + 1, length - 3, result);
}

const char *modbus_data_error(modbus_status_t status)
{
    switch (status) {
    case MODBUS_OK: return "";
    case MODBUS_EXCEPTION: return "Device returned a Modbus exception.";
    case MODBUS_INVALID_REQUEST: return "Use read function 1-4, quantity 1-16 and an address range within 0-65535.";
    case MODBUS_INVALID_RESPONSE: return "Invalid Modbus response: header, function, length or data did not match the request.";
    case MODBUS_CRC_MISMATCH: return "RTU CRC mismatch. Include the complete reply with its two CRC bytes, low byte first.";
    }
    return "Invalid Modbus response.";
}

bool modbus_parse_decimal(const char *text, uint16_t maximum, uint16_t *value)
{
    if (!value) return false;
    *value = 0;
    if (!text || !text[0]) return false;
    uint32_t parsed = 0;
    for (size_t i = 0; text[i]; i++) {
        if (i == 5 || text[i] < '0' || text[i] > '9') return false;
        parsed = parsed * 10 + (unsigned)(text[i] - '0');
        if (parsed > maximum) return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

bool modbus_format_values(const modbus_request_t *request, const modbus_result_t *result,
                          modbus_value_view_t view, modbus_byte_order_t order,
                          char *text, size_t capacity)
{
    if (!text || !capacity) return false;
    text[0] = '\0';
    if (!valid_request(request) || !result || result->exception_code ||
        result->count != request->quantity || (unsigned)view > MODBUS_VIEW_FLOAT32 ||
        (unsigned)order > MODBUS_ORDER_DCBA) return false;
    static const char *const views[] = {"", "Unsigned 32-bit", "Signed 32-bit", "Float32"};
    static const char *const orders[] = {"ABCD", "CDAB", "BADC", "DCBA"};
    bool bits = request->function <= 2;
    int written;
    if (bits) written = snprintf(text, capacity, "Zero-based address : bit value\n");
    else if (view == MODBUS_VIEW_WORDS)
        written = snprintf(text, capacity, "Address : unsigned | hex | signed int16\n");
    else written = snprintf(text, capacity, "%s | %s | zero-based address pairs\n", views[view], orders[order]);
    if (written < 0 || (size_t)written >= capacity) { text[0] = '\0'; return false; }
    size_t used = (size_t)written;
    for (size_t i = 0; i < result->count; ) {
        unsigned address = request->address + (unsigned)i;
        uint16_t first = result->values[i];
        if (bits) {
            if (first > 1) { text[0] = '\0'; return false; }
            written = snprintf(text + used, capacity - used, "%u : %u\n", address, (unsigned)first);
            i++;
        } else if (view == MODBUS_VIEW_WORDS) {
            long signed_value = first >= 32768 ? (long)first - 65536 : first;
            written = snprintf(text + used, capacity - used, "%u : %u | 0x%04X | %ld\n",
                               address, (unsigned)first, (unsigned)first, signed_value);
            i++;
        } else if (i + 1 == result->count) {
            written = snprintf(text + used, capacity - used,
                               "%u : unpaired register 0x%04X (needs the next register)\n",
                               address, (unsigned)first);
            i++;
        } else {
            uint16_t second = result->values[i + 1];
            uint32_t value = (uint32_t)first << 16 | second;
            if (order == MODBUS_ORDER_CDAB || order == MODBUS_ORDER_DCBA)
                value = value << 16 | value >> 16;
            if (order == MODBUS_ORDER_BADC || order == MODBUS_ORDER_DCBA)
                value = (value & UINT32_C(0x00ff00ff)) << 8 | (value & UINT32_C(0xff00ff00)) >> 8;
            char number[32];
            if (view == MODBUS_VIEW_UINT32) snprintf(number, sizeof(number), "%lu", (unsigned long)value);
            else if (view == MODBUS_VIEW_INT32) {
                int64_t signed_value = value;
                if (value & UINT32_C(0x80000000)) signed_value -= INT64_C(0x100000000);
                snprintf(number, sizeof(number), "%ld", (long)signed_value);
            } else {
                double floating = byte_data_float32(value);
                if (isnan(floating)) snprintf(number, sizeof(number), "NaN");
                else if (isinf(floating)) snprintf(number, sizeof(number), "%sInfinity", signbit(floating) ? "-" : "+");
                else snprintf(number, sizeof(number), "%.9g", floating);
            }
            written = snprintf(text + used, capacity - used,
                               "%u-%u : %s\n  Raw %04X %04X | bits 0x%08lX\n", address, address + 1,
                               number, (unsigned)first, (unsigned)second, (unsigned long)value);
            i += 2;
        }
        if (written < 0 || (size_t)written >= capacity - used) { text[0] = '\0'; return false; }
        used += (size_t)written;
    }
    return true;
}

void modbus_data_self_test(void)
{
#ifndef NDEBUG
    modbus_request_t request = {0x1234, 0x11, 3, 0, 3};
    uint8_t out[MODBUS_REQUEST_BYTES];
    const uint8_t expected[] = {0x12, 0x34, 0, 0, 0, 6, 0x11, 3, 0, 0, 0, 3};
    assert(modbus_build_request(&request, out));
    assert(memcmp(out, expected, sizeof(out)) == 0);
    const uint8_t response[] = {0x12, 0x34, 0, 0, 0, 9, 0x11, 3, 6, 2, 0x2b, 0, 0, 0, 0x64};
    modbus_result_t result;
    assert(modbus_parse_response(&request, response, sizeof(response), &result) == MODBUS_OK);
    assert(result.count == 3 && result.values[0] == 555 && result.values[1] == 0 && result.values[2] == 100);
    assert(modbus_parse_response(&request, response, sizeof(response) - 1, &result) == MODBUS_INVALID_RESPONSE);
    assert(result.count == 0 && result.values[0] == 0);
    request.quantity = 2;
    result = (modbus_result_t){.values = {0x47f1, 0x2000}, .count = 2};
    char text[MODBUS_VALUES_TEXT_SIZE];
    assert(modbus_format_values(&request, &result, MODBUS_VIEW_FLOAT32, MODBUS_ORDER_ABCD, text, sizeof(text)));
    assert(strstr(text, "0-1 : 123456\n"));
    request = (modbus_request_t){0, 1, 3, 0, 10};
    const uint8_t rtu_request[] = {1, 3, 0, 0, 0, 10, 0xc5, 0xcd};
    assert(modbus_rtu_build_request(&request, out));
    assert(!memcmp(out, rtu_request, sizeof(rtu_request)));
    const uint8_t rtu_exception[] = {1, 0x83, 2, 0xc0, 0xf1};
    assert(modbus_rtu_parse_response(&request, rtu_exception, sizeof(rtu_exception), &result) == MODBUS_EXCEPTION);
    assert(result.exception_code == 2 && !result.count);
#endif
}
