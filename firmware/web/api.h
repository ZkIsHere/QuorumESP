#pragma once
/* Diagnostic web UI (AGENTS.md 13). READ-ONLY by construction - only
 * GET handlers exist (POST -> 405); nothing here can change quorum
 * state, config, or firmware. (Config edit + firmware update live on the
 * EXTERNAL PC portal, never on-device.)
 *
 * Single interface: GET / renders the page, GET /events pushes live state
 * (uptime/device/log) over Server-Sent Events. Auth is HTTP Basic (browser
 * dialog, user/pass from menuconfig). /logout answers 401 to force a
 * re-prompt (true logout = close the browser). IPs that burn too many
 * wrong passwords go fully silent until the block expires.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* No-op (returns OK) unless QUORUMESP_WEB_ENABLE. With it on but no
 * user/password configured, refuses to start (returns FAIL, no open UI).
 * Serves GET / (page), GET /events (SSE), GET /logout (401 re-prompt).
 */
esp_err_t web_api_init(void);

#ifdef __cplusplus
}
#endif
