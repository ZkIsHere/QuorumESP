idf_component_register(SRCS "ethernet.c" "tls.c" "wifi.c"
                       INCLUDE_DIRS "."
                       REQUIRES log esp_wifi esp_netif esp_event)
