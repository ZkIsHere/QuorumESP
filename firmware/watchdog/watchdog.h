#pragma once
/* Hardware + task watchdog (AGENTS.md §11). TODO Phase 2.
 * A deadlocked QDevice state machine must reset, not look alive.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t quorumesp_watchdog_init(void);

#ifdef __cplusplus
}
#endif
