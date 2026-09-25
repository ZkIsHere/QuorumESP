idf_component_register(SRCS "api.c"
                        INCLUDE_DIRS "."
                        REQUIRES log esp_http_server esp_timer app_update
                                 qdevice quorum network config common)
