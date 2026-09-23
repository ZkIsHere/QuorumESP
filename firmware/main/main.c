/* QuorumESP main — scaffold only (Phase 2 entry point).
 *
 * Boot order will be: logging → config → storage → network → qdevice →
 * watchdog. Nothing here decides quorum (AGENTS.md §2): the firmware only
 * ever provides a QDevice vote; Corosync decides.
 */
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "BOOT";

void app_main(void) {
    esp_chip_info_t info;
    esp_chip_info(&info);
    ESP_LOGI(TAG, "QuorumESP boot (scaffold, EXPERIMENTAL - no QDevice yet)");
    ESP_LOGI(TAG, "cores=%d silicon_rev=%d", info.cores, info.revision);
    ESP_LOGW(TAG, "Phase 2+ initializers go here (network, qdevice, watchdog)");
}
