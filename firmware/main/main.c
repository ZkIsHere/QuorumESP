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
#include "tls.h"
#include "wifi.h"

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

    if (network_wifi_connect(NULL) != ESP_OK) {
        ESP_LOGE(TAG, "wifi up failed — set creds via menuconfig, then rebuild");
        return;
    }
    if (network_tls_init() != ESP_OK) {
        ESP_LOGW(TAG, "TLS unavailable (no dev certs) — plaintext only");
    }
    if (qdevice_server_start() != ESP_OK) {
        ESP_LOGE(TAG, "server task failed");
        return;
    }
    ESP_LOGI(TAG, "up. point your qdevice client at this IP, port %d",
             CONFIG_QUORUMESP_SERVER_PORT);
}
