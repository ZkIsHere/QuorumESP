/* QuorumESP — message framing implementation. Port of qdevices/msg.c. */
#include "msg.h"

/* Supported sets advertised in INIT/INIT_REPLY (mirrors the static tables). */
static const uint16_t SUP_MSGS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                                    12, 13, 14, 15, 16, 17};
static const uint16_t SUP_OPTS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                    13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
static const uint16_t SUP_ALGOS[] = {QESP_ALGO_FFSPLIT, QESP_ALGO_LMS};

static int add_seq(qesp_buf_t *b, int has_seq, uint32_t seq) {
    if (!has_seq) {
        return QESP_OK;
    }
    return qesp_tlv_add_u32(b, QESP_TLV_MSG_SEQ_NUMBER, seq);
}

int qesp_msg_begin(qesp_buf_t *b, uint16_t type) {
    uint8_t hdr[QESP_MSG_HEADER_LEN];
    if (type > QESP_MSG_TYPE_MAX) {
        return QESP_ERR_RANGE;
    }
    hdr[0] = (uint8_t)(type >> 8);
    hdr[1] = (uint8_t)(type & 0xff);
    hdr[2] = hdr[3] = hdr[4] = hdr[5] = 0; /* length backfilled by end() */
    if (b->len + sizeof(hdr) > b->cap) {
        return QESP_ERR_NOMEM;
    }
    for (size_t i = 0; i < sizeof(hdr); i++) {
        b->buf[b->len++] = hdr[i];
    }
    return QESP_OK;
}

int qesp_msg_end(qesp_buf_t *b) {
    uint32_t plen;
    if (b->len < QESP_MSG_HEADER_LEN) {
        return QESP_ERR_INVAL;
    }
    plen = (uint32_t)(b->len - QESP_MSG_HEADER_LEN);
    b->buf[2] = (uint8_t)(plen >> 24);
    b->buf[3] = (uint8_t)((plen >> 16) & 0xff);
    b->buf[4] = (uint8_t)((plen >> 8) & 0xff);
    b->buf[5] = (uint8_t)(plen & 0xff);
    return QESP_OK;
}

int qesp_msg_check_header(const uint8_t *hdr6, size_t cap,
                          uint16_t *type, uint32_t *plen) {
    uint16_t t;
    uint32_t l;
    if (hdr6 == NULL) {
        return QESP_ERR_INVAL;
    }
    t = (uint16_t)(((uint16_t)hdr6[0] << 8) | hdr6[1]);
    l = ((uint32_t)hdr6[2] << 24) | ((uint32_t)hdr6[3] << 16) |
        ((uint32_t)hdr6[4] << 8) | hdr6[5];
    if (t > QESP_MSG_TYPE_MAX) {
        return QESP_ERR_RANGE;
    }
    if ((size_t)QESP_MSG_HEADER_LEN + l > cap) {
        return QESP_ERR_NOMEM;
    }
    if (type != NULL) {
        *type = t;
    }
    if (plen != NULL) {
        *plen = l;
    }
    return QESP_OK;
}

int qesp_msg_check(const uint8_t *frame, size_t flen, size_t max,
                   uint16_t *type, uint32_t *plen) {
    uint16_t t;
    uint32_t l;
    int rc;
    if (frame == NULL || flen < QESP_MSG_HEADER_LEN) {
        return QESP_ERR_TRUNC;
    }
    rc = qesp_msg_check_header(frame, max, &t, &l);
    if (rc != QESP_OK) {
        return rc;
    }
    if (flen < (size_t)QESP_MSG_HEADER_LEN + l) {
        return QESP_ERR_TRUNC;
    }
    if (type != NULL) {
        *type = t;
    }
    if (plen != NULL) {
        *plen = l;
    }
    return QESP_OK;
}

#define BEGIN_OR_RETURN(b, t)                           \
    do {                                                \
        int _r = qesp_msg_begin((b), (t));              \
        if (_r != QESP_OK) {                            \
            return _r;                                  \
        }                                               \
    } while (0)

#define CALL_OR_RETURN(expr)                            \
    do {                                                \
        int _r = (expr);                                \
        if (_r != QESP_OK) {                            \
            return _r;                                  \
        }                                               \
    } while (0)

#define END_OR_RETURN(b)                                \
    do {                                                \
        int _r = qesp_msg_end((b));                     \
        if (_r != QESP_OK) {                            \
            return _r;                                  \
        }                                               \
    } while (0)

int qesp_msg_preinit(qesp_buf_t *b, const char *cluster, int has_seq, uint32_t seq) {
    BEGIN_OR_RETURN(b, QESP_MSG_PREINIT);
    CALL_OR_RETURN(add_seq(b, has_seq, seq));
    CALL_OR_RETURN(qesp_tlv_add_str(b, QESP_TLV_CLUSTER_NAME, cluster));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_preinit_reply(qesp_buf_t *b, uint8_t tls, uint8_t cert_req,
                           int has_seq, uint32_t seq) {
    BEGIN_OR_RETURN(b, QESP_MSG_PREINIT_REPLY);
    CALL_OR_RETURN(add_seq(b, has_seq, seq));
    CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_TLS_SUPPORTED, tls));
    CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_TLS_CLIENT_CERT_REQUIRED, cert_req));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_starttls(qesp_buf_t *b, int has_seq, uint32_t seq) {
    BEGIN_OR_RETURN(b, QESP_MSG_STARTTLS);
    CALL_OR_RETURN(add_seq(b, has_seq, seq));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_init(qesp_buf_t *b, int has_seq, uint32_t seq, uint16_t algorithm,
                  uint32_t node_id, uint32_t heartbeat,
                  uint8_t tb_mode, uint32_t tb_node,
                  uint32_t ring_node, uint64_t ring_seq) {
    BEGIN_OR_RETURN(b, QESP_MSG_INIT);
    CALL_OR_RETURN(add_seq(b, has_seq, seq));
    CALL_OR_RETURN(qesp_tlv_add_u16_array(b, QESP_TLV_SUPPORTED_MESSAGES,
                                         SUP_MSGS, sizeof(SUP_MSGS) / sizeof(SUP_MSGS[0])));
    CALL_OR_RETURN(qesp_tlv_add_u16_array(b, QESP_TLV_SUPPORTED_OPTIONS,
                                         SUP_OPTS, sizeof(SUP_OPTS) / sizeof(SUP_OPTS[0])));
    CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_NODE_ID, node_id));
    CALL_OR_RETURN(qesp_tlv_add_u16(b, QESP_TLV_DECISION_ALGORITHM, algorithm));
    CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_HEARTBEAT_INTERVAL, heartbeat));
    CALL_OR_RETURN(qesp_tlv_add_tie_breaker(b, QESP_TLV_TIE_BREAKER, tb_mode, tb_node));
    CALL_OR_RETURN(qesp_tlv_add_ring_id(b, QESP_TLV_RING_ID, ring_node, ring_seq));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_init_reply(qesp_buf_t *b, int has_seq, uint32_t seq, uint16_t error_code,
                        uint32_t max_req, uint32_t max_rep) {
    BEGIN_OR_RETURN(b, QESP_MSG_INIT_REPLY);
    CALL_OR_RETURN(qesp_tlv_add_u16(b, QESP_TLV_REPLY_ERROR_CODE, error_code));
    CALL_OR_RETURN(qesp_tlv_add_u16_array(b, QESP_TLV_SUPPORTED_MESSAGES,
                                         SUP_MSGS, sizeof(SUP_MSGS) / sizeof(SUP_MSGS[0])));
    CALL_OR_RETURN(qesp_tlv_add_u16_array(b, QESP_TLV_SUPPORTED_OPTIONS,
                                         SUP_OPTS, sizeof(SUP_OPTS) / sizeof(SUP_OPTS[0])));
    CALL_OR_RETURN(add_seq(b, has_seq, seq));
    CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_SERVER_MAXIMUM_REQUEST_SIZE, max_req));
    CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_SERVER_MAXIMUM_REPLY_SIZE, max_rep));
    CALL_OR_RETURN(qesp_tlv_add_u16_array(b, QESP_TLV_SUPPORTED_DECISION_ALGORITHMS,
                                         SUP_ALGOS, sizeof(SUP_ALGOS) / sizeof(SUP_ALGOS[0])));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_server_error(qesp_buf_t *b, uint16_t error_code, int has_seq, uint32_t seq) {
    BEGIN_OR_RETURN(b, QESP_MSG_SERVER_ERROR);
    CALL_OR_RETURN(add_seq(b, has_seq, seq));
    CALL_OR_RETURN(qesp_tlv_add_u16(b, QESP_TLV_REPLY_ERROR_CODE, error_code));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_echo_request(qesp_buf_t *b, int has_seq, uint32_t seq) {
    BEGIN_OR_RETURN(b, QESP_MSG_ECHO_REQUEST);
    CALL_OR_RETURN(add_seq(b, has_seq, seq));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_echo_reply_from(qesp_buf_t *b, const uint8_t *req, size_t req_len) {
    uint16_t t;
    uint32_t l;
    size_t i;
    int rc = qesp_msg_check(req, req_len, req_len, &t, &l);
    if (rc != QESP_OK || t != QESP_MSG_ECHO_REQUEST) {
        return QESP_ERR_INVAL;
    }
    if (b->len + req_len > b->cap) {
        return QESP_ERR_NOMEM;
    }
    for (i = 0; i < req_len; i++) {
        b->buf[b->len++] = req[i];
    }
    b->buf[b->len - req_len] = (uint8_t)(QESP_MSG_ECHO_REPLY >> 8);
    b->buf[b->len - req_len + 1] = (uint8_t)(QESP_MSG_ECHO_REPLY & 0xff);
    return QESP_OK;
}

int qesp_msg_node_list(qesp_buf_t *b, uint32_t seq, uint8_t list_type,
                       int has_ring, uint32_t ring_node, uint64_t ring_seq,
                       int has_config, uint64_t config_version,
                       int has_quorate, uint8_t quorate,
                       int has_heur, uint8_t heur,
                       const qesp_node_t *nodes, size_t n_nodes) {
    size_t i;
    BEGIN_OR_RETURN(b, QESP_MSG_NODE_LIST);
    CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_MSG_SEQ_NUMBER, seq));
    CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_NODE_LIST_TYPE, list_type));
    if (has_ring) {
        CALL_OR_RETURN(qesp_tlv_add_ring_id(b, QESP_TLV_RING_ID, ring_node, ring_seq));
    }
    if (has_config) {
        CALL_OR_RETURN(qesp_tlv_add_u64(b, QESP_TLV_CONFIG_VERSION, config_version));
    }
    if (has_quorate) {
        CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_QUORATE, quorate));
    }
    for (i = 0; i < n_nodes; i++) {
        CALL_OR_RETURN(qesp_tlv_add_node_info(b, QESP_TLV_NODE_INFO,
                                             nodes[i].node_id, nodes[i].dc_id,
                                             nodes[i].has_dc, nodes[i].state,
                                             nodes[i].has_state));
    }
    if (has_heur && heur != QESP_HEUR_UNDEFINED) {
        CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_HEURISTICS, heur));
    }
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_node_list_reply(qesp_buf_t *b, uint32_t seq, uint8_t list_type,
                             uint32_t ring_node, uint64_t ring_seq, uint8_t vote) {
    BEGIN_OR_RETURN(b, QESP_MSG_NODE_LIST_REPLY);
    CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_MSG_SEQ_NUMBER, seq));
    CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_NODE_LIST_TYPE, list_type));
    CALL_OR_RETURN(qesp_tlv_add_ring_id(b, QESP_TLV_RING_ID, ring_node, ring_seq));
    CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_VOTE, vote));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_ask_for_vote(qesp_buf_t *b, uint32_t seq) {
    BEGIN_OR_RETURN(b, QESP_MSG_ASK_FOR_VOTE);
    CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_MSG_SEQ_NUMBER, seq));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_ask_for_vote_reply(qesp_buf_t *b, uint32_t seq,
                                uint32_t ring_node, uint64_t ring_seq, uint8_t vote) {
    BEGIN_OR_RETURN(b, QESP_MSG_ASK_FOR_VOTE_REPLY);
    CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_MSG_SEQ_NUMBER, seq));
    CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_VOTE, vote));
    CALL_OR_RETURN(qesp_tlv_add_ring_id(b, QESP_TLV_RING_ID, ring_node, ring_seq));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_vote_info(qesp_buf_t *b, uint32_t seq,
                       uint32_t ring_node, uint64_t ring_seq, uint8_t vote) {
    BEGIN_OR_RETURN(b, QESP_MSG_VOTE_INFO);
    CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_MSG_SEQ_NUMBER, seq));
    CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_VOTE, vote));
    CALL_OR_RETURN(qesp_tlv_add_ring_id(b, QESP_TLV_RING_ID, ring_node, ring_seq));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_vote_info_reply(qesp_buf_t *b, uint32_t seq) {
    BEGIN_OR_RETURN(b, QESP_MSG_VOTE_INFO_REPLY);
    CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_MSG_SEQ_NUMBER, seq));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_heuristics_change(qesp_buf_t *b, uint32_t seq, uint8_t heur) {
    BEGIN_OR_RETURN(b, QESP_MSG_HEURISTICS_CHANGE);
    CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_MSG_SEQ_NUMBER, seq));
    CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_HEURISTICS, heur));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_heuristics_change_reply(qesp_buf_t *b, uint32_t seq,
                                     uint32_t ring_node, uint64_t ring_seq,
                                     uint8_t heur, uint8_t vote) {
    BEGIN_OR_RETURN(b, QESP_MSG_HEURISTICS_CHANGE_REPLY);
    CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_MSG_SEQ_NUMBER, seq));
    CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_VOTE, vote));
    CALL_OR_RETURN(qesp_tlv_add_ring_id(b, QESP_TLV_RING_ID, ring_node, ring_seq));
    CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_HEURISTICS, heur));
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_set_option(qesp_buf_t *b, int has_seq, uint32_t seq,
                        int has_hb, uint32_t hb, int has_kap, uint8_t kap) {
    BEGIN_OR_RETURN(b, QESP_MSG_SET_OPTION);
    CALL_OR_RETURN(add_seq(b, has_seq, seq));
    if (has_hb) {
        CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_HEARTBEAT_INTERVAL, hb));
    }
    if (has_kap) {
        CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_KEEP_ACTIVE_PARTITION_TB, kap));
    }
    END_OR_RETURN(b);
    return QESP_OK;
}

int qesp_msg_set_option_reply(qesp_buf_t *b, int has_seq, uint32_t seq,
                              int has_hb, uint32_t hb, int has_kap, uint8_t kap) {
    BEGIN_OR_RETURN(b, QESP_MSG_SET_OPTION_REPLY);
    CALL_OR_RETURN(add_seq(b, has_seq, seq));
    if (has_hb) {
        CALL_OR_RETURN(qesp_tlv_add_u32(b, QESP_TLV_HEARTBEAT_INTERVAL, hb));
    }
    if (has_kap) {
        CALL_OR_RETURN(qesp_tlv_add_u8(b, QESP_TLV_KEEP_ACTIVE_PARTITION_TB, kap));
    }
    END_OR_RETURN(b);
    return QESP_OK;
}

/* ---- Decoder ---- */

static int check_u8_range(uint8_t v, const uint8_t *allowed, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (v == allowed[i]) {
            return QESP_OK;
        }
    }
    return QESP_ERR_RANGE;
}

static int decode_range(const qesp_tlv_iter_t *it, uint8_t *out,
                        const uint8_t *allowed, size_t n) {
    uint8_t v;
    int rc = qesp_tlv_val_u8(it, &v);
    if (rc != QESP_OK) {
        return rc;
    }
    rc = check_u8_range(v, allowed, n);
    if (rc != QESP_OK) {
        return rc;
    }
    *out = v;
    return QESP_OK;
}

static int decode_node_info(const qesp_tlv_iter_t *it, qesp_node_t *node) {
    qesp_tlv_iter_t inner;
    int r;
    node->node_id = 0;
    node->has_dc = 0;
    node->has_state = 0;
    node->dc_id = 0;
    node->state = QESP_NODE_STATE_NOT_SET;
    qesp_tlv_iter_init(&inner, it->val, it->vlen);
    while ((r = qesp_tlv_iter_next(&inner)) > 0) {
        if (inner.type == QESP_TLV_NODE_ID) {
            if (qesp_tlv_val_u32(&inner, &node->node_id) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
        } else if (inner.type == QESP_TLV_DATA_CENTER_ID) {
            if (qesp_tlv_val_u32(&inner, &node->dc_id) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            node->has_dc = 1;
        } else if (inner.type == QESP_TLV_NODE_STATE) {
            static const uint8_t ok[] = {QESP_NODE_STATE_MEMBER, QESP_NODE_STATE_DEAD,
                                         QESP_NODE_STATE_LEAVING};
            uint8_t st;
            if (decode_range(&inner, &st, ok, sizeof(ok)) != QESP_OK) {
                return QESP_ERR_RANGE;
            }
            node->state = st;
            node->has_state = 1;
        }
    }
    if (r != 0) {
        return r;
    }
    if (node->node_id == 0) {
        return QESP_ERR_INVAL;
    }
    return QESP_OK;
}

int qesp_msg_decode(const uint8_t *frame, size_t flen, size_t max, qesp_msg_t *out) {
    qesp_tlv_iter_t it;
    uint16_t type;
    uint32_t plen;
    int r, rc;
    static const uint8_t TLS_OK[] = {QESP_TLS_UNSUPPORTED, QESP_TLS_SUPPORTED, QESP_TLS_REQUIRED};
    static const uint8_t VOTE_OK[] = {QESP_VOTE_ACK, QESP_VOTE_NACK, QESP_VOTE_ASK_LATER,
                                      QESP_VOTE_WAIT_FOR_REPLY, QESP_VOTE_NO_CHANGE};
    static const uint8_t QUOR_OK[] = {QESP_QUORATE_INQUORATE, QESP_QUORATE_QUORATE};
    static const uint8_t NLT_OK[] = {QESP_NL_INITIAL_CONFIG, QESP_NL_CHANGED_CONFIG,
                                     QESP_NL_MEMBERSHIP, QESP_NL_QUORUM};
    static const uint8_t HEUR_OK[] = {QESP_HEUR_PASS, QESP_HEUR_FAIL};
    static const uint8_t KAP_OK[] = {QESP_KAP_DISABLED, QESP_KAP_ENABLED};

    if (out == NULL) {
        return QESP_ERR_INVAL;
    }
    rc = qesp_msg_check(frame, flen, max, &type, &plen);
    if (rc != QESP_OK) {
        return rc;
    }
    /* Zero the struct without memset (works on all targets). */
    {
        uint8_t *p = (uint8_t *)out;
        size_t i;
        for (i = 0; i < sizeof(*out); i++) {
            p[i] = 0;
        }
    }
    out->type = type;
    qesp_tlv_iter_init(&it, frame + QESP_MSG_HEADER_LEN, plen);
    while ((r = qesp_tlv_iter_next(&it)) > 0) {
        switch (it.type) {
        case QESP_TLV_MSG_SEQ_NUMBER:
            if (qesp_tlv_val_u32(&it, &out->seq) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            out->has_seq = 1;
            break;
        case QESP_TLV_CLUSTER_NAME:
            out->cluster = (const char *)it.val;
            out->cluster_len = it.vlen;
            break;
        case QESP_TLV_TLS_SUPPORTED: {
            uint8_t v;
            if (decode_range(&it, &v, TLS_OK, sizeof(TLS_OK)) != QESP_OK) {
                return QESP_ERR_RANGE;
            }
            out->tls_supported = v;
            out->has_tls = 1;
            break;
        }
        case QESP_TLV_TLS_CLIENT_CERT_REQUIRED:
            if (qesp_tlv_val_u8(&it, &out->cert_req) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            out->has_cert_req = 1;
            break;
        case QESP_TLV_SUPPORTED_MESSAGES:
            if (qesp_tlv_val_u16_array(&it, out->sup_msgs, QESP_MAX_MSG_LIST,
                                       &out->n_sup_msgs) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            break;
        case QESP_TLV_SUPPORTED_OPTIONS:
            if (qesp_tlv_val_u16_array(&it, out->sup_opts, QESP_MAX_OPT_LIST,
                                       &out->n_sup_opts) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            break;
        case QESP_TLV_REPLY_ERROR_CODE:
            if (qesp_tlv_val_u16(&it, &out->error_code) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            out->has_error = 1;
            break;
        case QESP_TLV_SERVER_MAXIMUM_REQUEST_SIZE:
            if (qesp_tlv_val_u32(&it, &out->max_req) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            out->has_max_req = 1;
            break;
        case QESP_TLV_SERVER_MAXIMUM_REPLY_SIZE:
            if (qesp_tlv_val_u32(&it, &out->max_rep) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            out->has_max_rep = 1;
            break;
        case QESP_TLV_NODE_ID:
            if (qesp_tlv_val_u32(&it, &out->node_id) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            out->has_node_id = 1;
            break;
        case QESP_TLV_SUPPORTED_DECISION_ALGORITHMS:
            if (qesp_tlv_val_u16_array(&it, out->sup_algos, QESP_MAX_ALGO_LIST,
                                       &out->n_sup_algos) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            break;
        case QESP_TLV_DECISION_ALGORITHM:
            if (qesp_tlv_val_u16(&it, &out->algorithm) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            out->has_algorithm = 1;
            break;
        case QESP_TLV_HEARTBEAT_INTERVAL:
            if (qesp_tlv_val_u32(&it, &out->heartbeat) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            out->has_heartbeat = 1;
            break;
        case QESP_TLV_RING_ID:
            if (qesp_tlv_val_ring_id(&it, &out->ring_node, &out->ring_seq) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            out->has_ring = 1;
            break;
        case QESP_TLV_CONFIG_VERSION:
            if (qesp_tlv_val_u64(&it, &out->config_version) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            out->has_config = 1;
            break;
        case QESP_TLV_DATA_CENTER_ID:
            if (qesp_tlv_val_u32(&it, &out->dc_id) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            out->has_dc_id = 1;
            break;
        case QESP_TLV_NODE_STATE: {
            static const uint8_t ok[] = {QESP_NODE_STATE_MEMBER, QESP_NODE_STATE_DEAD,
                                         QESP_NODE_STATE_LEAVING};
            uint8_t v;
            if (decode_range(&it, &v, ok, sizeof(ok)) != QESP_OK) {
                return QESP_ERR_RANGE;
            }
            break;
        }
        case QESP_TLV_NODE_INFO:
            if (out->n_nodes >= QESP_MAX_NODES) {
                return QESP_ERR_NOMEM;
            }
            if (decode_node_info(&it, &out->nodes[out->n_nodes]) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            out->n_nodes++;
            break;
        case QESP_TLV_NODE_LIST_TYPE: {
            uint8_t v;
            if (decode_range(&it, &v, NLT_OK, sizeof(NLT_OK)) != QESP_OK) {
                return QESP_ERR_RANGE;
            }
            out->list_type = v;
            out->has_list_type = 1;
            break;
        }
        case QESP_TLV_VOTE: {
            uint8_t v;
            if (decode_range(&it, &v, VOTE_OK, sizeof(VOTE_OK)) != QESP_OK) {
                return QESP_ERR_RANGE;
            }
            out->vote = v;
            out->has_vote = 1;
            break;
        }
        case QESP_TLV_QUORATE: {
            uint8_t v;
            if (decode_range(&it, &v, QUOR_OK, sizeof(QUOR_OK)) != QESP_OK) {
                return QESP_ERR_RANGE;
            }
            out->quorate = v;
            out->has_quorate = 1;
            break;
        }
        case QESP_TLV_TIE_BREAKER:
            if (qesp_tlv_val_tie_breaker(&it, &out->tie_mode, &out->tie_node) != QESP_OK) {
                return QESP_ERR_INVAL;
            }
            out->has_tie = 1;
            break;
        case QESP_TLV_HEURISTICS: {
            uint8_t v;
            if (decode_range(&it, &v, HEUR_OK, sizeof(HEUR_OK)) != QESP_OK) {
                return QESP_ERR_RANGE;
            }
            out->heur = v;
            out->has_heur = 1;
            break;
        }
        case QESP_TLV_KEEP_ACTIVE_PARTITION_TB: {
            uint8_t v;
            if (decode_range(&it, &v, KAP_OK, sizeof(KAP_OK)) != QESP_OK) {
                return QESP_ERR_RANGE;
            }
            out->kap = v;
            out->has_kap = 1;
            break;
        }
        default:
            break; /* unknown options ignored — backward compat by design */
        }
    }
    if (r != 0) {
        return r;
    }
    return QESP_OK;
}
