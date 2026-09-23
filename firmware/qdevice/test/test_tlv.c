/* QuorumESP — host unit tests for the C TLV codec (gcc, WSL).
 * Compile: make (see Makefile). No IDF needed: codec has no IDF dependency.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../tlv.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while (0)

static void test_framing(void) {
    uint8_t mem[64];
    qesp_buf_t b;
    qesp_buf_init(&b, mem, sizeof(mem));
    CHECK(qesp_tlv_add(&b, 0x0102, (uint8_t[]){0xaa, 0xbb}, 2) == QESP_OK);
    CHECK(b.len == 6);
    CHECK(mem[0] == 0x01 && mem[1] == 0x02 && mem[2] == 0x00 && mem[3] == 0x02);
    CHECK(mem[4] == 0xaa && mem[5] == 0xbb);
    /* Overflow is fail-closed, buffer untouched beyond len. */
    CHECK(qesp_tlv_add(&b, 1, (uint8_t[60]){0}, 60) == QESP_ERR_NOMEM);
    CHECK(b.len == 6);
}

static void test_integers_be(void) {
    uint8_t mem[64];
    qesp_buf_t b;
    qesp_buf_init(&b, mem, sizeof(mem));
    CHECK(qesp_tlv_add_u32(&b, QESP_TLV_NODE_ID, 0x01020304) == QESP_OK);
    CHECK(memcmp(mem, "\x00\x09\x00\x04\x01\x02\x03\x04", 8) == 0);
}

static void test_layouts(void) {
    uint8_t mem[128];
    qesp_buf_t b;
    qesp_tlv_iter_t it;
    uint32_t node;
    uint64_t seq;
    uint8_t mode;
    qesp_buf_init(&b, mem, sizeof(mem));
    /* ring_id: 12 B */
    CHECK(qesp_tlv_add_ring_id(&b, QESP_TLV_RING_ID, 7, 0x0102030405060708ull) == QESP_OK);
    CHECK(b.len == 4 + 12);
    qesp_tlv_iter_init(&it, mem, b.len);
    CHECK(qesp_tlv_iter_next(&it) == 1);
    CHECK(qesp_tlv_val_ring_id(&it, &node, &seq) == QESP_OK);
    CHECK(node == 7 && seq == 0x0102030405060708ull);
    /* tie_breaker LOWEST zeroes node; bad mode rejected */
    b.len = 0;
    CHECK(qesp_tlv_add_tie_breaker(&b, QESP_TLV_TIE_BREAKER, QESP_TB_LOWEST, 99) == QESP_OK);
    CHECK(mem[4] == QESP_TB_LOWEST && mem[5] == 0 && mem[8] == 0);
    CHECK(qesp_tlv_add_tie_breaker(&b, QESP_TLV_TIE_BREAKER, 9, 0) == QESP_ERR_RANGE);
    qesp_tlv_iter_init(&it, mem, 4 + 5);
    CHECK(qesp_tlv_iter_next(&it) == 1);
    CHECK(qesp_tlv_val_tie_breaker(&it, &mode, &node) == QESP_OK);
    CHECK(mode == QESP_TB_LOWEST && node == 0);
    /* node_info nesting + mandatory node_id */
    b.len = 0;
    CHECK(qesp_tlv_add_node_info(&b, QESP_TLV_NODE_INFO, 3, 0, 0,
                                QESP_NODE_STATE_MEMBER, 1) == QESP_OK);
    CHECK(qesp_tlv_add_node_info(&b, QESP_TLV_NODE_INFO, 0, 0, 0, 0, 0) == QESP_ERR_INVAL);
}

static void test_malformed(void) {
    qesp_tlv_iter_t it;
    uint8_t u8;
    /* truncated header */
    uint8_t t1[] = {0x00, 0x09, 0x00};
    qesp_tlv_iter_init(&it, t1, sizeof(t1));
    CHECK(qesp_tlv_iter_next(&it) == QESP_ERR_TRUNC);
    /* overrun */
    uint8_t t2[] = {0x00, 0x09, 0x00, 0x04, 0x01, 0x02};
    qesp_tlv_iter_init(&it, t2, sizeof(t2));
    CHECK(qesp_tlv_iter_next(&it) == QESP_ERR_OVERRUN);
    /* wrong scalar length */
    uint8_t t3[] = {0x00, 0x13, 0x00, 0x02, 0x01, 0x01};
    qesp_tlv_iter_init(&it, t3, sizeof(t3));
    CHECK(qesp_tlv_iter_next(&it) == 1);
    CHECK(qesp_tlv_val_u8(&it, &u8) == QESP_ERR_INVAL);
    /* bad tie mode */
    uint8_t t4[] = {0x00, 0x15, 0x00, 0x05, 0x09, 0x00, 0x00, 0x00, 0x00};
    uint8_t mode;
    uint32_t node;
    qesp_tlv_iter_init(&it, t4, sizeof(t4));
    CHECK(qesp_tlv_iter_next(&it) == 1);
    CHECK(qesp_tlv_val_tie_breaker(&it, &mode, &node) == QESP_ERR_RANGE);
}

int main(void) {
    test_framing();
    test_integers_be();
    test_layouts();
    test_malformed();
    if (failures == 0) {
        printf("tlv: all tests passed\n");
        return 0;
    }
    printf("tlv: %d FAILURES\n", failures);
    return 1;
}
