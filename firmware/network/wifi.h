#pragma once
/* Dev Wi-Fi transport (STA). NON-PRODUCTION (AGENTS.md §5 wants Ethernet).
 * Credentials come from Kconfig (menuconfig, local sdkconfig — git-ignored),
 * never from committed files.
 */
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Block until GOT_IP. Returns ESP_OK with the STA IPv4 (host order),
 * or ESP_FAIL after retries / missing credentials. */
esp_err_t network_wifi_connect(uint32_t *out_ip_be);

/* Last known values for diagnostics (0 / 0 when down). */
uint32_t network_wifi_get_ip(void);
int network_wifi_get_rssi(void);

#ifdef __cplusplus
}
#endif
