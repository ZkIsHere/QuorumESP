#pragma once
/* qnetd-side TCP server (dev: over Wi-Fi; production: Ethernet later).
 * Single client, fail-closed: framing violations drop the transport,
 * message errors get error replies without advancing the session.
 * The reply vote is a TEST STUB (fixed ACK), not a quorum decision.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the server task and returns immediately. */
esp_err_t qdevice_server_start(void);

#ifdef __cplusplus
}
#endif
