/* QuorumESP — FFSplit decision unit tests (gcc, WSL).
 * Scenarios mirror reference behavior (qnetd-algo-ffsplit.c).
 */
#include <stdio.h>

#include "../ffsplit.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while (0)

static int add_node(qesp_ff_cluster_t *c, uint32_t id,
                    const uint32_t *cfg, size_t ncfg,
                    const uint32_t *memb, size_t nmemb,
                    uint32_t ring_node, uint64_t ring_seq, uint8_t heur) {
    int i = qesp_ff_slot(c, id);
    if (i < 0) {
        return -1;
    }
    qesp_ff_set_ids(c->clients[i].cfg, &c->clients[i].ncfg, cfg, ncfg);
    qesp_ff_set_ids(c->clients[i].memb, &c->clients[i].nmemb, memb, nmemb);
    c->clients[i].ring_node = ring_node;
    c->clients[i].ring_seq = ring_seq;
    c->clients[i].has_ring = 1;
    c->clients[i].heur = heur;
    return i;
}

static void test_single_node(void) {
    qesp_ff_cluster_t c;
    uint8_t votes[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t one[] = {1};
    int a;
    qesp_ff_init(&c);
    a = add_node(&c, 1, one, 1, one, 1, 1, 10, QESP_HEUR_UNDEFINED);
    CHECK(a >= 0);
    CHECK(qesp_ff_stable(&c) == 1);
    CHECK(qesp_ff_decide(&c, votes) == 1);
    CHECK(votes[a] == QESP_VOTE_ACK);
}

static void test_healthy_pair(void) {
    qesp_ff_cluster_t c;
    uint8_t votes[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t cfg[] = {1, 2};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, cfg, 2, cfg, 2, 1, 10, QESP_HEUR_UNDEFINED);
    b = add_node(&c, 2, cfg, 2, cfg, 2, 1, 10, QESP_HEUR_UNDEFINED);
    CHECK(qesp_ff_stable(&c) == 1);
    CHECK(qesp_ff_decide(&c, votes) == 1);
    CHECK(votes[a] == QESP_VOTE_ACK);
    CHECK(votes[b] == QESP_VOTE_ACK);
}

static void test_split_brain_tie_break_lowest(void) {
    /* A sees [1], B sees [2]: equal halves -> tie_breaker lowest -> A wins. */
    qesp_ff_cluster_t c;
    uint8_t votes[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t cfg[] = {1, 2};
    uint32_t ma[] = {1};
    uint32_t mb[] = {2};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, cfg, 2, ma, 1, 1, 10, QESP_HEUR_UNDEFINED);
    b = add_node(&c, 2, cfg, 2, mb, 1, 1, 20, QESP_HEUR_UNDEFINED);
    CHECK(qesp_ff_stable(&c) == 1);
    CHECK(qesp_ff_decide(&c, votes) == 1);
    CHECK(votes[a] == QESP_VOTE_ACK);
    CHECK(votes[b] == QESP_VOTE_NACK);
}

static void test_split_brain_tie_break_highest(void) {
    qesp_ff_cluster_t c;
    uint8_t votes[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t cfg[] = {1, 2};
    uint32_t ma[] = {1};
    uint32_t mb[] = {2};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, cfg, 2, ma, 1, 1, 10, QESP_HEUR_UNDEFINED);
    b = add_node(&c, 2, cfg, 2, mb, 1, 1, 20, QESP_HEUR_UNDEFINED);
    c.clients[a].tb_mode = QESP_TB_HIGHEST;
    c.clients[b].tb_mode = QESP_TB_HIGHEST;
    CHECK(qesp_ff_decide(&c, votes) == 1);
    CHECK(votes[a] == QESP_VOTE_NACK);
    CHECK(votes[b] == QESP_VOTE_ACK);
}

static void test_odd_majority_wins(void) {
    qesp_ff_cluster_t c;
    uint8_t votes[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t cfg[] = {1, 2, 3};
    uint32_t m12[] = {1, 2};
    uint32_t m3[] = {3};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, cfg, 3, m12, 2, 1, 10, QESP_HEUR_UNDEFINED);
    b = add_node(&c, 3, cfg, 3, m3, 1, 1, 20, QESP_HEUR_UNDEFINED);
    CHECK(qesp_ff_stable(&c) == 1);
    CHECK(qesp_ff_decide(&c, votes) == 1);
    CHECK(votes[a] == QESP_VOTE_ACK);
    CHECK(votes[b] == QESP_VOTE_NACK);
}

static void test_heuristics_break_tie(void) {
    /* 2v2 split: PASS partition outscores FAIL partition. */
    qesp_ff_cluster_t c;
    uint8_t votes[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t cfg[] = {1, 2, 3, 4};
    uint32_t m12[] = {1, 2};
    uint32_t m34[] = {3, 4};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, cfg, 4, m12, 2, 1, 10, QESP_HEUR_PASS);
    b = add_node(&c, 3, cfg, 4, m34, 2, 1, 20, QESP_HEUR_FAIL);
    CHECK(qesp_ff_stable(&c) == 1);
    CHECK(qesp_ff_decide(&c, votes) == 1);
    CHECK(votes[a] == QESP_VOTE_ACK);
    CHECK(votes[b] == QESP_VOTE_NACK);
}

static void test_keep_active_partition(void) {
    /* Tied halves, both kap=1, previous quorate=[2]: B keeps the vote. */
    qesp_ff_cluster_t c;
    uint8_t votes[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t cfg[] = {1, 2};
    uint32_t ma[] = {1};
    uint32_t mb[] = {2};
    uint32_t prev[] = {2};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, cfg, 2, ma, 1, 1, 10, QESP_HEUR_UNDEFINED);
    b = add_node(&c, 2, cfg, 2, mb, 1, 1, 20, QESP_HEUR_UNDEFINED);
    c.clients[a].kap = 1;
    c.clients[b].kap = 1;
    qesp_ff_set_ids(c.quorate, &c.nquorate, prev, 1);
    CHECK(qesp_ff_decide(&c, votes) == 1);
    CHECK(votes[a] == QESP_VOTE_NACK);
    CHECK(votes[b] == QESP_VOTE_ACK);
}

static void test_unstable_config(void) {
    qesp_ff_cluster_t c;
    uint8_t votes[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t c1[] = {1, 2};
    uint32_t c2[] = {1, 2, 3};
    uint32_t m[] = {1, 2};
    qesp_ff_init(&c);
    add_node(&c, 1, c1, 2, m, 2, 1, 10, QESP_HEUR_UNDEFINED);
    add_node(&c, 2, c2, 3, m, 2, 1, 10, QESP_HEUR_UNDEFINED);
    CHECK(qesp_ff_stable(&c) == 0);
    CHECK(qesp_ff_decide(&c, votes) == 0); /* WAIT_FOR_REPLY */
}

static void test_leave_and_table_limits(void) {
    qesp_ff_cluster_t c;
    uint8_t votes[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t cfg[] = {1, 2};
    uint32_t ma[] = {1};
    uint32_t mb[] = {2};
    int a, b, i, extra = 0;
    qesp_ff_init(&c);
    a = add_node(&c, 1, cfg, 2, ma, 1, 1, 10, QESP_HEUR_UNDEFINED);
    b = add_node(&c, 2, cfg, 2, mb, 1, 1, 20, QESP_HEUR_UNDEFINED);
    CHECK(qesp_ff_decide(&c, votes) == 1);
    CHECK(votes[b] == QESP_VOTE_NACK);
    /* B leaves: lone A still outscores "no partition" (active client wins
     * ties) -> ACK. This is how a 2-node cluster survives 1 failure. */
    qesp_ff_remove(&c, b);
    CHECK(qesp_ff_decide(&c, votes) == 1);
    CHECK(votes[a] == QESP_VOTE_ACK);
    /* Table holds 8: fill up, 9th node id must fail. */
    for (i = 10; i < 30; i++) {
        int s = qesp_ff_slot(&c, (uint32_t)i);
        if (s < 0) {
            extra++;
        }
    }
    CHECK(extra > 0);
}

int main(void) {
    test_single_node();
    test_healthy_pair();
    test_split_brain_tie_break_lowest();
    test_split_brain_tie_break_highest();
    test_odd_majority_wins();
    test_heuristics_break_tie();
    test_keep_active_partition();
    test_unstable_config();
    test_leave_and_table_limits();
    if (failures == 0) {
        printf("ffsplit: all tests passed\n");
        return 0;
    }
    printf("ffsplit: %d FAILURES\n", failures);
    return 1;
}
