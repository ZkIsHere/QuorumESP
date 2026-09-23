#pragma once
/* Explicit protocol state machine (AGENTS.md §7). TODO Phase 3.
 * No scattered booleans: one state enum, invalid transitions rejected.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t qdevice_state_init(void);

#ifdef __cplusplus
}
#endif
