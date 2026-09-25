/* QuorumESP main — dev bring-up (Wi-Fi transport, NON-PRODUCTION).
 *
 * Boot: nvs -> wifi (STA) -> qnetd-side TCP server task.
 * Nothing here decides quorum (AGENTS.md §2).
 */
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "server.h"
#include "config.h"
#include "watchdog.h"
#include "time_sync.h"
#include "tls.h"
#include "wifi.h"
#include "api.h"

static const char *TAG = "BOOT";

static void init_nvs(void) {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        r = nvs_flash_init();
    }
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(r));
    }
}

void app_main(void) {
    esp_chip_info_t info;
    esp_chip_info(&info);
    ESP_LOGI(TAG, "QuorumESP boot (EXPERIMENTAL, Wi-Fi dev transport)");
    ESP_LOGI(TAG, "cores=%d silicon_rev=%d", info.cores, info.revision);

    init_nvs();

    if (quorumesp_config_init() != ESP_OK) {
        ESP_LOGW(TAG, "config init failed, safe defaults in use");
    }
#if CONFIG_QUORUMESP_DEBUG
    esp_log_level_set("*", ESP_LOG_DEBUG);
    ESP_LOGW(TAG, "DEBUG mode on (verbose logs + extra web diagnostics)");
#endif
    quorumesp_watchdog_init();
    if (network_wifi_connect(NULL) != ESP_OK) {
        ESP_LOGE(TAG, "wifi up failed — provision NVS (docs/provisioning.md)");
        return;
    }
    if (network_time_sync() != ESP_OK) {
        ESP_LOGW(TAG, "no wall-clock — mutual TLS will fail-closed");
    }
    if (network_tls_init() != ESP_OK) {
        ESP_LOGW(TAG, "TLS unavailable (no dev certs) — plaintext only");
    }
    if (qdevice_server_start() != ESP_OK) {
        ESP_LOGE(TAG, "server task failed");
        return;
    }
    if (web_api_init() != ESP_OK) {
        ESP_LOGW(TAG, "web ui failed — diagnostics only, continuing");
    }
    ESP_LOGI(TAG, "up. point your qdevice client at this IP, port %d",
             (int)quorumesp_config_get()->qdevice_port);
}
