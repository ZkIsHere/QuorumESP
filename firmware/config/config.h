#pragma once
/* Versioned persistent configuration (AGENTS.md §10). Data + pure logic in
 * config_logic.h (host-testable); NVS I/O below (device only).
 */
#include "config_logic.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load (migrate if needed), apply log level. Safe defaults on any doubt. */
esp_err_t quorumesp_config_init(void);
const qesp_config_t *quorumesp_config_get(void);

/* Wi-Fi credentials: NVS wins, Kconfig fallback (dev). */
esp_err_t quorumesp_config_get_wifi(char *ssid, size_t ssid_cap,
                                    char *pass, size_t pass_cap);

/* Danger: wipes the namespace (reprovision afterwards). */
esp_err_t quorumesp_config_factory_reset(void);

#ifdef __cplusplus
}
#endif
