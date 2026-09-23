/* qnetd-side TCP server. Session logic ports host/src/session.js
 * (tlsMode off, fixed ACK vote) onto the C codec. Static buffers only.
 */
#include "server.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "msg.h"
#include "tls.h"

static const char *TAG = "QDEVICE";
static const char *PTAG = "PROTOCOL";
static const char *STAG = "STATE";

/* RX must hold the largest receivable frame (initial 32K, qnet-config.h). */
static uint8_t s_rx[QESP_INITIAL_MSG_SIZE];
static uint8_t s_tx[4096];

typedef enum { ST_CONNECTED, ST_PREINIT_DONE, ST_ACTIVE, ST_CLOSED } st_t;

/* Transport: plaintext fd, or TLS session after STARTTLS (owns the fd). */
typedef struct {
    int fd;
    qesp_tls_session_t *tls;
} tp_t;

typedef struct {
    st_t st;
    uint32_t node;
    uint16_t algo;
    uint32_t hb_ms;
    uint32_t init_rn;
    uint64_t init_rs;
    uint32_t last_rn;
    uint64_t last_rs;
    int has_ring;
    int64_t last_rx_us;
    int tls_upgraded;
    char cluster[64];
} sess_t;

static int64_t now_us(void) {
    return esp_timer_get_time();
}

/* send exactly len bytes or fail */
static int send_all(tp_t *tp, const uint8_t *p, size_t len) {
    if (tp->tls != NULL) {
        return network_tls_write(tp->tls, p, len);
    }
    while (len > 0) {
        int n = send(tp->fd, p, len, 0);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Build a SERVER_ERROR reply. Returns tx length (0 = OOM, fatal). */
static size_t err_reply(uint8_t *tx, size_t cap, uint16_t code, const qesp_msg_t *m) {
    qesp_buf_t b;
    qesp_buf_init(&b, tx, cap);
    if (qesp_msg_server_error(&b, code, m->has_seq, m->seq) != QESP_OK) {
        return 0;
    }
    return b.len;
}

/* Build INIT_REPLY (also used with error codes, like the reference). */
static size_t init_reply(uint8_t *tx, size_t cap, const qesp_msg_t *m, uint16_t code) {
    qesp_buf_t b;
    qesp_buf_init(&b, tx, cap);
    if (qesp_msg_init_reply(&b, m->has_seq, m->seq, code,
                            QESP_INITIAL_MSG_SIZE, QESP_INITIAL_MSG_SIZE) != QESP_OK) {
        return 0;
    }
    return b.len;
}

/* Upgrade return code: caller must wrap the fd in TLS. */
#define UPGRADE_REQ 2

/* Handle one validated frame. Returns: >0 reply bytes, 0 no reply, -1 drop. */
static int on_frame(sess_t *s, const uint8_t *f, size_t flen,
                    uint8_t *tx, size_t txcap, size_t *txlen) {
    qesp_msg_t m;
    int rc = qesp_msg_decode(f, flen, sizeof(s_rx), &m);
    *txlen = 0;
    if (rc != QESP_OK) {
        ESP_LOGW(PTAG, "decode failed rc=%d", rc);
        *txlen = err_reply(tx, txcap, QESP_E_ERROR_DECODING_MSG, &(qesp_msg_t){0});
        return *txlen > 0 ? 0 : -1;
    }
    s->last_rx_us = now_us();

    if (s->st == ST_CONNECTED) {
        if (m.type != QESP_MSG_PREINIT || m.cluster == NULL || m.cluster_len == 0 ||
            m.cluster_len >= sizeof(s->cluster)) {
            ESP_LOGW(STAG, "expected PREINIT, got type=%d", m.type);
            *txlen = err_reply(tx, txcap, m.type == QESP_MSG_PREINIT ?
                               QESP_E_DOESNT_CONTAIN_REQUIRED_OPTION :
                               QESP_E_PREINIT_REQUIRED, &m);
            return *txlen > 0 ? 0 : -1;
        }
        memcpy(s->cluster, m.cluster, m.cluster_len);
        s->cluster[m.cluster_len] = '\0';
        {
            qesp_buf_t b;
            uint8_t tls_mode = network_tls_available() ?
                               QESP_TLS_SUPPORTED : QESP_TLS_UNSUPPORTED;
            uint8_t cert_req =
#if CONFIG_QUORUMESP_REQUIRE_CLIENT_CERT
                1;
#else
                0;
#endif
            qesp_buf_init(&b, tx, txcap);
            if (qesp_msg_preinit_reply(&b, tls_mode, cert_req,
                                       m.has_seq, m.seq) != QESP_OK) {
                return -1;
            }
            *txlen = b.len;
        }
        s->st = ST_PREINIT_DONE;
        ESP_LOGI(STAG, "CONNECTED -> PREINIT_DONE");
        return 0;
    }

    if (s->st == ST_PREINIT_DONE) {
        if (m.type == QESP_MSG_STARTTLS) {
            if (!network_tls_available()) {
                ESP_LOGW(STAG, "STARTTLS rejected (no certs in build)");
                *txlen = err_reply(tx, txcap, QESP_E_UNSUPPORTED_MESSAGE, &m);
                return *txlen > 0 ? 0 : -1;
            }
            /* Silent upgrade: the caller wraps the fd, then INIT arrives
             * inside TLS (reference: no STARTTLS reply exists). */
            return UPGRADE_REQ;
        }
        if (m.type != QESP_MSG_INIT) {
            *txlen = err_reply(tx, txcap, QESP_E_INIT_REQUIRED, &m);
            return *txlen > 0 ? 0 : -1;
        }
        /* Validate like the reference: node, algo, heartbeat, tie, ring. */
        if (!m.has_node_id || m.node_id == 0 ||
            !m.has_tie || !m.has_ring) {
            *txlen = init_reply(tx, txcap, &m, QESP_E_DOESNT_CONTAIN_REQUIRED_OPTION);
            return *txlen > 0 ? 0 : -1;
        }
        if (!m.has_algorithm ||
            (m.algorithm != QESP_ALGO_FFSPLIT && m.algorithm != QESP_ALGO_LMS)) {
            *txlen = init_reply(tx, txcap, &m, QESP_E_UNSUPPORTED_DECISION_ALGORITHM);
            return *txlen > 0 ? 0 : -1;
        }
        if (!m.has_heartbeat ||
            m.heartbeat < QESP_HEARTBEAT_MIN || m.heartbeat > QESP_HEARTBEAT_MAX) {
            *txlen = init_reply(tx, txcap, &m, QESP_E_INVALID_HEARTBEAT_INTERVAL);
            return *txlen > 0 ? 0 : -1;
        }
        s->node = m.node_id;
        s->algo = m.algorithm;
        s->hb_ms = m.heartbeat;
        s->init_rn = m.ring_node;
        s->init_rs = m.ring_seq;
        s->last_rn = m.ring_node;
        s->last_rs = m.ring_seq;
        s->has_ring = 1;
        s->st = ST_ACTIVE;
        ESP_LOGI(STAG, "PREINIT_DONE -> ACTIVE node=%lu algo=%u hb=%lu",
                 (unsigned long)s->node, s->algo, (unsigned long)s->hb_ms);
        *txlen = init_reply(tx, txcap, &m, QESP_E_NO_ERROR);
        return *txlen > 0 ? 0 : -1;
    }

    /* ST_ACTIVE */
    switch (m.type) {
    case QESP_MSG_ECHO_REQUEST: {
        qesp_buf_t b;
        qesp_buf_init(&b, tx, txcap);
        if (qesp_msg_echo_reply_from(&b, f, flen) != QESP_OK) {
            return -1;
        }
        *txlen = b.len;
        return 0;
    }
    case QESP_MSG_NODE_LIST: {
        uint32_t rn = s->last_rn;
        uint64_t rs = s->last_rs;
        qesp_buf_t b;
        if (!m.has_seq || !m.has_list_type || !s->has_ring) {
            *txlen = err_reply(tx, txcap, QESP_E_DOESNT_CONTAIN_REQUIRED_OPTION, &m);
            return *txlen > 0 ? 0 : -1;
        }
        if (m.has_ring) {
            rn = m.ring_node;
            rs = m.ring_seq;
            s->last_rn = rn;
            s->last_rs = rs;
        }
        qesp_buf_init(&b, tx, txcap);
        if (qesp_msg_node_list_reply(&b, m.seq, m.list_type, rn, rs,
                                     QESP_VOTE_ACK) != QESP_OK) {
            return -1;
        }
        *txlen = b.len;
        return 0;
    }
    case QESP_MSG_ASK_FOR_VOTE: {
        qesp_buf_t b;
        if (!m.has_seq) {
            *txlen = err_reply(tx, txcap, QESP_E_DOESNT_CONTAIN_REQUIRED_OPTION, &m);
            return *txlen > 0 ? 0 : -1;
        }
        qesp_buf_init(&b, tx, txcap);
        if (qesp_msg_ask_for_vote_reply(&b, m.seq, s->node, 0,
                                        QESP_VOTE_ACK) != QESP_OK) {
            return -1;
        }
        *txlen = b.len;
        return 0;
    }
    case QESP_MSG_HEURISTICS_CHANGE: {
        qesp_buf_t b;
        if (!m.has_seq || !m.has_heur ||
            (m.heur != QESP_HEUR_PASS && m.heur != QESP_HEUR_FAIL)) {
            *txlen = err_reply(tx, txcap, QESP_E_DOESNT_CONTAIN_REQUIRED_OPTION, &m);
            return *txlen > 0 ? 0 : -1;
        }
        qesp_buf_init(&b, tx, txcap);
        if (qesp_msg_heuristics_change_reply(&b, m.seq, s->node, 0,
                                             m.heur, QESP_VOTE_ACK) != QESP_OK) {
            return -1;
        }
        *txlen = b.len;
        return 0;
    }
    case QESP_MSG_SET_OPTION: {
        qesp_buf_t b;
        qesp_buf_init(&b, tx, txcap);
        if (qesp_msg_set_option_reply(&b, m.has_seq, m.seq, m.has_heartbeat,
                                      m.heartbeat, m.has_kap, m.kap) != QESP_OK) {
            return -1;
        }
        *txlen = b.len;
        return 0;
    }
    case QESP_MSG_VOTE_INFO_REPLY:
        return 1; /* ack, nothing to send */
    default:
        ESP_LOGW(PTAG, "unexpected type %d in ACTIVE", m.type);
        *txlen = err_reply(tx, txcap, QESP_E_UNEXPECTED_MESSAGE, &m);
        return *txlen > 0 ? 0 : -1;
    }
}

/* Read exactly len bytes over either transport, or fail. */
static int tp_recv(tp_t *tp, uint8_t *p, size_t len) {
    if (tp->tls != NULL) {
        return network_tls_read(tp->tls, p, len);
    }
    while (len > 0) {
        int n = recv(tp->fd, p, len, 0);
        if (n == 0) {
            return -1; /* peer closed */
        }
        if (n < 0) {
            return -1; /* timeout (EAGAIN) or error: caller decides */
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Read exactly one frame (header first, then body). 0 ok, -1 drop/timeout-eof.
 * NOTE: the header is validated for type/size ONLY here. qesp_msg_check()
 * must not be used on a header-only buffer — it also demands the body and
 * would wrongly report TRUNC for every non-empty message. */
static int recv_frame(tp_t *tp, uint8_t *rx, size_t cap, size_t *flen) {
    uint16_t type;
    uint32_t plen;
    if (tp_recv(tp, rx, QESP_MSG_HEADER_LEN) != 0) {
        return -1;
    }
    if (qesp_msg_check_header(rx, cap, &type, &plen) != QESP_OK) {
        ESP_LOGW(PTAG, "bad header (type/size)");
        return -1;
    }
    if (tp_recv(tp, rx + QESP_MSG_HEADER_LEN, plen) != 0) {
        return -1;
    }
    *flen = QESP_MSG_HEADER_LEN + plen;
    return 0;
}

static void tp_close(tp_t *tp) {
    if (tp->tls != NULL) {
        network_tls_close(tp->tls); /* also closes the fd */
        tp->tls = NULL;
        tp->fd = -1;
    } else if (tp->fd >= 0) {
        close(tp->fd);
        tp->fd = -1;
    }
}

static void serve_client(int fd) {
    sess_t s;
    tp_t tp;
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    memset(&s, 0, sizeof(s));
    s.st = ST_CONNECTED;
    s.last_rx_us = now_us();
    tp.fd = fd;
    tp.tls = NULL;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(TAG, "client session start");
    for (;;) {
        size_t flen = 0;
        size_t txlen = 0;
        int r;
        if (recv_frame(&tp, s_rx, sizeof(s_rx), &flen) != 0) {
            /* Timeout: dead-peer check before giving up (DPD-style). */
            if (s.st == ST_ACTIVE && s.hb_ms > 0 &&
                now_us() - s.last_rx_us > (int64_t)s.hb_ms * 1500) {
                ESP_LOGW(TAG, "dead peer, closing");
                break;
            }
            if (s.st == ST_ACTIVE) {
                continue; /* idle but alive: keep waiting */
            }
            break; /* handshake must be prompt */
        }
        r = on_frame(&s, s_rx, flen, s_tx, sizeof(s_tx), &txlen);
        if (r == UPGRADE_REQ) {
            /* require flag from Kconfig; cluster CN for the 2b check.
             * Upgrade consumes the fd on failure — mark dead so tp_close
             * (which closes a live fd, or a TLS session owning it) stays
             * exactly-once. */
            int need_cc =
#if CONFIG_QUORUMESP_REQUIRE_CLIENT_CERT
                1;
#else
                0;
#endif
            tp.tls = network_tls_upgrade(tp.fd, s.cluster, need_cc);
            if (tp.tls == NULL) {
                ESP_LOGW(TAG, "TLS upgrade failed, closing");
                tp.fd = -1; /* consumed by upgrade (all failure paths) */
                break;
            }
            s.tls_upgraded = 1;
            ESP_LOGI(STAG, "transport upgraded to TLS, waiting for INIT");
            continue;
        }
        if (r < 0) {
            break;
        }
        if (r == 0 && txlen > 0 && send_all(&tp, s_tx, txlen) != 0) {
            break;
        }
    }
    ESP_LOGI(TAG, "client session end");
    tp_close(&tp);
}

static void server_task(void *arg) {
    int lfd;
    int opt = 1;
    struct sockaddr_in addr;
    (void)arg;
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONFIG_QUORUMESP_SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(lfd, 4) != 0) {
        ESP_LOGE(TAG, "bind/listen failed");
        close(lfd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "qnetd-side listening on port %d (TEST STUB vote=ACK)",
             CONFIG_QUORUMESP_SERVER_PORT);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) {
            continue;
        }
        {
            esp_ip4_addr_t ip;
            ip.addr = peer.sin_addr.s_addr;
            ESP_LOGI(TAG, "client " IPSTR, IP2STR(&ip));
        }
        serve_client(cfd); /* transport (fd or TLS) closed inside */
    }
}

esp_err_t qdevice_server_start(void) {
    if (xTaskCreate(server_task, "qnetd", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
