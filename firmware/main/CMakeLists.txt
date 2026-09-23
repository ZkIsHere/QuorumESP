idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES qdevice quorum network config storage watchdog web diagnostics nvs_flash ota)
