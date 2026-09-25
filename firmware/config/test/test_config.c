/* QuorumESP — config pure-logic tests (gcc, WSL). No IDF needed. */
#include <stdio.h>
#include <string.h>

#include "../config_logic.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while (0)

static void test_defaults(void) {
    qesp_config_t c;
    qesp_config_defaults(&c, "ssid1", "pass1", 5403);
    CHECK(c.version == QESP_CONFIG_VERSION);
    CHECK(strcmp(c.hostname, "quorumesp") == 0);
    CHECK(strcmp(c.wifi_ssid, "ssid1") == 0);
    CHECK(strcmp(c.wifi_pass, "pass1") == 0);
    CHECK(c.net_static == 0);
    CHECK(c.ip == 0 && c.dns1 == 0);
    CHECK(c.device_id[0] == '\0'); /* filled from MAC on device */
    CHECK(c.log_level == 3);
    CHECK(c.qdevice_port == 5403);
    /* Zero port / NULL strings fall back safely. */
    qesp_config_defaults(&c, NULL, NULL, 0);
    CHECK(c.wifi_ssid[0] == '\0');
    CHECK(c.qdevice_port == 5403);
}

static void test_migrate_same(void) {
    qesp_config_t c;
    qesp_config_defaults(&c, "a", "b", 1);
    CHECK(qesp_config_migrate(&c, QESP_CONFIG_VERSION) == 0);
    CHECK(strcmp(c.wifi_ssid, "a") == 0);
}

static void test_migrate_v0_keeps_wifi(void) {
    qesp_config_t c;
    uint8_t *p = (uint8_t *)&c;
    size_t i;
    for (i = 0; i < sizeof(c); i++) {
        p[i] = 0;
    }
    /* v0 era: only wifi keys exist. */
    strncpy(c.wifi_ssid, "myssid", sizeof(c.wifi_ssid) - 1);
    strncpy(c.wifi_pass, "mypass", sizeof(c.wifi_pass) - 1);
    CHECK(qesp_config_migrate(&c, 0) == 0);
    CHECK(c.version == QESP_CONFIG_VERSION);
    CHECK(strcmp(c.wifi_ssid, "myssid") == 0);
    CHECK(strcmp(c.wifi_pass, "mypass") == 0);
    CHECK(strcmp(c.hostname, "quorumesp") == 0);
    CHECK(c.log_level == 3);
    CHECK(c.qdevice_port == 5403);
}

static void test_migrate_refuses(void) {
    qesp_config_t c;
    qesp_config_defaults(&c, "a", "b", 1);
    /* Future version (downgrade): refuse. */
    CHECK(qesp_config_migrate(&c, QESP_CONFIG_VERSION + 1) == -1);
    /* Unknown past: refuse. */
    CHECK(qesp_config_migrate(&c, 9999) == -1);
    /* Current stays untouched on refuse. */
    CHECK(strcmp(c.wifi_ssid, "a") == 0);
}

static void test_truncation_safe(void) {
    qesp_config_t c;
    qesp_config_defaults(&c, "ssid- way -too -long -for -33 -chars -yes",
                         "p", 1);
    CHECK(c.wifi_ssid[sizeof(c.wifi_ssid) - 1] == '\0');
    CHECK(strlen(c.wifi_ssid) == sizeof(c.wifi_ssid) - 1);
}

int main(void) {
    test_defaults();
    test_migrate_same();
    test_migrate_v0_keeps_wifi();
    test_migrate_refuses();
    test_truncation_safe();
    if (failures == 0) {
        printf("config: all tests passed\n");
        return 0;
    }
    printf("config: %d FAILURES\n", failures);
    return 1;
}
