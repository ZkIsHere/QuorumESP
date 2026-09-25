#pragma once
/* Diagnostic web UI (AGENTS.md §13). READ-ONLY by construction:
 * only GET handlers exist — no endpoint can change quorum state,
 * config, or firmware. Anything mutating must never be added here.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* No-op (returns OK) unless QUORUMESP_WEB_ENABLE. Installs a log-ring
 * tap, starts httpd on QUORUMESP_WEB_PORT, serves:
 *   GET /            human-readable status page
 *   GET /api/status  JSON snapshot (version, net, cluster, clients)
 *   GET /api/log     JSON array of recent log lines (no secrets logged)
 */
esp_err_t web_api_init(void);

#ifdef __cplusplus
}
#endif
