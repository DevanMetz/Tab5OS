#include "psram_exec_compat.h"

#if defined(TAB5_PSRAM_EXEC_COMPAT)
#include <assert.h>
#include <pthread.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern char _instruction_reserved_start[];
extern char _instruction_reserved_end[];
bool __real_esp_ptr_executable(const void *pointer);

// ESP-IDF 5.4.2 excludes the P4 PSRAM instruction reservation from its
// executable-pointer check, aborting when a socket worker deletes pthread TLS.
// https://github.com/espressif/esp-idf/issues/15997
// Add only the linker-defined instruction range; preserve every other SDK check.
bool IRAM_ATTR __wrap_esp_ptr_executable(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    return (address >= (uintptr_t)_instruction_reserved_start &&
            address < (uintptr_t)_instruction_reserved_end) ||
           __real_esp_ptr_executable(pointer);
}

#ifndef NDEBUG
typedef struct {
    pthread_key_t key;
    SemaphoreHandle_t done;
    unsigned destroyed;
} cleanup_probe_t;

static void cleanup_destructor(void *value)
{
    cleanup_probe_t *probe = value;
    probe->destroyed++;
    xSemaphoreGive(probe->done);
}

static void cleanup_worker(void *argument)
{
    cleanup_probe_t *probe = argument;
    assert(pthread_setspecific(probe->key, probe) == 0);
    vTaskDelete(NULL);
}
#endif
#endif

void psram_exec_self_test(void)
{
#if defined(TAB5_PSRAM_EXEC_COMPAT) && !defined(NDEBUG)
    uintptr_t start = (uintptr_t)_instruction_reserved_start;
    uintptr_t end = (uintptr_t)_instruction_reserved_end;
    assert(start < end);
    assert(esp_ptr_executable((const void *)start));
    assert(esp_ptr_executable((const void *)(end - 1)));
    assert(esp_ptr_executable((const void *)cleanup_destructor));
    assert(!esp_ptr_executable(NULL));
    assert(!esp_ptr_executable((const void *)UINTPTR_MAX));
    const uintptr_t outside[] = {start - 1, end, (uintptr_t)&start};
    for (unsigned i = 0; i < sizeof(outside) / sizeof(outside[0]); i++) {
        const void *pointer = (const void *)outside[i];
        assert(esp_ptr_executable(pointer) == __real_esp_ptr_executable(pointer));
    }

    cleanup_probe_t probe = {.done = xSemaphoreCreateBinary()};
    assert(probe.done);
    assert(pthread_key_create(&probe.key, cleanup_destructor) == 0);
    for (int core = 0; core < CONFIG_FREERTOS_NUMBER_OF_CORES; core++) {
        assert(xTaskCreatePinnedToCore(cleanup_worker, "tls-cleanup-check", 3072,
                                      &probe, 2, NULL, core) == pdPASS);
        assert(xSemaphoreTake(probe.done, pdMS_TO_TICKS(3000)) == pdTRUE);
        assert(probe.destroyed == (unsigned)core + 1);
    }
    assert(pthread_key_delete(probe.key) == 0);
    vSemaphoreDelete(probe.done);
    ESP_LOGI("psram-exec", "Executable bounds and task TLS cleanup passed on both cores");
#endif
}
