#pragma once
/* QuorumESP — message framing. Port of qdevices/msg.c, no malloc.
 *
 * Frame: u16 type BE + u32 payload-len BE + TLV stream (len = total - 6).
 * ECHO_REPLY is a byte copy of the request with the type overwritten.
 * Unknown TLV options are skipped on decode (backward compat by design).
 */
#include "tlv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QESP_MAX_NODES 32
#define QESP_MAX_MSG_LIST 32
#define QESP_MAX_OPT_LIST 32
#define QESP_MAX_ALGO_LIST 8

typedef struct {
    uint32_t node_id;
    uint32_t dc_id;
    int has_dc;
    uint8_t state;
    int has_state;
} qesp_node_t;

typedef struct {
    uint16_t type;
    /* Optional fields: only valid when the matching has_ flag is set. */
    int has_seq;
    uint32_t seq;
    const char *cluster; /* NOT NUL-terminated */
    size_t cluster_len;
    int has_tls;
    uint8_t tls_supported;
    int has_cert_req;
    uint8_t cert_req;
    int has_node_id;
    uint32_t node_id;
    int has_algorithm;
    uint16_t algorithm;
    int has_heartbeat;
    uint32_t heartbeat;
    int has_ring;
    uint32_t ring_node;
    uint64_t ring_seq;
    int has_config;
    uint64_t config_version;
    int has_dc_id;
    uint32_t dc_id;
    int has_list_type;
    uint8_t list_type;
    int has_vote;
    uint8_t vote;
    int has_quorate;
    uint8_t quorate;
    int has_tie;
    uint8_t tie_mode;
    uint32_t tie_node;
    int has_heur;
    uint8_t heur;
    int has_kap;
    uint8_t kap;
    int has_error;
    uint16_t error_code;
    int has_max_req;
    uint32_t max_req;
    int has_max_rep;
    uint32_t max_rep;
    uint16_t sup_msgs[QESP_MAX_MSG_LIST];
    size_t n_sup_msgs;
    uint16_t sup_opts[QESP_MAX_OPT_LIST];
    size_t n_sup_opts;
    uint16_t sup_algos[QESP_MAX_ALGO_LIST];
    size_t n_sup_algos;
    qesp_node_t nodes[QESP_MAX_NODES];
    size_t n_nodes;
} qesp_msg_t;

/* Frame assembly: begin() reserves the header, end() backfills the length. */
int qesp_msg_begin(qesp_buf_t *b, uint16_t type);
int qesp_msg_end(qesp_buf_t *b);
/* Validate 6 header bytes (type range + size cap) WITHOUT demanding the body.
 * This is the only correct check on a header-only buffer — using qesp_msg_check
 * here wrongly reports TRUNC for every non-empty message (real bug, 2026-09-23). */
int qesp_msg_check_header(const uint8_t *hdr6, size_t cap,
                          uint16_t *type, uint32_t *plen);
/* Validate a complete frame in place. On success sets type/plen. */
int qesp_msg_check(const uint8_t *frame, size_t flen, size_t max,
                   uint16_t *type, uint32_t *plen);

/* Builders. has_seq gates the MSG_SEQ_NUMBER option; strings need no NUL. */
int qesp_msg_preinit(qesp_buf_t *b, const char *cluster, int has_seq, uint32_t seq);
int qesp_msg_preinit_reply(qesp_buf_t *b, uint8_t tls, uint8_t cert_req,
                           int has_seq, uint32_t seq);
int qesp_msg_starttls(qesp_buf_t *b, int has_seq, uint32_t seq);
int qesp_msg_init(qesp_buf_t *b, int has_seq, uint32_t seq, uint16_t algorithm,
                  uint32_t node_id, uint32_t heartbeat,
                  uint8_t tb_mode, uint32_t tb_node,
                  uint32_t ring_node, uint64_t ring_seq);
int qesp_msg_init_reply(qesp_buf_t *b, int has_seq, uint32_t seq, uint16_t error_code,
                        uint32_t max_req, uint32_t max_rep,
                        const uint16_t *algos, size_t nalgo);
int qesp_msg_server_error(qesp_buf_t *b, uint16_t error_code, int has_seq, uint32_t seq);
int qesp_msg_echo_request(qesp_buf_t *b, int has_seq, uint32_t seq);
/* Byte copy of a validated ECHO_REQUEST with type overwritten (msg.c). */
int qesp_msg_echo_reply_from(qesp_buf_t *b, const uint8_t *req, size_t req_len);
int qesp_msg_node_list(qesp_buf_t *b, uint32_t seq, uint8_t list_type,
                       int has_ring, uint32_t ring_node, uint64_t ring_seq,
                       int has_config, uint64_t config_version,
                       int has_quorate, uint8_t quorate,
                       int has_heur, uint8_t heur,
                       const qesp_node_t *nodes, size_t n_nodes);
int qesp_msg_node_list_reply(qesp_buf_t *b, uint32_t seq, uint8_t list_type,
                             uint32_t ring_node, uint64_t ring_seq, uint8_t vote);
int qesp_msg_ask_for_vote(qesp_buf_t *b, uint32_t seq);
int qesp_msg_ask_for_vote_reply(qesp_buf_t *b, uint32_t seq,
                                uint32_t ring_node, uint64_t ring_seq, uint8_t vote);
int qesp_msg_vote_info(qesp_buf_t *b, uint32_t seq,
                       uint32_t ring_node, uint64_t ring_seq, uint8_t vote);
int qesp_msg_vote_info_reply(qesp_buf_t *b, uint32_t seq);
int qesp_msg_heuristics_change(qesp_buf_t *b, uint32_t seq, uint8_t heur);
int qesp_msg_heuristics_change_reply(qesp_buf_t *b, uint32_t seq,
                                     uint32_t ring_node, uint64_t ring_seq,
                                     uint8_t heur, uint8_t vote);
int qesp_msg_set_option(qesp_buf_t *b, int has_seq, uint32_t seq,
                        int has_hb, uint32_t hb, int has_kap, uint8_t kap);
int qesp_msg_set_option_reply(qesp_buf_t *b, int has_seq, uint32_t seq,
                              int has_hb, uint32_t hb, int has_kap, uint8_t kap);

/* Generic decoder into qesp_msg_t. Zero-copy for cluster (points into frame).
 * Unknown options are skipped. Returns QESP_ERR_* on any violation. */
int qesp_msg_decode(const uint8_t *frame, size_t flen, size_t max, qesp_msg_t *out);

#ifdef __cplusplus
}
#endif
