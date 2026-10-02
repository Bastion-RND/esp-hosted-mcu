idf_component_register(SRCS "main.c" "w5500_bridge.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp-hosted
                    PRIV_REQUIRES nvs_flash esp_wifi esp_eth esp_driver_spi esp_driver_gpio w5500)
