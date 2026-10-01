#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define UART_HOST_WRITE_ALL (-2)
extern unsigned uart_host_starts, uart_host_writes, uart_host_routes, uart_host_allocations;
extern bool uart_host_running, uart_host_fail_allocation, uart_host_rs485;
extern uint8_t uart_host_last_tx[256];
extern size_t uart_host_last_length;
extern int uart_host_write_result;
extern unsigned uart_host_last_drain_ms;
extern bool uart_host_fail_drain;
extern bool uart_host_fail_pending, uart_host_fail_read;
extern unsigned uart_host_read_limit;
void uart_host_receive(const uint8_t *bytes, size_t length);
