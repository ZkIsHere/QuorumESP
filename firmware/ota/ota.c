/* OTA engine. Flow: version check -> download -> verify -> set boot ->
 * reboot -> health confirm (else automatic rollback). See docs/ota.md. */
#include "ota.h"

#include <stdlib.h>
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

/* GET <base>/version.txt into out (NUL-terminated). GitHub = HTTPS.
 * Follows redirects MANUALLY with a fresh client per hop: the auto-follow
 * inside perform() delivers status 200 but an unreadable body after 2 hops
 * (observed 2026-09-24). Max 4 hops. */
static esp_err_t fetch_version(const char *base, char *out, size_t cap) {
    char url[1024];
    int hop;
    if (snprintf(url, sizeof(url), "%s/version.txt", base) >= (int)sizeof(url)) {
        return ESP_FAIL;
    }
    for (hop = 0; hop < 5; hop++) {
        esp_http_client_config_t cfg;
        esp_http_client_handle_t cli;
        int status, got;
        char *loc_val = NULL;
        memset(&cfg, 0, sizeof(cfg));
        cfg.url = url;
        /* 3-host chain with a TLS handshake each: RSA verification on
         * ESP32 takes seconds per handshake. Generous budget per hop. */
        cfg.timeout_ms = 60000;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        /* GitHub serves ~5K of response headers; the 512B default overflows
         * ("Out of buffer", observed 2026-09-24). The signed redirect target
         * also makes the request line long. */
        cfg.buffer_size = 8192;
        cfg.buffer_size_tx = 2048;
        cfg.disable_auto_redirect = true;
        cli = esp_http_client_init(&cfg);
        if (cli == NULL) {
            return ESP_FAIL;
        }
        if (esp_http_client_open(cli, 0) != ESP_OK ||
            esp_http_client_fetch_headers(cli) < 0) {
            ESP_LOGW(TAG, "version fetch failed");
            esp_http_client_cleanup(cli);
            return ESP_FAIL;
        }
        status = esp_http_client_get_status_code(cli);
        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            /* NOTE: get_header() reads *request* headers; response headers
             * need get_response_header() + SAVE_RESPONSE_HEADERS. */
            if (esp_http_client_get_response_header(cli, "Location", &loc_val) != ESP_OK ||
                loc_val == NULL || loc_val[0] == '\0') {
                ESP_LOGW(TAG, "redirect without Location");
                esp_http_client_cleanup(cli);
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "redirect hop %d", hop + 1);
            snprintf(url, sizeof(url), "%s", loc_val);
            esp_http_client_cleanup(cli);
            continue;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "version check HTTP status %d", status);
            esp_http_client_cleanup(cli);
            return ESP_FAIL;
        }
        got = esp_http_client_read_response(cli, out, (int)(cap - 1));
        esp_http_client_cleanup(cli);
        if (got <= 0) {
            ESP_LOGW(TAG, "version body empty");
            return ESP_FAIL;
        }
        out[got] = '\0';
        str_trim(out);
        break;
    }
    if (hop >= 5) {
        ESP_LOGW(TAG, "too many redirects");
        return ESP_FAIL;
    }
    /* Sanity: a version is short alnum text. Without this, an error page
     * body (e.g. GitHub "Not Found") would pass as a version and trigger a
     * doomed download (observed 2026-09-24). */
    {
        size_t n = strlen(out);
        size_t i;
        if (n == 0 || n > 40) {
            return ESP_FAIL;
        }
        for (i = 0; i < n; i++) {
            char ch = out[i];
            if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') &&
                !(ch >= '0' && ch <= '9') && ch != '.' && ch != '_' &&
                ch != '-' && ch != '+') {
                ESP_LOGW(TAG, "version sanity reject: '%s'", out);
                return ESP_FAIL;
            }
        }
    }
    return ESP_OK;
}

/* OTA check must NOT run on the main task (3.5K stack): the TLS handshake
 * with cert-bundle parsing needs tens of KB. Dedicated 12K task instead;
 * main waits for it (bounded) and boots the server either way. */
#define OTA_TASK_STACK 12288
#define OTA_TASK_WAIT_MS 150000

typedef struct {
    TaskHandle_t main_task;
} ota_check_arg_t;

static void ota_check_task(void *arg) {
    ota_check_arg_t *a = (ota_check_arg_t *)arg;
    quorumesp_ota_check_and_update();
    xTaskNotifyGive(a->main_task);
    free(a);
    vTaskDelete(NULL);
}

void quorumesp_ota_check_async_and_wait(void) {
    ota_check_arg_t *a = (ota_check_arg_t *)malloc(sizeof(*a));
    if (a == NULL) {
        return;
    }
    a->main_task = xTaskGetCurrentTaskHandle();
    if (xTaskCreate(ota_check_task, "ota_chk", OTA_TASK_STACK, a, 5, NULL) != pdPASS) {
        free(a);
        return;
    }
    xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(OTA_TASK_WAIT_MS));
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
        /* ~1.5MB firmware over software TLS needs minutes, not seconds. */
        http_cfg.timeout_ms = 300000;
        http_cfg.keep_alive_enable = true;
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
        http_cfg.buffer_size = 8192;
        http_cfg.buffer_size_tx = 2048;
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
