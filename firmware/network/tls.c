/* TLS server via public esp-tls API (version-proof across IDF/mbedTLS majors).
 * See tls.h + docs/tls.md.
 *
 * Round 2a: server cert only, no client auth (cacert_buf left NULL).
 * Round 2b (TODO): client-cert verify + CN check via esp_tls_get_ssl_context
 * + mbedtls_ssl_set_verify on the underlying context.
 *
 * FD ownership: esp_tls_server_session_delete() owns the socket (same usage
 * as production esp_https_server) — never close() it separately.
 */
#include "tls.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_tls.h"

static const char *TAG = "TLS";

#ifdef HAS_DEV_CERTS
extern const char qesp_dev_ca_crt[]; /* reserved for round 2b */
extern const char qesp_dev_server_crt[];
extern const char qesp_dev_server_key[];
#endif

struct qesp_tls_session {
    esp_tls_t *tls;
};

static esp_tls_cfg_server_t s_cfg;
static int s_inited = 0;
static int s_available = 0;

esp_err_t network_tls_init(void) {
    if (s_inited) {
        return s_available ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    s_inited = 1;
#ifdef HAS_DEV_CERTS
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.servercert_buf = (const unsigned char *)qesp_dev_server_crt;
    s_cfg.servercert_bytes = (unsigned int)strlen(qesp_dev_server_crt) + 1;
    s_cfg.serverkey_buf = (const unsigned char *)qesp_dev_server_key;
    s_cfg.serverkey_bytes = (unsigned int)strlen(qesp_dev_server_key) + 1;
    /* No cacert_buf in 2a: the server does not request client certificates. */
    s_available = 1;
    ESP_LOGI(TAG, "TLS ready (dev certs embedded)");
    return ESP_OK;
#else
    ESP_LOGW(TAG, "no dev certs embedded — TLS unavailable (fail-closed)");
    return ESP_ERR_NOT_FOUND;
#endif
}

int network_tls_available(void) {
    return s_inited && s_available;
}

qesp_tls_session_t *network_tls_upgrade(int fd, const char *expected_cn,
                                        int require_client_cert) {
    qesp_tls_session_t *s;
    esp_tls_t *tls;
    (void)expected_cn;
    if (!network_tls_available()) {
        return NULL;
    }
    if (require_client_cert) {
        /* Round 2b: not implemented yet — refuse instead of downgrading. */
        ESP_LOGW(TAG, "mutual auth requested but not implemented (round 2b)");
        return NULL;
    }
    tls = esp_tls_init();
    if (tls == NULL) {
        return NULL;
    }
    /* Blocking handshake with esp-tls default server timeout. */
    if (esp_tls_server_session_create(&s_cfg, fd, tls) != 0) {
        ESP_LOGW(TAG, "TLS handshake failed, closing");
        esp_tls_server_session_delete(tls); /* owns fd, like esp_https_server */
        return NULL;
    }
    s = (qesp_tls_session_t *)malloc(sizeof(*s));
    if (s == NULL) {
        esp_tls_server_session_delete(tls);
        return NULL;
    }
    s->tls = tls;
    ESP_LOGI(TAG, "TLS handshake done (server-only auth)");
    return s;
}

int network_tls_read(qesp_tls_session_t *s, uint8_t *buf, size_t len) {
    size_t got = 0;
    if (s == NULL) {
        return -1;
    }
    while (got < len) {
        ssize_t r = esp_tls_conn_read(s->tls, (char *)buf + got, len - got);
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (r <= 0) {
            return -1; /* 0 = closed, <0 = error */
        }
        got += (size_t)r;
    }
    return 0;
}

int network_tls_write(qesp_tls_session_t *s, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    if (s == NULL) {
        return -1;
    }
    while (sent < len) {
        ssize_t r = esp_tls_conn_write(s->tls, (const char *)buf + sent, len - sent);
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        sent += (size_t)r;
    }
    return 0;
}

void network_tls_close(qesp_tls_session_t *s) {
    if (s == NULL) {
        return;
    }
    /* Owns the fd (see file header) — no separate close(). */
    esp_tls_server_session_delete(s->tls);
    free(s);
}
