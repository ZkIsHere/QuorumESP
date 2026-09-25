/* QuorumESP — LMS decision core. Port of qnetd-algo-lms.c + utils.
 * See lms.h. Notes on fidelity:
 * - Newcomer NACK is returned WITHOUT saving last_result (reference).
 * - NODE_ID tie-break quirk ported by EFFECT (nominated ACKs, others NACK):
 *   the reference compares nominated id against the CALLER in a loop over
 *   others and copies a loop-dependent ring; the observable outcome is the
 *   same. Documented here instead of replicated opaquely.
 */
#include "lms.h"

static int ring_eq(uint32_t n1, uint64_t s1, uint32_t n2, uint64_t s2) {
    return n1 == n2 && s1 == s2;
}

/* -1 if a same-partition peer disagrees on ring (wait for convergence). */
static int rings_match(const qesp_ff_cluster_t *c, int idx) {
    const qesp_ff_client_t *me = &c->clients[idx];
    int j, k;
    for (j = 0; j < QESP_FF_MAX_CLIENTS; j++) {
        const qesp_ff_client_t *o;
        int in_part = 0;
        if (!c->clients[j].used || j == idx) {
            continue;
        }
        o = &c->clients[j];
        for (k = 0; k < (int)me->nmemb; k++) {
            if (me->memb[k] == o->node_id) {
                in_part = 1;
                break;
            }
        }
        if (!in_part) {
            for (k = 0; k < (int)o->nmemb; k++) {
                if (o->memb[k] == me->node_id) {
                    in_part = 1;
                    break;
                }
            }
        }
        if (in_part &&
            !ring_eq(me->ring_node, me->ring_seq, o->ring_node, o->ring_seq)) {
            return -1;
        }
    }
    return 0;
}

/* Group connected, ring-initialized clients by ring. Returns count. */
static int make_partitions(const qesp_ff_cluster_t *c,
                           qesp_lms_part_t *parts, int cap) {
    int n = 0, j, k;
    for (j = 0; j < QESP_FF_MAX_CLIENTS; j++) {
        const qesp_ff_client_t *o;
        int p = -1;
        if (!c->clients[j].used || c->clients[j].ring_seq == 0) {
            continue; /* not initialised yet */
        }
        o = &c->clients[j];
        for (k = 0; k < n; k++) {
            if (ring_eq(parts[k].ring_node, parts[k].ring_seq,
                        o->ring_node, o->ring_seq)) {
                p = k;
                break;
            }
        }
        if (p < 0) {
            if (n >= cap) {
                return -1;
            }
            p = n++;
            parts[p].ring_node = o->ring_node;
            parts[p].ring_seq = o->ring_seq;
            parts[p].count = 0;
            parts[p].score = 0;
        }
        parts[p].count++;
        /* Score like FFSplit: +1 per active client, +/- heuristics. */
        parts[p].score++;
        if (o->heur == QESP_HEUR_PASS) {
            parts[p].score++;
        } else if (o->heur == QESP_HEUR_FAIL) {
            parts[p].score--;
        }
    }
    return n;
}

uint8_t qesp_lms_decide(qesp_ff_cluster_t *c, int idx, uint8_t *last) {
    const qesp_ff_client_t *me = &c->clients[idx];
    qesp_lms_part_t parts[QESP_FF_MAX_CLIENTS];
    int nparts, i, j;
    int best = -1, joint = 0;

    if (rings_match(c, idx) != 0) {
        last[idx] = QESP_VOTE_WAIT_FOR_REPLY;
        return QESP_VOTE_WAIT_FOR_REPLY;
    }
    nparts = make_partitions(c, parts, QESP_FF_MAX_CLIENTS);
    if (nparts <= 0) {
        last[idx] = QESP_VOTE_WAIT_FOR_REPLY;
        return QESP_VOTE_WAIT_FOR_REPLY;
    }
    if (nparts == 1) {
        last[idx] = QESP_VOTE_ACK;
        return QESP_VOTE_ACK;
    }
    /* Newcomer facing another active partition: NACK, do NOT save. */
    if (last[idx] == QESP_LMS_NEW) {
        for (j = 0; j < QESP_FF_MAX_CLIENTS; j++) {
            if (!c->clients[j].used || j == idx) {
                continue;
            }
            if (!ring_eq(me->ring_node, me->ring_seq,
                         c->clients[j].ring_node, c->clients[j].ring_seq) &&
                last[j] == QESP_VOTE_ACK) {
                return QESP_VOTE_NACK;
            }
        }
    }
    /* Best unique score wins. */
    for (i = 0; i < nparts; i++) {
        if (best < 0 || parts[best].score < parts[i].score) {
            best = i;
        }
    }
    for (i = 0; i < nparts; i++) {
        if (i != best && parts[best].score == parts[i].score) {
            joint = 1;
            break;
        }
    }
    if (!joint) {
        uint8_t v = ring_eq(parts[best].ring_node, parts[best].ring_seq,
                            me->ring_node, me->ring_seq) ?
                    QESP_VOTE_ACK :
                    QESP_VOTE_NACK;
        last[idx] = v;
        return v;
    }
    /* Score tie: unique largest wins. */
    {
        int big = -1;
        joint = 0;
        for (i = 0; i < nparts; i++) {
            if (big < 0 || parts[big].count < parts[i].count) {
                big = i;
            }
        }
        for (i = 0; i < nparts; i++) {
            if (i != big && parts[big].count == parts[i].count) {
                joint = 1;
                break;
            }
        }
        if (!joint) {
            uint8_t v = ring_eq(parts[big].ring_node, parts[big].ring_seq,
                                me->ring_node, me->ring_seq) ?
                        QESP_VOTE_ACK :
                        QESP_VOTE_NACK;
            last[idx] = v;
            return v;
        }
    }
    /* Size tie: tie-breaker node. */
    {
        uint32_t tb = 0;
        int have_tb = 0;
        uint32_t tb_ring_n = 0;
        uint64_t tb_ring_s = 0;
        if (me->tb_mode == QESP_TB_LOWEST || me->tb_mode == QESP_TB_HIGHEST) {
            for (j = 0; j < QESP_FF_MAX_CLIENTS; j++) {
                uint32_t nid;
                if (!c->clients[j].used) {
                    continue;
                }
                nid = c->clients[j].node_id;
                if (!have_tb) {
                    tb = nid;
                    have_tb = 1;
                } else if (me->tb_mode == QESP_TB_LOWEST && nid < tb) {
                    tb = nid;
                } else if (me->tb_mode == QESP_TB_HIGHEST && nid > tb) {
                    tb = nid;
                }
            }
            /* Ring of the tie-breaker node. */
            for (j = 0; j < QESP_FF_MAX_CLIENTS; j++) {
                if (c->clients[j].used && c->clients[j].node_id == tb) {
                    tb_ring_n = c->clients[j].ring_node;
                    tb_ring_s = c->clients[j].ring_seq;
                    break;
                }
            }
            {
                uint8_t v = (me->node_id == tb ||
                             ring_eq(tb_ring_n, tb_ring_s, me->ring_node,
                                     me->ring_seq)) ?
                            QESP_VOTE_ACK :
                            QESP_VOTE_NACK;
                last[idx] = v;
                return v;
            }
        }
        if (me->tb_mode == QESP_TB_NODE_ID) {
            /* Reference quirk, ported by effect: nominated node ACKs. */
            uint8_t v = (me->node_id == me->tb_node) ? QESP_VOTE_ACK :
                                                       QESP_VOTE_NACK;
            last[idx] = v;
            return v;
        }
        return QESP_VOTE_WAIT_FOR_REPLY;
    }
}
