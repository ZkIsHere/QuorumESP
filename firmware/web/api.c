/* Diagnostic web UI — read-only single page + SSE, Basic auth (see api.h).
 * Only GET handlers exist (POST -> 405). Never display secrets.
 */
#include "api.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "config.h"
#include "qesp_defs.h"
#include "server.h"

static const char *TAG = "WEB";

#if CONFIG_QUORUMESP_WEB_ENABLE

/* ---- rate limit (per IP, with fallback bucket) ---- */

typedef struct {
    uint32_t ip; /* 0 = free slot; s_fallback covers unknown peers */
    uint8_t fails;
    int64_t blocked_until_us;
} rate_t;

#define RATE_MAX 8

static rate_t s_rate[RATE_MAX];
static rate_t s_fallback;
static portMUX_TYPE s_web_spin = portMUX_INITIALIZER_UNLOCKED;

static int64_t now_us(void) {
    return esp_timer_get_time();
}

static uint32_t peer_ip(httpd_req_t *r) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = httpd_req_to_sockfd(r);
    memset(&addr, 0, sizeof(addr));
    if (getpeername(fd, (struct sockaddr *)&addr, &len) != 0) {
        return 0;
    }
    return addr.sin_addr.s_addr;
}

/* NULL when the IP is currently silenced (caller must drop wordlessly). */
static rate_t *rate_slot(uint32_t ip, int *silenced) {
    rate_t *free_slot = NULL;
    uint8_t i;
    *silenced = 0;
    if (ip == 0) {
        /* Peer unknown (getpeername failed): share one fallback bucket
         * so limiting still works instead of silently disabling. */
        ip = 0xFFFFFFFFu;
    }
    portENTER_CRITICAL(&s_web_spin);
    for (i = 0; i < RATE_MAX; i++) {
        if (s_rate[i].ip == ip) {
            if (s_rate[i].blocked_until_us > now_us()) {
                *silenced = 1;
            }
            portEXIT_CRITICAL(&s_web_spin);
            return &s_rate[i];
        }
        if (s_rate[i].ip == 0 && free_slot == NULL) {
            free_slot = &s_rate[i];
        }
    }
    if (ip == 0xFFFFFFFFu) {
        portEXIT_CRITICAL(&s_web_spin);
        return &s_fallback;
    }
    if (free_slot != NULL) {
        free_slot->ip = ip;
        free_slot->fails = 0;
        free_slot->blocked_until_us = 0;
    }
    portEXIT_CRITICAL(&s_web_spin);
    return free_slot;
}

static void rate_fail(rate_t *slot) {
    int maxf = CONFIG_QUORUMESP_WEB_LOGIN_MAX_FAIL;
    int block_s = CONFIG_QUORUMESP_WEB_LOGIN_BLOCK_S;
    if (slot == NULL) {
        return;
    }
    if (maxf < 1) {
        maxf = 1;
    }
    if (block_s < 1) {
        block_s = 1;
    }
    portENTER_CRITICAL(&s_web_spin);
    if (slot->fails < 255) {
        slot->fails++;
    }
    if (slot->fails >= (unsigned)maxf) {
        slot->blocked_until_us = now_us() + (int64_t)block_s * 1000000;
        ESP_LOGW(TAG, "login rate limit: ip silenced for %ds", block_s);
    }
    portEXIT_CRITICAL(&s_web_spin);
}

static void rate_clear(rate_t *slot) {
    if (slot == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_web_spin);
    slot->fails = 0;
    slot->blocked_until_us = 0;
    portEXIT_CRITICAL(&s_web_spin);
}

/* ---- Basic auth (browser dialog; credentials from menuconfig) ---- */

static int const_time_eq(const char *a, const char *b, size_t n) {
    unsigned diff = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    }
    return diff == 0;
}

/* 1 authed, 0 challenge (no creds presented), -1 wrong creds (counts). */
static int check_auth(httpd_req_t *r) {
    char hdr[160];
    char exp[128];
    unsigned char dec[128];
    size_t olen = 0;
    size_t explen;
    const char *b64;

    if (CONFIG_QUORUMESP_WEB_USER[0] == '\0' ||
        CONFIG_QUORUMESP_WEB_PASSWORD[0] == '\0') {
        return -1;
    }
    if (httpd_req_get_hdr_value_str(r, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return 0;
    }
    if (strncmp(hdr, "Basic ", 6) != 0) {
        return -1;
    }
    b64 = hdr + 6;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &olen, (const unsigned char *)b64,
                               strlen(b64)) != 0) {
        return -1;
    }
    dec[olen < sizeof(dec) ? olen : sizeof(dec) - 1] = '\0';
    snprintf(exp, sizeof(exp), "%s:%s",
             CONFIG_QUORUMESP_WEB_USER, CONFIG_QUORUMESP_WEB_PASSWORD);
    explen = strlen(exp);
    if (olen != explen) {
        return -1;
    }
    return const_time_eq((const char *)dec, exp, explen) ? 1 : -1;
}

static esp_err_t challenge(httpd_req_t *r) {
    httpd_resp_set_status(r, "401 Unauthorized");
    httpd_resp_set_hdr(r, "WWW-Authenticate", "Basic realm=\"QuorumESP\"");
    httpd_resp_send(r, "auth required", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Drop the connection with no response at all (rate-limit silence). */
static esp_err_t drop_silent(httpd_req_t *r) {
    int fd = httpd_req_to_sockfd(r);
    shutdown(fd, SHUT_RDWR);
    return ESP_FAIL;
}

/* Gate: silenced IP -> drop; missing creds -> 401; wrong creds -> count,
 * block-check, 401 (or silence once the limit trips). */
static int gate(httpd_req_t *r, rate_t **slot_out) {
    int silenced = 0;
    int rc;
    rate_t *slot = rate_slot(peer_ip(r), &silenced);
    if (slot_out != NULL) {
        *slot_out = slot;
    }
    if (silenced) {
        return -2;
    }
    rc = check_auth(r);
    if (rc == 1) {
        rate_clear(slot);
        return 1;
    }
    if (rc == 0) {
        return 0;
    }
    rate_fail(slot);
    portENTER_CRITICAL(&s_web_spin);
    silenced = (slot != NULL && slot->blocked_until_us > now_us());
    portEXIT_CRITICAL(&s_web_spin);
    return silenced ? -2 : -1;
}

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

/* One SSE payload: project/id/version/uptime/clients + last log lines. */
static size_t build_event(char *dst, size_t cap) {
    qdev_status_t st;
    const qesp_config_t *cfg = quorumesp_config_get();
    const esp_app_desc_t *app = esp_app_get_description();
    char up[64];
    size_t len = 0;
    uint8_t i;

    qdevice_status_snapshot(&st);
    fmt_uptime(up, sizeof(up), st.uptime_s);
    len += (size_t)snprintf(dst + len, cap - len,
                            "data: {\"project\":\"QuorumESP\",\"id\":\"%s\","
                            "\"version\":\"%s\",\"uptime_s\":%u,\"uptime\":\"%s\","
#if CONFIG_QUORUMESP_DEBUG
                            "\"heap\":%u,\"sessions\":%u,"
#endif
                            "\"clients\":[",
                            cfg->device_id, app->version,
                            (unsigned)st.uptime_s, up
#if CONFIG_QUORUMESP_DEBUG
                            , (unsigned)esp_get_free_heap_size(), (unsigned)st.n
#endif
                            );
    for (i = 0; i < st.n; i++) {
        len += (size_t)snprintf(dst + len, cap - len,
                                "%s{\"node\":%u,\"algo\":\"%s\"}",
                                i ? "," : "", (unsigned)st.cli[i].node,
                                algo_name(st.cli[i].algo));
        if (len >= cap) {
            return len;
        }
    }
    len += (size_t)snprintf(dst + len, cap - len, "],\"log\":[");
    {
        /* Last 10 lines per tick keep each event small. */
        unsigned count, head, k, shown = 0;
        char line[LOG_LINE_MAX * 2 + 8];
        portENTER_CRITICAL(&s_log_spin);
        count = s_count;
        head = s_head;
        portEXIT_CRITICAL(&s_log_spin);
        for (k = 0; k < count; k++) {
            char raw[LOG_LINE_MAX];
            unsigned idx = (head - count + k) % LOG_RING_N;
            if (count - k > 10) {
                continue; /* skip down to the last 10 */
            }
            portENTER_CRITICAL(&s_log_spin);
            strncpy(raw, s_ring[idx], sizeof(raw) - 1);
            raw[sizeof(raw) - 1] = '\0';
            portEXIT_CRITICAL(&s_log_spin);
            json_escape(line, sizeof(line), raw);
            len += (size_t)snprintf(dst + len, cap - len, "%s\"%s\"",
                                    shown ? "," : "", line);
            shown++;
            if (len + LOG_LINE_MAX * 2 + 16 >= cap) {
                break;
            }
        }
    }
    len += (size_t)snprintf(dst + len, cap - len, "]}\n\n");
    return len;
}

static esp_err_t h_root(httpd_req_t *r) {
    int g = gate(r, NULL);
    if (g == -2) {
        return drop_silent(r);
    }
    if (g != 1) {
        return challenge(r);
    }
    /* Single interface: static shell, live values arrive via /events. */
    if (xSemaphoreTake(s_resp_mux, pdMS_TO_TICKS(2000)) != pdTRUE) {
        httpd_resp_send_500(r);
        return ESP_FAIL;
    }
    snprintf(s_resp, RESP_MAX,
             "<html><head><title>QuorumESP</title></head><body>"
             "<h1>QuorumESP <span id=\"did\">?</span> "
             "<a href=\"/logout\" style=\"font-size:50%%\">logout</a></h1>"
             "<p>version <span id=\"ver\">?</span></p>"
             "<p>uptime <span id=\"up\">?</span></p>"
             "<h2>clients</h2><table border=1 id=\"cli\">"
             "<tr><th>node</th><th>algo</th></tr></table>"
             "<h2>log</h2><pre id=\"log\">?</pre>"
             "<script>"
             "var es=new EventSource('/events');"
             "es.onmessage=function(e){"
             "var d=JSON.parse(e.data);"
             "document.getElementById('did').textContent=d.id;"
             "document.getElementById('ver').textContent=d.version;"
             "document.getElementById('up').textContent=d.uptime;"
             "var t='<tr><th>node</th><th>algo</th></tr>';"
             "for(var i=0;i<d.clients.length;i++){"
             "t+='<tr><td>'+d.clients[i].node+'</td><td>'+d.clients[i].algo+'</td></tr>';}"
             "document.getElementById('cli').innerHTML=t;"
             "document.getElementById('log').textContent=d.log.join('\\n');"
             "};"
             "</script>"
             "<p>Read-only diagnostics. "
             "This page cannot change quorum state.</p>"
             "</body></html>");
    httpd_resp_set_type(r, "text/html");
    httpd_resp_send(r, s_resp, HTTPD_RESP_USE_STRLEN);
    xSemaphoreGive(s_resp_mux);
    return ESP_OK;
}

static esp_err_t h_logout(httpd_req_t *r) {
    /* Basic auth is cached by the browser: answer 401 so it re-prompts.
     * True logout = close the browser; documented in docs/web.md. */
    (void)r;
    return challenge(r);
}

static esp_err_t h_events(httpd_req_t *r) {
    int g = gate(r, NULL);
    int64_t end_us;
    if (g == -2) {
        return drop_silent(r);
    }
    if (g != 1) {
        return challenge(r);
    }
    httpd_resp_set_type(r, "text/event-stream");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(r, "X-Accel-Buffering", "no");
    /* Hold one httpd worker up to 60s; EventSource reconnects after. */
    end_us = now_us() + 60000000;
    for (;;) {
        size_t len;
        if (xSemaphoreTake(s_resp_mux, pdMS_TO_TICKS(2000)) != pdTRUE) {
            break;
        }
        len = build_event(s_resp, sizeof(s_resp));
        xSemaphoreGive(s_resp_mux);
        if (httpd_resp_send_chunk(r, s_resp, len) != ESP_OK) {
            break; /* client gone */
        }
        if (now_us() >= end_us) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    httpd_resp_send_chunk(r, NULL, 0);
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
    httpd_uri_t u_logout = {
        .uri = "/logout", .method = HTTP_GET,
        .handler = h_logout, .user_ctx = NULL
    };
    httpd_uri_t u_events = {
        .uri = "/events", .method = HTTP_GET,
        .handler = h_events, .user_ctx = NULL
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
    /* httpd workers default to a 4K stack: our handlers log through the
     * ring-hook (vsnprintf + emit) and format multi-KB pages — that
     * overflowed the worker and aborted in newlib locks (observed live:
     * crash-reboot on the 5th wrong login, caught by backtrace).
     * 8K leaves headroom; still one short-lived worker per connection. */
    hcfg.stack_size = 8192;
    if (httpd_start(&srv, &hcfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }
    httpd_register_uri_handler(srv, &u_root);
    httpd_register_uri_handler(srv, &u_logout);
    httpd_register_uri_handler(srv, &u_events);
    ESP_LOGI(TAG, "web ui on port %d (Basic auth + SSE, read-only)",
             CONFIG_QUORUMESP_WEB_PORT);
    return ESP_OK;
#endif
}
