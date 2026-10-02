idf_component_register(SRCS "softap_sta.c" "ethernet/remote_eth.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_netif esp_event
                    INCLUDE_DIRS ".")
