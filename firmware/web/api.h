#pragma once
/* Diagnostic web UI (AGENTS.md §13). READ-ONLY by construction:
 * only GET handlers exist — no endpoint can change quorum state,
 * config, or firmware. Anything mutating must never be added here.
 *
 * Access is gated by HTTP Basic auth (user/pass from menuconfig; the UI
 * refuses to start when they are empty). Display is deliberately minimal:
 * project name, version, uptime, connected-client list, recent log.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* No-op (returns OK) unless QUORUMESP_WEB_ENABLE. With it on but no
 * user/password configured, refuses to start (returns FAIL, no open UI).
 * Serves (all GET + Basic auth):
 *   GET /            human-readable status page
 *   GET /api/status  JSON snapshot (project/version/uptime/clients)
 *   GET /api/log     JSON array of recent log lines (no secrets logged)
 */
esp_err_t web_api_init(void);

#ifdef __cplusplus
}
#endif
