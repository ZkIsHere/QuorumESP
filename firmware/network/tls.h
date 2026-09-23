#pragma once
/* TLS via mbedTLS (Phase 2/3). TODO.
 * Production requires TLS when the QDevice model requires it (AGENTS.md §9).
 * No insecure bypass, no hard-coded keys, no committed credentials.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t network_tls_init(void);

#ifdef __cplusplus
}
#endif
