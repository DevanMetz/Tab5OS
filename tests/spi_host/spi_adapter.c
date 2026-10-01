#include "spi_adapter.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <assert.h>
#include <string.h>

unsigned spi_host_starts, spi_host_transfers, spi_host_gpio_writes;
bool spi_host_bus_owned, spi_host_device_owned;
uint8_t spi_host_last_tx[32];
size_t spi_host_last_length;
uint8_t spi_host_reply[32];
size_t spi_host_reply_length;
bool spi_host_fail_next_transfer;
unsigned spi_host_mode;
int spi_host_clock_hz;
struct spi_device { int unused; };
static struct spi_device device;

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config->pin_bit_mask == ((1ULL << 18) | (1ULL << 19) | (1ULL << 5) | (1ULL << 45)));
    assert(config->mode == GPIO_MODE_DISABLE && !spi_host_bus_owned && !spi_host_device_owned);
    return ESP_OK;
}
esp_err_t gpio_set_level(int pin, uint32_t level)
{
    assert(pin == 45 && level == 1);
    spi_host_gpio_writes++;
    return ESP_OK;
}
esp_err_t gpio_set_direction(int pin, int mode)
{
    assert(pin == 45 && mode == GPIO_MODE_OUTPUT);
    spi_host_gpio_writes++;
    return ESP_OK;
}
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma)
{
    assert(host == SPI2_HOST && dma == SPI_DMA_DISABLED && !spi_host_bus_owned);
    assert(config->mosi_io_num == 18 && config->miso_io_num == 19 && config->sclk_io_num == 5);
    assert(config->max_transfer_sz == 32);
    spi_host_bus_owned = true;
    spi_host_starts++;
    return ESP_OK;
}
esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config, spi_device_handle_t *out)
{
    assert(host == SPI2_HOST && spi_host_bus_owned && !spi_host_device_owned);
    assert(config->mode < 4 && config->spics_io_num == 45 && config->queue_size == 1);
    spi_host_mode = config->mode;
    spi_host_clock_hz = config->clock_speed_hz;
    spi_host_device_owned = true;
    *out = &device;
    return ESP_OK;
}
esp_err_t spi_bus_remove_device(spi_device_handle_t target)
{
    assert(target == &device && spi_host_device_owned);
    spi_host_device_owned = false;
    return ESP_OK;
}
esp_err_t spi_bus_free(int host)
{
    assert(host == SPI2_HOST && spi_host_bus_owned && !spi_host_device_owned);
    spi_host_bus_owned = false;
    return ESP_OK;
}
esp_err_t spi_device_transmit(spi_device_handle_t target, spi_transaction_t *transaction)
{
    assert(target == &device && spi_host_device_owned && spi_host_bus_owned);
    assert(transaction->length == transaction->rxlength && transaction->length % 8 == 0);
    assert(transaction->length >= 8 && transaction->length <= 256);
    spi_host_last_length = transaction->length / 8;
    memcpy(spi_host_last_tx, transaction->tx_buffer, spi_host_last_length);
    spi_host_transfers++;
    if (spi_host_fail_next_transfer) {
        spi_host_fail_next_transfer = false;
        /* A failed driver may already have touched RX; none of it is valid. */
        memset(transaction->rx_buffer, 0xee, spi_host_last_length);
        return ESP_FAIL;
    }
    assert(!spi_host_reply_length || spi_host_reply_length == spi_host_last_length);
    memcpy(transaction->rx_buffer, spi_host_reply_length ? spi_host_reply : transaction->tx_buffer,
           spi_host_last_length);
    return ESP_OK;
}
