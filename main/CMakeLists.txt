idf_component_register(SRCS "vb_demo.c" "opus_wapper.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_websocket_client opus vb6824 mbedtls
                    PRIV_INCLUDE_DIRS "../components/opus/include" "../components/vb6824"
                    PRIV_REQUIRES driver esp_wifi nvs_flash lwip esp_netif esp_http_client)