#include "modbus_data.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_failure(const modbus_request_t *request, const uint8_t *bytes,
                           size_t length, modbus_status_t status)
{
    modbus_result_t result;
    memset(&result, 0xff, sizeof(result));
    assert(modbus_parse_response(request, bytes, length, &result) == status);
    assert(result.count == 0 && result.exception_code == 0);
    for (size_t i = 0; i < MODBUS_MAX_VALUES; i++) assert(result.values[i] == 0);
    assert(modbus_data_error(status)[0]);
}

static void expect_bad_request(modbus_request_t request)
{
    uint8_t out[MODBUS_REQUEST_BYTES];
    memset(out, 0xff, sizeof(out));
    assert(!modbus_build_request(&request, out));
    for (size_t i = 0; i < sizeof(out); i++) assert(out[i] == 0);
    expect_failure(&request, NULL, 0, MODBUS_INVALID_REQUEST);
}

static void value_views(void)
{
    modbus_request_t request = {1, 1, 3, 10, 2};
    modbus_result_t result = {.values = {0x47f1, 0x2000}, .count = 2};
    char text[MODBUS_VALUES_TEXT_SIZE];
    /* Known 123456.0 examples from the four libmodbus float-order references. */
    const uint16_t words[4][2] = {{0x47f1, 0x2000}, {0x2000, 0x47f1}, {0xf147, 0x0020}, {0x0020, 0xf147}};
    for (unsigned order = 0; order < 4; order++) {
        memcpy(result.values, words[order], sizeof(words[order]));
        assert(modbus_format_values(&request, &result, MODBUS_VIEW_FLOAT32, (modbus_byte_order_t)order, text, sizeof(text)));
        assert(strstr(text, "10-11 : 123456\n") && strstr(text, "bits 0x47F12000"));
    }
    const struct { uint16_t first, second; const char *unsigned_text, *signed_text, *float_text; } known[] = {
        {0, 0, "0", "0", "0"},
        {0x8000, 0, "2147483648", "-2147483648", "-0"},
        {0xffff, 0xffff, "4294967295", "-1", "NaN"},
        {0x7fff, 0xffff, "2147483647", "2147483647", "NaN"},
        {0x7f80, 0, "2139095040", "2139095040", "+Infinity"},
        {0xff80, 0, "4286578688", "-8388608", "-Infinity"},
        {0x3f80, 0, "1065353216", "1065353216", "1"},
        {0xbf80, 0, "3212836864", "-1082130432", "-1"},
        {0, 1, "1", "1", "1.40129846e-45"},
        {0x7f7f, 0xffff, "2139095039", "2139095039", "3.40282347e+38"}
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        result.values[0] = known[i].first; result.values[1] = known[i].second;
        const char *numbers[] = {known[i].unsigned_text, known[i].signed_text, known[i].float_text};
        for (unsigned view = MODBUS_VIEW_UINT32; view <= MODBUS_VIEW_FLOAT32; view++) {
            assert(modbus_format_values(&request, &result, (modbus_value_view_t)view, MODBUS_ORDER_ABCD, text, sizeof(text)));
            char expected[64]; snprintf(expected, sizeof(expected), "10-11 : %s\n", numbers[view - 1]);
            assert(strstr(text, expected));
        }
    }
    request.quantity = 3; result.count = 3;
    result.values[0] = 0xffff; result.values[1] = 0x8000; result.values[2] = 0x7fff;
    assert(modbus_format_values(&request, &result, MODBUS_VIEW_WORDS, MODBUS_ORDER_DCBA, text, sizeof(text)));
    assert(strstr(text, "10 : 65535 | 0xFFFF | -1\n"));
    assert(strstr(text, "11 : 32768 | 0x8000 | -32768\n"));
    assert(strstr(text, "12 : 32767 | 0x7FFF | 32767\n"));
    assert(modbus_format_values(&request, &result, MODBUS_VIEW_FLOAT32, MODBUS_ORDER_ABCD, text, sizeof(text)));
    assert(strstr(text, "12 : unpaired register 0x7FFF"));
    request.address = 65535; request.quantity = 1; result.count = 1;
    assert(modbus_format_values(&request, &result, MODBUS_VIEW_UINT32, MODBUS_ORDER_ABCD, text, sizeof(text)));
    assert(strstr(text, "65535 : unpaired register 0xFFFF"));
    assert(!strstr(text, "65536"));
    /* The fixed output bound must hold for every quantity, view and byte order,
     * with the highest addresses and long negative/NaN/infinite representations. */
    for (unsigned count = 1; count <= MODBUS_MAX_VALUES; count++) {
        request.address = (uint16_t)(65536 - count); request.quantity = (uint16_t)count; result.count = count;
        for (unsigned i = 0; i < count; i++) result.values[i] = i % 2 ? 0xffff : 0xff7f;
        for (unsigned view = 0; view <= MODBUS_VIEW_FLOAT32; view++) {
            for (unsigned order = 0; order <= MODBUS_ORDER_DCBA; order++) {
                assert(modbus_format_values(&request, &result, (modbus_value_view_t)view, (modbus_byte_order_t)order, text, sizeof(text)));
                assert(strstr(text, "65535") && strlen(text) < sizeof(text));
            }
        }
    }
    request = (modbus_request_t){1, 1, 1, 10, 2};
    result = (modbus_result_t){.values = {1, 0}, .count = 2};
    for (unsigned function = 1; function <= 2; function++) {
        request.function = (uint8_t)function;
        assert(modbus_format_values(&request, &result, MODBUS_VIEW_FLOAT32, MODBUS_ORDER_DCBA, text, sizeof(text)));
        assert(!strcmp(text, "Zero-based address : bit value\n10 : 1\n11 : 0\n"));
    }
    /* Failure must not leave a partial number or read past a short buffer. */
    char guarded[] = "!!!!";
    assert(!modbus_format_values(&request, &result, MODBUS_VIEW_WORDS, MODBUS_ORDER_ABCD, guarded, 1));
    assert(guarded[0] == 0 && !memcmp(guarded + 1, "!!!", 3));
    for (size_t capacity = 1; capacity < strlen("Zero-based address : bit value\n10 : 1\n11 : 0\n") + 1; capacity++) {
        memset(text, '!', sizeof(text));
        assert(!modbus_format_values(&request, &result, MODBUS_VIEW_WORDS, MODBUS_ORDER_ABCD, text, capacity));
        assert(text[0] == 0 && text[capacity] == '!');
    }
    assert(!modbus_format_values(&request, &result, MODBUS_VIEW_WORDS, MODBUS_ORDER_ABCD, NULL, 10));
    assert(!modbus_format_values(&request, &result, MODBUS_VIEW_WORDS, MODBUS_ORDER_ABCD, text, 0));
    assert(!modbus_format_values(NULL, &result, MODBUS_VIEW_WORDS, MODBUS_ORDER_ABCD, text, sizeof(text)) && !text[0]);
    assert(!modbus_format_values(&request, NULL, MODBUS_VIEW_WORDS, MODBUS_ORDER_ABCD, text, sizeof(text)) && !text[0]);
    assert(!modbus_format_values(&request, &result, (modbus_value_view_t)-1, MODBUS_ORDER_ABCD, text, sizeof(text)) && !text[0]);
    assert(!modbus_format_values(&request, &result, MODBUS_VIEW_WORDS, (modbus_byte_order_t)4, text, sizeof(text)) && !text[0]);
    result.values[1] = 2;
    assert(!modbus_format_values(&request, &result, MODBUS_VIEW_WORDS, MODBUS_ORDER_ABCD, text, sizeof(text)) && !text[0]);
    result.values[1] = 0; result.exception_code = 2;
    assert(!modbus_format_values(&request, &result, MODBUS_VIEW_WORDS, MODBUS_ORDER_ABCD, text, sizeof(text)) && !text[0]);
    result.exception_code = 0; result.count = 1;
    assert(!modbus_format_values(&request, &result, MODBUS_VIEW_WORDS, MODBUS_ORDER_ABCD, text, sizeof(text)) && !text[0]);
    result.count = MODBUS_MAX_VALUES + 1;
    assert(!modbus_format_values(&request, &result, MODBUS_VIEW_WORDS, MODBUS_ORDER_ABCD, text, sizeof(text)) && !text[0]);
}

static void print_vectors(void)
{
    modbus_request_t request = {1, 1, 3, 10, 2};
    uint32_t seed = UINT32_C(0x96374bd1);
    for (unsigned i = 0; i < 1024; i++) {
        seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
        modbus_result_t result = {.values = {(uint16_t)(seed >> 16), (uint16_t)seed}, .count = 2};
        for (unsigned order = 0; order <= MODBUS_ORDER_DCBA; order++) {
            printf("%08lX,%u", (unsigned long)seed, order);
            for (unsigned view = MODBUS_VIEW_UINT32; view <= MODBUS_VIEW_FLOAT32; view++) {
                char text[MODBUS_VALUES_TEXT_SIZE];
                assert(modbus_format_values(&request, &result, (modbus_value_view_t)view, (modbus_byte_order_t)order, text, sizeof(text)));
                char *number = strstr(text, "10-11 : "); assert(number); number += 8;
                char *end = strchr(number, '\n'); assert(end); *end = 0;
                printf(",%s", number);
            }
            putchar('\n');
        }
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--vectors")) { print_vectors(); return 0; }
    value_views();
    modbus_data_self_test();
    uint16_t value;
    assert(modbus_parse_decimal("0", 65535, &value) && value == 0);
    assert(modbus_parse_decimal("65535", 65535, &value) && value == 65535);
    assert(modbus_parse_decimal("00016", 16, &value) && value == 16);
    assert(modbus_parse_decimal("255", 255, &value) && value == 255);
    const char *bad_numbers[] = {NULL, "", " ", "+1", "-1", "1 ", " 1", "1\n", "1.0", "0x1", "1a", "65536", "000001", "999999999999999999"};
    for (size_t i = 0; i < sizeof(bad_numbers) / sizeof(bad_numbers[0]); i++) {
        value = 1234;
        assert(!modbus_parse_decimal(bad_numbers[i], 65535, &value) && value == 0);
    }
    assert(!modbus_parse_decimal("256", 255, &value) && value == 0);
    assert(!modbus_parse_decimal("1", 0, &value) && value == 0);
    assert(!modbus_parse_decimal("1", 65535, NULL));

    modbus_request_t request = {0xabcd, 255, 3, 0x1234, 2};
    uint8_t out[MODBUS_REQUEST_BYTES];
    const uint8_t golden_request[] = {0xab, 0xcd, 0, 0, 0, 6, 0xff, 3, 0x12, 0x34, 0, 2};
    assert(modbus_build_request(&request, out));
    assert(memcmp(out, golden_request, sizeof(out)) == 0);
    for (unsigned function = 1; function <= 4; function++) {
        request.function = (uint8_t)function;
        request.address = 65535;
        request.quantity = 1;
        request.unit_id = 0;
        assert(modbus_build_request(&request, out));
        assert(out[6] == 0 && out[7] == function && out[8] == 0xff && out[9] == 0xff);
        request.quantity = 2;
        expect_bad_request(request);
        request.address = 65520;
        request.quantity = 16;
        assert(modbus_build_request(&request, out));
        request.address++;
        expect_bad_request(request);
    }
    request = (modbus_request_t){1, 1, 3, 0, 0};
    expect_bad_request(request);
    request.quantity = 17;
    expect_bad_request(request);
    request.quantity = 1;
    const uint8_t bad_functions[] = {0, 5, 6, 15, 16, 23, 0x83, 255};
    for (size_t i = 0; i < sizeof(bad_functions); i++) {
        request.function = bad_functions[i];
        expect_bad_request(request);
    }
    assert(!modbus_build_request(NULL, out));
    for (size_t i = 0; i < sizeof(out); i++) assert(out[i] == 0);
    assert(!modbus_build_request(&request, NULL));

    request = (modbus_request_t){0x1234, 0x11, 3, 0, 3};
    const uint8_t registers[] = {0x12, 0x34, 0, 0, 0, 9, 0x11, 3, 6, 2, 0x2b, 0, 0, 0xff, 0xff};
    modbus_result_t result;
    assert(modbus_parse_response(&request, registers, sizeof(registers), &result) == MODBUS_OK);
    assert(result.count == 3 && result.values[0] == 555 && result.values[1] == 0 && result.values[2] == 65535);
    assert(result.exception_code == 0);
    size_t length = 999;
    assert(modbus_response_length(&request, registers, &length) && length == sizeof(registers));
    for (size_t i = 0; i < sizeof(registers); i++) {
        expect_failure(&request, registers, i, MODBUS_INVALID_RESPONSE);
    }
    uint8_t damaged[MODBUS_MAX_RESPONSE_BYTES + 1] = {0};
    for (size_t i = 0; i < 9; i++) {
        memcpy(damaged, registers, sizeof(registers));
        damaged[i] ^= 0xff;
        expect_failure(&request, damaged, sizeof(registers), MODBUS_INVALID_RESPONSE);
    }
    memcpy(damaged, registers, sizeof(registers));
    expect_failure(&request, damaged, sizeof(registers) + 1, MODBUS_INVALID_RESPONSE);
    expect_failure(&request, damaged, sizeof(damaged), MODBUS_INVALID_RESPONSE);
    expect_failure(&request, NULL, sizeof(registers), MODBUS_INVALID_RESPONSE);
    expect_failure(NULL, registers, sizeof(registers), MODBUS_INVALID_REQUEST);
    assert(modbus_parse_response(&request, registers, sizeof(registers), NULL) == MODBUS_INVALID_RESPONSE);

    const uint16_t bad_lengths[] = {0, 1, 2, 4, 8, 10, 35, 36, 254, 65535};
    for (size_t i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); i++) {
        memcpy(damaged, registers, sizeof(registers));
        damaged[4] = (uint8_t)(bad_lengths[i] >> 8);
        damaged[5] = (uint8_t)bad_lengths[i];
        length = 999;
        assert(!modbus_response_length(&request, damaged, &length) && length == 0);
        expect_failure(&request, damaged, sizeof(registers), MODBUS_INVALID_RESPONSE);
    }
    length = 999;
    assert(!modbus_response_length(&request, NULL, &length) && length == 0);
    assert(!modbus_response_length(&request, registers, NULL));
    assert(!modbus_response_length(NULL, registers, &length) && length == 0);

    uint8_t exception[] = {0x12, 0x34, 0, 0, 0, 3, 0x11, 0x83, 2};
    memset(&result, 0xff, sizeof(result));
    assert(modbus_parse_response(&request, exception, sizeof(exception), &result) == MODBUS_EXCEPTION);
    assert(result.count == 0 && result.exception_code == 2);
    for (size_t i = 0; i < MODBUS_MAX_VALUES; i++) assert(result.values[i] == 0);
    exception[8] = 0xff; /* Unknown nonzero exceptions remain visible to the user. */
    assert(modbus_parse_response(&request, exception, sizeof(exception), &result) == MODBUS_EXCEPTION);
    assert(result.exception_code == 0xff && result.count == 0);
    exception[8] = 0;
    expect_failure(&request, exception, sizeof(exception), MODBUS_INVALID_RESPONSE);
    exception[8] = 2;
    exception[7] = 0x84;
    expect_failure(&request, exception, sizeof(exception), MODBUS_INVALID_RESPONSE);
    memcpy(damaged, registers, sizeof(registers));
    damaged[7] = 0x83;
    expect_failure(&request, damaged, sizeof(registers), MODBUS_INVALID_RESPONSE);
    memcpy(damaged, exception, sizeof(exception));
    damaged[7] = 3;
    expect_failure(&request, damaged, sizeof(exception), MODBUS_INVALID_RESPONSE);

    /* Every supported function and quantity, including full 41-byte responses. */
    for (unsigned function = 1; function <= 4; function++) {
        for (unsigned quantity = 1; quantity <= MODBUS_MAX_VALUES; quantity++) {
            request = (modbus_request_t){0xffff, 0xff, (uint8_t)function, 65520, (uint16_t)quantity};
            memset(damaged, 0, sizeof(damaged));
            size_t data_bytes = function <= 2 ? (quantity + 7) / 8 : quantity * 2;
            length = 9 + data_bytes;
            damaged[0] = 0xff;
            damaged[1] = 0xff;
            damaged[5] = (uint8_t)(length - 6);
            damaged[6] = 0xff;
            damaged[7] = (uint8_t)function;
            damaged[8] = (uint8_t)data_bytes;
            for (unsigned i = 0; i < quantity; i++) {
                if (function <= 2) damaged[9 + i / 8] |= (uint8_t)((i % 3 == 0) << (i % 8));
                else {
                    damaged[9 + i * 2] = (uint8_t)i;
                    damaged[10 + i * 2] = (uint8_t)(255 - i);
                }
            }
            assert(modbus_parse_response(&request, damaged, length, &result) == MODBUS_OK);
            assert(result.count == quantity && result.exception_code == 0);
            for (unsigned i = 0; i < quantity; i++) {
                assert(result.values[i] == (function <= 2 ? (i % 3 == 0) : (i * 256 + 255 - i)));
            }
            if (function <= 2 && quantity % 8) {
                damaged[length - 1] |= 0x80;
                expect_failure(&request, damaged, length, MODBUS_INVALID_RESPONSE);
            }
        }
    }
    /* Published bit-order example starts CD (LSB first: 1,0,1,1,0,0,1,1). */
    request = (modbus_request_t){1, 1, 1, 19, 16};
    const uint8_t coils[] = {0, 1, 0, 0, 0, 5, 1, 1, 2, 0xcd, 0x6b};
    const uint16_t bits[] = {1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0};
    assert(modbus_parse_response(&request, coils, sizeof(coils), &result) == MODBUS_OK);
    assert(memcmp(result.values, bits, sizeof(bits)) == 0);

    puts("modbus data tests passed");
    return 0;
}
