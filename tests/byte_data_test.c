#include "byte_data.h"

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void expect_error(const char *text, byte_data_mode_t mode, byte_data_status_t expected)
{
    byte_data_t data;
    memset(&data, 0xff, sizeof(data));
    assert(byte_data_parse(text, mode, &data) == expected);
    assert(data.length == 0 && data.sum8 == 0 && data.xor8 == 0);
    assert(data.crc16_modbus == 0 && data.crc32 == 0);
    assert(data.little_endian == 0 && data.big_endian == 0);
    assert(data.signed_little_endian == 0 && data.signed_big_endian == 0);
    assert(data.float32_little_endian == 0 && data.float32_big_endian == 0);
    for (size_t i = 0; i < sizeof(data.bytes); i++) assert(data.bytes[i] == 0);
    assert(byte_data_error(expected)[0] != '\0');
}

static void expect_float(uint32_t bits, double expected)
{
    for (unsigned order = 0; order < 2; order++) {
        char text[12];
        unsigned a = (bits >> 24) & 255, b = (bits >> 16) & 255;
        unsigned c = (bits >> 8) & 255, d = bits & 255;
        snprintf(text, sizeof(text), "%02X %02X %02X %02X",
                 order ? d : a, order ? c : b, order ? b : c, order ? a : d);
        byte_data_t data;
        assert(byte_data_parse(text, BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
        double actual = order ? data.float32_little_endian : data.float32_big_endian;
        if (isnan(expected)) assert(isnan(actual));
        else {
            assert(actual == expected);
            assert(!!signbit(actual) == !!signbit(expected));
        }
    }
}

static void print_vectors(void)
{
    uint32_t seed = UINT32_C(0x74cdef93);
    for (unsigned i = 0; i < 4096; i++) {
        seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
        char text[9];
        snprintf(text, sizeof(text), "%08" PRIX32, seed);
        byte_data_t data;
        assert(byte_data_parse(text, BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
        printf("%s,%" PRId32 ",%" PRId32 ",%a,%a\n", text,
               data.signed_big_endian, data.signed_little_endian,
               data.float32_big_endian, data.float32_little_endian);
    }
}

static void expect_encoded(const char *number, byte_number_type_t type, const char *hex)
{
    byte_data_t expected;
    assert(byte_data_parse(hex, BYTE_DATA_HEX, &expected) == BYTE_DATA_OK);
    for (unsigned little = 0; little < 2; little++) {
        byte_data_t encoded;
        assert(byte_data_encode(number, type, little != 0, &encoded) == BYTE_DATA_OK);
        assert(encoded.length == expected.length);
        for (size_t i = 0; i < encoded.length; i++)
            assert(encoded.bytes[i] == expected.bytes[little ? encoded.length - 1 - i : i]);
        char text[12];
        size_t used = 0;
        for (size_t i = 0; i < encoded.length; i++)
            used += (size_t)snprintf(text + used, sizeof(text) - used, "%02X", (unsigned)encoded.bytes[i]);
        byte_data_t parsed;
        assert(byte_data_parse(text, BYTE_DATA_HEX, &parsed) == BYTE_DATA_OK);
        assert(encoded.sum8 == parsed.sum8 && encoded.xor8 == parsed.xor8);
        assert(encoded.crc16_modbus == parsed.crc16_modbus && encoded.crc32 == parsed.crc32);
        assert(encoded.big_endian == parsed.big_endian && encoded.little_endian == parsed.little_endian);
    }
}
static void encode_failure(const char *number, byte_number_type_t type, byte_data_status_t status)
{
    byte_data_t data;
    memset(&data, 0xff, sizeof(data));
    assert(byte_data_encode(number, type, false, &data) == status);
    const unsigned char *bytes = (const unsigned char *)&data;
    for (size_t i = 0; i < sizeof(data); i++) assert(!bytes[i]);
    assert(byte_data_error(status)[0]);
}
static void encode_checks(void)
{
    const char *maximums[] = {"255", "127", "65535", "32767", "4294967295", "2147483647"};
    const char *max_hex[] = {"FF", "7F", "FFFF", "7FFF", "FFFFFFFF", "7FFFFFFF"};
    const char *over[] = {"256", "128", "65536", "32768", "4294967296", "2147483648"};
    const char *zeros[] = {"00", "00", "0000", "0000", "00000000", "00000000"};
    for (unsigned type = 0; type < BYTE_NUMBER_FLOAT32; type++) {
        expect_encoded(maximums[type], (byte_number_type_t)type, max_hex[type]);
        expect_encoded("+000", (byte_number_type_t)type, zeros[type]);
        encode_failure(over[type], (byte_number_type_t)type, BYTE_DATA_NUMBER_RANGE);
        const char *invalid[] = {"", "+", "-", "1.0", "1e2", "0x12", "1,000", "1 2", "1\n2", "12m", " 1", "1 ", "++1", "NaN", "Inf", "\xc2\xb9"};
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
            encode_failure(invalid[i], (byte_number_type_t)type, BYTE_DATA_BAD_NUMBER);
    }
    expect_encoded("-128", BYTE_NUMBER_INT8, "80");
    expect_encoded("-32768", BYTE_NUMBER_INT16, "8000");
    expect_encoded("-2147483648", BYTE_NUMBER_INT32, "80000000");
    expect_encoded("-42", BYTE_NUMBER_INT8, "D6");
    expect_encoded("-1234", BYTE_NUMBER_INT16, "FB2E");
    expect_encoded("-0", BYTE_NUMBER_INT32, "00000000");
    encode_failure("-129", BYTE_NUMBER_INT8, BYTE_DATA_NUMBER_RANGE);
    encode_failure("-32769", BYTE_NUMBER_INT16, BYTE_DATA_NUMBER_RANGE);
    encode_failure("-2147483649", BYTE_NUMBER_INT32, BYTE_DATA_NUMBER_RANGE);
    for (unsigned type = 0; type <= BYTE_NUMBER_UINT32; type += 2)
        encode_failure("-0", (byte_number_type_t)type, BYTE_DATA_BAD_NUMBER);
    const struct { const char *number, *hex; } floats[] = {
        {"0", "00000000"}, {"-0", "80000000"}, {"-0.0e-999", "80000000"},
        {"1.5", "3FC00000"}, {"+1E+2", "42C80000"}, {".5", "3F000000"},
        {"5.", "40A00000"}, {"0.1", "3DCCCCCD"}, {"-1", "BF800000"},
        {"1e-45", "00000001"}, {"-1e-45", "80000001"},
        {"1.17549435e-38", "00800000"}, {"1.17549421e-38", "007FFFFF"},
        {"3.40282347e38", "7F7FFFFF"}, {"-3.40282347e38", "FF7FFFFF"},
        {"16777217", "4B800000"}, {"16777219", "4B800002"},
        {"1.000000059604644775390625", "3F800000"},
        {"1.000000059604644775390626", "3F800001"},
        {"Inf", "7F800000"}, {"+Inf", "7F800000"}, {"-Inf", "FF800000"}, {"NaN", "7FC00000"}
    };
    for (size_t i = 0; i < sizeof(floats) / sizeof(floats[0]); i++)
        expect_encoded(floats[i].number, BYTE_NUMBER_FLOAT32, floats[i].hex);
    const char *invalid_float[] = {"", ".", "+", "-", "1e", "1e+", "e1", ".e1", "1.2.3", "1e2e3",
        "1e2.0", "1e 2", "1\r2", "1\t2", "1\n2", "1,5", "0x1p0", "1f", "1 ", " 1", "nan", "NAN", "-NaN", "nan(1)", "Infinity"};
    for (size_t i = 0; i < sizeof(invalid_float) / sizeof(invalid_float[0]); i++)
        encode_failure(invalid_float[i], BYTE_NUMBER_FLOAT32, BYTE_DATA_BAD_NUMBER);
    const char *out_of_range[] = {"1e39", "-1e39", "1e-46", "-1e-46", "1e99999", "1e-99999"};
    for (size_t i = 0; i < sizeof(out_of_range) / sizeof(out_of_range[0]); i++)
        encode_failure(out_of_range[i], BYTE_NUMBER_FLOAT32, BYTE_DATA_NUMBER_RANGE);
    char long_text[BYTE_NUMBER_MAX_TEXT + 2];
    memset(long_text, '0', sizeof(long_text) - 1); long_text[sizeof(long_text) - 1] = 0;
    encode_failure(long_text, BYTE_NUMBER_FLOAT32, BYTE_DATA_NUMBER_TOO_LONG);
    encode_failure(long_text, BYTE_NUMBER_UINT32, BYTE_DATA_NUMBER_TOO_LONG);
    long_text[BYTE_NUMBER_MAX_TEXT] = 0;
    expect_encoded(long_text, BYTE_NUMBER_UINT8, "00");
    encode_failure(NULL, BYTE_NUMBER_UINT32, BYTE_DATA_BAD_ARGUMENT);
    encode_failure("1", (byte_number_type_t)-1, BYTE_DATA_BAD_ARGUMENT);
    encode_failure("1", (byte_number_type_t)7, BYTE_DATA_BAD_ARGUMENT);
    assert(byte_data_encode("1", BYTE_NUMBER_UINT32, false, NULL) == BYTE_DATA_BAD_ARGUMENT);
}
static void encode_stream(void)
{
    char line[128];
    while (fgets(line, sizeof(line), stdin)) {
        assert(line[0] >= '0' && line[0] <= '6' && line[1] == ',' &&
               (line[2] == '0' || line[2] == '1') && line[3] == ',');
        unsigned type = (unsigned)(line[0] - '0'), little = (unsigned)(line[2] - '0');
        line[strcspn(line, "\r\n")] = 0;
        byte_data_t data;
        byte_data_status_t status = byte_data_encode(line + 4, (byte_number_type_t)type, little != 0, &data);
        if (status != BYTE_DATA_OK) { printf("ERROR:%u\n", (unsigned)status); continue; }
        for (size_t i = 0; i < data.length; i++) printf("%02X", (unsigned)data.bytes[i]);
        putchar('\n');
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--vectors") == 0) {
        print_vectors();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--encode")) { encode_stream(); return 0; }
    encode_checks();
    byte_data_self_test();
    byte_data_t data;
    assert(byte_data_parse("", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.length == 0 && data.crc16_modbus == 0xffff && data.crc32 == 0);
    assert(byte_data_parse(" \t\n\r\v\f", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.length == 0);
    assert(byte_data_parse("", BYTE_DATA_ASCII, &data) == BYTE_DATA_OK);
    assert(data.length == 0 && data.crc16_modbus == 0xffff && data.crc32 == 0);

    assert(byte_data_parse("31323334 35363738\n39", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.length == 9 && memcmp(data.bytes, "123456789", 9) == 0);
    assert(data.sum8 == 0xdd && data.xor8 == 0x31);
    assert(data.crc16_modbus == 0x4b37 && data.crc32 == UINT32_C(0xcbf43926));
    assert(byte_data_parse("01 03 00 00 00 0a", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.crc16_modbus == 0xcdc5);
    assert((data.crc16_modbus & 0xff) == 0xc5 && (data.crc16_modbus >> 8) == 0xcd);

    assert(byte_data_parse("00 fF 7f 80", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.length == 4 && data.bytes[0] == 0 && data.bytes[1] == 0xff);
    assert(data.bytes[2] == 0x7f && data.bytes[3] == 0x80);
    assert(data.sum8 == 0xfe && data.xor8 == 0);
    assert(data.big_endian == UINT32_C(0x00ff7f80));
    assert(data.little_endian == UINT32_C(0x807fff00));
    assert(byte_data_parse("FF FF FF FF", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.big_endian == UINT32_MAX && data.little_endian == UINT32_MAX);
    assert(data.signed_big_endian == -1 && data.signed_little_endian == -1);
    assert(isnan(data.float32_big_endian) && isnan(data.float32_little_endian));
    assert(byte_data_parse("12 34", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.big_endian == 4660 && data.little_endian == 13330);
    assert(byte_data_parse("80", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.big_endian == 128 && data.little_endian == 128);
    assert(data.signed_big_endian == -128 && data.signed_little_endian == -128);
    assert(byte_data_parse("7F", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.signed_big_endian == 127 && data.signed_little_endian == 127);
    assert(byte_data_parse("FF", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.signed_big_endian == -1 && data.signed_little_endian == -1);
    assert(byte_data_parse("80 00", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.signed_big_endian == -32768 && data.signed_little_endian == 128);
    assert(byte_data_parse("00 80", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.signed_big_endian == 128 && data.signed_little_endian == -32768);
    assert(byte_data_parse("FF 7F", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.signed_big_endian == -129 && data.signed_little_endian == 32767);
    assert(byte_data_parse("7F FF FF FF", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.signed_big_endian == INT32_MAX && data.signed_little_endian == -129);
    assert(byte_data_parse("80 00 00 00", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.signed_big_endian == INT32_MIN && data.signed_little_endian == 128);
    assert(byte_data_parse("00 00 00 80", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.signed_big_endian == 128 && data.signed_little_endian == INT32_MIN);
    assert(byte_data_parse("12 34 56", BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.big_endian == 0 && data.little_endian == 0);
    assert(data.signed_big_endian == 0 && data.signed_little_endian == 0);
    assert(data.float32_big_endian == 0 && data.float32_little_endian == 0);

    expect_float(UINT32_C(0x00000000), 0.0);
    expect_float(UINT32_C(0x80000000), -0.0);
    expect_float(UINT32_C(0x3f800000), 1.0);
    expect_float(UINT32_C(0xbf800000), -1.0);
    expect_float(UINT32_C(0xc0200000), -2.5);
    expect_float(UINT32_C(0x3dcccccd), 0x1.99999ap-4); /* Nearest binary32 to 0.1. */
    expect_float(UINT32_C(0x7f7fffff), 0x1.fffffep127);
    expect_float(UINT32_C(0xff7fffff), -0x1.fffffep127);
    expect_float(UINT32_C(0x00800000), 0x1p-126);
    expect_float(UINT32_C(0x007fffff), 0x1.fffffcp-127);
    expect_float(UINT32_C(0x00000001), 0x1p-149);
    expect_float(UINT32_C(0x80000001), -0x1p-149);
    expect_float(UINT32_C(0x7f800000), INFINITY);
    expect_float(UINT32_C(0xff800000), -INFINITY);
    expect_float(UINT32_C(0x7fc00000), NAN);
    expect_float(UINT32_C(0x7f800001), NAN); /* Signalling-NaN wire payload. */
    assert(byte_data_parse("A\n\\n\177", BYTE_DATA_ASCII, &data) == BYTE_DATA_OK);
    assert(data.length == 5 && data.bytes[1] == '\n' && data.bytes[2] == '\\' && data.bytes[4] == 0x7f);

    expect_error("0", BYTE_DATA_HEX, BYTE_DATA_ODD_HEX);
    expect_error("1 2", BYTE_DATA_HEX, BYTE_DATA_ODD_HEX);
    expect_error("12 345", BYTE_DATA_HEX, BYTE_DATA_ODD_HEX);
    expect_error("12 3\t4", BYTE_DATA_HEX, BYTE_DATA_ODD_HEX);
    expect_error("0x12", BYTE_DATA_HEX, BYTE_DATA_BAD_HEX);
    expect_error("12,34", BYTE_DATA_HEX, BYTE_DATA_BAD_HEX);
    expect_error("12:34", BYTE_DATA_HEX, BYTE_DATA_BAD_HEX);
    expect_error("12-34", BYTE_DATA_HEX, BYTE_DATA_BAD_HEX);
    expect_error("12 GG", BYTE_DATA_HEX, BYTE_DATA_BAD_HEX);
    expect_error("12\xc2\xa0" "34", BYTE_DATA_HEX, BYTE_DATA_BAD_HEX);
    expect_error("caf\xc3\xa9", BYTE_DATA_ASCII, BYTE_DATA_NON_ASCII);
    expect_error("\x80", BYTE_DATA_ASCII, BYTE_DATA_NON_ASCII);
    expect_error(NULL, BYTE_DATA_HEX, BYTE_DATA_BAD_ARGUMENT);
    expect_error("12", (byte_data_mode_t)2, BYTE_DATA_BAD_ARGUMENT);
    assert(byte_data_parse("12", BYTE_DATA_HEX, NULL) == BYTE_DATA_BAD_ARGUMENT);

    char ascii[BYTE_DATA_MAX_BYTES + 2];
    memset(ascii, 'A', sizeof(ascii));
    ascii[BYTE_DATA_MAX_BYTES] = '\0';
    assert(byte_data_parse(ascii, BYTE_DATA_ASCII, &data) == BYTE_DATA_OK);
    assert(data.length == BYTE_DATA_MAX_BYTES && data.sum8 == 0x80 && data.xor8 == 0);
    ascii[BYTE_DATA_MAX_BYTES] = 'A';
    ascii[BYTE_DATA_MAX_BYTES + 1] = '\0';
    expect_error(ascii, BYTE_DATA_ASCII, BYTE_DATA_TOO_MANY_BYTES);

    char hex[BYTE_DATA_MAX_BYTES * 3 + 4];
    for (size_t i = 0; i < BYTE_DATA_MAX_BYTES; i++) memcpy(hex + i * 3, "a5 ", 3);
    hex[BYTE_DATA_MAX_BYTES * 3] = '\0';
    assert(byte_data_parse(hex, BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.length == BYTE_DATA_MAX_BYTES);
    for (size_t i = 0; i < data.length; i++) assert(data.bytes[i] == 0xa5);
    memcpy(hex + BYTE_DATA_MAX_BYTES * 3, "00", 3);
    expect_error(hex, BYTE_DATA_HEX, BYTE_DATA_TOO_MANY_BYTES);

    char spaces[BYTE_DATA_MAX_TEXT + 2];
    memset(spaces, ' ', sizeof(spaces));
    spaces[BYTE_DATA_MAX_TEXT] = '\0';
    assert(byte_data_parse(spaces, BYTE_DATA_HEX, &data) == BYTE_DATA_OK);
    assert(data.length == 0);
    spaces[BYTE_DATA_MAX_TEXT] = ' ';
    spaces[BYTE_DATA_MAX_TEXT + 1] = '\0';
    expect_error(spaces, BYTE_DATA_HEX, BYTE_DATA_TEXT_TOO_LONG);

    puts("byte data tests passed");
    return 0;
}
