/* QuorumESP — FFSplit decision core. Port of qnetd-algo-ffsplit.c decision
 * functions. See ffsplit.h for scope. Differences from reference:
 * - Leaving clients are removed from the table BEFORE decide() instead of
 *   passed as client_leaving (equivalent: their data is excluded either way).
 * - size_t score arithmetic kept identical to reference (wraps on fail>pass).
 */
#include "ffsplit.h"

void qesp_ff_init(qesp_ff_cluster_t *c) {
    size_t i;
    uint8_t *p = (uint8_t *)c;
    for (i = 0; i < sizeof(*c); i++) {
        p[i] = 0;
    }
}

int qesp_ff_slot(qesp_ff_cluster_t *c, uint32_t node_id) {
    int i;
    for (i = 0; i < QESP_FF_MAX_CLIENTS; i++) {
        if (c->clients[i].used && c->clients[i].node_id == node_id) {
            return i;
        }
    }
    for (i = 0; i < QESP_FF_MAX_CLIENTS; i++) {
        if (!c->clients[i].used) {
            qesp_ff_client_t *cl = &c->clients[i];
            uint8_t *p = (uint8_t *)cl;
            size_t k;
            for (k = 0; k < sizeof(*cl); k++) {
                p[k] = 0;
            }
            cl->used = 1;
            cl->node_id = node_id;
            cl->tb_mode = QESP_TB_LOWEST;
            return i;
        }
    }
    return -1;
}

void qesp_ff_remove(qesp_ff_cluster_t *c, int idx) {
    if (idx >= 0 && idx < QESP_FF_MAX_CLIENTS) {
        c->clients[idx].used = 0;
    }
}

void qesp_ff_set_ids(uint32_t *dst, size_t *ndst, const uint32_t *src, size_t n) {
    size_t i;
    if (n > QESP_FF_MAX_NODES) {
        n = QESP_FF_MAX_NODES;
    }
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
    *ndst = n;
}

static int has_id(const uint32_t *set, size_t n, uint32_t id) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (set[i] == id) {
            return 1;
        }
    }
    return 0;
}

/* Set equality both directions (reference checks ordered pairs both ways). */
static int sets_eq(const uint32_t *a, size_t na, const uint32_t *b, size_t nb) {
    size_t i;
    if (na != nb) {
        return 0;
    }
    for (i = 0; i < na; i++) {
        if (!has_id(b, nb, a[i])) {
            return 0;
        }
    }
    return 1;
}

static int preferred_in_memb(const qesp_ff_client_t *cl) {
    uint32_t pref = 0;
    size_t i;
    int have = 0;
    if (cl->tb_mode == QESP_TB_NODE_ID) {
        pref = cl->tb_node;
        have = 1;
    } else if (cl->tb_mode == QESP_TB_LOWEST || cl->tb_mode == QESP_TB_HIGHEST) {
        for (i = 0; i < cl->ncfg; i++) {
            if (!have) {
                pref = cl->cfg[i];
                have = 1;
            } else if (cl->tb_mode == QESP_TB_LOWEST && cl->cfg[i] < pref) {
                pref = cl->cfg[i];
            } else if (cl->tb_mode == QESP_TB_HIGHEST && cl->cfg[i] > pref) {
                pref = cl->cfg[i];
            }
        }
    }
    if (!have || cl->ncfg == 0) {
        return 0;
    }
    return has_id(cl->memb, cl->nmemb, pref);
}

static void stats(const qesp_ff_cluster_t *c, int idx,
                  size_t *active, size_t *pass, size_t *fail) {
    const qesp_ff_client_t *cl = &c->clients[idx];
    size_t i;
    int j;
    *active = 0;
    *pass = 0;
    *fail = 0;
    for (i = 0; i < cl->nmemb; i++) {
        for (j = 0; j < QESP_FF_MAX_CLIENTS; j++) {
            if (c->clients[j].used && c->clients[j].node_id == cl->memb[i]) {
                uint8_t h;
                (*active)++;
                h = (j == idx) ? cl->heur : c->clients[j].heur;
                if (h == QESP_HEUR_PASS) {
                    (*pass)++;
                } else if (h == QESP_HEUR_FAIL) {
                    (*fail)++;
                }
                break;
            }
        }
    }
}

int qesp_ff_in_quorate(const qesp_ff_cluster_t *c, uint32_t id) {
    return has_id(c->quorate, c->nquorate, id);
}

int qesp_ff_better(const qesp_ff_cluster_t *c, int idx, int best_idx) {
    const qesp_ff_client_t *a = &c->clients[idx];
    size_t b_ncfg = 0, b_nmemb = 0;
    size_t a_act, a_pass, a_fail, b_act = 0, b_pass = 0, b_fail = 0;
    size_t score_a, score_b;
    int kap_on;
    int j;

    if (best_idx >= 0) {
        b_ncfg = c->clients[best_idx].ncfg;
        b_nmemb = c->clients[best_idx].nmemb;
        (void)b_ncfg;
        (void)b_nmemb;
    }

    if (a->ncfg % 2 != 0) {
        /* Odd clusters never split 50:50: majority of config wins. */
        return a->nmemb > a->ncfg / 2 ? 1 : 0;
    }
    if (a->nmemb > a->ncfg / 2) {
        return 1;
    }
    if (a->nmemb < a->ncfg / 2) {
        return 0;
    }
    /* 50:50 split: score = active + (pass - fail), unsigned like reference. */
    stats(c, idx, &a_act, &a_pass, &a_fail);
    if (best_idx >= 0) {
        stats(c, best_idx, &b_act, &b_pass, &b_fail);
    }
    score_a = a_act + (a_pass - a_fail);
    score_b = b_act + (b_pass - b_fail);
    if (score_a > score_b) {
        return 1;
    }
    if (score_a < score_b) {
        return 0;
    }
    if (a_act > b_act) {
        return 1;
    }
    if (a_act < b_act) {
        return 0;
    }
    if (best_idx < 0) {
        return preferred_in_memb(a) ? 1 : 0;
    }
    kap_on = 1;
    for (j = 0; j < QESP_FF_MAX_CLIENTS; j++) {
        if (c->clients[j].used && !c->clients[j].kap) {
            kap_on = 0;
            break;
        }
    }
    if (kap_on) {
        int a_in = has_id(c->quorate, c->nquorate, a->node_id);
        int b_in = has_id(c->quorate, c->nquorate, c->clients[best_idx].node_id);
        if (a_in && !b_in) {
            return 1;
        }
        if (!a_in && b_in) {
            return 0;
        }
    }
    return preferred_in_memb(a) ? 1 : 0;
}

int qesp_ff_select(const qesp_ff_cluster_t *c, uint32_t *out, size_t *nout) {
    int best = -1;
    int i;
    for (i = 0; i < QESP_FF_MAX_CLIENTS; i++) {
        if (!c->clients[i].used) {
            continue;
        }
        if (qesp_ff_better(c, i, best)) {
            best = i;
        }
    }
    if (best < 0) {
        *nout = 0;
        return 0;
    }
    qesp_ff_set_ids(out, nout, c->clients[best].memb, c->clients[best].nmemb);
    return 1;
}

int qesp_ff_stable(const qesp_ff_cluster_t *c) {
    int i, j, k;
    /* All pairs share the same config set. */
    for (i = 0; i < QESP_FF_MAX_CLIENTS; i++) {
        int a;
        if (!c->clients[i].used) {
            continue;
        }
        for (j = 0; j < QESP_FF_MAX_CLIENTS; j++) {
            if (!c->clients[j].used || i == j) {
                continue;
            }
            if (!sets_eq(c->clients[i].cfg, c->clients[i].ncfg,
                         c->clients[j].cfg, c->clients[j].ncfg)) {
                return 0;
            }
        }
        /* Same partition => same ring and same membership set. */
        for (a = 0; a < (int)c->clients[i].nmemb; a++) {
            int peer = -1;
            for (k = 0; k < QESP_FF_MAX_CLIENTS; k++) {
                if (c->clients[k].used && c->clients[k].node_id == c->clients[i].memb[a]) {
                    peer = k;
                    break;
                }
            }
            if (peer < 0) {
                continue; /* member not connected: no constraint */
            }
            if (!c->clients[i].has_ring || !c->clients[peer].has_ring ||
                c->clients[i].ring_node != c->clients[peer].ring_node ||
                c->clients[i].ring_seq != c->clients[peer].ring_seq) {
                return 0;
            }
            if (!sets_eq(c->clients[i].memb, c->clients[i].nmemb,
                         c->clients[peer].memb, c->clients[peer].nmemb)) {
                return 0;
            }
        }
    }
    return 1;
}

int qesp_ff_decide(qesp_ff_cluster_t *c, uint8_t *votes) {
    uint32_t best[QESP_FF_MAX_NODES];
    size_t nbest = 0;
    int i;
    if (!qesp_ff_stable(c)) {
        return 0; /* WAIT_FOR_REPLY */
    }
    if (qesp_ff_select(c, best, &nbest)) {
        qesp_ff_set_ids(c->quorate, &c->nquorate, best, nbest);
    } else {
        c->nquorate = 0; /* none quorate-capable: NACK everyone */
    }
    for (i = 0; i < QESP_FF_MAX_CLIENTS; i++) {
        if (!c->clients[i].used) {
            continue;
        }
        votes[i] = has_id(c->quorate, c->nquorate, c->clients[i].node_id) ?
                   QESP_VOTE_ACK : QESP_VOTE_NACK;
    }
    return 1;
}
