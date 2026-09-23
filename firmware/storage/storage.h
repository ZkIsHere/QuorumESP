#pragma once
/* NVS-backed persistent storage (config, certs). TODO Phase 2.
 * Never stores plaintext secrets without protection; never logs secrets.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t quorumesp_storage_init(void);

#ifdef __cplusplus
}
#endif
