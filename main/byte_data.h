#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define BYTE_DATA_MAX_BYTES 128
#define BYTE_DATA_MAX_TEXT 512
#define BYTE_NUMBER_MAX_TEXT 32

typedef enum {
    BYTE_DATA_HEX,
    BYTE_DATA_ASCII
} byte_data_mode_t;

typedef enum {
    BYTE_DATA_OK,
    BYTE_DATA_BAD_HEX,
    BYTE_DATA_ODD_HEX,
    BYTE_DATA_NON_ASCII,
    BYTE_DATA_TOO_MANY_BYTES,
    BYTE_DATA_TEXT_TOO_LONG,
    BYTE_DATA_BAD_ARGUMENT,
    BYTE_DATA_BAD_NUMBER,
    BYTE_DATA_NUMBER_RANGE,
    BYTE_DATA_NUMBER_TOO_LONG
} byte_data_status_t;

typedef enum {
    BYTE_NUMBER_UINT8, BYTE_NUMBER_INT8,
    BYTE_NUMBER_UINT16, BYTE_NUMBER_INT16,
    BYTE_NUMBER_UINT32, BYTE_NUMBER_INT32,
    BYTE_NUMBER_FLOAT32
} byte_number_type_t;

typedef struct {
    uint8_t bytes[BYTE_DATA_MAX_BYTES];
    size_t length;
    uint8_t sum8;
    uint8_t xor8;
    uint16_t crc16_modbus;
    uint32_t crc32;
    /* Integer interpretations, available only for exactly 1, 2 or 4 bytes.
     * Signed values use two's complement, independent of the host C ABI. */
    uint32_t little_endian;
    uint32_t big_endian;
    int32_t signed_little_endian;
    int32_t signed_big_endian;
    /* IEEE 754 binary32 interpretations for exactly 4 bytes. Stored as double
     * to preserve every finite binary32 value, including subnormals. */
    double float32_little_endian;
    double float32_big_endian;
} byte_data_t;

/* Hex requires complete byte pairs, with optional ASCII whitespace between
 * bytes. Prefixes, punctuation, odd nibbles and non-ASCII input are rejected.
 * ASCII means literal 7-bit bytes: no escape expansion or UTF-8 conversion.
 * Empty input is valid. On any error, the entire result is cleared. */
byte_data_status_t byte_data_parse(const char *text, byte_data_mode_t mode,
                                   byte_data_t *result);
/* Decimal integer or binary32 encoder, at most 32 ASCII characters. Integers
 * allow a leading +, and signed types allow -. Float32 also accepts decimal
 * fractions/exponents and exact tokens NaN, Inf, +Inf, -Inf. Finite decimal
 * overflow and nonzero values rounding to zero fail. Subnormals are retained.
 * Output includes checksums/interpretations; errors clear the entire result.
 * Requires an IEEE 754 binary32 C float (ESP32-P4 and supported test hosts). */
byte_data_status_t byte_data_encode(const char *text, byte_number_type_t type,
                                    bool little_endian, byte_data_t *result);
const char *byte_data_error(byte_data_status_t status);
/* IEEE 754 binary32 bits to double, preserving finite values and signed zero.
 * Shared by Byte Lab and multi-register views; no host float layout assumed. */
double byte_data_float32(uint32_t bits);
/* CRC-16/MODBUS, initial value 0xffff; append low byte then high byte on RTU.
 * bytes must hold length readable bytes; NULL is allowed only for length zero. */
uint16_t byte_data_crc16_modbus(const uint8_t *bytes, size_t length);
void byte_data_self_test(void);
