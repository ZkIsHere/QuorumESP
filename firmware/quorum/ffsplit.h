#pragma once
/* QuorumESP — FFSplit decision core. Faithful port of the DECISION part of
 * qdevices/qnetd-algo-ffsplit.c (partition_cmp, select_partition,
 * is_membership_stable, preferred_partition). No sockets, no malloc,
 * fixed-size tables for ESP32.
 *
 * Out of scope here (wired later in server.c, like the reference):
 * VOTE_INFO push sequencing (NACKs before ACKs), expected-seq tracking,
 * per-client send states. This unit answers "who gets ACK" only.
 *
 * Source: https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/qnetd-algo-ffsplit.c
 */
#include "qesp_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QESP_FF_MAX_CLIENTS 8
#define QESP_FF_MAX_NODES 32

typedef struct {
    int used;
    uint32_t node_id;
    uint8_t tb_mode;   /* QESP_TB_* */
    uint32_t tb_node;  /* valid when mode == NODE_ID */
    int kap;           /* keep-active-partition tie-breaker flag */
    uint32_t cfg[QESP_FF_MAX_NODES];
    size_t ncfg;
    uint32_t memb[QESP_FF_MAX_NODES];
    size_t nmemb;
    uint32_t ring_node;
    uint64_t ring_seq;
    int has_ring;
    uint8_t heur;      /* QESP_HEUR_* */
} qesp_ff_client_t;

typedef struct {
    qesp_ff_client_t clients[QESP_FF_MAX_CLIENTS];
    uint32_t quorate[QESP_FF_MAX_NODES]; /* last selected best partition */
    size_t nquorate;
} qesp_ff_cluster_t;

void qesp_ff_init(qesp_ff_cluster_t *c);
/* Find slot by node_id, or allocate a fresh one. Returns index or -1 (full). */
int qesp_ff_slot(qesp_ff_cluster_t *c, uint32_t node_id);
void qesp_ff_remove(qesp_ff_cluster_t *c, int idx);

void qesp_ff_set_ids(uint32_t *dst, size_t *ndst, const uint32_t *src, size_t n);

/* 1 if id is in the cluster's current quorate selection. */
int qesp_ff_in_quorate(const qesp_ff_cluster_t *c, uint32_t id);

/* 1 if partition of client idx is "better" than the current best
 * (best_idx < 0 = no best yet). Direct port of partition_cmp. */
int qesp_ff_better(const qesp_ff_cluster_t *c, int idx, int best_idx);

/* Select best partition; out = member ids, *nout = count.
 * Returns 1 with a selection, 0 when no partition can be quorate
 * (mirrors select_partition returning NULL). */
int qesp_ff_select(const qesp_ff_cluster_t *c, uint32_t *out, size_t *nout);

/* 1 when all connected clients agree (same config sets; same ring+membership
 * within each reported partition). Direct port of is_membership_stable. */
int qesp_ff_stable(const qesp_ff_cluster_t *c);

/* Full decision for caller idx (data already stored in its slot):
 * votes[i] per used slot = QESP_VOTE_ACK/NACK; returns 1 if decided,
 * 0 if not stable (caller must answer WAIT_FOR_REPLY). */
int qesp_ff_decide(qesp_ff_cluster_t *c, uint8_t *votes);

#ifdef __cplusplus
}
#endif
