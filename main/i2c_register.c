#include "i2c_register.h"

#include <string.h>
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"

static i2c_master_dev_handle_t device;

bool i2c_register_busy(void)
{
    return device != NULL || bsp_ext_i2c_get_handle() != NULL;
}

esp_err_t i2c_register_stop(void)
{
    if (device) {
        esp_err_t error = i2c_master_bus_rm_device(device);
        if (error != ESP_OK) return error;
        device = NULL;
    }
    return bsp_ext_i2c_deinit();
}

static bool valid_request(uint8_t address, uint32_t speed_hz)
{
    return address >= 0x08 && address <= 0x77 &&
           (speed_hz == 100000 || speed_hz == 400000);
}

static esp_err_t finish(esp_err_t transaction_error)
{
    esp_err_t cleanup_error = i2c_register_stop();
    return cleanup_error == ESP_OK ? transaction_error : cleanup_error;
}

static esp_err_t begin(uint8_t address, uint32_t speed_hz)
{
    esp_err_t error = i2c_register_stop();
    if (error != ESP_OK) return error;
    error = bsp_ext_i2c_init();
    if (error != ESP_OK) return error;
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = speed_hz,
    };
    error = i2c_master_bus_add_device(bsp_ext_i2c_get_handle(), &config, &device);
    return error == ESP_OK ? ESP_OK : finish(error);
}

esp_err_t i2c_register_read(uint8_t address, uint8_t reg, uint32_t speed_hz,
                            uint8_t *bytes, size_t length)
{
    if (!valid_request(address, speed_hz) || !bytes || !length || length > I2C_REGISTER_MAX_BYTES)
        return ESP_ERR_INVALID_ARG;
    memset(bytes, 0, length);
    esp_err_t error = begin(address, speed_hz);
    if (error == ESP_OK)
        error = finish(i2c_master_transmit_receive(device, &reg, 1, bytes, length, 50));
    if (error != ESP_OK) memset(bytes, 0, length);
    return error;
}

esp_err_t i2c_register_write_byte(uint8_t address, uint8_t reg, uint32_t speed_hz,
                                  uint8_t value)
{
    if (!valid_request(address, speed_hz)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = begin(address, speed_hz);
    if (error != ESP_OK) return error;
    const uint8_t bytes[] = {reg, value};
    return finish(i2c_master_transmit(device, bytes, sizeof(bytes), 50));
}
