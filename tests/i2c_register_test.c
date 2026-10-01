/* SDK/BSP service adapter: no threads, physical bus or firmware runtime. */
#include "i2c_register.h"
#include "bsp/esp-bsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check(expression) do { if (!(expression)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); exit(1); \
} } while (0)

struct host_i2c_bus { int unused; };
struct host_i2c_device { int unused; };
static struct host_i2c_bus bus;
static struct host_i2c_device device;
static bool bus_live, device_live;
static unsigned operations, starts, reads, writes, removals;
static uint8_t last_address, last_reg, last_write;
static uint32_t last_speed;
static size_t last_length;
static esp_err_t start_error, add_error, read_error, write_error, remove_error, delete_error;

esp_err_t bsp_ext_i2c_init(void)
{
    operations++; starts++;
    check(!bus_live && !device_live);
    if (start_error) return start_error;
    bus_live = true;
    return ESP_OK;
}

esp_err_t bsp_ext_i2c_deinit(void)
{
    operations++;
    check(!device_live); /* Never delete a bus still holding a device. */
    if (!bus_live) return ESP_OK;
    if (delete_error) return delete_error;
    bus_live = false;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_ext_i2c_get_handle(void)
{
    return bus_live ? &bus : NULL;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t target, const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *out)
{
    operations++;
    check(target == &bus && bus_live && !device_live);
    check(config->dev_addr_length == I2C_ADDR_BIT_LEN_7);
    check(config->device_address >= 0x08 && config->device_address <= 0x77);
    check(config->scl_speed_hz == 100000 || config->scl_speed_hz == 400000);
    last_address = (uint8_t)config->device_address;
    last_speed = config->scl_speed_hz;
    if (add_error) { *out = NULL; return add_error; }
    device_live = true; *out = &device;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t target)
{
    operations++; removals++;
    check(target == &device && bus_live && device_live);
    if (remove_error) return remove_error;
    device_live = false;
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t target, const uint8_t *tx, size_t tx_length,
                                     uint8_t *rx, size_t rx_length, int timeout_ms)
{
    operations++; reads++;
    check(target == &device && bus_live && device_live);
    check(tx && tx_length == 1 && rx && rx_length >= 1 && rx_length <= 32 && timeout_ms == 50);
    last_reg = tx[0]; last_length = rx_length;
    /* Even a failing driver may have written receive data. */
    for (size_t i = 0; i < rx_length; i++) rx[i] = (uint8_t)(0x40 + i);
    return read_error;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t target, const uint8_t *bytes, size_t length,
                             int timeout_ms)
{
    operations++; writes++;
    check(target == &device && bus_live && device_live && bytes && length == 2 && timeout_ms == 50);
    last_reg = bytes[0]; last_write = bytes[1];
    return write_error;
}

static void expect_zero(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) check(bytes[i] == 0);
}

int main(void)
{
    uint8_t guarded[34];
    const uint8_t addresses[] = {0x08, 0x3c, 0x77};
    const uint32_t speeds[] = {100000, 400000};
    for (size_t a = 0; a < sizeof(addresses); a++) {
        for (size_t s = 0; s < sizeof(speeds) / sizeof(speeds[0]); s++) {
            for (size_t length = 1; length <= 32; length++) {
                memset(guarded, 0xcc, sizeof(guarded));
                uint8_t reg = length % 2 ? 0xff : 0x00;
                check(i2c_register_read(addresses[a], reg, speeds[s], guarded + 1, length) == ESP_OK);
                check(last_address == addresses[a] && last_speed == speeds[s] && last_reg == reg && last_length == length);
                check(guarded[0] == 0xcc);
                for (size_t i = 0; i < length; i++) check(guarded[i + 1] == (uint8_t)(0x40 + i));
                for (size_t i = length + 1; i < sizeof(guarded); i++) check(guarded[i] == 0xcc);
                check(!bus_live && !device_live && !i2c_register_busy());
            }
        }
    }
    check(reads == 192 && !writes);
    unsigned before = operations;
    memset(guarded, 0xcc, sizeof(guarded));
    const size_t bad_lengths[] = {0, 33, SIZE_MAX};
    for (size_t i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); i++)
        check(i2c_register_read(0x3c, 0, 100000, guarded, bad_lengths[i]) == ESP_ERR_INVALID_ARG);
    check(i2c_register_read(0x3c, 0, 100000, NULL, 1) == ESP_ERR_INVALID_ARG);
    const uint8_t bad_addresses[] = {0, 7, 0x78, 0x7f, 0xff};
    for (size_t i = 0; i < sizeof(bad_addresses); i++) {
        check(i2c_register_read(bad_addresses[i], 0, 100000, guarded, 1) == ESP_ERR_INVALID_ARG);
        check(i2c_register_write_byte(bad_addresses[i], 0, 100000, 0) == ESP_ERR_INVALID_ARG);
    }
    const uint32_t bad_speeds[] = {0, 99999, 100001, 399999, 400001, UINT32_MAX};
    for (size_t i = 0; i < sizeof(bad_speeds) / sizeof(bad_speeds[0]); i++) {
        check(i2c_register_read(0x3c, 0, bad_speeds[i], guarded, 1) == ESP_ERR_INVALID_ARG);
        check(i2c_register_write_byte(0x3c, 0, bad_speeds[i], 0) == ESP_ERR_INVALID_ARG);
    }
    check(operations == before);
    for (size_t i = 0; i < sizeof(guarded); i++) check(guarded[i] == 0xcc);

    esp_err_t *faults[] = {&start_error, &add_error, &read_error};
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        *faults[i] = ESP_ERR_TIMEOUT; memset(guarded, 0xcc, sizeof(guarded));
        check(i2c_register_read(0x3c, 0x10, 400000, guarded + 1, 7) == ESP_ERR_TIMEOUT);
        expect_zero(guarded + 1, 7); check(guarded[0] == 0xcc && guarded[8] == 0xcc);
        check(!i2c_register_busy()); *faults[i] = ESP_OK;
    }

    remove_error = ESP_FAIL;
    check(i2c_register_read(0x3c, 0x10, 100000, guarded, 32) == ESP_FAIL);
    expect_zero(guarded, 32); check(i2c_register_busy() && bus_live && device_live);
    unsigned reads_before = reads, starts_before = starts;
    check(i2c_register_read(0x3c, 0, 400000, guarded, 1) == ESP_FAIL);
    check(reads == reads_before && starts == starts_before);
    check(i2c_register_stop() == ESP_FAIL && device_live);
    remove_error = ESP_OK; delete_error = ESP_ERR_TIMEOUT;
    check(i2c_register_stop() == ESP_ERR_TIMEOUT && !device_live && bus_live);
    unsigned removals_before = removals;
    check(i2c_register_read(0x3c, 0, 400000, guarded, 1) == ESP_ERR_TIMEOUT);
    check(reads == reads_before && starts == starts_before && removals == removals_before);
    delete_error = ESP_OK;
    check(i2c_register_stop() == ESP_OK && !i2c_register_busy());
    check(i2c_register_stop() == ESP_OK);

    delete_error = ESP_FAIL;
    memset(guarded, 0xcc, sizeof(guarded));
    check(i2c_register_read(0x3c, 0, 100000, guarded, 32) == ESP_FAIL);
    expect_zero(guarded, 32); check(i2c_register_busy() && !device_live && bus_live);
    delete_error = ESP_OK; check(i2c_register_stop() == ESP_OK);

    add_error = ESP_ERR_NO_MEM; delete_error = ESP_FAIL;
    check(i2c_register_read(0x3c, 0, 100000, guarded, 4) == ESP_FAIL);
    expect_zero(guarded, 4); check(i2c_register_busy() && !device_live);
    add_error = delete_error = ESP_OK; check(i2c_register_stop() == ESP_OK);
    read_error = ESP_ERR_TIMEOUT; remove_error = ESP_FAIL;
    check(i2c_register_read(0x3c, 0, 100000, guarded, 4) == ESP_FAIL);
    expect_zero(guarded, 4);
    read_error = remove_error = ESP_OK; check(i2c_register_stop() == ESP_OK);

    check(i2c_register_write_byte(0x77, 0xff, 400000, 0xa5) == ESP_OK);
    check(last_address == 0x77 && last_reg == 0xff && last_speed == 400000 && last_write == 0xa5);
    check(!i2c_register_busy());
    write_error = ESP_ERR_TIMEOUT;
    check(i2c_register_write_byte(0x08, 0, 100000, 0) == ESP_ERR_TIMEOUT && !i2c_register_busy());
    write_error = ESP_OK; remove_error = ESP_FAIL;
    check(i2c_register_write_byte(0x08, 0, 100000, 0xff) == ESP_FAIL && i2c_register_busy());
    unsigned writes_before = writes;
    check(i2c_register_write_byte(0x08, 0, 100000, 0xff) == ESP_FAIL && writes == writes_before);
    remove_error = ESP_OK;
    check(i2c_register_stop() == ESP_OK && writes == writes_before && !i2c_register_busy());
    check(i2c_register_read(0x3c, 0x80, 400000, guarded, 32) == ESP_OK && !i2c_register_busy());
    puts("I2C transactions: 192 bounded reads, invalid requests, exact writes, failures and cleanup retries PASS");
    return 0;
}
