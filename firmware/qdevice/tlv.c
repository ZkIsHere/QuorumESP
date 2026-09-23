/* QuorumESP — TLV codec implementation. Port of qdevices/tlv.c.
 * Manual big-endian shifts (no platform headers) so this compiles for
 * ESP32 (xtensa/riscv), host gcc, and clang alike.
 */
#include "tlv.h"

static inline void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xff);
    p[2] = (uint8_t)((v >> 8) & 0xff);
    p[3] = (uint8_t)(v & 0xff);
}

static inline void put_u64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)((v >> 48) & 0xff);
    p[2] = (uint8_t)((v >> 40) & 0xff);
    p[3] = (uint8_t)((v >> 32) & 0xff);
    p[4] = (uint8_t)((v >> 24) & 0xff);
    p[5] = (uint8_t)((v >> 16) & 0xff);
    p[6] = (uint8_t)((v >> 8) & 0xff);
    p[7] = (uint8_t)(v & 0xff);
}

static inline uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline uint64_t get_u64(const uint8_t *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | p[7];
}

int qesp_tlv_add(qesp_buf_t *b, uint16_t type, const void *val, uint16_t vlen) {
    size_t need;
    const uint8_t *vp;
    size_t i;
    if (b == NULL || (val == NULL && vlen != 0)) {
        return QESP_ERR_INVAL;
    }
    need = (size_t)4 + vlen;
    if (b->len + need > b->cap) {
        return QESP_ERR_NOMEM;
    }
    put_u16(b->buf + b->len, type);
    put_u16(b->buf + b->len + 2, vlen);
    vp = (const uint8_t *)val;
    for (i = 0; i < vlen; i++) {
        b->buf[b->len + 4 + i] = vp[i];
    }
    b->len += need;
    return QESP_OK;
}

int qesp_tlv_add_u8(qesp_buf_t *b, uint16_t type, uint8_t v) {
    return qesp_tlv_add(b, type, &v, 1);
}

int qesp_tlv_add_u16(qesp_buf_t *b, uint16_t type, uint16_t v) {
    uint8_t tmp[2];
    put_u16(tmp, v);
    return qesp_tlv_add(b, type, tmp, 2);
}

int qesp_tlv_add_u32(qesp_buf_t *b, uint16_t type, uint32_t v) {
    uint8_t tmp[4];
    put_u32(tmp, v);
    return qesp_tlv_add(b, type, tmp, 4);
}

int qesp_tlv_add_u64(qesp_buf_t *b, uint16_t type, uint64_t v) {
    uint8_t tmp[8];
    put_u64(tmp, v);
    return qesp_tlv_add(b, type, tmp, 8);
}

int qesp_tlv_add_str(qesp_buf_t *b, uint16_t type, const char *s) {
    size_t n = 0;
    if (s == NULL) {
        return QESP_ERR_INVAL;
    }
    while (s[n] != '\0') {
        n++;
        if (n > 0xffff) {
            return QESP_ERR_INVAL;
        }
    }
    return qesp_tlv_add(b, type, s, (uint16_t)n);
}

int qesp_tlv_add_u16_array(qesp_buf_t *b, uint16_t type, const uint16_t *arr, size_t n) {
    /* Stack staging keeps the no-malloc guarantee (reference mallocs). */
    uint8_t tmp[256];
    size_t i;
    if (n * 2 > sizeof(tmp)) {
        return QESP_ERR_NOMEM;
    }
    if (n > 0 && arr == NULL) {
        return QESP_ERR_INVAL;
    }
    for (i = 0; i < n; i++) {
        put_u16(tmp + i * 2, arr[i]);
    }
    return qesp_tlv_add(b, type, tmp, (uint16_t)(n * 2));
}

int qesp_tlv_add_ring_id(qesp_buf_t *b, uint16_t type, uint32_t node_id, uint64_t seq) {
    uint8_t tmp[12];
    put_u32(tmp, node_id);
    put_u64(tmp + 4, seq);
    return qesp_tlv_add(b, type, tmp, sizeof(tmp));
}

int qesp_tlv_add_tie_breaker(qesp_buf_t *b, uint16_t type, uint8_t mode, uint32_t node_id) {
    uint8_t tmp[5];
    if (mode != QESP_TB_LOWEST && mode != QESP_TB_HIGHEST && mode != QESP_TB_NODE_ID) {
        return QESP_ERR_RANGE;
    }
    tmp[0] = mode;
    put_u32(tmp + 1, mode == QESP_TB_NODE_ID ? node_id : 0);
    return qesp_tlv_add(b, type, tmp, sizeof(tmp));
}

int qesp_tlv_add_node_info(qesp_buf_t *b, uint16_t type, uint32_t node_id,
                           uint32_t dc_id, int has_dc, uint8_t state, int has_state) {
    qesp_buf_t inner;
    uint8_t tmp[32];
    int rc;
    if (node_id == 0) {
        return QESP_ERR_INVAL; /* reference: node_id == 0 is a decode error */
    }
    qesp_buf_init(&inner, tmp, sizeof(tmp));
    rc = qesp_tlv_add_u32(&inner, QESP_TLV_NODE_ID, node_id);
    if (rc != QESP_OK) {
        return rc;
    }
    if (has_dc) {
        rc = qesp_tlv_add_u32(&inner, QESP_TLV_DATA_CENTER_ID, dc_id);
        if (rc != QESP_OK) {
            return rc;
        }
    }
    if (has_state) {
        rc = qesp_tlv_add_u8(&inner, QESP_TLV_NODE_STATE, state);
        if (rc != QESP_OK) {
            return rc;
        }
    }
    return qesp_tlv_add(b, type, inner.buf, (uint16_t)inner.len);
}

void qesp_tlv_iter_init(qesp_tlv_iter_t *it, const uint8_t *payload, size_t plen) {
    it->p = payload;
    it->remain = plen;
    it->type = 0;
    it->val = NULL;
    it->vlen = 0;
}

int qesp_tlv_iter_next(qesp_tlv_iter_t *it) {
    uint16_t vlen;
    if (it->remain == 0) {
        return 0;
    }
    if (it->remain < 4) {
        return QESP_ERR_TRUNC;
    }
    it->type = get_u16(it->p);
    vlen = get_u16(it->p + 2);
    if ((size_t)4 + vlen > it->remain) {
        return QESP_ERR_OVERRUN;
    }
    it->val = it->p + 4;
    it->vlen = vlen;
    it->p += 4 + vlen;
    it->remain -= 4 + vlen;
    return 1;
}

int qesp_tlv_val_u8(const qesp_tlv_iter_t *it, uint8_t *out) {
    if (it->vlen != 1) {
        return QESP_ERR_INVAL;
    }
    *out = it->val[0];
    return QESP_OK;
}

int qesp_tlv_val_u16(const qesp_tlv_iter_t *it, uint16_t *out) {
    if (it->vlen != 2) {
        return QESP_ERR_INVAL;
    }
    *out = get_u16(it->val);
    return QESP_OK;
}

int qesp_tlv_val_u32(const qesp_tlv_iter_t *it, uint32_t *out) {
    if (it->vlen != 4) {
        return QESP_ERR_INVAL;
    }
    *out = get_u32(it->val);
    return QESP_OK;
}

int qesp_tlv_val_u64(const qesp_tlv_iter_t *it, uint64_t *out) {
    if (it->vlen != 8) {
        return QESP_ERR_INVAL;
    }
    *out = get_u64(it->val);
    return QESP_OK;
}

int qesp_tlv_val_u16_array(const qesp_tlv_iter_t *it, uint16_t *out, size_t cap, size_t *n) {
    size_t i, count;
    if (it->vlen % 2 != 0) {
        return QESP_ERR_INVAL;
    }
    count = it->vlen / 2;
    if (count > cap) {
        return QESP_ERR_NOMEM;
    }
    for (i = 0; i < count; i++) {
        out[i] = get_u16(it->val + i * 2);
    }
    *n = count;
    return QESP_OK;
}

int qesp_tlv_val_ring_id(const qesp_tlv_iter_t *it, uint32_t *node_id, uint64_t *seq) {
    if (it->vlen != 12) {
        return QESP_ERR_INVAL;
    }
    *node_id = get_u32(it->val);
    *seq = get_u64(it->val + 4);
    return QESP_OK;
}

int qesp_tlv_val_tie_breaker(const qesp_tlv_iter_t *it, uint8_t *mode, uint32_t *node_id) {
    uint8_t m;
    if (it->vlen != 5) {
        return QESP_ERR_INVAL;
    }
    m = it->val[0];
    if (m != QESP_TB_LOWEST && m != QESP_TB_HIGHEST && m != QESP_TB_NODE_ID) {
        return QESP_ERR_RANGE;
    }
    *mode = m;
    *node_id = m == QESP_TB_NODE_ID ? get_u32(it->val + 1) : 0;
    return QESP_OK;
}
