#include "watchdog.h"

#include "esp_log.h"
#include "esp_task_wdt.h"

static const char *TAG = "WATCHDOG";

esp_err_t quorumesp_watchdog_init(void) {
    /* Timeout/enable come from sdkconfig (CONFIG_ESP_TASK_WDT_*).
     * Nothing to do at runtime besides logging the contract. */
    ESP_LOGI(TAG, "task watchdog armed (feed every loop wake)");
    return ESP_OK;
}

esp_err_t quorumesp_watchdog_add_current(void) {
    esp_err_t r = esp_task_wdt_add(NULL);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "wdt add failed: %s", esp_err_to_name(r));
    }
    return r;
}

void quorumesp_watchdog_feed(void) {
    esp_task_wdt_reset();
}

void quorumesp_watchdog_remove_current(void) {
    esp_task_wdt_delete(NULL);
}
