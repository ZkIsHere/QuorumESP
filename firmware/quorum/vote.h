#pragma once
/* Quorum vote bookkeeping. TODO Phase 3.
 * Provides a vote ONLY — never decides cluster state (AGENTS.md §2).
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t quorum_vote_init(void);

#ifdef __cplusplus
}
#endif
