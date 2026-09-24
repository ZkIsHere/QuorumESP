/* OTA engine. Flow: version check -> download -> verify -> set boot ->
 * reboot -> health confirm (else automatic rollback). See docs/ota.md. */
#include "ota.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "OTA";

/* Boot into the new image only after it proves itself this long. */
#define OTA_HEALTHY_MS 60000

static void str_trim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

/* Update channel is hardcoded to GitHub releases (no custom URL mode):
 * .../releases/latest/download/{version.txt,firmware.bin} redirect to the
 * newest release assets (esp_http_client follows redirects). The repo must
 * be public, or provide a token (not implemented — see docs/ota.md). */
static void ota_base(char *out, size_t cap) {
    snprintf(out, cap, "https://github.com/%s/releases/latest/download",
             CONFIG_QUORUMESP_OTA_GITHUB_REPO);
}

/* GET <base>/version.txt into out (NUL-terminated). GitHub = HTTPS. */
static esp_err_t fetch_version(const char *base, char *out, size_t cap) {
    char url[256];
    esp_http_client_config_t cfg;
    esp_http_client_handle_t cli;
    int got;
    if (snprintf(url, sizeof(url), "%s/version.txt", base) >= (int)sizeof(url)) {
        return ESP_FAIL;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = url;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        return ESP_FAIL;
    }
    if (esp_http_client_open(cli, 0) != ESP_OK ||
        esp_http_client_fetch_headers(cli) < 0) {
        esp_http_client_cleanup(cli);
        return ESP_FAIL;
    }
    got = esp_http_client_read_response(cli, out, (int)(cap - 1));
    esp_http_client_cleanup(cli);
    if (got <= 0) {
        return ESP_FAIL;
    }
    out[got] = '\0';
    str_trim(out);
    return ESP_OK;
}

esp_err_t quorumesp_ota_check_and_update(void) {
    char base[192];
    const esp_app_desc_t *running;
    char server_ver[64];
    char url[256];
    esp_err_t r;

    ota_base(base, sizeof(base));
    running = esp_app_get_description();
    ESP_LOGI(TAG, "running version %s, checking %s", running->version, base);
    if (fetch_version(base, server_ver, sizeof(server_ver)) != ESP_OK) {
        ESP_LOGW(TAG, "version check failed, keeping current image");
        return ESP_FAIL;
    }
    if (strcmp(server_ver, running->version) == 0) {
        ESP_LOGI(TAG, "already on %s", server_ver);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "updating %s -> %s", running->version, server_ver);
    if (snprintf(url, sizeof(url), "%s/firmware.bin", base) >= (int)sizeof(url)) {
        return ESP_FAIL;
    }
    {
        esp_http_client_config_t http_cfg;
        esp_https_ota_config_t ota_cfg;
        memset(&http_cfg, 0, sizeof(http_cfg));
        http_cfg.url = url;
        http_cfg.timeout_ms = 30000;
        http_cfg.keep_alive_enable = true;
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
        memset(&ota_cfg, 0, sizeof(ota_cfg));
        ota_cfg.http_config = &http_cfg;
        r = esp_https_ota(&ota_cfg);
    }
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "OTA download/verify failed: %s (keeping old image)",
                 esp_err_to_name(r));
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "OTA written, rebooting into %s", server_ver);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK; /* unreachable */
}

static void confirm_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_HEALTHY_MS));
    ESP_LOGI(TAG, "healthy for %ds, confirming image", OTA_HEALTHY_MS / 1000);
    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
        ESP_LOGE(TAG, "confirm failed — next reboot may roll back");
    }
    vTaskDelete(NULL);
}

void quorumesp_ota_confirm_task_start(void) {
    if (xTaskCreate(confirm_task, "ota_ok", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "confirm task spawn failed");
    }
}
