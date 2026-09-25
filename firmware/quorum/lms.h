#pragma once
/* QuorumESP — LMS ("last man standing") decision core. Faithful port of the
 * DECISION part of qdevices/qnetd-algo-lms.c + qnetd-algo-utils.c
 * (create_partitions, all_ring_ids_match, do_lms_algorithm).
 *
 * Rules (reference):
 * - Same-partition peer with different ring -> WAIT_FOR_REPLY.
 * - 0 known partitions -> WAIT_FOR_REPLY. 1 partition -> ACK.
 * - Newcomer (never voted) facing another ACK partition -> NACK (not saved).
 * - Unique best score -> in it ? ACK : NACK. Score ties -> unique largest
 *   wins. Size ties -> tie-breaker (lowest/highest/NODE_ID).
 * - Votes go DIRECTLY in replies (no VOTE_INFO push sequencing like FFSplit).
 *
 * Shares the cluster membership table with FFSplit (ffsplit.h); per-client
 * LMS state (last_result) lives in a parallel array owned by the caller.
 * Entry points config (count only) and heuristics_change (NO_CHANGE, LMS
 * ignores them) need no decision function.
 *
 * Sources:
 * https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/qnetd-algo-lms.c
 * https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/qnetd-algo-utils.c
 */
#include "ffsplit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* last_result values: QESP_VOTE_* or QESP_LMS_NEW (never voted). */
#define QESP_LMS_NEW 0

typedef struct {
    uint32_t ring_node;
    uint64_t ring_seq;
    int count;
    int score;
} qesp_lms_part_t;

/* Decide for caller idx (its fresh ring/membership/heuristics already stored
 * in the table). `last` is the caller's parallel per-slot array holding each
 * client's saved result (QESP_VOTE_* or QESP_LMS_NEW); the newcomer rule
 * reads siblings from it. Updates last[idx] per reference rules (WAIT saves
 * WAIT; newcomer-NACK does NOT save; ACK/NACK save) and returns the vote:
 * ACK / NACK / WAIT_FOR_REPLY. */
uint8_t qesp_lms_decide(qesp_ff_cluster_t *c, int idx, uint8_t *last);

#ifdef __cplusplus
}
#endif
