#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef struct mdns_result_s {
    struct mdns_result_s *next;
    const char *service_type;
    const char *proto;
} mdns_result_t;

esp_err_t mdns_init(void);
esp_err_t mdns_query_ptr(const char *service, const char *proto, uint32_t timeout,
                         size_t limit, mdns_result_t **results);
void mdns_query_results_free(mdns_result_t *results);
