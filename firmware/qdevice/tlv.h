#pragma once
/* QuorumESP — TLV codec. Direct port of qdevices/tlv.c, no malloc.
 *
 * Wire: u16 type BE + u16 len BE + value. All integers big-endian.
 * RING_ID = u32 node + u64 seq (12 B). TIE_BREAKER = u8 mode + u32 node (5 B).
 * NODE_INFO = nested TLVs (NODE_ID mandatory non-zero).
 */
#include "qesp_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
} qesp_buf_t;

static inline void qesp_buf_init(qesp_buf_t *b, uint8_t *mem, size_t cap) {
    b->buf = mem;
    b->cap = cap;
    b->len = 0;
}

int qesp_tlv_add(qesp_buf_t *b, uint16_t type, const void *val, uint16_t vlen);
int qesp_tlv_add_u8(qesp_buf_t *b, uint16_t type, uint8_t v);
int qesp_tlv_add_u16(qesp_buf_t *b, uint16_t type, uint16_t v);
int qesp_tlv_add_u32(qesp_buf_t *b, uint16_t type, uint32_t v);
int qesp_tlv_add_u64(qesp_buf_t *b, uint16_t type, uint64_t v);
/* String: raw bytes, NO NUL terminator (strlen semantics like tlv_add_string). */
int qesp_tlv_add_str(qesp_buf_t *b, uint16_t type, const char *s);
int qesp_tlv_add_u16_array(qesp_buf_t *b, uint16_t type, const uint16_t *arr, size_t n);
int qesp_tlv_add_ring_id(qesp_buf_t *b, uint16_t type, uint32_t node_id, uint64_t seq);
int qesp_tlv_add_tie_breaker(qesp_buf_t *b, uint16_t type, uint8_t mode, uint32_t node_id);
int qesp_tlv_add_node_info(qesp_buf_t *b, uint16_t type, uint32_t node_id,
                           uint32_t dc_id, int has_dc, uint8_t state, int has_state);

/* Strict iterator: 1 = item ready, 0 = end, negative = fatal (TRUNC/OVERRUN). */
typedef struct {
    const uint8_t *p;
    size_t remain;
    uint16_t type;
    const uint8_t *val;
    uint16_t vlen;
} qesp_tlv_iter_t;

void qesp_tlv_iter_init(qesp_tlv_iter_t *it, const uint8_t *payload, size_t plen);
int qesp_tlv_iter_next(qesp_tlv_iter_t *it);

int qesp_tlv_val_u8(const qesp_tlv_iter_t *it, uint8_t *out);
int qesp_tlv_val_u16(const qesp_tlv_iter_t *it, uint16_t *out);
int qesp_tlv_val_u32(const qesp_tlv_iter_t *it, uint32_t *out);
int qesp_tlv_val_u64(const qesp_tlv_iter_t *it, uint64_t *out);
int qesp_tlv_val_u16_array(const qesp_tlv_iter_t *it, uint16_t *out, size_t cap, size_t *n);
int qesp_tlv_val_ring_id(const qesp_tlv_iter_t *it, uint32_t *node_id, uint64_t *seq);
int qesp_tlv_val_tie_breaker(const qesp_tlv_iter_t *it, uint8_t *mode, uint32_t *node_id);

#ifdef __cplusplus
}
#endif
