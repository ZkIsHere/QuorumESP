'use strict';

/* TLV encoder/decoder tests — byte-exact vs tlv.c behavior. */

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const { TLV, TIE_BREAKER_MODE, NODE_STATE, VOTE } = require('../src/consts');
const tlv = require('../src/tlv');

describe('TLV framing', () => {
  it('encodes type+len big-endian with raw value', () => {
    const out = tlv.encodeTlv(0x0102, Buffer.from([0xaa, 0xbb]));
    assert.deepEqual(out, Buffer.from([0x01, 0x02, 0x00, 0x02, 0xaa, 0xbb]));
  });

  it('u32 is big-endian (htonl)', () => {
    const out = tlv.encodeU32(TLV.NODE_ID, 0x01020304);
    assert.deepEqual(out, Buffer.from([0x00, 0x09, 0x00, 0x04, 0x01, 0x02, 0x03, 0x04]));
  });

  it('u16 is big-endian (htons), u64 uses 8 bytes BE', () => {
    assert.deepEqual(tlv.encodeU16(TLV.REPLY_ERROR_CODE, 0x0102).subarray(4), Buffer.from([0x01, 0x02]));
    const u64 = tlv.encodeU64(TLV.CONFIG_VERSION, 0x0102030405060708n);
    assert.equal(u64.readUInt16BE(2), 8);
    assert.equal(u64.readBigUInt64BE(4), 0x0102030405060708n);
  });

  it('string has no NUL terminator (strlen semantics)', () => {
    const out = tlv.encodeString(TLV.CLUSTER_NAME, 'proxmox');
    assert.equal(out.readUInt16BE(2), 7);
    assert.equal(out.subarray(4).toString(), 'proxmox');
  });

  it('u16 array encodes each element BE', () => {
    const out = tlv.encodeU16Array(TLV.SUPPORTED_MESSAGES, [0, 1, 0x0102]);
    assert.deepEqual(out.subarray(4), Buffer.from([0x00, 0x00, 0x00, 0x01, 0x01, 0x02]));
  });

  it('ring_id is 12 bytes: u32 node_id + u64 seq', () => {
    const out = tlv.encodeRingId(TLV.RING_ID, 7, 0x0102030405060708n);
    assert.equal(out.readUInt16BE(2), 12);
    assert.equal(out.readUInt32BE(4), 7);
    assert.equal(out.readBigUInt64BE(8), 0x0102030405060708n);
    const back = tlv.decodeRingId(tlv.decodeTlvs(out)[0]);
    assert.deepEqual(back, { nodeId: 7, seq: 0x0102030405060708n });
  });

  it('tie_breaker is 5 bytes: u8 mode + u32 node_id (0 unless NODE_ID)', () => {
    const lowest = tlv.encodeTieBreaker(TLV.TIE_BREAKER, TIE_BREAKER_MODE.LOWEST, 99);
    assert.equal(lowest.readUInt16BE(2), 5);
    assert.deepEqual([...lowest.subarray(4)], [TIE_BREAKER_MODE.LOWEST, 0, 0, 0, 0]);
    const byId = tlv.encodeTieBreaker(TLV.TIE_BREAKER, TIE_BREAKER_MODE.NODE_ID, 0x01020304);
    assert.deepEqual([...byId.subarray(4)], [TIE_BREAKER_MODE.NODE_ID, 0x01, 0x02, 0x03, 0x04]);
    assert.throws(() => tlv.encodeTieBreaker(TLV.TIE_BREAKER, 9, 0), tlv.TlvError);
  });

  it('node_info nests TLVs and omits unset fields', () => {
    const full = tlv.encodeNodeInfo(TLV.NODE_INFO, {
      nodeId: 3,
      dataCenterId: 5,
      nodeState: NODE_STATE.MEMBER,
    });
    const inner = tlv.decodeTlvs(full.subarray(4));
    assert.deepEqual(inner.map((t) => t.type), [TLV.NODE_ID, TLV.DATA_CENTER_ID, TLV.NODE_STATE]);
    assert.deepEqual(tlv.decodeNodeInfo(tlv.decodeTlvs(full)[0]), {
      nodeId: 3,
      dataCenterId: 5,
      nodeState: NODE_STATE.MEMBER,
    });
    const minimal = tlv.encodeNodeInfo(TLV.NODE_INFO, { nodeId: 3 });
    assert.deepEqual(tlv.decodeTlvs(minimal.subarray(4)).map((t) => t.type), [TLV.NODE_ID]);
    assert.throws(() => tlv.encodeNodeInfo(TLV.NODE_INFO, { nodeId: 0 }), tlv.TlvError);
    // node_info without node_id must be rejected (tlv.c returns -4)
    const bad = tlv.encodeTlv(TLV.NODE_INFO, tlv.encodeU8(TLV.NODE_STATE, NODE_STATE.MEMBER));
    assert.throws(() => tlv.decodeNodeInfo(tlv.decodeTlvs(bad)[0]), tlv.TlvError);
  });
});

describe('TLV malformed input (fail-closed)', () => {
  it('rejects truncated TLV header', () => {
    assert.throws(() => tlv.decodeTlvs(Buffer.from([0x00, 0x09, 0x00])), tlv.TlvError);
  });

  it('rejects TLV overrunning the buffer', () => {
    assert.throws(
      () => tlv.decodeTlvs(Buffer.from([0x00, 0x09, 0x00, 0x04, 0x01, 0x02])),
      tlv.TlvError
    );
  });

  it('rejects wrong-length scalars', () => {
    const badU32 = { type: TLV.NODE_ID, len: 2, value: Buffer.from([1, 2]) };
    assert.throws(() => tlv.decodeU32(badU32), tlv.TlvError);
    const badU8 = { type: TLV.VOTE, len: 2, value: Buffer.from([1, 1]) };
    assert.throws(() => tlv.decodeVote(badU8), tlv.TlvError);
  });

  it('rejects out-of-range enums', () => {
    const tls = { type: TLV.TLS_SUPPORTED, len: 1, value: Buffer.from([9]) };
    assert.throws(() => tlv.decodeTlsSupported(tls), tlv.TlvError);
    const vote = { type: TLV.VOTE, len: 1, value: Buffer.from([VOTE.UNDEFINED]) };
    assert.throws(() => tlv.decodeVote(vote), tlv.TlvError); // UNDEFINED never valid on wire
    const tb = tlv.encodeTlv(TLV.TIE_BREAKER, Buffer.from([9, 0, 0, 0, 0]));
    assert.throws(() => tlv.decodeTieBreaker(tlv.decodeTlvs(tb)[0]), tlv.TlvError);
  });

  it('rejects odd-length u16 arrays', () => {
    const odd = { type: TLV.SUPPORTED_MESSAGES, len: 3, value: Buffer.from([0, 1, 2]) };
    assert.throws(() => tlv.decodeU16Array(odd), tlv.TlvError);
  });

  it('round-trips every supported enum value through its decoder', () => {
    for (const v of [0, 1, 2]) {
      tlv.decodeTlsSupported({ type: 0, len: 1, value: Buffer.from([v]) });
    }
    for (const v of [1, 2, 3, 4, 5]) {
      tlv.decodeVote({ type: 0, len: 1, value: Buffer.from([v]) });
    }
  });
});
