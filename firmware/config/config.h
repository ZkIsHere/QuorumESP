#pragma once
/* Persistent configuration with versioned format (AGENTS.md §10). TODO Phase 2. */
#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t quorumesp_config_init(void);

/* Wi-Fi credentials: NVS namespace "qesp" (keys ssid/pass) wins; Kconfig
 * COMPILE_TIME defaults are fallback only (dev). Release builds carry no
 * credentials — they must be provisioned (see docs/provisioning.md).
 * Returns ESP_OK with NUL-terminated strings, ESP_FAIL if none usable. */
esp_err_t quorumesp_config_get_wifi(char *ssid, size_t ssid_cap,
                                    char *pass, size_t pass_cap);

#ifdef __cplusplus
}
#endif
