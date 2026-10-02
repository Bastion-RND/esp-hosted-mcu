/* Ethernet bridge over ESP-Hosted. if_num=0 carries raw Ethernet frames;
 * if_num=1 carries the link/MAC snapshot below. Wire enum values are stable. */
#pragma once

#include <stdint.h>

#define EH_ETH_BRIDGE_WIRE_VERSION   1u
#define EH_ETH_BRIDGE_DATA_IF_NUM    0u
#define EH_ETH_BRIDGE_STATUS_IF_NUM  1u

typedef struct {
    uint8_t version;
    uint8_t link_up;
    uint8_t mac[6];
} eh_eth_bridge_status_t;
