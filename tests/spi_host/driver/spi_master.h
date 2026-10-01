#pragma once
#include <stddef.h>
#include "esp_err.h"
enum { SPI2_HOST, SPI_DMA_DISABLED, ESP_INTR_CPU_AFFINITY_AUTO, SPI_CLK_SRC_DEFAULT };
typedef struct spi_device *spi_device_handle_t;
typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num;
    int data4_io_num, data5_io_num, data6_io_num, data7_io_num, max_transfer_sz, isr_cpu_id;
} spi_bus_config_t;
typedef struct { unsigned mode; int clock_source, clock_speed_hz, spics_io_num, queue_size; } spi_device_interface_config_t;
typedef struct { size_t length, rxlength; const void *tx_buffer; void *rx_buffer; } spi_transaction_t;
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma);
esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config, spi_device_handle_t *device);
esp_err_t spi_bus_remove_device(spi_device_handle_t device);
esp_err_t spi_bus_free(int host);
esp_err_t spi_device_transmit(spi_device_handle_t device, spi_transaction_t *transaction);
