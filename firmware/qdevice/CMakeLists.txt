idf_component_register(SRCS "protocol.c" "session.c" "state.c" "tlv.c" "msg.c" "server.c"
                       INCLUDE_DIRS "."
                       REQUIRES log esp_timer esp_netif network)
