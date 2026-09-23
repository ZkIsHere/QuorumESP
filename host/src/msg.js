'use strict';

/*
 * QuorumESP Phase 1 — message framing + builders + generic decoder.
 *
 * Wire format from the reference implementation (qdevices/msg.c):
 *   header = u16 type BE + u32 payload-len BE (6 bytes total)
 *   payload = TLV stream; len = total size - 6
 * ECHO_REPLY is a byte copy of the request with the type field overwritten.
 * Unknown TLV options are ignored on decode (backward compat).
 *
 * Sources:
 * https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/msg.c
 * https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/msg.h
 */

const {
  MSG,
  TLV,
  REPLY_ERROR,
  DEFAULTS,
  SUPPORTED_MESSAGES,
  SUPPORTED_OPTIONS,
  SUPPORTED_ALGORITHMS,
} = require('./consts');
const tlv = require('./tlv');

const HEADER_LEN = DEFAULTS.HEADER_LEN;
const MAX_TYPE = MSG.HEURISTICS_CHANGE_REPLY;

class MsgError extends Error {
  constructor(message) {
    super(message);
    this.name = 'MsgError';
  }
}

function encodeMsg(type, tlvBuffers = []) {
  if (!Number.isInteger(type) || type < 0 || type > MAX_TYPE) {
    throw new MsgError(`invalid message type ${type}`);
  }
  const payload = Buffer.concat(tlvBuffers);
  const out = Buffer.allocUnsafe(HEADER_LEN + payload.length);
  out.writeUInt16BE(type, 0);
  out.writeUInt32BE(payload.length, 2);
  payload.copy(out, HEADER_LEN);
  return out;
}

function peekHeader(buf) {
  if (buf.length < HEADER_LEN) {
    return null;
  }
  return { type: buf.readUInt16BE(0), len: buf.readUInt32BE(2) };
}

/*
 * Validate + split one complete frame. Throws MsgError on:
 * invalid type (mirror msg_is_valid_msg_type), length mismatch,
 * or payload exceeding maxSize (mirror msgio_read -5/-6).
 */
function splitFrame(buf, maxSize = DEFAULTS.QNETD_MAX_RECEIVE_SIZE) {
  const h = peekHeader(buf);
  if (!h) {
    throw new MsgError('buffer smaller than header');
  }
  if (h.type > MAX_TYPE) {
    throw new MsgError(`invalid message type ${h.type}`);
  }
  if (HEADER_LEN + h.len > maxSize) {
    throw new MsgError(`message too long: ${HEADER_LEN + h.len} > ${maxSize}`);
  }
  if (buf.length < HEADER_LEN + h.len) {
    throw new MsgError('incomplete message');
  }
  return {
    type: h.type,
    payload: buf.subarray(HEADER_LEN, HEADER_LEN + h.len),
    totalLen: HEADER_LEN + h.len,
  };
}

function seqTlv(seq) {
  return seq === undefined ? [] : [tlv.encodeU32(TLV.MSG_SEQ_NUMBER, seq)];
}

/* Builders — field order mirrors msg_create_* in msg.c. */
function preinit(clusterName, seq) {
  return encodeMsg(MSG.PREINIT, [
    ...seqTlv(seq),
    tlv.encodeString(TLV.CLUSTER_NAME, clusterName),
  ]);
}

function preinitReply(tlsSupported, clientCertRequired, seq) {
  return encodeMsg(MSG.PREINIT_REPLY, [
    ...seqTlv(seq),
    tlv.encodeU8(TLV.TLS_SUPPORTED, tlsSupported),
    tlv.encodeU8(TLV.TLS_CLIENT_CERT_REQUIRED, clientCertRequired ? 1 : 0),
  ]);
}

function starttls(seq) {
  return encodeMsg(MSG.STARTTLS, [...seqTlv(seq)]);
}

function init({ seq, algorithm, nodeId, heartbeatInterval, tieBreaker, ringId }) {
  return encodeMsg(MSG.INIT, [
    ...seqTlv(seq),
    tlv.encodeU16Array(TLV.SUPPORTED_MESSAGES, [...SUPPORTED_MESSAGES]),
    tlv.encodeU16Array(TLV.SUPPORTED_OPTIONS, [...SUPPORTED_OPTIONS]),
    tlv.encodeU32(TLV.NODE_ID, nodeId),
    tlv.encodeU16(TLV.DECISION_ALGORITHM, algorithm),
    tlv.encodeU32(TLV.HEARTBEAT_INTERVAL, heartbeatInterval),
    tlv.encodeTieBreaker(TLV.TIE_BREAKER, tieBreaker.mode, tieBreaker.nodeId),
    tlv.encodeRingId(TLV.RING_ID, ringId.nodeId, ringId.seq),
  ]);
}

function initReply({ seq, errorCode = REPLY_ERROR.NO_ERROR, maxRequest, maxReply, algorithms }) {
  return encodeMsg(MSG.INIT_REPLY, [
    tlv.encodeU16(TLV.REPLY_ERROR_CODE, errorCode),
    tlv.encodeU16Array(TLV.SUPPORTED_MESSAGES, [...SUPPORTED_MESSAGES]),
    tlv.encodeU16Array(TLV.SUPPORTED_OPTIONS, [...SUPPORTED_OPTIONS]),
    ...seqTlv(seq),
    tlv.encodeU32(TLV.SERVER_MAXIMUM_REQUEST_SIZE, maxRequest),
    tlv.encodeU32(TLV.SERVER_MAXIMUM_REPLY_SIZE, maxReply),
    tlv.encodeU16Array(TLV.SUPPORTED_DECISION_ALGORITHMS, [...algorithms]),
  ]);
}

function serverError(errorCode, seq) {
  return encodeMsg(MSG.SERVER_ERROR, [
    ...seqTlv(seq),
    tlv.encodeU16(TLV.REPLY_ERROR_CODE, errorCode),
  ]);
}

function echoRequest(seq) {
  return encodeMsg(MSG.ECHO_REQUEST, [...seqTlv(seq)]);
}

function echoReply(requestFrame) {
  // Reference: byte copy of the request, type overwritten with ECHO_REPLY.
  const { type } = splitFrame(requestFrame);
  if (type !== MSG.ECHO_REQUEST) {
    throw new MsgError('echo reply requires an echo request');
  }
  const out = Buffer.from(requestFrame);
  out.writeUInt16BE(MSG.ECHO_REPLY, 0);
  return out;
}

function nodeList({ seq, listType, ringId, configVersion, quorate, heuristics, nodes = [] }) {
  const parts = [
    tlv.encodeU32(TLV.MSG_SEQ_NUMBER, seq),
    tlv.encodeU8(TLV.NODE_LIST_TYPE, listType),
  ];
  if (ringId) {
    parts.push(tlv.encodeRingId(TLV.RING_ID, ringId.nodeId, ringId.seq));
  }
  if (configVersion !== undefined) {
    parts.push(tlv.encodeU64(TLV.CONFIG_VERSION, configVersion));
  }
  if (quorate !== undefined) {
    parts.push(tlv.encodeU8(TLV.QUORATE, quorate));
  }
  for (const n of nodes) {
    parts.push(tlv.encodeNodeInfo(TLV.NODE_INFO, n));
  }
  if (heuristics !== undefined) {
    parts.push(tlv.encodeU8(TLV.HEURISTICS, heuristics));
  }
  return encodeMsg(MSG.NODE_LIST, parts);
}

function nodeListReply(seq, listType, ringId, vote) {
  return encodeMsg(MSG.NODE_LIST_REPLY, [
    tlv.encodeU32(TLV.MSG_SEQ_NUMBER, seq),
    tlv.encodeU8(TLV.NODE_LIST_TYPE, listType),
    tlv.encodeRingId(TLV.RING_ID, ringId.nodeId, ringId.seq),
    tlv.encodeU8(TLV.VOTE, vote),
  ]);
}

function askForVote(seq) {
  return encodeMsg(MSG.ASK_FOR_VOTE, [tlv.encodeU32(TLV.MSG_SEQ_NUMBER, seq)]);
}

function askForVoteReply(seq, ringId, vote) {
  return encodeMsg(MSG.ASK_FOR_VOTE_REPLY, [
    tlv.encodeU32(TLV.MSG_SEQ_NUMBER, seq),
    tlv.encodeU8(TLV.VOTE, vote),
    tlv.encodeRingId(TLV.RING_ID, ringId.nodeId, ringId.seq),
  ]);
}

function voteInfo(seq, ringId, vote) {
  return encodeMsg(MSG.VOTE_INFO, [
    tlv.encodeU32(TLV.MSG_SEQ_NUMBER, seq),
    tlv.encodeU8(TLV.VOTE, vote),
    tlv.encodeRingId(TLV.RING_ID, ringId.nodeId, ringId.seq),
  ]);
}

function voteInfoReply(seq) {
  return encodeMsg(MSG.VOTE_INFO_REPLY, [tlv.encodeU32(TLV.MSG_SEQ_NUMBER, seq)]);
}

function heuristicsChange(seq, heuristics) {
  return encodeMsg(MSG.HEURISTICS_CHANGE, [
    tlv.encodeU32(TLV.MSG_SEQ_NUMBER, seq),
    tlv.encodeU8(TLV.HEURISTICS, heuristics),
  ]);
}

function heuristicsChangeReply(seq, ringId, heuristics, vote) {
  return encodeMsg(MSG.HEURISTICS_CHANGE_REPLY, [
    tlv.encodeU32(TLV.MSG_SEQ_NUMBER, seq),
    tlv.encodeU8(TLV.VOTE, vote),
    tlv.encodeRingId(TLV.RING_ID, ringId.nodeId, ringId.seq),
    tlv.encodeU8(TLV.HEURISTICS, heuristics),
  ]);
}

/*
 * Generic decoder → plain object with only present fields set.
 * Mirrors struct msg_decoded (msg.h) without cmap/votequorum coupling.
 * Unknown TLV types are skipped (reference msg_decode has no default case).
 */
function decodeMessage(buf, maxSize = DEFAULTS.QNETD_MAX_RECEIVE_SIZE) {
  const { type, payload } = splitFrame(buf, maxSize);
  const entries = tlv.decodeTlvs(payload);
  const m = { type, nodes: [] };
  for (const e of entries) {
    switch (e.type) {
      case TLV.MSG_SEQ_NUMBER:
        m.seq = tlv.decodeU32(e);
        break;
      case TLV.CLUSTER_NAME:
        m.clusterName = tlv.decodeString(e);
        break;
      case TLV.TLS_SUPPORTED:
        m.tlsSupported = tlv.decodeTlsSupported(e);
        break;
      case TLV.TLS_CLIENT_CERT_REQUIRED:
        m.tlsClientCertRequired = tlv.decodeU8(e);
        break;
      case TLV.SUPPORTED_MESSAGES:
        m.supportedMessages = tlv.decodeU16Array(e);
        break;
      case TLV.SUPPORTED_OPTIONS:
        m.supportedOptions = tlv.decodeU16Array(e);
        break;
      case TLV.REPLY_ERROR_CODE:
        m.errorCode = tlv.decodeU16(e);
        break;
      case TLV.SERVER_MAXIMUM_REQUEST_SIZE:
        m.maxRequest = tlv.decodeU32(e);
        break;
      case TLV.SERVER_MAXIMUM_REPLY_SIZE:
        m.maxReply = tlv.decodeU32(e);
        break;
      case TLV.NODE_ID:
        m.nodeId = tlv.decodeU32(e);
        break;
      case TLV.SUPPORTED_DECISION_ALGORITHMS:
        m.supportedAlgorithms = tlv.decodeU16Array(e);
        break;
      case TLV.DECISION_ALGORITHM:
        m.algorithm = tlv.decodeU16(e);
        break;
      case TLV.HEARTBEAT_INTERVAL:
        m.heartbeatInterval = tlv.decodeU32(e);
        break;
      case TLV.RING_ID:
        m.ringId = tlv.decodeRingId(e);
        break;
      case TLV.CONFIG_VERSION:
        m.configVersion = tlv.decodeU64(e);
        break;
      case TLV.DATA_CENTER_ID:
        m.dataCenterId = tlv.decodeU32(e);
        break;
      case TLV.NODE_STATE:
        m.nodeState = tlv.decodeNodeState(e);
        break;
      case TLV.NODE_INFO:
        m.nodes.push(tlv.decodeNodeInfo(e));
        break;
      case TLV.NODE_LIST_TYPE:
        m.listType = tlv.decodeNodeListType(e);
        break;
      case TLV.VOTE:
        m.vote = tlv.decodeVote(e);
        break;
      case TLV.QUORATE:
        m.quorate = tlv.decodeQuorate(e);
        break;
      case TLV.TIE_BREAKER:
        m.tieBreaker = tlv.decodeTieBreaker(e);
        break;
      case TLV.HEURISTICS:
        m.heuristics = tlv.decodeHeuristics(e);
        break;
      case TLV.KEEP_ACTIVE_PARTITION_TIE_BREAKER:
        m.kapTb = tlv.decodeKapTb(e);
        break;
      default:
        break; // ignore unknown options — backward compat by design
    }
  }
  return m;
}

module.exports = {
  HEADER_LEN,
  MsgError,
  encodeMsg,
  peekHeader,
  splitFrame,
  decodeMessage,
  preinit,
  preinitReply,
  starttls,
  init,
  initReply,
  serverError,
  echoRequest,
  echoReply,
  nodeList,
  nodeListReply,
  askForVote,
  askForVoteReply,
  voteInfo,
  voteInfoReply,
  heuristicsChange,
  heuristicsChangeReply,
  SUPPORTED_ALGORITHMS_REF: SUPPORTED_ALGORITHMS,
};
