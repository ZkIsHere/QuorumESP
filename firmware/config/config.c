/* Versioned NVS configuration. See config.h. */
#include "config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"

static const char *TAG = "CONFIG";

static qesp_config_t s_cfg;
static int s_loaded;

static void put_str(char *dst, size_t cap, const char *src) {
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static void fill_device_id(void) {
    uint8_t mac[6];
    if (s_cfg.device_id[0] != '\0') {
        return;
    }
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        put_str(s_cfg.device_id, sizeof(s_cfg.device_id), "qesp-unknown");
        return;
    }
    snprintf(s_cfg.device_id, sizeof(s_cfg.device_id), "qesp-%02x%02x%02x",
             mac[3], mac[4], mac[5]);
}

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap) {
    size_t n = cap;
    if (nvs_get_str(h, key, dst, &n) != ESP_OK) {
        dst[0] = '\0';
    }
}

esp_err_t quorumesp_config_init(void) {
    nvs_handle_t h;
    esp_err_t r = nvs_open(QESP_CONFIG_NS, NVS_READWRITE, &h);
    uint32_t ver = 0;
    int has_ver = 0;
    char pre_ssid[33], pre_pass[65];
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed: %s", esp_err_to_name(r));
        qesp_config_defaults(&s_cfg, CONFIG_QUORUMESP_WIFI_SSID,
                             CONFIG_QUORUMESP_WIFI_PASSWORD,
                             CONFIG_QUORUMESP_SERVER_PORT);
        s_loaded = 1;
        return ESP_FAIL;
    }
    has_ver = (nvs_get_u32(h, "ver", &ver) == ESP_OK);
    /* Pre-read wifi keys: a provisioned-but-unversioned namespace (v0 era:
     * ssid/pass written by the provisioning doc) must MIGRATE, never be
     * wiped as "fresh". Only a fully empty namespace gets defaults. */
    pre_ssid[0] = pre_pass[0] = '\0';
    load_str(h, "wssid", pre_ssid, sizeof(pre_ssid));
    load_str(h, "wpass", pre_pass, sizeof(pre_pass));
    if (!has_ver && pre_ssid[0] == '\0' && pre_pass[0] == '\0') {
        /* Truly fresh namespace: write full defaults (v1). */
        qesp_config_defaults(&s_cfg, CONFIG_QUORUMESP_WIFI_SSID,
                             CONFIG_QUORUMESP_WIFI_PASSWORD,
                             CONFIG_QUORUMESP_SERVER_PORT);
        fill_device_id();
        nvs_set_u32(h, "ver", s_cfg.version);
        nvs_set_str(h, "host", s_cfg.hostname);
        nvs_set_str(h, "wssid", s_cfg.wifi_ssid);
        nvs_set_str(h, "wpass", s_cfg.wifi_pass);
        nvs_set_u8(h, "netmode", s_cfg.net_static);
        nvs_set_str(h, "devid", s_cfg.device_id);
        nvs_set_u8(h, "logl", s_cfg.log_level);
        nvs_set_u16(h, "qdport", s_cfg.qdevice_port);
        nvs_commit(h);
        ESP_LOGI(TAG, "fresh config v%lu id=%s", (unsigned long)s_cfg.version,
                 s_cfg.device_id);
    } else {
        /* Load everything, then migrate/validate. */
        qesp_config_defaults(&s_cfg, "", "", 0);
        load_str(h, "host", s_cfg.hostname, sizeof(s_cfg.hostname));
        put_str(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), pre_ssid);
        put_str(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), pre_pass);
        {
            uint8_t b = 0;
            nvs_get_u8(h, "netmode", &b);
            s_cfg.net_static = b ? 1 : 0;
        }
        nvs_get_u32(h, "ip", &s_cfg.ip);
        nvs_get_u32(h, "msk", &s_cfg.mask);
        nvs_get_u32(h, "gw", &s_cfg.gw);
        nvs_get_u32(h, "dns1", &s_cfg.dns1);
        nvs_get_u32(h, "dns2", &s_cfg.dns2);
        load_str(h, "devid", s_cfg.device_id, sizeof(s_cfg.device_id));
        {
            uint8_t b = 3;
            nvs_get_u8(h, "logl", &b);
            s_cfg.log_level = b <= 5 ? b : 3;
        }
        {
            uint16_t p = 0;
            nvs_get_u16(h, "qdport", &p);
            s_cfg.qdevice_port = p != 0 ? p : 5403;
        }
        if (qesp_config_migrate(&s_cfg, has_ver ? ver : 0) != 0) {
            ESP_LOGE(TAG, "config v%lu unreadable (downgrade?), safe defaults",
                     (unsigned long)(has_ver ? ver : 0));
            qesp_config_defaults(&s_cfg, CONFIG_QUORUMESP_WIFI_SSID,
                                 CONFIG_QUORUMESP_WIFI_PASSWORD,
                                 CONFIG_QUORUMESP_SERVER_PORT);
        } else if (!has_ver || ver != QESP_CONFIG_VERSION) {
            ESP_LOGW(TAG, "config migrated v%lu -> v%u",
                     (unsigned long)(has_ver ? ver : 0), QESP_CONFIG_VERSION);
            nvs_set_u32(h, "ver", s_cfg.version);
            nvs_set_str(h, "host", s_cfg.hostname);
            nvs_set_u8(h, "netmode", s_cfg.net_static);
            nvs_set_str(h, "devid", s_cfg.device_id);
            nvs_set_u8(h, "logl", s_cfg.log_level);
            nvs_set_u16(h, "qdport", s_cfg.qdevice_port);
            nvs_commit(h);
        }
        fill_device_id();
        if (s_cfg.hostname[0] == '\0') {
            put_str(s_cfg.hostname, sizeof(s_cfg.hostname), "quorumesp");
        }
        ESP_LOGI(TAG, "config v%lu id=%s", (unsigned long)s_cfg.version,
                 s_cfg.device_id);
    }
    nvs_close(h);
    s_loaded = 1;
    if (s_cfg.log_level <= 5) {
        esp_log_level_set("*", (esp_log_level_t)s_cfg.log_level);
    }
    return ESP_OK;
}

const qesp_config_t *quorumesp_config_get(void) {
    if (!s_loaded) {
        return NULL;
    }
    return &s_cfg;
}

esp_err_t quorumesp_config_get_wifi(char *ssid, size_t ssid_cap,
                                    char *pass, size_t pass_cap) {
    if (!s_loaded) {
        return ESP_FAIL;
    }
    if (ssid_cap < 2 || pass_cap < 2 || s_cfg.wifi_ssid[0] == '\0') {
        /* NVS empty: Kconfig fallback (dev). */
        if (CONFIG_QUORUMESP_WIFI_SSID[0] == '\0') {
            return ESP_FAIL;
        }
        put_str(ssid, ssid_cap, CONFIG_QUORUMESP_WIFI_SSID);
        put_str(pass, pass_cap, CONFIG_QUORUMESP_WIFI_PASSWORD);
        ESP_LOGW(TAG, "wifi creds from Kconfig fallback (dev only)");
        return ESP_OK;
    }
    put_str(ssid, ssid_cap, s_cfg.wifi_ssid);
    put_str(pass, pass_cap, s_cfg.wifi_pass);
    ESP_LOGI(TAG, "wifi creds from NVS");
    return ESP_OK;
}

esp_err_t quorumesp_config_factory_reset(void) {
    nvs_handle_t h;
    if (nvs_open(QESP_CONFIG_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "config namespace erased — reprovision, then reboot");
    return ESP_OK;
}
