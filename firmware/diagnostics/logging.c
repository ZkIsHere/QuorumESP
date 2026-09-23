#include "logging.h"
#include "esp_log.h"

static const char *TAG = "BOOT";

esp_err_t diagnostics_logging_init(void) {
    ESP_LOGI(TAG, "logging: not implemented (Phase 2)");
    return ESP_OK;
}
