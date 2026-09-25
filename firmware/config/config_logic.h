#pragma once
/* QuorumESP — config data + pure logic (no IDF dependency, host-testable).
 * NVS I/O lives in config.c.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QESP_CONFIG_VERSION 1u
#define QESP_CONFIG_NS "qesp"

typedef struct {
    uint32_t version;
    char hostname[32];
    char wifi_ssid[33];
    char wifi_pass[65];
    uint8_t net_static; /* 0 = DHCP (default), 1 = static */
    uint32_t ip;        /* network byte order */
    uint32_t mask;
    uint32_t gw;
    uint32_t dns1;
    uint32_t dns2;
    char device_id[32];
    uint8_t log_level; /* 0..5 = NONE,ERROR,WARN,INFO,DEBUG,VERBOSE */
    uint16_t qdevice_port;
} qesp_config_t;

void qesp_config_defaults(qesp_config_t *c, const char *k_ssid,
                          const char *k_pass, uint16_t k_port);
/* Migrate cfg in place from from_version to current. 0 ok, -1 refuse
 * (unknown past or future/downgrade). v0 = pre-version era: keeps wifi
 * ssid/pass, fills everything else with defaults. */
int qesp_config_migrate(qesp_config_t *c, uint32_t from_version);

#ifdef __cplusplus
}
#endif
