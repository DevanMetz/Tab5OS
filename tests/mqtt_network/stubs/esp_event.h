#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef const char *esp_event_base_t;
typedef struct mqtt_net_loop *esp_event_loop_handle_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
typedef struct { int queue_size; const char *task_name; } esp_event_loop_args_t;
#define ESP_EVENT_ANY_ID (-1)
#define ESP_EVENT_DECLARE_BASE(name) extern esp_event_base_t const name
#define ESP_EVENT_DEFINE_BASE(name) esp_event_base_t const name = #name
esp_err_t esp_event_loop_create(const esp_event_loop_args_t *, esp_event_loop_handle_t *);
esp_err_t esp_event_loop_delete(esp_event_loop_handle_t);
esp_err_t esp_event_loop_run(esp_event_loop_handle_t, TickType_t);
esp_err_t esp_event_post_to(esp_event_loop_handle_t, esp_event_base_t, int32_t,
                          const void *, size_t, TickType_t);
esp_err_t esp_event_handler_register_with(esp_event_loop_handle_t, esp_event_base_t,
                                        int32_t, esp_event_handler_t, void *);
esp_err_t esp_event_handler_unregister_with(esp_event_loop_handle_t, esp_event_base_t,
                                          int32_t, esp_event_handler_t);
