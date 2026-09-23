#pragma once
/* Cluster/node membership view from NODE_LIST messages. TODO Phase 3. */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t quorum_membership_init(void);

#ifdef __cplusplus
}
#endif
