/* Diagnostic web UI — read-only (see api.h). Never add POST/PUT/DELETE. */
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

#include "config.h"
#include "qesp_defs.h"
#include "server.h"
#include "tls.h"
#include "wifi.h"

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

static const char *vote_name(uint8_t v) {
    switch (v) {
    case QESP_VOTE_ACK:
        return "ack";
    case QESP_VOTE_NACK:
        return "nack";
    case QESP_VOTE_ASK_LATER:
        return "ask-later";
    case QESP_VOTE_NO_CHANGE:
        return "no-change";
    case QESP_VOTE_WAIT_FOR_REPLY:
        return "wait-for-reply";
    default:
        return "undefined";
    }
}

/* s_ip_be is network byte order (matches IP2STR usage in wifi.c). */
static void fmt_ip(char *dst, size_t cap, uint32_t ip) {
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(dst, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static esp_err_t h_status(httpd_req_t *r) {
    qdev_status_t st;
    const qesp_config_t *cfg = quorumesp_config_get();
    const esp_app_desc_t *app = esp_app_get_description();
    char ip[16];
    char cluster_esc[sizeof(st.cluster) * 2 + 8];
    size_t len = 0;
    uint8_t i;

    qdevice_status_snapshot(&st);
    fmt_ip(ip, sizeof(ip), network_wifi_get_ip());
    json_escape(cluster_esc, sizeof(cluster_esc), st.cluster);

    if (xSemaphoreTake(s_resp_mux, pdMS_TO_TICKS(2000)) != pdTRUE) {
        httpd_resp_send_500(r);
        return ESP_FAIL;
    }
    len += (size_t)snprintf(s_resp + len, RESP_MAX - len,
                            "{\"firmware\":{\"version\":\"%s\",\"id\":\"%s\","
                            "\"uptime_s\":%u,\"heap_free\":%u},"
                            "\"net\":{\"ip\":\"%s\",\"rssi_dbm\":%d,\"tls\":%d},"
                            "\"cluster\":{\"name\":\"%s\",\"algo\":\"%s\"},"
                            "\"clients\":[",
                            app->version, cfg->device_id,
                            (unsigned)st.uptime_s,
                            (unsigned)esp_get_free_heap_size(),
                            ip, network_wifi_get_rssi(),
                            network_tls_available(),
                            cluster_esc, algo_name(st.algo));
    for (i = 0; i < st.n; i++) {
        len += (size_t)snprintf(s_resp + len, RESP_MAX - len,
                                "%s{\"node\":%u,\"algo\":\"%s\",\"state\":%u,"
                                "\"tls\":%u,\"vote\":\"%s\"}",
                                i ? "," : "", (unsigned)st.cli[i].node,
                                algo_name(st.cli[i].algo),
                                (unsigned)st.cli[i].state,
                                (unsigned)st.cli[i].tls,
                                vote_name(st.cli[i].vote));
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
    char ip[16];
    size_t len = 0;
    uint8_t i;

    qdevice_status_snapshot(&st);
    fmt_ip(ip, sizeof(ip), network_wifi_get_ip());

    if (xSemaphoreTake(s_resp_mux, pdMS_TO_TICKS(2000)) != pdTRUE) {
        httpd_resp_send_500(r);
        return ESP_FAIL;
    }
    len += (size_t)snprintf(s_resp + len, RESP_MAX - len,
                            "<html><head><title>QuorumESP</title></head><body>"
                            "<h1>QuorumESP (EXPERIMENTAL)</h1>"
                            "<p>firmware %s id %s uptime %us heap %u</p>"
                            "<p>net %s rssi %d dBm tls %d</p>"
                            "<p>cluster %.60s algo %s qdevice port %u</p>"
                            "<table border=1><tr><th>node</th><th>algo</th>"
                            "<th>tls</th><th>vote</th></tr>",
                            app->version, cfg->device_id,
                            (unsigned)st.uptime_s,
                            (unsigned)esp_get_free_heap_size(),
                            ip, network_wifi_get_rssi(),
                            network_tls_available(),
                            st.cluster, algo_name(st.algo),
                            (unsigned)cfg->qdevice_port);
    for (i = 0; i < st.n; i++) {
        len += (size_t)snprintf(s_resp + len, RESP_MAX - len,
                                "<tr><td>%u</td><td>%s</td><td>%u</td>"
                                "<td>%s</td></tr>",
                                (unsigned)st.cli[i].node,
                                algo_name(st.cli[i].algo),
                                (unsigned)st.cli[i].tls,
                                vote_name(st.cli[i].vote));
    }
    len += (size_t)snprintf(s_resp + len, RESP_MAX - len,
                            "</table>"
                            "<p><a href=\"/api/status\">status json</a> "
                            "<a href=\"/api/log\">log json</a></p>"
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
    ESP_LOGI(TAG, "web ui on port %d (GET only, read-only)",
             CONFIG_QUORUMESP_WEB_PORT);
    return ESP_OK;
#endif
}
