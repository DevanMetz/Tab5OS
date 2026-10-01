#pragma once
#include "driver/i2c_master.h"
esp_err_t bsp_ext_i2c_init(void);
esp_err_t bsp_ext_i2c_deinit(void);
i2c_master_bus_handle_t bsp_ext_i2c_get_handle(void);
