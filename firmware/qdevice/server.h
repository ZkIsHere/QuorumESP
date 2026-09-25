#pragma once
/* qnetd-side TCP server (dev: over Wi-Fi; production: Ethernet later).
 * Multi-client FFSplit + LMS with reference-faithful vote paths.
 * Fail-closed: framing violations drop the transport, message errors get
 * error replies without advancing the session.
 */
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the server task and returns immediately. */
esp_err_t qdevice_server_start(void);

#define QDEV_SNAP_MAX 8

typedef struct {
    uint32_t node;
    uint8_t algo;   /* QESP_ALGO_* */
    uint8_t state;  /* 0 pre-init, 1 handshake, 2 active */
    uint8_t tls;    /* transport upgraded */
    uint8_t vote;   /* last decided vote (FFSplit desired / LMS saved) */
} qdev_cli_snap_t;

typedef struct {
    uint8_t n;
    uint8_t algo; /* cluster algorithm, 0 = none yet */
    uint32_t uptime_s;
    char cluster[64];
    qdev_cli_snap_t cli[QDEV_SNAP_MAX];
} qdev_status_t;

/* Point-in-time copy for diagnostics (web UI). Never blocks long. */
void qdevice_status_snapshot(qdev_status_t *out);

#ifdef __cplusplus
}
#endif
