idf_component_register(SRCS "tlv.c" "msg.c" "server.c"
                       INCLUDE_DIRS "."
                       REQUIRES log esp_timer esp_netif network quorum esp_system freertos common config watchdog)
