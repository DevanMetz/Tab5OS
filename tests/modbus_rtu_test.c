#include "modbus_data.h"
#include "byte_data.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Independent, MSB-first polynomial calculation with reflected input/output. */
static uint16_t reference_crc(const uint8_t *bytes, size_t length)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; i++) {
        uint8_t reflected = 0;
        for (unsigned bit = 0; bit < 8; bit++)
            reflected |= (uint8_t)(((bytes[i] >> bit) & 1u) << (7 - bit));
        crc ^= (uint16_t)((uint16_t)reflected << 8);
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x8005 : 0));
    }
    uint16_t reflected = 0;
    for (unsigned bit = 0; bit < 16; bit++)
        reflected |= (uint16_t)(((crc >> bit) & 1u) << (15 - bit));
    return reflected;
}

static void append_crc(uint8_t *bytes, size_t length)
{
    uint16_t crc = reference_crc(bytes, length);
    bytes[length] = (uint8_t)crc;
    bytes[length + 1] = (uint8_t)(crc >> 8);
}

static void failure(const modbus_request_t *request, const uint8_t *bytes,
                    size_t length, modbus_status_t expected)
{
    modbus_result_t result, zero = {0};
    memset(&result, 0xff, sizeof(result));
    assert(modbus_rtu_parse_response(request, bytes, length, &result) == expected);
    assert(memcmp(&result, &zero, sizeof(result)) == 0);
}

static void bad_request(modbus_request_t request)
{
    uint8_t out[MODBUS_RTU_REQUEST_BYTES];
    memset(out, 0xff, sizeof(out));
    assert(!modbus_rtu_build_request(&request, out));
    for (size_t i = 0; i < sizeof(out); i++) assert(!out[i]);
    failure(&request, NULL, 0, MODBUS_INVALID_REQUEST);
}

int main(void)
{
    assert(reference_crc((const uint8_t *)"123456789", 9) == 0x4b37);
    assert(byte_data_crc16_modbus((const uint8_t *)"123456789", 9) == 0x4b37);
    assert(byte_data_crc16_modbus(NULL, 0) == 0xffff);
    modbus_request_t request = {0xbeef, 1, 3, 0, 10};
    uint8_t out[MODBUS_RTU_REQUEST_BYTES];
    const uint8_t golden[] = {1, 3, 0, 0, 0, 10, 0xc5, 0xcd};
    assert(modbus_rtu_build_request(&request, out));
    assert(!memcmp(out, golden, sizeof(golden)));
    assert(reference_crc(golden, 6) == 0xcdc5);
    assert(byte_data_crc16_modbus(golden, sizeof(golden)) == 0);

    for (unsigned unit = 1; unit <= 247; unit++) {
        for (unsigned function = 1; function <= 4; function++) {
            request = (modbus_request_t){0xffff, (uint8_t)unit, (uint8_t)function, 65535, 1};
            assert(modbus_rtu_build_request(&request, out));
            assert(out[0] == unit && out[1] == function && out[2] == 255 && out[3] == 255);
            assert(out[4] == 0 && out[5] == 1);
            assert((out[6] | (out[7] << 8)) == reference_crc(out, 6));
            request.quantity = 2;
            bad_request(request);
        }
    }
    request = (modbus_request_t){0, 1, 3, 0, 1};
    for (unsigned unit = 0; unit <= 255; unit++) {
        if (unit >= 1 && unit <= 247) continue;
        request.unit_id = (uint8_t)unit;
        bad_request(request);
    }
    request.unit_id = 1;
    for (unsigned function = 0; function <= 255; function++) {
        if (function >= 1 && function <= 4) continue;
        request.function = (uint8_t)function;
        bad_request(request);
    }
    request.function = 3;
    request.quantity = 0; bad_request(request);
    request.quantity = 17; bad_request(request);
    assert(!modbus_rtu_build_request(NULL, out));
    for (size_t i = 0; i < sizeof(out); i++) assert(!out[i]);
    assert(!modbus_rtu_build_request(&request, NULL));
    failure(NULL, NULL, 0, MODBUS_INVALID_REQUEST);
    assert(modbus_rtu_parse_response(&request, NULL, 0, NULL) == MODBUS_INVALID_RESPONSE);

    for (unsigned function = 1; function <= 4; function++) {
        for (unsigned count = 1; count <= MODBUS_MAX_VALUES; count++) {
            request = (modbus_request_t){0x1234, 247, (uint8_t)function,
                                         (uint16_t)(65536 - count), (uint16_t)count};
            uint8_t reply[MODBUS_RTU_MAX_RESPONSE_BYTES + 1] = {247, (uint8_t)function, 0};
            size_t data_size = function <= 2 ? (count + 7) / 8 : 2 * count;
            reply[2] = (uint8_t)data_size;
            for (unsigned i = 0; i < count; i++) {
                if (function <= 2) reply[3 + i / 8] |= (uint8_t)((i % 2) << (i % 8));
                else { reply[3 + 2 * i] = (uint8_t)i; reply[4 + 2 * i] = (uint8_t)(255 - i); }
            }
            size_t length = data_size + 5;
            append_crc(reply, length - 2);
            modbus_result_t result;
            assert(modbus_rtu_parse_response(&request, reply, length, &result) == MODBUS_OK);
            assert(result.count == count && !result.exception_code);
            for (unsigned i = 0; i < count; i++)
                assert(result.values[i] == (function <= 2 ? i % 2 : 256 * i + 255 - i));
            for (size_t i = count; i < MODBUS_MAX_VALUES; i++) assert(!result.values[i]);
            /* The same PDU must decode identically in TCP and RTU envelopes. */
            uint8_t tcp[MODBUS_MAX_RESPONSE_BYTES] = {0x12, 0x34, 0, 0, 0, 0};
            tcp[5] = (uint8_t)(length - 2);
            memcpy(tcp + 6, reply, length - 2);
            modbus_result_t tcp_result;
            assert(modbus_parse_response(&request, tcp, length + 4, &tcp_result) == MODBUS_OK);
            assert(!memcmp(&result, &tcp_result, sizeof(result)));

            /* Every single-bit corruption, short frame and extra byte fails. */
            for (size_t i = 0; i < length; i++) {
                for (unsigned bit = 0; bit < 8; bit++) {
                    reply[i] ^= (uint8_t)(1u << bit);
                    failure(&request, reply, length, i ? MODBUS_CRC_MISMATCH : MODBUS_INVALID_RESPONSE);
                    reply[i] ^= (uint8_t)(1u << bit);
                }
            }
            for (size_t shorter = 0; shorter < length; shorter++) {
                modbus_status_t status = modbus_rtu_parse_response(&request, reply, shorter, &result);
                assert(status != MODBUS_OK && status != MODBUS_EXCEPTION && !result.count);
            }
            append_crc(reply, length - 1); /* Consistent CRC but extra PDU byte. */
            failure(&request, reply, length + 1, MODBUS_INVALID_RESPONSE);
            if (function <= 2 && count % 8) {
                reply[2 + data_size] |= 0x80;
                append_crc(reply, length - 2);
                failure(&request, reply, length, MODBUS_INVALID_RESPONSE);
            }
        }
    }
    request = (modbus_request_t){0, 1, 3, 0, 1};
    uint8_t exception[] = {1, 0x83, 2, 0xc0, 0xf1};
    modbus_result_t result;
    assert(modbus_rtu_parse_response(&request, exception, sizeof(exception), &result) == MODBUS_EXCEPTION);
    assert(result.exception_code == 2 && !result.count);
    for (size_t i = 0; i < MODBUS_MAX_VALUES; i++) assert(!result.values[i]);
    exception[2] = 0; append_crc(exception, 3);
    failure(&request, exception, sizeof(exception), MODBUS_INVALID_RESPONSE);
    exception[1] = 0x84; exception[2] = 2; append_crc(exception, 3);
    failure(&request, exception, sizeof(exception), MODBUS_INVALID_RESPONSE);
    uint8_t wrong[] = {1, 4, 2, 0, 10, 0, 0};
    append_crc(wrong, 5);
    failure(&request, wrong, sizeof(wrong), MODBUS_INVALID_RESPONSE);
    wrong[1] = 3; wrong[2] = 1; append_crc(wrong, 5);
    failure(&request, wrong, sizeof(wrong), MODBUS_INVALID_RESPONSE);
    failure(&request, NULL, 5, MODBUS_INVALID_RESPONSE);
    assert(strstr(modbus_data_error(MODBUS_CRC_MISMATCH), "CRC mismatch"));
    puts("Modbus RTU framing, CRC, shared PDU and failure-clearing checks passed.");
    return 0;
}
