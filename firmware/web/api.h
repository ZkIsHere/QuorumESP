#pragma once
/* Diagnostic web API — AFTER the QDevice core is stable (AGENTS.md §13). TODO.
 * Read-only diagnostics; never allows changing quorum state.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_api_init(void);

#ifdef __cplusplus
}
#endif
