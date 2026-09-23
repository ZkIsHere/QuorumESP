/* TLS server: esp-tls for 2a, raw mbedTLS for 2b mutual (see below).
 * See tls.h + docs/tls.md.
 *
 * Round 2a: server cert only, no client auth (cacert_buf left NULL).
 * Round 2b: raw mbedTLS because esp-tls never wires ca_chain server-side,
 * so its CertificateRequest carries an empty CA list and clients never
 * select a cert to send. Raw path sets OPTIONAL + ca_chain; enforcement =
 * post-handshake chain + CN check.
 *
 * FD ownership: upgrade-NULL always means fd dead (esp-tls delete closes it;
 * raw paths close explicitly). network_tls_close() closes exactly once.
 */
#include "tls.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_tls.h"
#ifdef CONFIG_MBEDTLS_DEBUG
#include "mbedtls/esp_debug.h"
#endif
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509.h"

static const char *TAG = "TLS";

#ifdef HAS_DEV_CERTS
extern const char qesp_dev_ca_crt[]; /* reserved for round 2b */
extern const char qesp_dev_server_crt[];
extern const char qesp_dev_server_key[];
#endif

struct qesp_tls_session {
    int raw; /* 0 = esp-tls (2a), 1 = raw mbedTLS (2b mutual) */
    esp_tls_t *tls;
    mbedtls_ssl_context ssl;
    mbedtls_net_context net;
    char cn[64];
    int peer_ok; /* set by mutual_verify during the handshake */
};

static esp_tls_cfg_server_t s_cfg;
static int s_inited = 0;
static int s_available = 0;

/* Raw-mbedTLS mutual globals (2b). esp-tls never wires ca_chain server-side,
 * so the mutual path configures mbedTLS directly. */
static mbedtls_x509_crt s_ca;
static mbedtls_x509_crt s_server_crt;
static mbedtls_pk_context s_server_key;
static mbedtls_ssl_config s_conf_mutual;
static int s_mutual_ok = 0;

static void log_mbedtls(int rc, const char *what) {
    char eb[96];
    mbedtls_strerror(rc, eb, sizeof(eb));
    ESP_LOGW(TAG, "%s failed: -0x%04x %s", what, (unsigned)-rc, eb);
}

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

    /* Raw mutual stack (2b): esp-tls never wires ca_chain server-side, so a
     * cert-requesting server needs direct mbedTLS config (4.x public API).
     * OPTIONAL request + post-handshake chain + CN check = enforcement. */
    {
        int rc;
        mbedtls_x509_crt_init(&s_ca);
        mbedtls_x509_crt_init(&s_server_crt);
        mbedtls_pk_init(&s_server_key);
        mbedtls_ssl_config_init(&s_conf_mutual);
        rc = mbedtls_x509_crt_parse(&s_ca,
                                    (const unsigned char *)qesp_dev_ca_crt,
                                    strlen(qesp_dev_ca_crt) + 1);
        if (rc != 0) {
            log_mbedtls(rc, "mutual ca parse");
            return ESP_OK; /* 2a still usable */
        }
        rc = mbedtls_x509_crt_parse(&s_server_crt,
                                    (const unsigned char *)qesp_dev_server_crt,
                                    strlen(qesp_dev_server_crt) + 1);
        if (rc != 0) {
            log_mbedtls(rc, "mutual server cert parse");
            return ESP_OK;
        }
        rc = mbedtls_pk_parse_key(&s_server_key,
                                  (const unsigned char *)qesp_dev_server_key,
                                  strlen(qesp_dev_server_key) + 1, NULL, 0);
        if (rc != 0) {
            log_mbedtls(rc, "mutual server key parse");
            return ESP_OK;
        }
        rc = mbedtls_ssl_config_defaults(&s_conf_mutual, MBEDTLS_SSL_IS_SERVER,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc != 0) {
            log_mbedtls(rc, "mutual conf defaults");
            return ESP_OK;
        }
        mbedtls_ssl_conf_min_tls_version(&s_conf_mutual, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_authmode(&s_conf_mutual, MBEDTLS_SSL_VERIFY_OPTIONAL);
        mbedtls_ssl_conf_ca_chain(&s_conf_mutual, &s_ca, NULL);
        rc = mbedtls_ssl_conf_own_cert(&s_conf_mutual, &s_server_crt, &s_server_key);
        if (rc != 0) {
            log_mbedtls(rc, "mutual own cert");
            return ESP_OK;
        }
        s_mutual_ok = 1;
        ESP_LOGI(TAG, "mutual TLS ready (client CA wired)");
#ifdef CONFIG_MBEDTLS_DEBUG
        /* Temporary: verbose handshake trace until mutual auth is proven. */
        mbedtls_esp_enable_debug_log(&s_conf_mutual, 4);
#endif
    }
    return ESP_OK;
    return ESP_OK;
#else
    ESP_LOGW(TAG, "no dev certs embedded — TLS unavailable (fail-closed)");
    return ESP_ERR_NOT_FOUND;
#endif
}

int network_tls_available(void) {
    return s_inited && s_available;
}

/* Verify callback for round 2b (runs DURING the handshake while the peer
 * cert is alive — get_peer_cert() after the fact always returns NULL because
 * IDF disables MBEDTLS_SSL_KEEP_PEER_CERTIFICATE by default).
 * Mirrors reference CERT_VerifyCertName: chain must be clean and leaf CN must
 * equal the PREINIT cluster_name. Nonzero return aborts the handshake. */
static int mutual_verify(void *data, mbedtls_x509_crt *crt, int depth,
                         uint32_t *flags) {
    qesp_tls_session_t *s = (qesp_tls_session_t *)data;
    char dn[128];
    const char *cn;
    size_t exp_len;
    if (depth != 0) {
        return 0; /* chain anchors judged via leaf flags below */
    }
    if (*flags != 0) {
        ESP_LOGW(TAG, "client chain verify failed flags=0x%lx", (unsigned long)*flags);
        return 1;
    }
    if (mbedtls_x509_dn_gets(dn, sizeof(dn), &crt->subject) <= 0) {
        return 1;
    }
    cn = strstr(dn, "CN=");
    if (cn == NULL) {
        return 1;
    }
    cn += 3;
    exp_len = strlen(s->cn);
    if (exp_len == 0 || strncmp(cn, s->cn, exp_len) != 0 ||
        (cn[exp_len] != ',' && cn[exp_len] != '\0')) {
        ESP_LOGW(TAG, "client CN mismatch (dn=%s, want %s)", dn, s->cn);
        return 1;
    }
    ESP_LOGI(TAG, "client CN verified (%s)", s->cn);
    s->peer_ok = 1;
    return 0;
}

qesp_tls_session_t *network_tls_upgrade(int fd, const char *expected_cn,
                                        int require_client_cert) {
    qesp_tls_session_t *s;
    if (!network_tls_available()) {
        return NULL;
    }
    s = (qesp_tls_session_t *)malloc(sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    /* FD ownership contract: on NULL return the fd is ALWAYS consumed.
     * esp-tls delete closes it (production https_server usage); the raw
     * path closes it explicitly below. The caller must treat fd as dead. */
    if (!require_client_cert) {
        /* Round 2a path (proven): esp-tls server session, no client auth. */
        s->raw = 0;
        s->tls = esp_tls_init();
        if (s->tls == NULL) {
            close(fd);
            goto fail_free;
        }
        if (esp_tls_server_session_create(&s_cfg, fd, s->tls) != 0) {
            ESP_LOGW(TAG, "TLS handshake failed, closing");
            esp_tls_server_session_delete(s->tls); /* consumes fd */
            s->tls = NULL;
            goto fail_free;
        }
        ESP_LOGI(TAG, "TLS handshake done (server-only auth)");
        return s;
    }
    /* Round 2b path: raw mbedTLS with wired ca_chain (esp-tls never wires
     * it server-side, so clients never select a cert to send). */
    if (!s_mutual_ok) {
        ESP_LOGW(TAG, "mutual TLS not initialized");
        goto fail_raw;
    }
    if (expected_cn == NULL || expected_cn[0] == '\0' ||
        strlen(expected_cn) >= sizeof(s->cn)) {
        goto fail_raw;
    }
    memcpy(s->cn, expected_cn, strlen(expected_cn) + 1);
    s->raw = 1;
    s->tls = NULL;
    mbedtls_ssl_init(&s->ssl);
    mbedtls_net_init(&s->net);
    s->net.fd = fd; /* wrap accepted socket; closed below on failure */
    {
        int rc = mbedtls_ssl_setup(&s->ssl, &s_conf_mutual);
        if (rc != 0) {
            log_mbedtls(rc, "mutual ssl_setup");
            goto fail_raw;
        }
    }
    mbedtls_ssl_set_bio(&s->ssl, &s->net,
                        mbedtls_net_send, mbedtls_net_recv, NULL);
    /* In-handshake verify (mutual_verify): get_peer_cert() is useless here
     * because IDF disables KEEP_PEER_CERTIFICATE — the cert is gone after
     * the handshake. */
    mbedtls_ssl_set_verify(&s->ssl, mutual_verify, s);
    for (;;) {
        int rc = mbedtls_ssl_handshake(&s->ssl);
        if (rc == 0) {
            break;
        }
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            log_mbedtls(rc, "mutual handshake");
            goto fail_raw_ssl;
        }
    }
    if (!s->peer_ok || mbedtls_ssl_get_verify_result(&s->ssl) != 0) {
        ESP_LOGW(TAG, "client cert rejected, closing");
        goto fail_raw_ssl;
    }
    ESP_LOGI(TAG, "TLS handshake done (mutual)");
    return s;

fail_raw_ssl:
    mbedtls_ssl_free(&s->ssl);
    /* NOTE: no mbedtls_net_free() — it would close(fd); closed below. */
fail_raw:
    close(fd);
    /* fall through */
fail_free:
    free(s);
    return NULL;
}

/* Exact read with an overall deadline so a silent peer surfaces as a
 * timeout (-1) instead of blocking forever, and a dead one as -2.
 * WANT_READ from SO_RCVTIMEO expiry counts toward the deadline. */
#define TLS_IO_DEADLINE_MS 6000

static int raw_read_n(mbedtls_ssl_context *ssl, uint8_t *buf, size_t len) {
    size_t got = 0;
    int64_t start = esp_timer_get_time();
    while (got < len) {
        int r = mbedtls_ssl_read(ssl, buf + got, len - got);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (esp_timer_get_time() - start > (int64_t)TLS_IO_DEADLINE_MS * 1000) {
                return -1;
            }
            continue;
        }
        if (r == 0) {
            return -2; /* clean EOF */
        }
        if (r < 0) {
            log_mbedtls(r, "mutual ssl_read");
            return -2;
        }
        got += (size_t)r;
    }
    return 0;
}

int network_tls_read(qesp_tls_session_t *s, uint8_t *buf, size_t len) {
    size_t got = 0;
    int64_t start;
    if (s == NULL) {
        return -2;
    }
    if (s->raw) {
        return raw_read_n(&s->ssl, buf, len);
    }
    start = esp_timer_get_time();
    while (got < len) {
        ssize_t r = esp_tls_conn_read(s->tls, (char *)buf + got, len - got);
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) {
            if (esp_timer_get_time() - start > (int64_t)TLS_IO_DEADLINE_MS * 1000) {
                return -1;
            }
            continue;
        }
        if (r == 0) {
            return -2; /* clean EOF */
        }
        if (r < 0) {
            return -2;
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
    if (s->raw) {
        while (sent < len) {
            int r = mbedtls_ssl_write(&s->ssl, buf + sent, len - sent);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            if (r <= 0) {
                log_mbedtls(r, "mutual ssl_write");
                return -1;
            }
            sent += (size_t)r;
        }
        return 0;
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
    if (s->raw) {
        int fd = s->net.fd;
        mbedtls_ssl_close_notify(&s->ssl);
        mbedtls_ssl_free(&s->ssl);
        /* No mbedtls_net_free(): close the wrapped fd exactly once, here. */
        if (fd >= 0) {
            close(fd);
        }
        free(s);
        return;
    }
    /* esp-tls delete owns the fd (production https_server usage). */
    esp_tls_server_session_delete(s->tls);
    free(s);
}
