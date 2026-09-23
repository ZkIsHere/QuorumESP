/* QuorumESP — host unit tests for the C message layer (gcc, WSL).
 *
 * Byte vectors below were generated from host/ (Node harness) and match
 * bytes captured from a real corosync-qdevice 3.1.9 client byte-for-byte.
 * Any C/Harness divergence fails here first — never on the device.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../msg.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* hex -> bytes helper for embedding vectors readably. */
static size_t unhex(const char *h, uint8_t *out, size_t cap) {
    size_t n = 0;
    unsigned v;
    while (h[0] && h[1]) {
        char tmp[3] = {h[0], h[1], 0};
        if (sscanf(tmp, "%x", &v) != 1 || n >= cap) {
            return 0;
        }
        out[n++] = (uint8_t)v;
        h += 2;
    }
    return n;
}

static size_t build(uint8_t *mem, size_t cap,
                    int (*fn)(qesp_buf_t *), size_t *out_len) {
    qesp_buf_t b;
    qesp_buf_init(&b, mem, cap);
    if (fn(&b) != QESP_OK) {
        return 0;
    }
    *out_len = b.len;
    return b.len;
}

/* Builder shims so `build` stays uniform. */
static int mk_preinit(qesp_buf_t *b) { return qesp_msg_preinit(b, "c1", 1, 1); }
static int mk_echo(qesp_buf_t *b) { return qesp_msg_echo_request(b, 1, 0x01020304); }
static int mk_nlr(qesp_buf_t *b) {
    return qesp_msg_node_list_reply(b, 5, QESP_NL_MEMBERSHIP, 1, 10, QESP_VOTE_ACK);
}
static int mk_err(qesp_buf_t *b) { return qesp_msg_server_error(b, 7, 1, 3); }
static int mk_init(qesp_buf_t *b) {
    return qesp_msg_init(b, 1, 2, QESP_ALGO_FFSPLIT, 1, 8000,
                         QESP_TB_LOWEST, 0, 1, 10);
}

static void test_vectors_byte_exact(void) {
    static const struct {
        int (*mk)(qesp_buf_t *);
        const char *hex;
    } vec[] = {
        {mk_preinit, "00000000000e0000000400000001000100026331"},
        {mk_echo, "0008000000080000000401020304"},
        {mk_nlr, "000b0000002200000004000000050012000102000d000c00000001000000000000000a0013000101"},
        {mk_err, "00050000000e0000000400000003000600020007"},
        {mk_init, "0003000000930000000400000002000400240000000100020003000400050006000700080009000a000b000c000d000e000f00100011000500300000000100020003000400050006000700080009000a000b000c000d000e000f001000110012001300140015001600170009000400000001000b00020001000c000400001f40001500050100000000000d000c00000001000000000000000a"},
    };
    size_t i;
    for (i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        uint8_t mem[512], expect[512];
        size_t got = 0, want;
        CHECK(build(mem, sizeof(mem), vec[i].mk, &got) != 0);
        want = unhex(vec[i].hex, expect, sizeof(expect));
        CHECK(want != 0 && want == got && memcmp(mem, expect, want) == 0);
    }
}

static void test_decode_roundtrip(void) {
    uint8_t mem[512];
    size_t len = 0;
    qesp_msg_t m;
    CHECK(build(mem, sizeof(mem), mk_init, &len) != 0);
    CHECK(qesp_msg_decode(mem, len, QESP_QDEVICE_MAX_RECEIVE_SIZE, &m) == QESP_OK);
    CHECK(m.type == QESP_MSG_INIT);
    CHECK(m.has_seq && m.seq == 2);
    CHECK(m.has_node_id && m.node_id == 1);
    CHECK(m.has_algorithm && m.algorithm == QESP_ALGO_FFSPLIT);
    CHECK(m.has_heartbeat && m.heartbeat == 8000);
    CHECK(m.has_tie && m.tie_mode == QESP_TB_LOWEST && m.tie_node == 0);
    CHECK(m.has_ring && m.ring_node == 1 && m.ring_seq == 10);
    CHECK(m.n_sup_msgs == 18 && m.n_sup_opts == 24);
    /* Unknown options are skipped, not fatal. */
    {
        qesp_buf_t b;
        uint8_t extra[8] = {0xFD, 0xE8, 0x00, 0x02, 0xAA, 0xBB};
        (void)extra;
        qesp_buf_init(&b, mem, sizeof(mem));
        CHECK(qesp_msg_echo_request(&b, 1, 9) == QESP_OK);
        CHECK(qesp_tlv_add(&b, 65000, (uint8_t[]){1, 2, 3}, 3) == QESP_OK);
        /* fix up length after manual append */
        {
            uint32_t plen = (uint32_t)(b.len - QESP_MSG_HEADER_LEN);
            mem[2] = (uint8_t)(plen >> 24);
            mem[3] = (uint8_t)((plen >> 16) & 0xff);
            mem[4] = (uint8_t)((plen >> 8) & 0xff);
            mem[5] = (uint8_t)(plen & 0xff);
        }
        CHECK(qesp_msg_decode(mem, b.len, QESP_QDEVICE_MAX_RECEIVE_SIZE, &m) == QESP_OK);
        CHECK(m.seq == 9);
    }
}

static void test_malformed(void) {
    uint8_t mem[512];
    size_t len = 0;
    qesp_msg_t m;
    uint16_t t;
    uint32_t l;
    CHECK(build(mem, sizeof(mem), mk_preinit, &len) != 0);
    /* truncated */
    CHECK(qesp_msg_decode(mem, len - 1, QESP_QDEVICE_MAX_RECEIVE_SIZE, &m) == QESP_ERR_TRUNC);
    /* lying length */
    mem[2] = 0; mem[3] = 0; mem[4] = 0x03; mem[5] = 0xE8;
    CHECK(qesp_msg_decode(mem, len, QESP_QDEVICE_MAX_RECEIVE_SIZE, &m) == QESP_ERR_TRUNC);
    /* bad type */
    mem[0] = 0; mem[1] = 99;
    CHECK(qesp_msg_check(mem, len, QESP_QDEVICE_MAX_RECEIVE_SIZE, &t, &l) == QESP_ERR_RANGE);
    /* oversize */
    {
        uint8_t hdr[6] = {0, 8, 0x01, 0x00, 0x00, 0x00}; /* len = 16MB */
        CHECK(qesp_msg_check(hdr, sizeof(hdr), QESP_INITIAL_MSG_SIZE, &t, &l) == QESP_ERR_NOMEM);
    }
    /* echo reply must derive from an echo request */
    {
        qesp_buf_t b;
        qesp_buf_init(&b, mem, sizeof(mem));
        CHECK(qesp_msg_echo_reply_from(&b, mem, 0) == QESP_ERR_INVAL);
    }
}

static void test_echo_byte_copy(void) {
    uint8_t req[64], rep[64];
    size_t reqlen = 0;
    qesp_buf_t b;
    CHECK(build(req, sizeof(req), mk_echo, &reqlen) != 0);
    qesp_buf_init(&b, rep, sizeof(rep));
    CHECK(qesp_msg_echo_reply_from(&b, req, reqlen) == QESP_OK);
    CHECK(b.len == reqlen);
    CHECK(rep[0] == 0x00 && rep[1] == QESP_MSG_ECHO_REPLY);
    CHECK(memcmp(rep + 2, req + 2, reqlen - 2) == 0);
}

static void test_header_only_check(void) {
    /* Regression: recv_frame validates a header-only buffer. Using the full
     * frame check here would report TRUNC for every non-empty message. */
    uint8_t mem[512];
    size_t len = 0;
    uint16_t t;
    uint32_t l;
    uint8_t bad_type[6] = {0x00, 0x63, 0x00, 0x00, 0x00, 0x02};
    uint8_t huge[6] = {0x00, 0x08, 0x01, 0x00, 0x00, 0x00};
    CHECK(build(mem, sizeof(mem), mk_init, &len) != 0);
    /* header-only (6 bytes) must validate WITHOUT demanding the body */
    CHECK(qesp_msg_check_header(mem, sizeof(mem), &t, &l) == QESP_OK);
    CHECK(t == QESP_MSG_INIT && l == len - QESP_MSG_HEADER_LEN);
    CHECK(qesp_msg_check_header(bad_type, sizeof(mem), &t, &l) == QESP_ERR_RANGE);
    CHECK(qesp_msg_check_header(huge, QESP_INITIAL_MSG_SIZE, &t, &l) == QESP_ERR_NOMEM);
}

int main(void) {
    test_vectors_byte_exact();
    test_header_only_check();
    test_decode_roundtrip();
    test_malformed();
    test_echo_byte_copy();
    if (failures == 0) {
        printf("msg: all tests passed\n");
        return 0;
    }
    printf("msg: %d FAILURES\n", failures);
    return 1;
}
