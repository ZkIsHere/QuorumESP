'use strict';

/*
 * QuorumESP Phase 1 — TLV encoder/decoder.
 *
 * Direct port of the wire format from the reference implementation:
 * - qdevices/tlv.c : tlv_add (u16 type BE + u16 len BE + value),
 *   tlv_add_u8/u16/u32/u64, tlv_add_u16_array, ring_id (12 B),
 *   tie_breaker (5 B), node_info (nested TLVs),
 *   tlv_iter_next validity rules + typed decoders with range checks.
 *
 * Source: https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/tlv.c
 */

const {
  TLV,
  TLS,
  NODE_STATE,
  NODE_LIST_TYPE,
  VOTE,
  QUORATE,
  TIE_BREAKER_MODE,
  HEURISTICS,
  KAP_TB,
} = require('./consts');

class TlvError extends Error {
  constructor(message) {
    super(message);
    this.name = 'TlvError';
  }
}

function encodeTlv(type, value) {
  if (!Number.isInteger(type) || type < 0 || type > 0xffff) {
    throw new TlvError(`invalid TLV type ${type}`);
  }
  const v = Buffer.isBuffer(value) ? value : Buffer.from(value);
  if (v.length > 0xffff) {
    throw new TlvError('TLV value too long');
  }
  const out = Buffer.allocUnsafe(4 + v.length);
  out.writeUInt16BE(type, 0);
  out.writeUInt16BE(v.length, 2);
  v.copy(out, 4);
  return out;
}

function encodeU8(type, n) {
  const b = Buffer.allocUnsafe(1);
  b.writeUInt8(n, 0);
  return encodeTlv(type, b);
}

function encodeU16(type, n) {
  const b = Buffer.allocUnsafe(2);
  b.writeUInt16BE(n, 0);
  return encodeTlv(type, b);
}

function encodeU32(type, n) {
  const b = Buffer.allocUnsafe(4);
  b.writeUInt32BE(n, 0);
  return encodeTlv(type, b);
}

function encodeU64(type, n) {
  const b = Buffer.allocUnsafe(8);
  b.writeBigUInt64BE(BigInt(n), 0);
  return encodeTlv(type, b);
}

function encodeString(type, str) {
  // Reference: tlv_add_string uses strlen, no NUL terminator on the wire.
  return encodeTlv(type, Buffer.from(str, 'utf8'));
}

function encodeU16Array(type, arr) {
  const b = Buffer.allocUnsafe(2 * arr.length);
  arr.forEach((n, i) => b.writeUInt16BE(n, i * 2));
  return encodeTlv(type, b);
}

function encodeRingId(type, nodeId, seq) {
  // Reference: 4 B node_id BE + 8 B seq BE = 12 B.
  const b = Buffer.allocUnsafe(12);
  b.writeUInt32BE(nodeId, 0);
  b.writeBigUInt64BE(BigInt(seq), 4);
  return encodeTlv(type, b);
}

function encodeTieBreaker(type, mode, nodeId = 0) {
  // Reference: 1 B mode + 4 B node_id BE (0 unless NODE_ID mode) = 5 B.
  if (!Object.values(TIE_BREAKER_MODE).includes(mode)) {
    throw new TlvError(`invalid tie breaker mode ${mode}`);
  }
  const b = Buffer.allocUnsafe(5);
  b.writeUInt8(mode, 0);
  b.writeUInt32BE(mode === TIE_BREAKER_MODE.NODE_ID ? nodeId : 0, 1);
  return encodeTlv(type, b);
}

function encodeNodeInfo(type, { nodeId, dataCenterId = 0, nodeState = NODE_STATE.NOT_SET }) {
  // Reference: nested TLVs; NODE_ID mandatory, others only when set.
  if (!Number.isInteger(nodeId) || nodeId === 0) {
    throw new TlvError('node_info requires non-zero node_id');
  }
  const parts = [encodeU32(TLV.NODE_ID, nodeId)];
  if (dataCenterId !== 0) {
    parts.push(encodeU32(TLV.DATA_CENTER_ID, dataCenterId));
  }
  if (nodeState !== NODE_STATE.NOT_SET) {
    parts.push(encodeU8(TLV.NODE_STATE, nodeState));
  }
  return encodeTlv(type, Buffer.concat(parts));
}

/*
 * Strict TLV stream parser.
 * Mirrors tlv_iter_next: each entry must fit fully inside the buffer,
 * otherwise throws (reference returns -1 → treated as fatal).
 */
function decodeTlvs(buf) {
  const out = [];
  let pos = 0;
  while (pos < buf.length) {
    if (pos + 4 > buf.length) {
      throw new TlvError('truncated TLV header');
    }
    const type = buf.readUInt16BE(pos);
    const len = buf.readUInt16BE(pos + 2);
    if (pos + 4 + len > buf.length) {
      throw new TlvError('TLV overruns message');
    }
    out.push({ type, len, value: buf.subarray(pos + 4, pos + 4 + len) });
    pos += 4 + len;
  }
  return out;
}

/* Typed value decoders with exact-length + range checks (mirror tlv_iter_decode_*). */
function expectLen(entry, n, name) {
  if (entry.len !== n) {
    throw new TlvError(`invalid length for ${name}: ${entry.len} != ${n}`);
  }
}

function decodeU8(entry) {
  expectLen(entry, 1, 'u8');
  return entry.value.readUInt8(0);
}

function decodeU16(entry) {
  expectLen(entry, 2, 'u16');
  return entry.value.readUInt16BE(0);
}

function decodeU32(entry) {
  expectLen(entry, 4, 'u32');
  return entry.value.readUInt32BE(0);
}

function decodeU64(entry) {
  expectLen(entry, 8, 'u64');
  return entry.value.readBigUInt64BE(0);
}

function decodeString(entry) {
  return entry.value.toString('utf8');
}

function decodeU16Array(entry) {
  if (entry.len % 2 !== 0) {
    throw new TlvError('u16 array length not even');
  }
  const out = [];
  for (let i = 0; i < entry.len; i += 2) {
    out.push(entry.value.readUInt16BE(i));
  }
  return out;
}

function decodeTlsSupported(entry) {
  const v = decodeU8(entry);
  if (!Object.values(TLS).includes(v)) {
    throw new TlvError(`invalid tls_supported ${v}`);
  }
  return v;
}

function decodeRingId(entry) {
  expectLen(entry, 12, 'ring_id');
  return {
    nodeId: entry.value.readUInt32BE(0),
    seq: entry.value.readBigUInt64BE(4),
  };
}

function decodeTieBreaker(entry) {
  expectLen(entry, 5, 'tie_breaker');
  const mode = entry.value.readUInt8(0);
  if (!Object.values(TIE_BREAKER_MODE).includes(mode)) {
    throw new TlvError(`invalid tie breaker mode ${mode}`);
  }
  const nodeId = entry.value.readUInt32BE(1);
  return { mode, nodeId: mode === TIE_BREAKER_MODE.NODE_ID ? nodeId : 0 };
}

function decodeNodeState(entry) {
  const v = decodeU8(entry);
  if (![NODE_STATE.MEMBER, NODE_STATE.DEAD, NODE_STATE.LEAVING].includes(v)) {
    throw new TlvError(`invalid node_state ${v}`);
  }
  return v;
}

function decodeNodeListType(entry) {
  const v = decodeU8(entry);
  if (!Object.values(NODE_LIST_TYPE).includes(v)) {
    throw new TlvError(`invalid node_list_type ${v}`);
  }
  return v;
}

function decodeVote(entry) {
  const v = decodeU8(entry);
  if (![VOTE.ACK, VOTE.NACK, VOTE.ASK_LATER, VOTE.WAIT_FOR_REPLY, VOTE.NO_CHANGE].includes(v)) {
    throw new TlvError(`invalid vote ${v}`);
  }
  return v;
}

function decodeQuorate(entry) {
  const v = decodeU8(entry);
  if (!Object.values(QUORATE).includes(v)) {
    throw new TlvError(`invalid quorate ${v}`);
  }
  return v;
}

function decodeHeuristics(entry) {
  const v = decodeU8(entry);
  if (![HEURISTICS.PASS, HEURISTICS.FAIL].includes(v)) {
    throw new TlvError(`invalid heuristics ${v}`);
  }
  return v;
}

function decodeKapTb(entry) {
  const v = decodeU8(entry);
  if (!Object.values(KAP_TB).includes(v)) {
    throw new TlvError(`invalid keep_active_partition_tb ${v}`);
  }
  return v;
}

function decodeNodeInfo(entry) {
  // Reference: nested TLVs; unknown inner options ignored; node_id != 0 required.
  const inner = decodeTlvs(entry.value);
  const info = { nodeId: 0, dataCenterId: 0, nodeState: NODE_STATE.NOT_SET };
  for (const t of inner) {
    if (t.type === TLV.NODE_ID) {
      info.nodeId = decodeU32(t);
    } else if (t.type === TLV.DATA_CENTER_ID) {
      info.dataCenterId = decodeU32(t);
    } else if (t.type === TLV.NODE_STATE) {
      info.nodeState = decodeNodeState(t);
    }
  }
  if (info.nodeId === 0) {
    throw new TlvError('node_info without node_id');
  }
  return info;
}

module.exports = {
  TlvError,
  encodeTlv,
  encodeU8,
  encodeU16,
  encodeU32,
  encodeU64,
  encodeString,
  encodeU16Array,
  encodeRingId,
  encodeTieBreaker,
  encodeNodeInfo,
  decodeTlvs,
  decodeU8,
  decodeU16,
  decodeU32,
  decodeU64,
  decodeString,
  decodeU16Array,
  decodeTlsSupported,
  decodeRingId,
  decodeTieBreaker,
  decodeNodeState,
  decodeNodeListType,
  decodeVote,
  decodeQuorate,
  decodeHeuristics,
  decodeKapTb,
  decodeNodeInfo,
};
