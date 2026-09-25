idf_component_register(SRCS "main.c"
                        INCLUDE_DIRS "."
                        REQUIRES qdevice quorum network config watchdog web nvs_flash)
