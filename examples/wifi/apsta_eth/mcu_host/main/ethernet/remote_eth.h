#pragma once

#include "esp_err.h"
#include "esp_netif.h"

/* The TCP/IP stack and DHCP client run on the MCU host. */
esp_err_t remote_eth_init(esp_netif_t **out_netif);
