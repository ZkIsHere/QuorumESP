#pragma once
/* Central logging setup with per-category tags (AGENTS.md §15). TODO Phase 2.
 * Categories: BOOT NETWORK TLS QDEVICE PROTOCOL STATE WATCHDOG CONFIG OTA WEB.
 * Never logs keys, passwords, or private material.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t diagnostics_logging_init(void);

#ifdef __cplusplus
}
#endif
