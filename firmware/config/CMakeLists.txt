idf_component_register(SRCS "config.c" "config_logic.c"
                       INCLUDE_DIRS "."
                       REQUIRES log nvs_flash esp_system)
