#pragma once
/* Persistent configuration with versioned format (AGENTS.md §10). TODO Phase 2. */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t quorumesp_config_init(void);

#ifdef __cplusplus
}
#endif
