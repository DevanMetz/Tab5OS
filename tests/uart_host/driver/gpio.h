#pragma once
#include <stdint.h>
#include "esp_err.h"
enum { GPIO_NUM_47 = 47, GPIO_NUM_48 = 48, GPIO_NUM_20 = 20, GPIO_NUM_21 = 21, GPIO_NUM_34 = 34 };
enum { GPIO_MODE_DISABLE, GPIO_MODE_OUTPUT, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE };
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(int pin, uint32_t level);
esp_err_t gpio_set_direction(int pin, int mode);
