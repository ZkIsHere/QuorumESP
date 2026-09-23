#pragma once
/* OTA update engine (AGENTS.md §12). Dev transport is plain HTTP;
 * production needs HTTPS + image signatures (see docs/ota.md).
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* If Kconfig OTA enabled and the version server reports a different
 * version than running: download, verify, set boot partition, reboot.
 * Never returns on success. Returns ESP_OK (nothing to do) or ESP_FAIL
 * (tried and failed — keep running the old image). */
esp_err_t quorumesp_ota_check_and_update(void);

/* Confirm this image healthy after OTA_HEALTHY_MS; calls
 * esp_ota_mark_app_valid_cancel_rollback(). Without confirmation a
 * crash/reboot rolls back to the previous image automatically. */
void quorumesp_ota_confirm_task_start(void);

#ifdef __cplusplus
}
#endif
