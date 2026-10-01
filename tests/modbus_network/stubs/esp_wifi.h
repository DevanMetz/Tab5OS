#pragma once
#include "host.h"
typedef struct{int dummy;}wifi_ap_record_t;
static inline int esp_wifi_sta_get_ap_info(wifi_ap_record_t*a){(void)a;return host_online ? 0 : -1;}
