idf_component_register(SRCS "ota.c" "ota_logic.c"
                       INCLUDE_DIRS "."
                       REQUIRES log esp_http_client esp_https_ota app_update mbedtls nvs_flash)
