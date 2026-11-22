idf_component_register(
    SRCS "app_main.c" "mrrc_net.c" "mrrc_ui.c" "mrrc_audio.c" "mrrc_tx.c"
         "mrrc_cfg.c" "mrrc_prov.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_netif esp_timer esp_wifi nvs_flash
             esp_http_client esp_http_server esp-tls json esp_websocket_client
)
