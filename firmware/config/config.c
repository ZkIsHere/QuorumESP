#include "config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "CONFIG";

esp_err_t quorumesp_config_init(void) {
    ESP_LOGI(TAG, "config: NVS-backed Wi-Fi lookup active");
    return ESP_OK;
}

esp_err_t quorumesp_config_get_wifi(char *ssid, size_t ssid_cap,
                                    char *pass, size_t pass_cap) {
    nvs_handle_t h;
    esp_err_t r;
    size_t n;
    if (ssid == NULL || pass == NULL || ssid_cap < 2 || pass_cap < 2) {
        return ESP_FAIL;
    }
    r = nvs_open("qesp", NVS_READONLY, &h);
    if (r == ESP_OK) {
        n = ssid_cap;
        if (nvs_get_str(h, "ssid", ssid, &n) == ESP_OK && ssid[0] != '\0') {
            n = pass_cap;
            /* Open networks: pass key absent/empty is fine. */
            if (nvs_get_str(h, "pass", pass, &n) != ESP_OK) {
                pass[0] = '\0';
            }
            nvs_close(h);
            ESP_LOGI(TAG, "wifi creds from NVS");
            return ESP_OK;
        }
        nvs_close(h);
    }
    /* Fallback: compile-time (menuconfig) credentials. */
    if (CONFIG_QUORUMESP_WIFI_SSID[0] == '\0') {
        return ESP_FAIL;
    }
    strncpy(ssid, CONFIG_QUORUMESP_WIFI_SSID, ssid_cap - 1);
    ssid[ssid_cap - 1] = '\0';
    strncpy(pass, CONFIG_QUORUMESP_WIFI_PASSWORD, pass_cap - 1);
    pass[pass_cap - 1] = '\0';
    ESP_LOGW(TAG, "wifi creds from Kconfig fallback (dev only)");
    return ESP_OK;
}
