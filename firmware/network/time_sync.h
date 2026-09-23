#pragma once
/* Wall-clock sync via SNTP. Required for X.509 validity checks: without NTP
 * the RTC restarts at the epoch and every freshly issued cert looks "not yet
 * valid", failing mutual TLS closed (observed 2026-09-23). Production MUST
 * have reliable time (NTP and/or battery RTC).
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Best-effort sync, waits up to ~15 s. Logs the resulting time.
 * Returns ESP_OK when synced, ESP_ERR_TIMEOUT otherwise (caller decides;
 * TLS client-cert verify will fail-closed without valid time). */
esp_err_t network_time_sync(void);

#ifdef __cplusplus
}
#endif
