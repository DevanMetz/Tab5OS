/* Only the UART/GPIO/time/allocation/storage entrypoints used by this app are
 * adapted. The app and LVGL run unchanged; no physical serial/SD is touched. */
#include "host.h"
#include "lvgl.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "storage_io.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#undef assert
#define assert(expression) do { if (!(expression)) { \
    fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expression); exit(1); \
} } while (0)

unsigned uart_host_starts, uart_host_writes, uart_host_routes, uart_host_allocations;
bool uart_host_running, uart_host_fail_allocation, uart_host_rs485;
uint8_t uart_host_last_tx[256];
size_t uart_host_last_length;
int uart_host_write_result = UART_HOST_WRITE_ALL;
unsigned uart_host_last_drain_ms;
bool uart_host_fail_drain;
bool uart_host_fail_pending, uart_host_fail_read;
unsigned uart_host_read_limit;
static uint8_t received[2048];
static size_t received_length;
static bool loopback;

void *heap_caps_calloc(size_t count, size_t size, unsigned capabilities)
{
    assert(capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (uart_host_fail_allocation) { uart_host_fail_allocation = false; return NULL; }
    void *memory = calloc(count, size);
    if (memory) uart_host_allocations++;
    return memory;
}
void heap_caps_free(void *memory)
{
    if (memory) { assert(uart_host_allocations); uart_host_allocations--; free(memory); }
}
int64_t esp_timer_get_time(void) { return (int64_t)lv_tick_get() * 1000; }
TickType_t xTaskGetTickCount(void) { return lv_tick_get(); }
int storage_sync_file(FILE *file) { (void)file; assert(!"SD is not exercised by this adapter"); return -1; }
int storage_commit_new_file(FILE **file, const char *temporary, const char *final)
{
    (void)file; (void)temporary; (void)final;
    assert(!"SD is not exercised by this adapter"); return -1;
}
esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config->pin_bit_mask == ((1ULL << 47) | (1ULL << 48) | (1ULL << 20) | (1ULL << 21) | (1ULL << 34)));
    assert(config->mode == GPIO_MODE_DISABLE && !uart_host_running);
    return ESP_OK;
}
esp_err_t gpio_set_level(int pin, uint32_t level) { assert(pin == 34 && level == 0); return ESP_OK; }
esp_err_t gpio_set_direction(int pin, int mode) { assert(pin == 34 && mode == GPIO_MODE_OUTPUT); return ESP_OK; }
esp_err_t uart_param_config(int port, const uart_config_t *config)
{
    assert(port == UART_NUM_1 && !uart_host_running && config->data_bits == UART_DATA_8_BITS);
    assert(config->baud_rate >= 9600 && config->baud_rate <= 921600);
    return ESP_OK;
}
esp_err_t uart_driver_install(int port, int rx_size, int tx_size, int queue_size, void *queue, int flags)
{
    assert(port == UART_NUM_1 && rx_size == 2048 && !tx_size && !queue_size && !queue && !flags);
    assert(!uart_host_running);
    uart_host_running = true; uart_host_starts++; return ESP_OK;
}
esp_err_t uart_set_mode(int port, int mode)
{
    assert(port == UART_NUM_1 && uart_host_running);
    assert(mode == UART_MODE_UART || mode == UART_MODE_RS485_HALF_DUPLEX);
    uart_host_rs485 = mode == UART_MODE_RS485_HALF_DUPLEX; return ESP_OK;
}
esp_err_t uart_set_pin(int port, int tx, int rx, int rts, int cts)
{
    assert(port == UART_NUM_1 && uart_host_running && cts == UART_PIN_NO_CHANGE);
    if (uart_host_rs485) assert(tx == 20 && rx == 21 && rts == 34);
    else assert(tx == 47 && rx == 48 && rts == UART_PIN_NO_CHANGE);
    uart_host_routes++; return ESP_OK;
}
esp_err_t uart_driver_delete(int port)
{
    assert(port == UART_NUM_1 && uart_host_running);
    uart_host_running = false; received_length = 0; return ESP_OK;
}
esp_err_t uart_get_buffered_data_len(int port, size_t *length)
{
    assert(port == UART_NUM_1 && uart_host_running);
    if (uart_host_fail_pending) { uart_host_fail_pending = false; return ESP_FAIL; }
    *length = received_length; return ESP_OK;
}
int uart_read_bytes(int port, void *buffer, uint32_t length, TickType_t ticks)
{
    (void)ticks;
    assert(port == UART_NUM_1 && uart_host_running);
    if (uart_host_fail_read) { uart_host_fail_read = false; return -1; }
    if (length > received_length) length = (uint32_t)received_length;
    if (uart_host_read_limit && length > uart_host_read_limit) length = uart_host_read_limit;
    memcpy(buffer, received, length); received_length -= length;
    memmove(received, received + length, received_length); return (int)length;
}
void uart_host_receive(const uint8_t *bytes, size_t length)
{
    assert(received_length + length <= sizeof(received));
    memcpy(received + received_length, bytes, length); received_length += length;
}
int uart_write_bytes(int port, const void *bytes, size_t length)
{
    assert(port == UART_NUM_1 && uart_host_running && length > 0 && length <= 256);
    uart_host_writes++;
    int result = uart_host_write_result == UART_HOST_WRITE_ALL ? (int)length : uart_host_write_result;
    uart_host_write_result = UART_HOST_WRITE_ALL;
    uart_host_last_length = result > 0 ? (size_t)result : 0;
    assert(uart_host_last_length <= length);
    memcpy(uart_host_last_tx, bytes, uart_host_last_length);
    if (loopback) uart_host_receive(bytes, uart_host_last_length);
    return result;
}
esp_err_t uart_wait_tx_done(int port, TickType_t ticks)
{
    assert(port == UART_NUM_1 && uart_host_running);
    uart_host_last_drain_ms = ticks;
    if (uart_host_fail_drain) { uart_host_fail_drain = false; return ESP_FAIL; }
    return ESP_OK;
}
esp_err_t uart_set_loop_back(int port, bool enabled)
{
    assert(port == UART_NUM_1 && uart_host_running); loopback = enabled; return ESP_OK;
}
