#include "byte_data.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(float) == sizeof(uint32_t) && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
               FLT_MAX_EXP == 128 && FLT_MIN_EXP == -125, "Number encoding requires binary32 float");

static int hex_nibble(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int ascii_space(unsigned char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
           ch == '\v' || ch == '\f';
}

static int32_t signed_value(uint32_t value, unsigned bits)
{
    /* The intermediate fits in int64_t even for an unsigned 32-bit input.
     * Avoid implementation-defined out-of-range unsigned-to-signed casts. */
    int64_t result = value;
    if (value & (UINT32_C(1) << (bits - 1))) result -= INT64_C(1) << bits;
    return (int32_t)result;
}

double byte_data_float32(uint32_t bits)
{
    unsigned exponent = (bits >> 23) & 255;
    uint32_t fraction = bits & UINT32_C(0x7fffff);
    bool negative = (bits & UINT32_C(0x80000000)) != 0;
    if (exponent == 255) return fraction ? NAN : (negative ? -INFINITY : INFINITY);
    /* Decode the specified wire format without type punning or relying on the
     * host float layout/denormal handling. All finite binary32 values fit in
     * binary64 exactly; exponent zero has no implicit leading significand bit. */
    double value = ldexp((double)(fraction | (exponent ? UINT32_C(0x800000) : 0)),
                         exponent ? (int)exponent - 150 : -149);
    return negative ? -value : value;
}

uint16_t byte_data_crc16_modbus(const uint8_t *bytes, size_t length)
{
    uint16_t crc16 = UINT16_C(0xffff);
    for (size_t i = 0; i < length; i++) {
        crc16 ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc16 = (uint16_t)((crc16 >> 1) ^ ((crc16 & 1) ? UINT16_C(0xa001) : 0));
    }
    return crc16;
}

static void calculate(byte_data_t *data)
{
    uint32_t crc32 = UINT32_C(0xffffffff);
    for (size_t i = 0; i < data->length; i++) {
        uint8_t value = data->bytes[i];
        data->sum8 = (uint8_t)(data->sum8 + value);
        data->xor8 ^= value;
        crc32 ^= value;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc32 = (crc32 >> 1) ^ ((crc32 & 1) ? UINT32_C(0xedb88320) : 0);
        }
    }
    data->crc16_modbus = byte_data_crc16_modbus(data->bytes, data->length);
    data->crc32 = crc32 ^ UINT32_C(0xffffffff);
    if (data->length == 1 || data->length == 2 || data->length == 4) {
        for (size_t i = 0; i < data->length; i++) {
            data->big_endian = (data->big_endian << 8) | data->bytes[i];
            data->little_endian |= (uint32_t)data->bytes[i] << (8 * i);
        }
        data->signed_big_endian = signed_value(data->big_endian, (unsigned)data->length * 8);
        data->signed_little_endian = signed_value(data->little_endian, (unsigned)data->length * 8);
        if (data->length == 4) {
            data->float32_big_endian = byte_data_float32(data->big_endian);
            data->float32_little_endian = byte_data_float32(data->little_endian);
        }
    }
}

byte_data_status_t byte_data_parse(const char *text, byte_data_mode_t mode,
                                   byte_data_t *result)
{
    if (!result) return BYTE_DATA_BAD_ARGUMENT;
    memset(result, 0, sizeof(*result));
    if (!text || (mode != BYTE_DATA_HEX && mode != BYTE_DATA_ASCII)) {
        return BYTE_DATA_BAD_ARGUMENT;
    }

    byte_data_t parsed = {0};
    int high_nibble = -1;
    for (size_t i = 0; ; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (!ch) break;
        if (i == BYTE_DATA_MAX_TEXT) return BYTE_DATA_TEXT_TOO_LONG;
        uint8_t value;
        if (mode == BYTE_DATA_ASCII) {
            if (ch > 0x7f) return BYTE_DATA_NON_ASCII;
            value = ch;
        } else {
            if (ascii_space(ch)) {
                if (high_nibble >= 0) return BYTE_DATA_ODD_HEX;
                continue;
            }
            int nibble = hex_nibble(ch);
            if (nibble < 0) return BYTE_DATA_BAD_HEX;
            if (high_nibble < 0) {
                high_nibble = nibble;
                continue;
            }
            value = (uint8_t)((high_nibble << 4) | nibble);
            high_nibble = -1;
        }
        if (parsed.length == BYTE_DATA_MAX_BYTES) return BYTE_DATA_TOO_MANY_BYTES;
        parsed.bytes[parsed.length++] = value;
    }
    if (high_nibble >= 0) return BYTE_DATA_ODD_HEX;
    calculate(&parsed);
    *result = parsed;
    return BYTE_DATA_OK;
}

const char *byte_data_error(byte_data_status_t status)
{
    switch (status) {
    case BYTE_DATA_OK: return "";
    case BYTE_DATA_BAD_HEX: return "Use only hex byte pairs and whitespace. No 0x prefixes or punctuation.";
    case BYTE_DATA_ODD_HEX: return "Each hex byte needs two digits. Put spaces only between complete bytes.";
    case BYTE_DATA_NON_ASCII: return "ASCII input accepts only 7-bit characters (00-7F). Use hex for other bytes.";
    case BYTE_DATA_TOO_MANY_BYTES: return "Payload exceeds the 128-byte limit. No result calculated.";
    case BYTE_DATA_TEXT_TOO_LONG: return "Input exceeds the 512-character limit. No result calculated.";
    case BYTE_DATA_BAD_ARGUMENT: return "Invalid input or mode.";
    case BYTE_DATA_BAD_NUMBER: return "Invalid number for this type. No spaces, units, hex prefixes or line breaks.";
    case BYTE_DATA_NUMBER_RANGE: return "Number is outside this type's range, or a nonzero Float32 would round to zero.";
    case BYTE_DATA_NUMBER_TOO_LONG: return "Number exceeds the 32-character limit. No bytes encoded.";
    }
    return "Invalid input.";
}

static bool float_syntax(const char *text, bool *nonzero)
{
    *nonzero = false;
    if (*text == '+' || *text == '-') text++;
    bool digits = false;
    for (; *text >= '0' && *text <= '9'; text++) {
        digits = true;
        if (*text != '0') *nonzero = true;
    }
    if (*text == '.') {
        for (text++; *text >= '0' && *text <= '9'; text++) {
            digits = true;
            if (*text != '0') *nonzero = true;
        }
    }
    if (!digits) return false;
    if (*text == 'e' || *text == 'E') {
        text++;
        if (*text == '+' || *text == '-') text++;
        if (*text < '0' || *text > '9') return false;
        while (*text >= '0' && *text <= '9') text++;
    }
    return *text == '\0';
}

byte_data_status_t byte_data_encode(const char *text, byte_number_type_t type,
                                    bool little_endian, byte_data_t *result)
{
    if (!result) return BYTE_DATA_BAD_ARGUMENT;
    memset(result, 0, sizeof(*result));
    if (!text || (unsigned)type > BYTE_NUMBER_FLOAT32) return BYTE_DATA_BAD_ARGUMENT;
    for (size_t i = 0; text[i]; i++) {
        if (i == BYTE_NUMBER_MAX_TEXT) return BYTE_DATA_NUMBER_TOO_LONG;
        if ((unsigned char)text[i] > 127) return BYTE_DATA_BAD_NUMBER;
    }
    uint32_t bits;
    unsigned width;
    if (type == BYTE_NUMBER_FLOAT32) {
        width = 4;
        if (!strcmp(text, "NaN")) bits = UINT32_C(0x7fc00000);
        else if (!strcmp(text, "Inf") || !strcmp(text, "+Inf")) bits = UINT32_C(0x7f800000);
        else if (!strcmp(text, "-Inf")) bits = UINT32_C(0xff800000);
        else {
            bool nonzero;
            if (!float_syntax(text, &nonzero)) return BYTE_DATA_BAD_NUMBER;
            char *end;
            float value = strtof(text, &end);
            if (*end) return BYTE_DATA_BAD_NUMBER;
            /* Some C libraries report ERANGE for representable subnormals.
             * Reject the actual overflow/zero result, not that flag alone. */
            if (!isfinite(value) || (value == 0 && nonzero)) return BYTE_DATA_NUMBER_RANGE;
            memcpy(&bits, &value, sizeof(bits));
        }
    } else {
        width = 1U << ((unsigned)type / 2);
        bool signed_type = (unsigned)type % 2 != 0;
        bool negative = *text == '-';
        if (negative && !signed_type) return BYTE_DATA_BAD_NUMBER;
        if (*text == '+' || negative) text++;
        if (!*text) return BYTE_DATA_BAD_NUMBER;
        uint64_t limit = signed_type ? (UINT64_C(1) << (width * 8 - 1)) - (negative ? 0 : 1) :
                                      (UINT64_C(1) << (width * 8)) - 1;
        uint64_t magnitude = 0;
        for (; *text; text++) {
            if (*text < '0' || *text > '9') return BYTE_DATA_BAD_NUMBER;
            magnitude = magnitude * 10 + (unsigned)(*text - '0');
            if (magnitude > limit) return BYTE_DATA_NUMBER_RANGE;
        }
        bits = negative ? (uint32_t)(UINT64_C(0x100000000) - magnitude) : (uint32_t)magnitude;
    }
    byte_data_t encoded = {0};
    encoded.length = width;
    for (unsigned i = 0; i < width; i++) {
        unsigned shift = (little_endian ? i : width - 1 - i) * 8;
        encoded.bytes[i] = (uint8_t)(bits >> shift);
    }
    calculate(&encoded);
    *result = encoded;
    return BYTE_DATA_OK;
}

void byte_data_self_test(void)
{
#ifndef NDEBUG
    byte_data_t data;
    assert(byte_data_parse("123456789", BYTE_DATA_ASCII, &data) == BYTE_DATA_OK);
    assert(data.length == 9 && data.sum8 == 0xdd && data.xor8 == 0x31);
    assert(data.crc16_modbus == 0x4b37 && data.crc32 == UINT32_C(0xcbf43926));
    assert(byte_data_parse("01 03 00 00 00 0A", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.length == 6 && data.crc16_modbus == 0xcdc5);
    assert(byte_data_parse("BF 80 00 00", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.signed_big_endian == -1082130432 && data.float32_big_endian == -1.0);
    assert(byte_data_parse("12 3", BYTE_DATA_HEX, &data) == BYTE_DATA_ODD_HEX);
    assert(data.length == 0 && data.crc16_modbus == 0 && data.crc32 == 0);
    assert(byte_data_encode("-1234", BYTE_NUMBER_INT16, false, &data) == BYTE_DATA_OK);
    assert(data.length == 2 && data.bytes[0] == 0xfb && data.bytes[1] == 0x2e);
    assert(byte_data_encode("1.5", BYTE_NUMBER_FLOAT32, true, &data) == BYTE_DATA_OK);
    assert(data.length == 4 && data.little_endian == UINT32_C(0x3fc00000));
    assert(byte_data_encode("1e-45", BYTE_NUMBER_FLOAT32, false, &data) == BYTE_DATA_OK);
    assert(data.big_endian == 1);
#endif
}
