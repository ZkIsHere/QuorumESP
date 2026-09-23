idf_component_register(SRCS "ota.c"
                       INCLUDE_DIRS "."
                       REQUIRES log esp_http_client esp_https_ota app_update mbedtls)
