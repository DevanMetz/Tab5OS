#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_FALSE(condition, value, ...) do { if (!(condition)) return (value); } while (0)
#define ESP_GOTO_ON_FALSE(condition, value, label, ...) do { if (!(condition)) { ret = (value); goto label; } } while (0)
#define ESP_GOTO_ON_ERROR(expression, label, ...) do { ret = (expression); if (ret != ESP_OK) goto label; } while (0)
