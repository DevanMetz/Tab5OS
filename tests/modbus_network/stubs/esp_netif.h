#pragma once
#include "host.h"
#define ESP_OK 0
typedef struct{int dummy;}esp_netif_t;
typedef struct{struct{unsigned addr;}ip,netmask;}esp_netif_ip_info_t;
static inline esp_netif_t*esp_netif_get_handle_from_ifkey(const char*k){(void)k;static esp_netif_t netif_instance;return &netif_instance;}
static inline bool esp_netif_is_netif_up(esp_netif_t*n){(void)n;return host_online != 0;}
static inline int esp_netif_get_ip_info(esp_netif_t*n,esp_netif_ip_info_t*i){(void)n;i->ip.addr=htonl(0x7f000001);i->netmask.addr=htonl(0xff000000);return ESP_OK;}
