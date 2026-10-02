#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_net_stack.h"
#include "eh_common_interface.h"
#include "eh_host_mcu_transport.h"
#include "eh_host_mcu_transport_channels.h"
#include "remote_eth.h"

typedef struct {
    esp_netif_driver_base_t base;
    eh_host_channel_t *channel;
    eh_host_channel_tx_fn_t tx;
    bool mac_valid;
    bool link_up;
    uint8_t mac[6];
} remote_eth_driver_t;

static const char *TAG = "remote_w5500";
static remote_eth_driver_t s_driver;

static esp_err_t transmit(void *handle, void *buffer, size_t len)
{
    remote_eth_driver_t *driver = handle;
    if (!driver->channel || !driver->link_up) {
        return ESP_ERR_INVALID_STATE;
    }
    return driver->tx(driver->channel, buffer, len);
}

static void free_rx_buffer(void *handle, void *buffer)
{
    (void)handle;
    free(buffer);
}

static esp_err_t post_attach(esp_netif_t *netif, esp_netif_iodriver_handle handle)
{
    remote_eth_driver_t *driver = handle;
    const esp_netif_driver_ifconfig_t config = {
        .handle = driver,
        .transmit = transmit,
        .driver_free_rx_buffer = free_rx_buffer,
    };
    driver->base.netif = netif;
    return esp_netif_set_driver_config(netif, &config);
}

/* The transport gives this callback a heap copy, owned by esp-netif afterward. */
static esp_err_t receive(void *handle, void *buffer, void *to_free, size_t len)
{
    (void)handle;
    if (!s_driver.base.netif || !s_driver.link_up ||
        !buffer || len < 14 || len > 1514) {
        free(to_free);
        return ESP_ERR_INVALID_STATE;
    }
    return esp_netif_receive(s_driver.base.netif, buffer, len, to_free);
}

static bool valid_mac(const uint8_t mac[6])
{
    static const uint8_t zero[6];
    return !(mac[0] & 1) && memcmp(mac, zero, sizeof(zero)) != 0;
}

static void link_task(void *arg)
{
    (void)arg;
    for (;;) {
        eh_eth_bridge_status_t status;
        esp_err_t err = eh_host_mcu_transport_eth_status_get(&status);
        if (err == ESP_OK && valid_mac(status.mac)) {
            if (!s_driver.mac_valid || memcmp(s_driver.mac, status.mac, 6)) {
                if (s_driver.link_up) {
                    s_driver.link_up = false;
                    esp_netif_action_disconnected(s_driver.base.netif, NULL, 0, NULL);
                }
                memcpy(s_driver.mac, status.mac, 6);
                ESP_ERROR_CHECK(esp_netif_set_mac(s_driver.base.netif, status.mac));
                s_driver.mac_valid = true;
                ESP_LOGI(TAG, "W5500 MAC %02x:%02x:%02x:%02x:%02x:%02x",
                         status.mac[0], status.mac[1], status.mac[2],
                         status.mac[3], status.mac[4], status.mac[5]);
            }
            if (!!status.link_up != s_driver.link_up) {
                s_driver.link_up = !!status.link_up;
                if (s_driver.link_up) {
                    esp_netif_action_connected(s_driver.base.netif, NULL, 0, NULL);
                } else {
                    esp_netif_action_disconnected(s_driver.base.netif, NULL, 0, NULL);
                }
                ESP_LOGI(TAG, "W5500 link %s", s_driver.link_up ? "up" : "down");
            }
        } else if (s_driver.link_up) {
            s_driver.link_up = false;
            esp_netif_action_disconnected(s_driver.base.netif, NULL, 0, NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *event = data;
    if (event->esp_netif == s_driver.base.netif) {
        ESP_LOGI(TAG, "W5500 IP " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t remote_eth_init(esp_netif_t **out_netif)
{
    if (!out_netif) return ESP_ERR_INVALID_ARG;
    if (s_driver.base.netif) {
        *out_netif = s_driver.base.netif;
        return ESP_OK;
    }

    esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
    inherent.if_key = "ETH_W5500";
    inherent.if_desc = "w5500";
    const esp_netif_config_t netif_config = {
        .base = &inherent,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    esp_netif_t *netif = esp_netif_new(&netif_config);
    if (!netif) return ESP_ERR_NO_MEM;

    s_driver.base.post_attach = post_attach;
    esp_err_t err = esp_netif_attach(netif, &s_driver);
    if (err != ESP_OK) {
        esp_netif_destroy(netif);
        return err;
    }
    s_driver.channel = eh_host_transport_add_channel(NULL, ESP_ETH_IF, false,
                                                      &s_driver.tx, receive);
    if (!s_driver.channel) {
        esp_netif_destroy(netif);
        s_driver.base.netif = NULL;
        return ESP_FAIL;
    }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip, NULL);
    if (err != ESP_OK) return err;
    esp_netif_action_start(netif, NULL, 0, NULL);
    if (xTaskCreate(link_task, "w5500_link", 3072, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    *out_netif = netif;
    return ESP_OK;
}
