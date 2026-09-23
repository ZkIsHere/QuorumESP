#pragma once
/* Ethernet via esp_eth/esp_netif (Phase 2).
 * PHY-agnostic: LAN8720 (RMII) now, W5500 (SPI) later — see docs/hardware.md.
 * Core QDevice code must not depend on one controller (AGENTS.md §5).
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t network_ethernet_init(void);

#ifdef __cplusplus
}
#endif
