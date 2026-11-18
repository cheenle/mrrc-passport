idf_component_register(SRCS "probe_main.c" "mrrc_link.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_http_client json nvs_flash esp_event esp_netif)
