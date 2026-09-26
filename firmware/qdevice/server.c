/* qnetd-side TCP server: multi-client FFSplit + LMS (reference-faithful).
 *
 * Roles mirror qnetd + qnetd-algo-{ffsplit,lms}.c, one algorithm per cluster
 * (first INIT decides; mismatch refused with ALGORITHM_DIFFERS):
 * - FFSplit: replies carry decision STATUS; votes travel by server-pushed
 *   VOTE_INFO, NACKs before ACKs, sequenced by VOTE_INFO_REPLY matching.
 *   ASK_FOR_VOTE is unsupported (reference behavior).
 * - LMS: votes ride IN replies (membership/quorum/ask_for_vote); no push
 *   sequencing. Waiting clients are recomputed on every cluster event
 *   (timer-equivalent for the reference algo timer). Heuristics changes
 *   are ignored (reference).
 * - Single-cluster dev scope (8 vote-table slots / 32 nodes, 2 concurrent
 *   sessions on classic ESP32 RAM): a second cluster_name is refused.
 * - Advertised max message 32768 (reference minimum); bigger frames are
 *   drained and answered MESSAGE_TOO_LONG. Bounded, honest RAM budget.
 * - Task watchdog fed every loop wake (recv timeout 5s < WDT 10s).
 */
#include "server.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "watchdog.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ffsplit.h"
#include "lms.h"
#include "config.h"
#include "msg.h"
#include "tls.h"

static const char *TAG = "QDEVICE";
static const char *PTAG = "PROTOCOL";
static const char *STAG = "STATE";

/* Session budget (ESP32 classic RAM, measured live 2026-09-25).
 * Per TLS session steady-state ~= 35-47K (8K stack + ~25K TLS heap +
 * LWIP); the 4th concurrent TLS handshake needs ~50K free, so the heap
 * floor (Kconfig, default 40K) refuses instead of crashing. Proven:
 * 4 concurrent ACTIVE with correct votes (FOUR 4/4); 5th refuses
 * naturally below the floor, client retries.
 * RX honors the reference 32K minimum a server must accept (qnet-config.h);
 * the real client aborts otherwise (observed: "Server accepts maximum 4096
 * bytes message but this client minimum is 32768 bytes").
 *
 * Squeezing (see docs/bench.md): RX/TX are SINGLE shared stash buffers
 * (all processing is serialized by s_mux), TLS I/O is tuned down via
 * sdkconfig, and admission is self-assessed (heap floor below) instead of
 * a hard lock — the ceiling only caps the slot tables. */
#define QESP_RX_SIZE QESP_INITIAL_MSG_SIZE
#define QESP_TX_SIZE 2048
/* 8K: measured high-water leaves 12K free of 16K (peak incl. RSA
 * handshake uses ~4K). Keeps 4K margin; the end-of-session log proves
 * it every run (stack=...). */
#define QESP_SESSION_STACK 8192

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
    int ff_idx;      /* slot in s_ff, -1 = none */
    int has_config;
    int has_memb;
    uint8_t heur;
    uint8_t kap;
    tp_t tp_ref;     /* current transport snapshot for push routing */
} sess_t;

/* ---- Shared cluster state (guarded by s_mux) ---- */
static SemaphoreHandle_t s_mux;
static qesp_ff_cluster_t s_ff;
static char s_cluster[64];
static int s_have_cluster;
static uint16_t s_algo; /* cluster algorithm, 0 = unset (first INIT decides) */
static int s_decided; /* last FFSplit decide() outcome */
/* Per-slot push state: 0 none, 1 nack-due, 2 ack-due, 3 nack-pending, 4 ack-pending */
static uint8_t s_st[QESP_FF_MAX_CLIENTS];
static uint32_t s_seq[QESP_FF_MAX_CLIENTS];
static uint8_t s_lms_last[QESP_FF_MAX_CLIENTS]; /* saved LMS votes */
static tp_t s_tp[QESP_FF_MAX_CLIENTS];
static int s_phase; /* 0 idle, 1 sending nacks, 2 sending acks */
static int s_nsessions;
/* Own socket accounting: LWIP gives no enumeration API. Incremented on
 * every accepted fd, decremented wherever that fd is closed. If accept
 * later fails ENFILE while this stays low, the leak is outside qdevice. */
static int s_nsocks;
static portMUX_TYPE s_sock_spin = portMUX_INITIALIZER_UNLOCKED;

/* Shared message stash (see budget note above): every recv/process/send
 * runs under s_mux, so one RX + one TX buffer serves all sessions. */
static uint8_t s_rx[QESP_RX_SIZE];
static uint8_t s_tx[QESP_TX_SIZE];

static int64_t now_us(void) {
    return esp_timer_get_time();
}

/* Close without LWIP TIME_WAIT: with test churn (storm/probes) the 60s
 * MSL TIME_WAIT per closed socket exhausts the small PCB pool and new
 * SYNs get RST with zero logs (observed live). RST-on-close is fine here:
 * every session is fail-closed and short-lived by design. */
static void tune_servant_socket(int fd) {
    struct linger lg = {.l_onoff = 1, .l_linger = 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
}

static void sock_opened(void) {
    portENTER_CRITICAL(&s_sock_spin);
    s_nsocks++;
    portEXIT_CRITICAL(&s_sock_spin);
}

static void sock_closed(void) {
    portENTER_CRITICAL(&s_sock_spin);
    if (s_nsocks > 0) {
        s_nsocks--;
    }
    portEXIT_CRITICAL(&s_sock_spin);
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

/* Build INIT_REPLY (also used with error codes, like the reference).
 * Advertises what this firmware implements (FFSplit + LMS). */
static size_t init_reply(uint8_t *tx, size_t cap, const qesp_msg_t *m, uint16_t code) {
    static const uint16_t algos[] = {QESP_ALGO_FFSPLIT, QESP_ALGO_LMS};
    qesp_buf_t b;
    qesp_buf_init(&b, tx, cap);
    if (qesp_msg_init_reply(&b, m->has_seq, m->seq, code,
                            QESP_RX_SIZE, QESP_TX_SIZE,
                            algos, 2) != QESP_OK) {
        return 0;
    }
    return b.len;
}

/* Upgrade return code: caller must wrap the fd in TLS. */
#define UPGRADE_REQ 2

/* Push one VOTE_INFO to slot (mutex held). Returns 1 if sent. */
static int push_vote(int slot, uint8_t vote, uint8_t *scratch, size_t scratch_cap) {
    qesp_ff_client_t *cl = &s_ff.clients[slot];
    qesp_buf_t b;
    qesp_buf_init(&b, scratch, scratch_cap);
    s_seq[slot]++;
    if (qesp_msg_vote_info(&b, s_seq[slot], cl->ring_node, cl->ring_seq,
                           vote) != QESP_OK) {
        return 0;
    }
    if (send_all(&s_tp[slot], b.buf, b.len) != 0) {
        return 0;
    }
    ESP_LOGI(TAG, "VOTE_INFO node=%lu vote=%u seq=%lu",
             (unsigned long)cl->node_id, vote, (unsigned long)s_seq[slot]);
    return 1;
}

/* Send all due NACKs (mutex held). Returns count sent. */
static int drain_nacks(uint8_t *scratch, size_t cap) {
    int i, sent = 0;
    for (i = 0; i < QESP_FF_MAX_CLIENTS; i++) {
        if (s_ff.clients[i].used && s_st[i] == 1) {
            if (push_vote(i, QESP_VOTE_NACK, scratch, cap)) {
                s_st[i] = 3;
                sent++;
            }
        }
    }
    return sent;
}

static int drain_acks(uint8_t *scratch, size_t cap) {
    int i, sent = 0;
    for (i = 0; i < QESP_FF_MAX_CLIENTS; i++) {
        if (s_ff.clients[i].used && s_st[i] == 2) {
            if (push_vote(i, QESP_VOTE_ACK, scratch, cap)) {
                s_st[i] = 4;
                sent++;
            }
        }
    }
    return sent;
}

static int any_state(uint8_t a, uint8_t b) {
    int i;
    for (i = 0; i < QESP_FF_MAX_CLIENTS; i++) {
        if (s_ff.clients[i].used && (s_st[i] == a || s_st[i] == b)) {
            return 1;
        }
    }
    return 0;
}

/* Recompute desired votes and push NACKs (mutex held). ACKs follow once
 * NACKs are acknowledged (see on_vote_info_reply). */
static void recompute(uint8_t *scratch, size_t cap) {
    uint8_t votes[QESP_FF_MAX_CLIENTS];
    int i;
    for (i = 0; i < QESP_FF_MAX_CLIENTS; i++) {
        votes[i] = QESP_VOTE_NO_CHANGE;
    }
    if (!qesp_ff_decide(&s_ff, votes)) {
        s_decided = 0; /* WAIT_FOR_REPLY: keep existing push states */
        return;
    }
    s_decided = 1;
    for (i = 0; i < QESP_FF_MAX_CLIENTS; i++) {
        if (s_ff.clients[i].used) {
            s_st[i] = (votes[i] == QESP_VOTE_ACK) ? 2 : 1;
        }
    }
    if (drain_nacks(scratch, cap) > 0) {
        s_phase = 1;
    } else if (drain_acks(scratch, cap) > 0) {
        s_phase = 2;
    } else {
        s_phase = 0;
    }
}

/* VOTE_INFO_REPLY from slot: match expected seq, advance NACK->ACK (mutex). */
static void on_vote_info_reply(int slot, uint32_t seq, uint8_t *scratch, size_t cap) {
    if (s_seq[slot] != seq || (s_st[slot] != 3 && s_st[slot] != 4)) {
        ESP_LOGI(TAG, "stale vote_info_reply node=%lu seq=%lu",
                 (unsigned long)s_ff.clients[slot].node_id, (unsigned long)seq);
        return; /* old reply: ignore like the reference */
    }
    s_st[slot] = 0;
    if (s_phase == 1 && !any_state(1, 3)) {
        if (drain_acks(scratch, cap) > 0) {
            s_phase = 2;
        } else {
            s_phase = 0;
        }
    } else if (s_phase == 2 && !any_state(2, 4)) {
        s_phase = 0;
    }
}

/* LMS timer-equivalent: the reference re-runs waiting clients on a timer;
 * here every cluster event re-runs them synchronously (same effect: no
 * waiter stalls forever while peers keep reporting). Decided votes are
 * pushed via VOTE_INFO. Mutex held. */
static void lms_refresh_others(int exclude, uint8_t *scratch, size_t cap) {
    int i;
    if (s_algo != QESP_ALGO_LMS) {
        return;
    }
    for (i = 0; i < QESP_FF_MAX_CLIENTS; i++) {
        uint8_t v;
        if (!s_ff.clients[i].used || i == exclude) {
            continue;
        }
        if (s_lms_last[i] != QESP_VOTE_WAIT_FOR_REPLY) {
            continue;
        }
        v = qesp_lms_decide(&s_ff, i, s_lms_last);
        if (v == QESP_VOTE_ACK || v == QESP_VOTE_NACK) {
            if (push_vote(i, v, scratch, cap)) {
                ESP_LOGI(TAG, "LMS refresh pushed node=%lu vote=%u",
                         (unsigned long)s_ff.clients[i].node_id, v);
            }
        }
    }
}

/* Handle one validated frame. Returns: >0 reply bytes, 0 no reply, -1 drop.
 * Sends for OTHER sessions happen inside (mutex held by caller). */
static int on_frame(sess_t *s, const uint8_t *f, size_t flen, size_t rxcap,
                    uint8_t *tx, size_t txcap, size_t *txlen) {
    qesp_msg_t m;
    int rc = qesp_msg_decode(f, flen, rxcap, &m);
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
        if (s_have_cluster && strncmp(s_cluster, m.cluster, m.cluster_len) != 0) {
            ESP_LOGW(STAG, "second cluster refused (single-cluster dev scope)");
            return -1;
        }
        if (!s_have_cluster) {
            memcpy(s_cluster, m.cluster, m.cluster_len);
            s_cluster[m.cluster_len] = '\0';
            s_have_cluster = 1;
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
        if (s_algo != 0 && s_algo != m.algorithm) {
            /* One algorithm per cluster (reference: ALGORITHM_DIFFERS). */
            *txlen = init_reply(tx, txcap, &m,
                                QESP_E_ALGORITHM_DIFFERS_FROM_OTHER_NODES);
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
        {
            int idx = qesp_ff_slot(&s_ff, s->node);
            if (idx < 0) {
                ESP_LOGE(TAG, "cluster table full");
                return -1;
            }
            s->ff_idx = idx;
            s_ff.clients[idx].tb_mode = m.tie_mode;
            s_ff.clients[idx].tb_node = m.tie_node;
            s_ff.clients[idx].heur = QESP_HEUR_UNDEFINED;
            s_tp[idx] = s->tp_ref;
            /* Fresh slot state (slots are reused across connections). */
            s_st[idx] = 0;
            s_seq[idx] = 0;
            s_lms_last[idx] = QESP_LMS_NEW;
            s->has_config = 0;
            s->has_memb = 0;
            s->heur = QESP_HEUR_UNDEFINED;
            s->kap = 0;
            if (s_algo == 0) {
                s_algo = m.algorithm;
                ESP_LOGI(TAG, "cluster algorithm: %s",
                         s_algo == QESP_ALGO_LMS ? "lms" : "ffsplit");
            }
        }
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
        uint8_t status;
        qesp_buf_t b;
        qesp_ff_client_t *cl;
        size_t k;
        if (!m.has_seq || !m.has_list_type || !s->has_ring || s->ff_idx < 0) {
            *txlen = err_reply(tx, txcap, QESP_E_DOESNT_CONTAIN_REQUIRED_OPTION, &m);
            return *txlen > 0 ? 0 : -1;
        }
        cl = &s_ff.clients[s->ff_idx];
        if (s_algo == QESP_ALGO_LMS) {
            /* LMS: votes ride IN replies (no push sequencing). Config lists
             * are counted, never decided (reference). */
            uint8_t vote;
            if (m.list_type == QESP_NL_QUORUM) {
                vote = qesp_lms_decide(&s_ff, s->ff_idx, s_lms_last);
            } else if (m.list_type == QESP_NL_MEMBERSHIP) {
                if (m.n_nodes == 0) {
                    *txlen = err_reply(tx, txcap,
                                       QESP_E_INVALID_MEMBERSHIP_NODE_LIST, &m);
                    return *txlen > 0 ? 0 : -1;
                }
                {
                    int self = 0;
                    for (k = 0; k < m.n_nodes; k++) {
                        if (m.nodes[k].node_id == s->node) {
                            self = 1;
                            break;
                        }
                    }
                    if (!self) {
                        *txlen = err_reply(tx, txcap,
                                           QESP_E_INVALID_MEMBERSHIP_NODE_LIST,
                                           &m);
                        return *txlen > 0 ? 0 : -1;
                    }
                }
                for (k = 0; k < m.n_nodes; k++) {
                    cl->memb[k] = m.nodes[k].node_id;
                }
                cl->nmemb = m.n_nodes;
                if (m.has_ring) {
                    cl->ring_node = m.ring_node;
                    cl->ring_seq = m.ring_seq;
                    cl->has_ring = 1;
                    rn = m.ring_node;
                    rs = m.ring_seq;
                    s->last_rn = rn;
                    s->last_rs = rs;
                }
                if (m.has_heur) {
                    cl->heur = m.heur;
                    s->heur = m.heur;
                }
                s->has_memb = 1;
                vote = qesp_lms_decide(&s_ff, s->ff_idx, s_lms_last);
                lms_refresh_others(s->ff_idx, tx, txcap);
            } else {
                vote = QESP_VOTE_NO_CHANGE;
            }
            qesp_buf_init(&b, tx, txcap);
            if (qesp_msg_node_list_reply(&b, m.seq, m.list_type, rn, rs,
                                         vote) != QESP_OK) {
                return -1;
            }
            *txlen = b.len;
            return 0;
        }
        if (m.list_type == QESP_NL_QUORUM) {
            /* Informative only (reference): no state change. */
            status = QESP_VOTE_NO_CHANGE;
        } else {
            /* INITIAL_CONFIG / CHANGED_CONFIG / MEMBERSHIP need node entries. */
            if (m.n_nodes == 0) {
                uint16_t code = (m.list_type == QESP_NL_MEMBERSHIP) ?
                    QESP_E_INVALID_MEMBERSHIP_NODE_LIST :
                    QESP_E_INVALID_CONFIG_NODE_LIST;
                *txlen = err_reply(tx, txcap, code, &m);
                return *txlen > 0 ? 0 : -1;
            }
            {
                int self = 0;
                for (k = 0; k < m.n_nodes; k++) {
                    if (m.nodes[k].node_id == s->node) {
                        self = 1;
                        break;
                    }
                }
                if (!self) {
                    uint16_t code = (m.list_type == QESP_NL_MEMBERSHIP) ?
                        QESP_E_INVALID_MEMBERSHIP_NODE_LIST :
                        QESP_E_INVALID_CONFIG_NODE_LIST;
                    *txlen = err_reply(tx, txcap, code, &m);
                    return *txlen > 0 ? 0 : -1;
                }
            }
            if (m.list_type == QESP_NL_MEMBERSHIP) {
                for (k = 0; k < m.n_nodes; k++) {
                    cl->memb[k] = m.nodes[k].node_id;
                }
                cl->nmemb = m.n_nodes;
                if (m.has_ring) {
                    cl->ring_node = m.ring_node;
                    cl->ring_seq = m.ring_seq;
                    cl->has_ring = 1;
                    rn = m.ring_node;
                    rs = m.ring_seq;
                    s->last_rn = rn;
                    s->last_rs = rs;
                }
                if (m.has_heur) {
                    cl->heur = m.heur;
                    s->heur = m.heur;
                }
                s->has_memb = 1;
            } else {
                for (k = 0; k < m.n_nodes; k++) {
                    cl->cfg[k] = m.nodes[k].node_id;
                }
                cl->ncfg = m.n_nodes;
                s->has_config = 1;
            }
            if (!s->has_config || !s->has_memb) {
                status = QESP_VOTE_ASK_LATER;
            } else {
                recompute(tx, txcap);
                status = s_decided ? QESP_VOTE_NO_CHANGE : QESP_VOTE_WAIT_FOR_REPLY;
            }
        }
        qesp_buf_init(&b, tx, txcap);
        /* NOTE: vote field carries decision STATUS (reference model). */
        if (qesp_msg_node_list_reply(&b, m.seq, m.list_type, rn, rs,
                                     status) != QESP_OK) {
            return -1;
        }
        *txlen = b.len;
        return 0;
    }
    case QESP_MSG_ASK_FOR_VOTE: {
        qesp_buf_t b;
        uint8_t vote;
        if (!m.has_seq || s->ff_idx < 0) {
            *txlen = err_reply(tx, txcap, QESP_E_DOESNT_CONTAIN_REQUIRED_OPTION, &m);
            return *txlen > 0 ? 0 : -1;
        }
        if (s_algo != QESP_ALGO_LMS) {
            /* FFSplit has no ask-for-vote (reference: unsupported message). */
            *txlen = err_reply(tx, txcap,
                               QESP_E_UNSUPPORTED_DECISION_ALGORITHM_MESSAGE, &m);
            return *txlen > 0 ? 0 : -1;
        }
        vote = qesp_lms_decide(&s_ff, s->ff_idx, s_lms_last);
        lms_refresh_others(s->ff_idx, tx, txcap);
        qesp_buf_init(&b, tx, txcap);
        if (qesp_msg_ask_for_vote_reply(&b, m.seq, s->last_rn, s->last_rs,
                                        vote) != QESP_OK) {
            return -1;
        }
        *txlen = b.len;
        return 0;
    }
    case QESP_MSG_HEURISTICS_CHANGE: {
        qesp_buf_t b;
        uint8_t status;
        if (!m.has_seq || !m.has_heur ||
            (m.heur != QESP_HEUR_PASS && m.heur != QESP_HEUR_FAIL) ||
            s->ff_idx < 0) {
            *txlen = err_reply(tx, txcap, QESP_E_DOESNT_CONTAIN_REQUIRED_OPTION, &m);
            return *txlen > 0 ? 0 : -1;
        }
        if (s_algo == QESP_ALGO_LMS) {
            /* LMS ignores heuristics changes (reference): no state change. */
            status = QESP_VOTE_NO_CHANGE;
        } else {
            s_ff.clients[s->ff_idx].heur = m.heur;
            s->heur = m.heur;
            if (!s->has_config || !s->has_memb) {
                status = QESP_VOTE_ASK_LATER;
            } else {
                recompute(tx, txcap);
                status = s_decided ? QESP_VOTE_NO_CHANGE : QESP_VOTE_WAIT_FOR_REPLY;
            }
        }
        qesp_buf_init(&b, tx, txcap);
        if (qesp_msg_heuristics_change_reply(&b, m.seq, s->last_rn, s->last_rs,
                                             m.heur, status) != QESP_OK) {
            return -1;
        }
        *txlen = b.len;
        return 0;
    }
    case QESP_MSG_SET_OPTION: {
        qesp_buf_t b;
        qesp_buf_init(&b, tx, txcap);
        if (m.has_kap && s->ff_idx >= 0) {
            s_ff.clients[s->ff_idx].kap = m.kap ? 1 : 0;
            s->kap = m.kap;
        }
        if (qesp_msg_set_option_reply(&b, m.has_seq, m.seq, m.has_heartbeat,
                                      m.heartbeat, m.has_kap, m.kap) != QESP_OK) {
            return -1;
        }
        *txlen = b.len;
        return 0;
    }
    case QESP_MSG_VOTE_INFO_REPLY:
        /* FFSplit: advance the NACK->ACK push sequencing. LMS needs no
         * tracking (votes ride in replies; reference is a no-op here). */
        if (s_algo == QESP_ALGO_FFSPLIT && s->ff_idx >= 0 && m.has_seq) {
            on_vote_info_reply(s->ff_idx, m.seq, tx, txcap);
        }
        return 1; /* ack, nothing to send back */
    default:
        ESP_LOGW(PTAG, "unexpected type %d in ACTIVE", m.type);
        *txlen = err_reply(tx, txcap, QESP_E_UNEXPECTED_MESSAGE, &m);
        return *txlen > 0 ? 0 : -1;
    }
}

/* Read exactly len bytes over either transport.
 * Returns 0 ok, -1 timeout/retryable, -2 dead (close now). */
static int tp_recv(tp_t *tp, uint8_t *p, size_t len) {
    if (tp->tls != NULL) {
        return network_tls_read(tp->tls, p, len);
    }
    while (len > 0) {
        int n = recv(tp->fd, p, len, 0);
        if (n == 0) {
            return -2; /* peer closed */
        }
        if (n < 0) {
            return -1; /* timeout (EAGAIN) or error: caller decides */
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Read exactly one frame (header first, then body). 0 ok, -1 timeout, -2 dead.
 * NOTE: the header is validated for type/size ONLY here. qesp_msg_check()
 * must not be used on a header-only buffer — it also demands the body and
 * would wrongly report TRUNC for every non-empty message. Oversize frames
 * are drained and answered MESSAGE_TOO_LONG by the caller. */
static int recv_frame(tp_t *tp, uint8_t *rx, size_t cap, size_t *flen) {
    uint16_t type;
    uint32_t plen;
    int rc = tp_recv(tp, rx, QESP_MSG_HEADER_LEN);
    if (rc != 0) {
        return rc; /* -1 timeout, -2 dead */
    }
    if (qesp_msg_check_header(rx, cap, &type, &plen) != QESP_OK) {
        ESP_LOGW(PTAG, "bad header (type/size)");
        return -2; /* framing violation: never retry a broken stream */
    }
    rc = tp_recv(tp, rx + QESP_MSG_HEADER_LEN, plen);
    if (rc != 0) {
        return rc;
    }
    *flen = QESP_MSG_HEADER_LEN + plen;
    return 0;
}

/* Drain a frame bigger than our stash, then the caller answers
 * MESSAGE_TOO_LONG (fail-closed, connection stays up). */
static int drain_frame(tp_t *tp, uint32_t plen) {
    uint8_t tmp[256];
    size_t left = plen;
    while (left > 0) {
        size_t want = left > sizeof(tmp) ? sizeof(tmp) : left;
        int rc = tp_recv(tp, tmp, want);
        if (rc != 0) {
            return -1;
        }
        left -= want;
    }
    return 0;
}

static void tp_close(tp_t *tp) {
    if (tp->tls != NULL) {
        network_tls_close(tp->tls); /* also closes the fd */
        tp->tls = NULL;
        tp->fd = -1;
        sock_closed();
    } else if (tp->fd >= 0) {
        close(tp->fd);
        tp->fd = -1;
        sock_closed();
    }
}

static void session_task(void *arg) {
    tp_t tp;
    sess_t s;
    /* Idle recv holds s_mux (shared stash): keep turns short so N idle
     * sessions stall a newcomer by ~N x 250ms, never seconds. DPD (12s)
     * and 8s heartbeats tolerate this with wide margin. */
    struct timeval tv = {.tv_sec = 0, .tv_usec = 250000};
    struct timeval snd_tv = {.tv_sec = 5, .tv_usec = 0};
    tp.fd = *(int *)arg;
    tp.tls = NULL;
    free(arg);
    memset(&s, 0, sizeof(s));
    s.st = ST_CONNECTED;
    s.ff_idx = -1;
    s.last_rx_us = now_us();
    s.tp_ref = tp;
    setsockopt(tp.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(tp.fd, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));
    quorumesp_watchdog_add_current();
    ESP_LOGI(TAG, "client session start");
    for (;;) {
        size_t flen = 0;
        size_t txlen = 0;
        int r;
        int fr;
        quorumesp_watchdog_feed();
        /* Whole turn under one mux hold (shared stash): every exit path
         * below must give before break/continue. */
        xSemaphoreTake(s_mux, portMAX_DELAY);
        fr = recv_frame(&tp, s_rx, QESP_RX_SIZE, &flen);
        if (fr == -2) {
            xSemaphoreGive(s_mux);
            break; /* dead transport or broken framing: close now */
        }
        if (fr == -3) {
            /* Oversize for our stash: drain, answer MESSAGE_TOO_LONG,
             * stay connected (fail-closed, bounded RAM). */
            uint32_t drain = (uint32_t)(flen - QESP_MSG_HEADER_LEN);
            if (drain_frame(&tp, drain) == 0) {
                size_t tl = err_reply(s_tx, QESP_TX_SIZE,
                                      QESP_E_MESSAGE_TOO_LONG,
                                      &(qesp_msg_t){0});
                if (tl > 0) {
                    send_all(&tp, s_tx, tl);
                }
            }
            xSemaphoreGive(s_mux);
            continue;
        }
        if (fr != 0) {
            /* Timeout: dead-peer check before giving up (DPD-style). */
            if (s.st == ST_ACTIVE && s.hb_ms > 0 &&
                now_us() - s.last_rx_us > (int64_t)s.hb_ms * 1500) {
                ESP_LOGW(TAG, "dead peer, closing");
                xSemaphoreGive(s_mux);
                break;
            }
            if (s.st == ST_ACTIVE) {
                xSemaphoreGive(s_mux);
                continue; /* idle but alive: keep waiting */
            }
            xSemaphoreGive(s_mux);
            break; /* handshake must be prompt */
        }
        r = on_frame(&s, s_rx, flen, QESP_RX_SIZE, s_tx, QESP_TX_SIZE, &txlen);
        if (r == UPGRADE_REQ) {
            /* require flag from Kconfig; cluster CN for the 2b check.
             * The handshake blocks on the network: NEVER hold s_mux
             * across it, or one slow peer stalls every session + accept.
             * sess_t is task-local and slots are only taken at INIT, so
             * re-taking the mutex after the handshake is safe. */
            int need_cc =
#if CONFIG_QUORUMESP_REQUIRE_CLIENT_CERT
                1;
#else
                0;
#endif
            xSemaphoreGive(s_mux);
            tp.tls = network_tls_upgrade(tp.fd, s.cluster, need_cc);
            xSemaphoreTake(s_mux, portMAX_DELAY);
            if (tp.tls == NULL) {
                ESP_LOGW(TAG, "TLS upgrade failed, closing");
                tp.fd = -1; /* consumed by upgrade (all failure paths) */
                sock_closed(); /* fd died inside the TLS layer, not tp_close */
                xSemaphoreGive(s_mux);
                break;
            }
            s.tls_upgraded = 1;
            s.tp_ref = tp;
            ESP_LOGI(STAG, "transport upgraded to TLS, waiting for INIT");
            xSemaphoreGive(s_mux);
            continue;
        }
        if (r < 0) {
            xSemaphoreGive(s_mux);
            break;
        }
        if (r == 0 && txlen > 0 && send_all(&tp, s_tx, txlen) != 0) {
            xSemaphoreGive(s_mux);
            break;
        }
        xSemaphoreGive(s_mux);
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s.ff_idx >= 0) {
        ESP_LOGI(TAG, "client node=%lu left, recomputing",
                 (unsigned long)s.node);
        qesp_ff_remove(&s_ff, s.ff_idx);
        s_tp[s.ff_idx].fd = -1;
        s_tp[s.ff_idx].tls = NULL;
        s_lms_last[s.ff_idx] = QESP_LMS_NEW;
        if (s_algo == QESP_ALGO_FFSPLIT) {
            recompute(s_tx, QESP_TX_SIZE);
        } else if (s_algo == QESP_ALGO_LMS) {
            /* Reference runs no recompute here; refresh waiters instead
             * (timer-equivalent, see lms_refresh_others). */
            lms_refresh_others(-1, s_tx, QESP_TX_SIZE);
        }
        s.ff_idx = -1;
        /* Last client left: drop cluster state like the reference frees
         * algorithm_data, so a later cluster starts fresh (any algorithm). */
        {
            int any = 0, k;
            for (k = 0; k < QESP_FF_MAX_CLIENTS; k++) {
                if (s_ff.clients[k].used) {
                    any = 1;
                    break;
                }
            }
            if (!any) {
                s_algo = 0;
                s_ff.nquorate = 0;
                s_decided = 0;
                s_phase = 0;
            }
        }
    }
    s_nsessions--;
    xSemaphoreGive(s_mux);
    tp_close(&tp);
    {
        /* Event-driven status (no periodic heartbeat by design): sessions
         * + sockets + heap + task stack watermark exactly when a session
         * comes or goes. Read AFTER tp_close so socks is exact. */
        int sk;
        unsigned hwm;
        portENTER_CRITICAL(&s_sock_spin);
        sk = s_nsocks;
        portEXIT_CRITICAL(&s_sock_spin);
        hwm = (unsigned)uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(TAG, "client session end (sessions=%d socks=%d heap=%u stack=%u)",
                 s_nsessions, sk, (unsigned)esp_get_free_heap_size(), hwm);
    }
    quorumesp_watchdog_remove_current();
    vTaskDelete(NULL);
}

static void server_task(void *arg) {
    int lfd;
    int opt = 1;
    int nrun = 0;
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
    addr.sin_port = htons(quorumesp_config_get()->qdevice_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(lfd, 4) != 0) {
        ESP_LOGE(TAG, "bind/listen failed");
        close(lfd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "qnetd-side listening on port %d (FFSplit+LMS, lwip_socks=%d)",
             (int)quorumesp_config_get()->qdevice_port, CONFIG_LWIP_MAX_SOCKETS);
    quorumesp_watchdog_add_current();
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        fd_set rfds;
        struct timeval sel_tv = {.tv_sec = 5, .tv_usec = 0};
        int *pfd;
        int cfd;
        quorumesp_watchdog_feed();
        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        if (select(lfd + 1, &rfds, NULL, NULL, &sel_tv) <= 0) {
            continue; /* quiet idle: status only prints on events below */
        }
        cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) {
            ESP_LOGW(TAG, "accept failed errno=%d heap=%u (pcb/socket exhaustion?)",
                     errno, (unsigned)esp_get_free_heap_size());
            continue;
        }
        tune_servant_socket(cfd);
        sock_opened();
        {
            /* Self-assessed admission (no hard lock): admit only under the
             * slot ceiling AND above the heap floor. Either refusal is
             * fail-closed and logged distinctly; the client retries. The
             * heap read races concurrent tasks by design — worst case a
             * later malloc fails, and every one of those paths already
             * fails closed with a clear log. */
            uint32_t heap = esp_get_free_heap_size();
            int admit = 0;
            xSemaphoreTake(s_mux, portMAX_DELAY);
            nrun = s_nsessions;
            if (nrun < CONFIG_QUORUMESP_MAX_SESSIONS &&
                heap > CONFIG_QUORUMESP_MIN_HEAP_ACCEPT) {
                s_nsessions++;
                admit = 1;
            }
            xSemaphoreGive(s_mux);
            if (!admit) {
                if (nrun >= CONFIG_QUORUMESP_MAX_SESSIONS) {
                    ESP_LOGW(TAG, "session table full (%d), refusing client",
                             nrun);
                } else {
                    ESP_LOGW(TAG, "low heap (%u), refusing client", (unsigned)heap);
                }
                close(cfd);
                sock_closed();
                continue;
            }
        }
        {
            esp_ip4_addr_t ip;
            ip.addr = peer.sin_addr.s_addr;
            ESP_LOGI(TAG, "client " IPSTR " (sessions=%d heap=%u)", IP2STR(&ip),
                     nrun + 1, (unsigned)esp_get_free_heap_size());
        }
        pfd = (int *)malloc(sizeof(int));
        if (pfd == NULL) {
            xSemaphoreTake(s_mux, portMAX_DELAY);
            s_nsessions--;
            xSemaphoreGive(s_mux);
            close(cfd);
            sock_closed();
            continue;
        }
        *pfd = cfd;
        if (xTaskCreate(session_task, "qdev", QESP_SESSION_STACK,
                        pfd, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "spawn session failed");
            xSemaphoreTake(s_mux, portMAX_DELAY);
            s_nsessions--;
            xSemaphoreGive(s_mux);
            close(cfd);
            sock_closed();
            free(pfd);
            continue;
        }
    }
}

esp_err_t qdevice_server_start(void) {
    s_mux = xSemaphoreCreateMutex();
    if (s_mux == NULL) {
        return ESP_FAIL;
    }
    qesp_ff_init(&s_ff);
    if (xTaskCreate(server_task, "qnetd", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void qdevice_status_snapshot(qdev_status_t *out) {
    uint8_t *p = (uint8_t *)out;
    size_t i;
    for (i = 0; i < sizeof(*out); i++) {
        p[i] = 0;
    }
    out->uptime_s = (uint32_t)(now_us() / 1000000);
    out->algo = (uint8_t)s_algo;
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(200)) != pdTRUE) {
        return; /* best effort: partial/empty snapshot, never block */
    }
    if (s_have_cluster) {
        strncpy(out->cluster, s_cluster, sizeof(out->cluster) - 1);
    }
    for (i = 0; i < QESP_FF_MAX_CLIENTS && out->n < QDEV_SNAP_MAX; i++) {
        qesp_ff_client_t *cl = &s_ff.clients[i];
        qdev_cli_snap_t *dst;
        if (!cl->used) {
            continue;
        }
        dst = &out->cli[out->n++];
        dst->node = cl->node_id;
        dst->algo = (uint8_t)s_algo;
        dst->state = 2; /* registered = past handshake */
        dst->tls = (s_tp[i].tls != NULL) ? 1 : 0;
        if (s_algo == QESP_ALGO_LMS) {
            dst->vote = s_lms_last[i];
        } else {
            dst->vote = qesp_ff_in_quorate(&s_ff, cl->node_id) ?
                        QESP_VOTE_ACK : QESP_VOTE_NACK;
        }
    }
    xSemaphoreGive(s_mux);
}
