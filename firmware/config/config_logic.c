/* QuorumESP — pure config logic (no IDF). See config_logic.h. */
#include "config_logic.h"

#include <string.h>

static void put_str(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

void qesp_config_defaults(qesp_config_t *c, const char *k_ssid,
                          const char *k_pass, uint16_t k_port) {
    uint8_t *p = (uint8_t *)c;
    size_t i;
    for (i = 0; i < sizeof(*c); i++) {
        p[i] = 0;
    }
    c->version = QESP_CONFIG_VERSION;
    put_str(c->hostname, sizeof(c->hostname), "quorumesp");
    put_str(c->wifi_ssid, sizeof(c->wifi_ssid), k_ssid);
    put_str(c->wifi_pass, sizeof(c->wifi_pass), k_pass);
    c->net_static = 0;
    c->ip = 0;
    c->mask = 0;
    c->gw = 0;
    c->dns1 = 0;
    c->dns2 = 0;
    put_str(c->device_id, sizeof(c->device_id), "");
    c->log_level = 3; /* INFO */
    c->qdevice_port = k_port != 0 ? k_port : 5403;
}

int qesp_config_migrate(qesp_config_t *c, uint32_t from_version) {
    if (from_version == QESP_CONFIG_VERSION) {
        return 0;
    }
    if (from_version > QESP_CONFIG_VERSION) {
        /* Downgrade: refuse to interpret the future. */
        return -1;
    }
    if (from_version == 0) {
        char ssid[33], pass[65];
        put_str(ssid, sizeof(ssid), c->wifi_ssid);
        put_str(pass, sizeof(pass), c->wifi_pass);
        put_str(c->hostname, sizeof(c->hostname), "quorumesp");
        c->net_static = 0;
        c->ip = 0;
        c->mask = 0;
        c->gw = 0;
        c->dns1 = 0;
        c->dns2 = 0;
        put_str(c->device_id, sizeof(c->device_id), "");
        c->log_level = 3;
        if (c->qdevice_port == 0) {
            c->qdevice_port = 5403;
        }
        put_str(c->wifi_ssid, sizeof(c->wifi_ssid), ssid);
        put_str(c->wifi_pass, sizeof(c->wifi_pass), pass);
        c->version = QESP_CONFIG_VERSION;
        return 0;
    }
    return -1;
}
