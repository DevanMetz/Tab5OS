#pragma once
#include "host.h"
#include "esp_err.h"

#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) ((const unsigned char *)&(ip)->addr)[0], ((const unsigned char *)&(ip)->addr)[1], \
                  ((const unsigned char *)&(ip)->addr)[2], ((const unsigned char *)&(ip)->addr)[3]
#define IPADDR_STRLEN_MAX 64
#define ESP_IPADDR_TYPE_V4 0
#define ESP_NETIF_DNS_MAIN 0

typedef struct { unsigned addr; } esp_ip4_addr_t;
typedef struct { int dummy; } esp_netif_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct { struct { int type; union { esp_ip4_addr_t ip4; } u_addr; } ip; } esp_netif_dns_info_t;

static inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{ (void)key; static esp_netif_t netif; return &netif; }
static inline esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info)
{
    (void)netif;
    if (!host_online) return ESP_ERR_INVALID_STATE;
    info->ip.addr = htonl(0xc0000202); info->netmask.addr = htonl(0xffffff00); info->gw.addr = htonl(0xc0000201);
    return ESP_OK;
}
static inline esp_err_t esp_netif_get_dns_info(esp_netif_t *netif, int type, esp_netif_dns_info_t *info)
{
    (void)netif; (void)type;
    info->ip.type = ESP_IPADDR_TYPE_V4; info->ip.u_addr.ip4.addr = htonl(0xc0000201); return ESP_OK;
}
