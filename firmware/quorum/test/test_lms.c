/* QuorumESP — LMS decision unit tests (gcc, WSL). Mirror reference
 * semantics from qnetd-algo-lms.c. The caller owns uint8_t last[8]
 * (all QESP_LMS_NEW initially); slots store rings/membership/heuristics.
 */
#include <stdio.h>

#include "../ffsplit.h"
#include "../lms.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while (0)

static int add_node(qesp_ff_cluster_t *c, uint32_t id,
                    const uint32_t *memb, size_t nmemb,
                    uint32_t ring_node, uint64_t ring_seq, uint8_t heur) {
    int i = qesp_ff_slot(c, id);
    if (i < 0) {
        return -1;
    }
    qesp_ff_set_ids(c->clients[i].memb, &c->clients[i].nmemb, memb, nmemb);
    c->clients[i].ncfg = 0; /* LMS ignores config counts */
    c->clients[i].ring_node = ring_node;
    c->clients[i].ring_seq = ring_seq;
    c->clients[i].has_ring = 1;
    c->clients[i].heur = heur;
    return i;
}

static void test_single_partition_ack(void) {
    qesp_ff_cluster_t c;
    uint8_t last[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t m[] = {1};
    int a;
    qesp_ff_init(&c);
    a = add_node(&c, 1, m, 1, 1, 10, QESP_HEUR_UNDEFINED);
    CHECK(qesp_lms_decide(&c, a, last) == QESP_VOTE_ACK);
    CHECK(last[a] == QESP_VOTE_ACK);
}

static void test_one_partition_two_nodes(void) {
    /* Same ring: 1 partition -> votequorum's problem -> ACK both. */
    qesp_ff_cluster_t c;
    uint8_t last[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t m[] = {1, 2};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, m, 2, 1, 10, QESP_HEUR_UNDEFINED);
    b = add_node(&c, 2, m, 2, 1, 10, QESP_HEUR_UNDEFINED);
    CHECK(qesp_lms_decide(&c, a, last) == QESP_VOTE_ACK);
    CHECK(qesp_lms_decide(&c, b, last) == QESP_VOTE_ACK);
}

static void test_split_lowest_wins(void) {
    qesp_ff_cluster_t c;
    uint8_t last[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t ma[] = {1};
    uint32_t mb[] = {2};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, ma, 1, 1, 10, QESP_HEUR_UNDEFINED);
    b = add_node(&c, 2, mb, 1, 1, 20, QESP_HEUR_UNDEFINED);
    /* A decides first (both NEW, no active elsewhere): tie score+size,
     * tie-break lowest -> A contains node 1 -> ACK, saved. */
    CHECK(qesp_lms_decide(&c, a, last) == QESP_VOTE_ACK);
    /* B decides: newcomer facing ACK partition -> NACK, NOT saved. */
    CHECK(qesp_lms_decide(&c, b, last) == QESP_VOTE_NACK);
    CHECK(last[b] == QESP_LMS_NEW);
}

static void test_newcomer_keeps_nacking(void) {
    qesp_ff_cluster_t c;
    uint8_t last[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t ma[] = {1};
    uint32_t mb[] = {2};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, ma, 1, 1, 10, QESP_HEUR_UNDEFINED);
    b = add_node(&c, 2, mb, 1, 1, 20, QESP_HEUR_UNDEFINED);
    CHECK(qesp_lms_decide(&c, a, last) == QESP_VOTE_ACK);
    CHECK(qesp_lms_decide(&c, b, last) == QESP_VOTE_NACK);
    CHECK(qesp_lms_decide(&c, b, last) == QESP_VOTE_NACK);
    CHECK(last[b] == QESP_LMS_NEW);
    (void)a;
}

static void test_best_score_wins(void) {
    /* Equal size, A PASS vs B FAIL: unique best score -> A ACK, B NACK.
     * (A decides first with no active elsewhere; B newcomer... B faces
     * ACK partition so newcomer rule fires first! To test scoring, give B
     * a saved non-new state via a prior lone decision.) */
    qesp_ff_cluster_t c;
    uint8_t last[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t ma[] = {1};
    uint32_t mb[] = {2};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, ma, 1, 1, 10, QESP_HEUR_PASS);
    b = add_node(&c, 2, mb, 1, 1, 20, QESP_HEUR_FAIL);
    /* B alone first: single partition (only B's ring known... A also stored
     * with ring -> 2 partitions; B NEW, no ACK elsewhere -> scoring:
     * A score 2 vs B score 0 -> unique best A, B not in it -> NACK saved. */
    CHECK(qesp_lms_decide(&c, b, last) == QESP_VOTE_NACK);
    CHECK(last[b] == QESP_VOTE_NACK);
    CHECK(qesp_lms_decide(&c, a, last) == QESP_VOTE_ACK);
}

static void test_largest_wins_score_tie(void) {
    /* Scores tied (both UNDEFINED heur: 2 vs 1... make equal: A:[1,2] with
     * node2 unconnected so active=1; B:[3] active=1. Scores 1 vs 1, sizes
     * 2 vs 1 -> largest unique (A) wins. */
    qesp_ff_cluster_t c;
    uint8_t last[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t ma[] = {1, 2};
    uint32_t mb[] = {3};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, ma, 2, 1, 10, QESP_HEUR_UNDEFINED);
    b = add_node(&c, 3, mb, 1, 1, 20, QESP_HEUR_UNDEFINED);
    CHECK(qesp_lms_decide(&c, a, last) == QESP_VOTE_ACK);
    CHECK(qesp_lms_decide(&c, b, last) == QESP_VOTE_NACK);
}

static void test_ring_mismatch_waits(void) {
    /* Same partition view but rings disagree -> WAIT. */
    qesp_ff_cluster_t c;
    uint8_t last[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t m[] = {1, 2};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, m, 2, 1, 10, QESP_HEUR_UNDEFINED);
    b = add_node(&c, 2, m, 2, 1, 99, QESP_HEUR_UNDEFINED);
    CHECK(qesp_lms_decide(&c, a, last) == QESP_VOTE_WAIT_FOR_REPLY);
    CHECK(last[a] == QESP_VOTE_WAIT_FOR_REPLY);
    (void)b;
}

static void test_uninitialized_peer_skipped(void) {
    /* B ring seq 0 (never reported): skipped from partitions -> A alone. */
    qesp_ff_cluster_t c;
    uint8_t last[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t ma[] = {1};
    uint32_t mb[] = {2};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, ma, 1, 1, 10, QESP_HEUR_UNDEFINED);
    b = add_node(&c, 2, mb, 1, 9, 0, QESP_HEUR_UNDEFINED);
    CHECK(qesp_lms_decide(&c, a, last) == QESP_VOTE_ACK);
    (void)b;
}

static void test_node_id_mode(void) {
    qesp_ff_cluster_t c;
    uint8_t last[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t ma[] = {1};
    uint32_t mb[] = {2};
    int a, b;
    qesp_ff_init(&c);
    a = add_node(&c, 1, ma, 1, 1, 10, QESP_HEUR_UNDEFINED);
    b = add_node(&c, 2, mb, 1, 1, 20, QESP_HEUR_UNDEFINED);
    c.clients[a].tb_mode = QESP_TB_NODE_ID;
    c.clients[a].tb_node = 2;
    c.clients[b].tb_mode = QESP_TB_NODE_ID;
    c.clients[b].tb_node = 2;
    /* A first: tie -> nominated 2, A is not 2 -> NACK saved. */
    CHECK(qesp_lms_decide(&c, a, last) == QESP_VOTE_NACK);
    /* B: not NEW anymore... still tie path (A NACKed, no ACK partition) ->
     * nominated 2 == B -> ACK. */
    CHECK(qesp_lms_decide(&c, b, last) == QESP_VOTE_ACK);
}

static void test_ask_path_equals_membership(void) {
    /* ask_for_vote runs the same decide (reference). Smoke: lone node. */
    qesp_ff_cluster_t c;
    uint8_t last[QESP_FF_MAX_CLIENTS] = {0};
    uint32_t m[] = {5};
    int a;
    qesp_ff_init(&c);
    a = add_node(&c, 5, m, 1, 5, 7, QESP_HEUR_UNDEFINED);
    CHECK(qesp_lms_decide(&c, a, last) == QESP_VOTE_ACK);
}

int main(void) {
    test_single_partition_ack();
    test_one_partition_two_nodes();
    test_split_lowest_wins();
    test_newcomer_keeps_nacking();
    test_best_score_wins();
    test_largest_wins_score_tie();
    test_ring_mismatch_waits();
    test_uninitialized_peer_skipped();
    test_node_id_mode();
    test_ask_path_equals_membership();
    if (failures == 0) {
        printf("lms: all tests passed\n");
        return 0;
    }
    printf("lms: %d FAILURES\n", failures);
    return 1;
}
