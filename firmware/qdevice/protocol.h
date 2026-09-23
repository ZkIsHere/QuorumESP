#pragma once
/* QDevice wire protocol (server/qnetd side). TODO Phase 3.
 * Byte format is specified in docs/protocol.md §2.1 — do not invent fields.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t qdevice_protocol_init(void);

#ifdef __cplusplus
}
#endif
