/* SNTP time sync. See time_sync.h. */
#include "time_sync.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "NETWORK";

static esp_err_t wait_sync(const char *label) {
    int i;
    for (i = 0; i < 30; i++) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(500)) == ESP_OK) {
            ESP_LOGI(TAG, "time synced via %s", label);
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t network_time_sync(void) {
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    time_t now = 0;
    struct tm tm;
    char buf[32];

    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "sntp init failed");
        return ESP_FAIL;
    }
    if (esp_netif_sntp_start() != ESP_OK) {
        ESP_LOGE(TAG, "sntp start failed");
        return ESP_FAIL;
    }
    if (wait_sync("pool.ntp.org") != ESP_OK) {
        /* Fallback server: AP DNS relay is flaky on some networks. */
        ESP_LOGW(TAG, "pool.ntp.org failed, trying time.google.com");
        esp_netif_sntp_set_server(0, "time.google.com");
        if (wait_sync("time.google.com") != ESP_OK) {
            ESP_LOGE(TAG, "sntp failed on both servers");
        }
    }
    time(&now);
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    /* Dev certs are short-lived; fail loudly if time is implausible. */
    if (now < 1700000000L) { /* 2023-11-14 */
        ESP_LOGE(TAG, "time not synced (now=%s) — TLS verify will fail", buf);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "time synced: %s", buf);
    return ESP_OK;
}
