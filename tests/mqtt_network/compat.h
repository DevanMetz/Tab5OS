#pragma once
#include "sdkconfig.h"
#include "esp_err.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "mqtt_supported_features.h"
#include "esp_tls.h"
#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strdup mqtt_net_strdup
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_LOGI ESP_LOGD
#define ESP_MEM_CHECK(tag, value, action) do { (void)(tag); if (!(value)) { action; } } while (0);
#define ESP_OK_CHECK(tag, value, action) do { (void)(tag); if ((value) != ESP_OK) { action; } } while (0)
char *platform_create_id_string(void);
int platform_random(int);
uint64_t platform_tick_get_ms(void);
void *mqtt_net_malloc(size_t);
void *mqtt_net_calloc(size_t, size_t);
void *mqtt_net_realloc(void *, size_t);
void mqtt_net_free(void *);
char *mqtt_net_strdup(const char *);
int asprintf(char **, const char *, ...);
