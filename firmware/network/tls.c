/* TLS server via raw mbedTLS. See tls.h + docs/tls.md. */
#include "tls.h"

#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/asn1.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "TLS";

#ifdef HAS_DEV_CERTS
extern const char qesp_dev_ca_crt[];
extern const char qesp_dev_server_crt[];
extern const char qesp_dev_server_key[];
#endif

struct qesp_tls_session {
    mbedtls_ssl_context ssl;
    mbedtls_net_context net;
    char cn[64];
};

static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static mbedtls_x509_crt s_ca;
static mbedtls_x509_crt s_server_crt;
static mbedtls_pk_context s_server_key;
static mbedtls_ssl_config s_conf_plain; /* 2a: no client auth */
static mbedtls_ssl_config s_conf_mutual; /* 2b: VERIFY_REQUIRED + ca chain */
static int s_inited = 0;
static int s_available = 0;

static void log_mbedtls(int rc, const char *what) {
    char eb[96];
    mbedtls_strerror(rc, eb, sizeof(eb));
    ESP_LOGW(TAG, "%s failed: -0x%04x %s", what, (unsigned)-rc, eb);
}

esp_err_t network_tls_init(void) {
    int rc;
    if (s_inited) {
        return s_available ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    s_inited = 1;
#ifdef HAS_DEV_CERTS
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);
    mbedtls_x509_crt_init(&s_ca);
    mbedtls_x509_crt_init(&s_server_crt);
    mbedtls_pk_init(&s_server_key);
    mbedtls_ssl_config_init(&s_conf_plain);
    mbedtls_ssl_config_init(&s_conf_mutual);

    rc = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                               (const unsigned char *)"quorumesp", 9);
    if (rc != 0) {
        log_mbedtls(rc, "drbg_seed");
        return ESP_FAIL;
    }
    rc = mbedtls_x509_crt_parse(&s_ca, (const unsigned char *)qesp_dev_ca_crt,
                                strlen(qesp_dev_ca_crt) + 1);
    if (rc != 0) {
        log_mbedtls(rc, "ca parse");
        return ESP_FAIL;
    }
    rc = mbedtls_x509_crt_parse(&s_server_crt,
                                (const unsigned char *)qesp_dev_server_crt,
                                strlen(qesp_dev_server_crt) + 1);
    if (rc != 0) {
        log_mbedtls(rc, "server cert parse");
        return ESP_FAIL;
    }
    rc = mbedtls_pk_parse_key(&s_server_key,
                              (const unsigned char *)qesp_dev_server_key,
                              strlen(qesp_dev_server_key) + 1, NULL, 0, NULL, NULL);
    if (rc != 0) {
        log_mbedtls(rc, "server key parse");
        return ESP_FAIL;
    }
    rc = mbedtls_ssl_config_defaults(&s_conf_plain, MBEDTLS_SSL_IS_SERVER,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        log_mbedtls(rc, "conf plain defaults");
        return ESP_FAIL;
    }
    mbedtls_ssl_conf_min_version(&s_conf_plain, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_authmode(&s_conf_plain, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s_conf_plain, mbedtls_ctr_drbg_random, &s_drbg);
    /* mbedtls_ssl_conf_own_cert() is deprecated in favor of ..._own_cert(); both exist. */
    rc = mbedtls_ssl_conf_own_cert(&s_conf_plain, &s_server_crt, &s_server_key);
    if (rc != 0) {
        log_mbedtls(rc, "conf own cert");
        return ESP_FAIL;
    }
    /* Mutual-auth config shares everything, adds chain + REQUIRED. */
    s_conf_mutual = s_conf_plain;
    mbedtls_ssl_conf_authmode(&s_conf_mutual, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&s_conf_mutual, &s_ca, NULL);

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

/* Verify callback for round 2b: chain must be clean and leaf CN must equal
 * the cluster_name from PREINIT (mirrors CERT_VerifyCertName). */
static int verify_cn(void *data, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    const char *exp = (const char *)data;
    const mbedtls_asn1_named_data *cn;
    if (*flags != 0) {
        return 1;
    }
    if (depth == 0 && exp != NULL) {
        cn = mbedtls_asn1_find_named_data(&crt->subject, "CN", 2);
        if (cn == NULL || cn->val.len != strlen(exp) ||
            memcmp(cn->val.p, exp, cn->val.len) != 0) {
            ESP_LOGW(TAG, "client CN mismatch");
            return 1;
        }
    }
    return 0;
}

qesp_tls_session_t *network_tls_upgrade(int fd, const char *expected_cn,
                                        int require_client_cert) {
    qesp_tls_session_t *s;
    int rc;
    if (!network_tls_available()) {
        return NULL;
    }
    s = (qesp_tls_session_t *)heap_caps_malloc(sizeof(*s), MALLOC_CAP_8BIT);
    if (s == NULL) {
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    if (require_client_cert) {
        size_t n;
        if (expected_cn == NULL) {
            goto fail;
        }
        n = strlen(expected_cn);
        if (n == 0 || n >= sizeof(s->cn)) {
            goto fail;
        }
        memcpy(s->cn, expected_cn, n + 1);
    }
    mbedtls_ssl_init(&s->ssl);
    mbedtls_net_init(&s->net);
    s->net.fd = fd; /* wrap the accepted socket; we keep owning it */
    rc = mbedtls_ssl_setup(&s->ssl,
                           require_client_cert ? &s_conf_mutual : &s_conf_plain);
    if (rc != 0) {
        log_mbedtls(rc, "ssl_setup");
        goto fail_ssl;
    }
    if (require_client_cert) {
        mbedtls_ssl_set_verify(&s->ssl, verify_cn, s->cn);
    }
    mbedtls_ssl_set_bio(&s->ssl, &s->net,
                        mbedtls_net_send, mbedtls_net_recv, NULL);
    while ((rc = mbedtls_ssl_handshake(&s->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            log_mbedtls(rc, "handshake");
            goto fail_ssl;
        }
    }
    ESP_LOGI(TAG, "TLS handshake done (%s)",
             require_client_cert ? "mutual" : "server-only");
    return s;

fail_ssl:
    mbedtls_ssl_free(&s->ssl);
fail:
    heap_caps_free(s);
    return NULL;
}

int network_tls_read(qesp_tls_session_t *s, uint8_t *buf, size_t len) {
    size_t got = 0;
    if (s == NULL) {
        return -1;
    }
    while (got < len) {
        int r = mbedtls_ssl_read(&s->ssl, buf + got, len - got);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (r <= 0) {
            if (r != 0) {
                log_mbedtls(r, "ssl_read");
            }
            return -1;
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
        int r = mbedtls_ssl_write(&s->ssl, buf + sent, len - sent);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (r <= 0) {
            log_mbedtls(r, "ssl_write");
            return -1;
        }
        sent += (size_t)r;
    }
    return 0;
}

void network_tls_close(qesp_tls_session_t *s) {
    int fd;
    if (s == NULL) {
        return;
    }
    fd = s->net.fd;
    mbedtls_ssl_close_notify(&s->ssl);
    mbedtls_ssl_free(&s->ssl);
    /* NOTE: no mbedtls_net_free() — it would close(fd); we own the fd. */
    heap_caps_free(s);
    if (fd >= 0) {
        close(fd);
    }
}
