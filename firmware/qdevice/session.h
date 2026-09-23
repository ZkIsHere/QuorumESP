#pragma once
/* QDevice session management (per-client state). TODO Phase 3. */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t qdevice_session_init(void);

#ifdef __cplusplus
}
#endif
