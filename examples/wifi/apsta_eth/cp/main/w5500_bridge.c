#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "eh_common_interface.h"
#include "eh_cp.h"
#include "eh_cp_event.h"
#include "eh_eth_bridge_wire.h"
#include "eh_transport_cp.h"
#include "w5500_bridge.h"

static const char *TAG = "w5500_bridge";
static esp_eth_handle_t s_eth;
static bool s_link_up;
static bool s_host_ready;
static uint8_t s_mac[6];

/* An untagged 1500-byte Ethernet frame is 1514 bytes without the FCS.
 * 1514 + the hosted V2 header (20) fits the current 1536-byte SDIO slot. */
#define ETH_FRAME_MIN 14u
#define ETH_FRAME_MAX 1514u

static void send_status(void)
{
    if (!s_host_ready) {
        return;
    }
    eh_eth_bridge_status_t *status = malloc(sizeof(*status));
    if (!status) {
        ESP_LOGE(TAG, "No memory for Ethernet status");
        return;
    }
    status->version = EH_ETH_BRIDGE_WIRE_VERSION;
    status->link_up = s_link_up ? 1 : 0;
    memcpy(status->mac, s_mac, sizeof(status->mac));

    interface_buffer_handle_t frame = {
        .if_type = ESP_ETH_IF,
        .if_num = EH_ETH_BRIDGE_STATUS_IF_NUM,
        .payload = (uint8_t *)status,
        .payload_len = sizeof(*status),
        .priv_buffer_handle = status,
        .free_buf_handle = free,
    };
    if (send_to_host_queue(&frame, PRIO_Q_OTHERS) != ESP_OK) {
        free(status);
    }
}

static void hosted_ready(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_host_ready = true;
    send_status();
}

static void ethernet_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (data && *(esp_eth_handle_t *)data != s_eth) {
        return;
    }
    if (id == ETHERNET_EVENT_CONNECTED) {
        s_link_up = true;
    } else if (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP) {
        s_link_up = false;
    } else {
        return;
    }
    ESP_LOGI(TAG, "W5500 link %s", s_link_up ? "up" : "down");
    send_status();
}

/* The W5500 driver gives ownership of buffer to this input callback. */
static esp_err_t ethernet_input(esp_eth_handle_t eth, uint8_t *buffer,
                                uint32_t len, void *priv)
{
    (void)eth;
    (void)priv;
    if (!buffer || len < ETH_FRAME_MIN || len > ETH_FRAME_MAX) {
        free(buffer);
        return ESP_OK;
    }
    interface_buffer_handle_t frame = {
        .if_type = ESP_ETH_IF,
        .if_num = EH_ETH_BRIDGE_DATA_IF_NUM,
        .payload = buffer,
        .payload_len = (uint16_t)len,
        .priv_buffer_handle = buffer,
        .free_buf_handle = free,
    };
    if (send_to_host_queue(&frame, PRIO_Q_OTHERS) != ESP_OK) {
        free(buffer);
    }
    return ESP_OK;
}

static esp_err_t ethernet_output(void *buffer, uint16_t len, void *priv)
{
    (void)priv;
    if (!s_eth || !s_link_up || !buffer ||
        len < ETH_FRAME_MIN || len > ETH_FRAME_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_eth_transmit(s_eth, buffer, len);
}

esp_err_t w5500_bridge_init(void)
{
    if (s_eth) {
        return ESP_OK;
    }
    if (CONFIG_EXAMPLE_W5500_MOSI < 0 || CONFIG_EXAMPLE_W5500_MISO < 0 ||
        CONFIG_EXAMPLE_W5500_SCLK < 0 || CONFIG_EXAMPLE_W5500_CS < 0) {
        ESP_LOGE(TAG, "Set W5500 SPI2 GPIOs in menuconfig before flashing");
        return ESP_ERR_INVALID_ARG;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_EXAMPLE_W5500_MOSI,
        .miso_io_num = CONFIG_EXAMPLE_W5500_MISO,
        .sclk_io_num = CONFIG_EXAMPLE_W5500_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ESP_TRANSPORT_SDIO_MAX_BUF_SIZE,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    if (CONFIG_EXAMPLE_W5500_INT >= 0) {
        err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            spi_bus_free(SPI2_HOST);
            return err;
        }
    }

    spi_device_interface_config_t spi_dev = {
        .mode = 0,
        .clock_speed_hz = CONFIG_EXAMPLE_W5500_SPI_CLOCK_MHZ * 1000000,
        .spics_io_num = CONFIG_EXAMPLE_W5500_CS,
        .queue_size = 16,
    };
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &spi_dev);
    w5500.base.int_gpio_num = CONFIG_EXAMPLE_W5500_INT;
    if (CONFIG_EXAMPLE_W5500_INT < 0) {
        w5500.base.poll_period_ms = CONFIG_EXAMPLE_W5500_POLL_PERIOD_MS;
    }
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = CONFIG_EXAMPLE_W5500_RESET;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (!mac || !phy) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &s_eth);
    if (err != ESP_OK) {
        goto fail;
    }
    /* W5500's SHAR register resets to zero. IDF assigns a distinct,
     * stable Ethernet MAC from this C6's factory base address. */
    err = esp_read_mac(s_mac, ESP_MAC_ETH);
    if (err == ESP_OK) {
        err = esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, s_mac);
    }
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_eth_update_input_path(s_eth, ethernet_input, NULL);
    if (err != ESP_OK) {
        goto fail;
    }
    err = eh_cp_rx_register(ESP_ETH_IF, ethernet_output);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                     ethernet_event, NULL);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_event_handler_register(EH_CP_EVENT, EH_CP_EVT_PRIVATE_RPC_READY,
                                     hosted_ready, NULL);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_eth_start(s_eth);
    if (err != ESP_OK) {
        goto fail;
    }
    ESP_LOGI(TAG, "W5500 started, MAC %02x:%02x:%02x:%02x:%02x:%02x",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "W5500 initialization failed: %s", esp_err_to_name(err));
    if (s_eth) {
        esp_eth_driver_uninstall(s_eth);
        s_eth = NULL;
    }
    if (phy) {
        phy->del(phy);
    }
    if (mac) {
        mac->del(mac);
    }
    spi_bus_free(SPI2_HOST);
    return err;
}
