/* Diagnostic web UI — read-only + Basic auth (see api.h).
 * Never add POST/PUT/DELETE. Never display secrets.
 */
#include "api.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

#include "config.h"
#include "qesp_defs.h"
#include "server.h"

static const char *TAG = "WEB";

#if CONFIG_QUORUMESP_WEB_ENABLE

/* ---- recent-log ring (tap on esp_log output) ---- */

#define LOG_RING_N 40
#define LOG_LINE_MAX 128

static char s_ring[LOG_RING_N][LOG_LINE_MAX];
static unsigned s_head; /* next write slot */
static unsigned s_count;
static portMUX_TYPE s_log_spin = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_vprintf;

static int web_log_hook(const char *fmt, va_list ap) {
    va_list aq;
    char tmp[LOG_LINE_MAX];
    size_t len;

    va_copy(aq, ap);
    vsnprintf(tmp, sizeof(tmp), fmt, aq);
    va_end(aq);

    len = strlen(tmp);
    while (len > 0 && (tmp[len - 1] == '\n' || tmp[len - 1] == '\r')) {
        tmp[--len] = '\0';
    }
    portENTER_CRITICAL(&s_log_spin);
    strncpy(s_ring[s_head % LOG_RING_N], tmp, LOG_LINE_MAX - 1);
    s_ring[s_head % LOG_RING_N][LOG_LINE_MAX - 1] = '\0';
    s_head++;
    if (s_count < LOG_RING_N) {
        s_count++;
    }
    portEXIT_CRITICAL(&s_log_spin);

    if (s_prev_vprintf != NULL) {
        return s_prev_vprintf(fmt, ap);
    }
    return vprintf(fmt, ap);
}

/* ---- Basic auth (credentials from menuconfig, never committed) ---- */

static int const_time_eq(const char *a, const char *b, size_t n) {
    unsigned diff = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    }
    return diff == 0;
}

static int has_auth(httpd_req_t *r) {
    /* "user:pass" expected; header value "Basic <b64>" (160B cap: longer
     * credentials fail closed). */
    char hdr[160];
    char exp[128];
    unsigned char dec[128];
    size_t olen = 0;
    size_t explen;
    const char *b64;

    if (CONFIG_QUORUMESP_WEB_USER[0] == '\0' ||
        CONFIG_QUORUMESP_WEB_PASSWORD[0] == '\0') {
        return 0;
    }
    if (httpd_req_get_hdr_value_str(r, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return 0;
    }
    if (strncmp(hdr, "Basic ", 6) != 0) {
        return 0;
    }
    b64 = hdr + 6;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &olen, (const unsigned char *)b64,
                               strlen(b64)) != 0) {
        return 0;
    }
    dec[olen < sizeof(dec) ? olen : sizeof(dec) - 1] = '\0';
    snprintf(exp, sizeof(exp), "%s:%s",
             CONFIG_QUORUMESP_WEB_USER, CONFIG_QUORUMESP_WEB_PASSWORD);
    explen = strlen(exp);
    if (olen != explen) {
        return 0;
    }
    return const_time_eq((const char *)dec, exp, explen);
}

static esp_err_t deny(httpd_req_t *r) {
    httpd_resp_set_status(r, "401 Unauthorized");
    httpd_resp_set_hdr(r, "WWW-Authenticate", "Basic realm=\"QuorumESP\"");
    httpd_resp_send(r, "auth required", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- shared response buffer (handlers serialize on s_resp_mux) ---- */

#define RESP_MAX 8192
static char s_resp[RESP_MAX];
static SemaphoreHandle_t s_resp_mux;

/* JSON-escape src into dst (cap incl. NUL). Returns bytes written excl. NUL. */
static size_t json_escape(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (; *src != '\0'; src++) {
        char c = *src;
        const char *esc = NULL;
        char ubuf[7];
        if (c == '"') {
            esc = "\\\"";
        } else if (c == '\\') {
            esc = "\\\\";
        } else if (c == '\n') {
            esc = "\\n";
        } else if (c == '\r') {
            esc = "\\r";
        } else if (c == '\t') {
            esc = "\\t";
        } else if ((unsigned char)c < 0x20) {
            snprintf(ubuf, sizeof(ubuf), "\\u%04x", (unsigned)c);
            esc = ubuf;
        }
        if (esc != NULL) {
            size_t el = strlen(esc);
            if (n + el >= cap) {
                break;
            }
            memcpy(dst + n, esc, el);
            n += el;
        } else {
            if (n + 1 >= cap) {
                break;
            }
            dst[n++] = c;
        }
    }
    if (cap > 0) {
        dst[n < cap ? n : cap - 1] = '\0';
    }
    return n;
}

/* 90 -> "1m 30s"; 90061 -> "1d 1h 1m 1s"; 0 -> "0s". */
static void fmt_uptime(char *dst, size_t cap, uint32_t sec) {
    static const struct {
        const char *sfx;
        uint32_t div;
    } units[] = {
        {"y", 365 * 86400}, {"mo", 30 * 86400}, {"w", 7 * 86400},
        {"d", 86400}, {"h", 3600}, {"m", 60}, {"s", 1},
    };
    size_t len = 0, u = 0;
    int any = 0;
    for (u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
        uint32_t v = sec / units[u].div;
        /* Skip leading zero units but always show seconds. */
        if (v == 0 && !(any || units[u].div == 1)) {
            continue;
        }
        sec -= v * units[u].div;
        len += (size_t)snprintf(dst + len, len < cap ? cap - len : 0,
                                "%s%u%s", any ? " " : "",
                                (unsigned)v, units[u].sfx);
        any = 1;
        if (len >= cap) {
            break;
        }
    }
    if (cap > 0) {
        dst[cap - 1] = '\0';
    }
}

static const char *algo_name(uint8_t a) {
    switch (a) {
    case QESP_ALGO_FFSPLIT:
        return "ffsplit";
    case QESP_ALGO_2NODELMS:
        return "2nodelms";
    case QESP_ALGO_LMS:
        return "lms";
    case QESP_ALGO_TEST:
        return "test";
    default:
        return "none";
    }
}

static esp_err_t h_status(httpd_req_t *r) {
    qdev_status_t st;
    const qesp_config_t *cfg = quorumesp_config_get();
    const esp_app_desc_t *app = esp_app_get_description();
    char up[64];
    size_t len = 0;
    uint8_t i;

    if (!has_auth(r)) {
        return deny(r);
    }
    qdevice_status_snapshot(&st);
    fmt_uptime(up, sizeof(up), st.uptime_s);

    if (xSemaphoreTake(s_resp_mux, pdMS_TO_TICKS(2000)) != pdTRUE) {
        httpd_resp_send_500(r);
        return ESP_FAIL;
    }
    len += (size_t)snprintf(s_resp + len, RESP_MAX - len,
                            "{\"project\":\"QuorumESP\",\"id\":\"%s\","
                            "\"version\":\"%s\",\"uptime_s\":%u,\"uptime\":\"%s\","
                            "\"clients\":[",
                            cfg->device_id, app->version,
                            (unsigned)st.uptime_s, up);
    for (i = 0; i < st.n; i++) {
        len += (size_t)snprintf(s_resp + len, RESP_MAX - len,
                                "%s{\"node\":%u,\"algo\":\"%s\"}",
                                i ? "," : "", (unsigned)st.cli[i].node,
                                algo_name(st.cli[i].algo));
    }
    len += (size_t)snprintf(s_resp + len, RESP_MAX - len, "]}");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_send(r, s_resp, (ssize_t)len);
    xSemaphoreGive(s_resp_mux);
    return ESP_OK;
}

static esp_err_t h_log(httpd_req_t *r) {
    char line[LOG_LINE_MAX * 2 + 8];
    unsigned count, head, i;
    size_t len = 0;

    if (!has_auth(r)) {
        return deny(r);
    }
    portENTER_CRITICAL(&s_log_spin);
    count = s_count;
    head = s_head;
    portEXIT_CRITICAL(&s_log_spin);

    if (xSemaphoreTake(s_resp_mux, pdMS_TO_TICKS(2000)) != pdTRUE) {
        httpd_resp_send_500(r);
        return ESP_FAIL;
    }
    len += (size_t)snprintf(s_resp + len, RESP_MAX - len, "{\"lines\":[");
    for (i = 0; i < count; i++) {
        /* oldest first; copy out of the ring under the spinlock */
        char raw[LOG_LINE_MAX];
        unsigned idx = (head - count + i) % LOG_RING_N;
        portENTER_CRITICAL(&s_log_spin);
        strncpy(raw, s_ring[idx], sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
        portEXIT_CRITICAL(&s_log_spin);
        json_escape(line, sizeof(line), raw);
        len += (size_t)snprintf(s_resp + len, RESP_MAX - len, "%s\"%s\"",
                                i ? "," : "", line);
        if (len + LOG_LINE_MAX * 2 + 16 >= RESP_MAX) {
            break; /* never overrun; newer lines would need a bigger buffer */
        }
    }
    len += (size_t)snprintf(s_resp + len, RESP_MAX - len, "]}");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_send(r, s_resp, (ssize_t)len);
    xSemaphoreGive(s_resp_mux);
    return ESP_OK;
}

static esp_err_t h_root(httpd_req_t *r) {
    qdev_status_t st;
    const qesp_config_t *cfg = quorumesp_config_get();
    const esp_app_desc_t *app = esp_app_get_description();
    char up[64];
    size_t len = 0;
    uint8_t i;

    if (!has_auth(r)) {
        return deny(r);
    }
    qdevice_status_snapshot(&st);
    fmt_uptime(up, sizeof(up), st.uptime_s);

    if (xSemaphoreTake(s_resp_mux, pdMS_TO_TICKS(2000)) != pdTRUE) {
        httpd_resp_send_500(r);
        return ESP_FAIL;
    }
    len += (size_t)snprintf(s_resp + len, RESP_MAX - len,
                            "<html><head><title>QuorumESP</title></head><body>"
                            "<h1>QuorumESP %s</h1>"
                            "<p>version %s</p>"
                            "<p>uptime %s</p>"
                            "<h2>clients</h2>"
                            "<table border=1><tr><th>node</th><th>algo</th></tr>",
                            cfg->device_id, app->version, up);
    for (i = 0; i < st.n; i++) {
        len += (size_t)snprintf(s_resp + len, RESP_MAX - len,
                                "<tr><td>%u</td><td>%s</td></tr>",
                                (unsigned)st.cli[i].node,
                                algo_name(st.cli[i].algo));
    }
    len += (size_t)snprintf(s_resp + len, RESP_MAX - len,
                            "</table>"
                            "<p><a href=\"/api/status\">status json</a> "
                            "<a href=\"/api/log\">log json</a></p>"
                            "<h2>log</h2><pre>");
    {
        unsigned count, head, k;
        portENTER_CRITICAL(&s_log_spin);
        count = s_count;
        head = s_head;
        portEXIT_CRITICAL(&s_log_spin);
        for (k = 0; k < count; k++) {
            char raw[LOG_LINE_MAX];
            unsigned idx = (head - count + k) % LOG_RING_N;
            portENTER_CRITICAL(&s_log_spin);
            strncpy(raw, s_ring[idx], sizeof(raw) - 1);
            raw[sizeof(raw) - 1] = '\0';
            portEXIT_CRITICAL(&s_log_spin);
            /* HTML-escape the two characters that break <pre>. */
            char *p;
            for (p = raw; *p != '\0'; p++) {
                const char *rep = NULL;
                if (*p == '<') {
                    rep = "&lt;";
                } else if (*p == '&') {
                    rep = "&amp;";
                }
                if (rep != NULL) {
                    len += (size_t)snprintf(s_resp + len, RESP_MAX - len, "%s", rep);
                } else if (len + 2 < RESP_MAX) {
                    s_resp[len++] = *p;
                    s_resp[len] = '\0';
                } else {
                    break;
                }
            }
            if (len + 2 < RESP_MAX) {
                s_resp[len++] = '\n';
                s_resp[len] = '\0';
            } else {
                break;
            }
        }
    }
    len += (size_t)snprintf(s_resp + len, RESP_MAX - len,
                            "</pre>"
                            "<p>Read-only diagnostics. "
                            "This page cannot change quorum state.</p>"
                            "</body></html>");
    httpd_resp_set_type(r, "text/html");
    httpd_resp_send(r, s_resp, (ssize_t)len);
    xSemaphoreGive(s_resp_mux);
    return ESP_OK;
}

#endif /* CONFIG_QUORUMESP_WEB_ENABLE */

esp_err_t web_api_init(void) {
#if !CONFIG_QUORUMESP_WEB_ENABLE
    ESP_LOGI(TAG, "web ui disabled (QUORUMESP_WEB_ENABLE=n)");
    return ESP_OK;
#else
    httpd_handle_t srv = NULL;
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t u_root = {
        .uri = "/", .method = HTTP_GET, .handler = h_root, .user_ctx = NULL
    };
    httpd_uri_t u_status = {
        .uri = "/api/status", .method = HTTP_GET,
        .handler = h_status, .user_ctx = NULL
    };
    httpd_uri_t u_log = {
        .uri = "/api/log", .method = HTTP_GET,
        .handler = h_log, .user_ctx = NULL
    };

    /* Fail closed: no credentials -> no UI at all (never an open page). */
    if (CONFIG_QUORUMESP_WEB_USER[0] == '\0' ||
        CONFIG_QUORUMESP_WEB_PASSWORD[0] == '\0') {
        ESP_LOGE(TAG, "web auth not configured (QUORUMESP_WEB_USER/PASSWORD "
                      "empty) — refusing to start an open UI");
        return ESP_FAIL;
    }
    s_resp_mux = xSemaphoreCreateMutex();
    if (s_resp_mux == NULL) {
        return ESP_FAIL;
    }
    /* Tap logs from here on (boot lines before this point are not kept). */
    s_prev_vprintf = esp_log_set_vprintf(web_log_hook);

    hcfg.server_port = CONFIG_QUORUMESP_WEB_PORT;
    if (httpd_start(&srv, &hcfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }
    httpd_register_uri_handler(srv, &u_root);
    httpd_register_uri_handler(srv, &u_status);
    httpd_register_uri_handler(srv, &u_log);
    ESP_LOGI(TAG, "web ui on port %d (GET + Basic auth, read-only)",
             CONFIG_QUORUMESP_WEB_PORT);
    return ESP_OK;
#endif
}
