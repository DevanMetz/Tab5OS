#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
extern unsigned spi_host_starts, spi_host_transfers, spi_host_gpio_writes;
extern bool spi_host_bus_owned, spi_host_device_owned;
extern uint8_t spi_host_last_tx[32];
extern size_t spi_host_last_length;
extern uint8_t spi_host_reply[32];
extern size_t spi_host_reply_length; /* Zero selects loopback. */
extern bool spi_host_fail_next_transfer;
extern unsigned spi_host_mode;
extern int spi_host_clock_hz;
