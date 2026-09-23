'use strict';

/* Message framing tests — byte-exact header + builders vs msg.c. */

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const {
  MSG,
  TLV,
  TLS,
  REPLY_ERROR,
  ALGO,
  VOTE,
  NODE_STATE,
  NODE_LIST_TYPE,
  QUORATE,
  TIE_BREAKER_MODE,
  HEURISTICS,
  DEFAULTS,
} = require('../src/consts');
const msg = require('../src/msg');
const tlv = require('../src/tlv');

const RING = { nodeId: 1, seq: 42n };
const TB = { mode: TIE_BREAKER_MODE.LOWEST, nodeId: 0 };

function fullInit(seq = 7) {
  return msg.init({
    seq,
    algorithm: ALGO.FFSPLIT,
    nodeId: 1,
    heartbeatInterval: 10000,
    tieBreaker: TB,
    ringId: RING,
  });
}

describe('message header', () => {
  it('is 6 bytes: u16 type BE + u32 payload-len BE', () => {
    const f = msg.preinit('c1', 1);
    assert.equal(f.readUInt16BE(0), MSG.PREINIT);
    assert.equal(f.readUInt32BE(2), f.length - 6);
    assert.equal(msg.HEADER_LEN, 6);
  });

  it('rejects invalid types and oversize frames', () => {
    const badType = Buffer.alloc(6);
    badType.writeUInt16BE(99, 0);
    assert.throws(() => msg.splitFrame(badType), msg.MsgError);
    const big = Buffer.alloc(6);
    big.writeUInt16BE(MSG.ECHO_REQUEST, 0);
    big.writeUInt32BE(DEFAULTS.QNETD_MAX_RECEIVE_SIZE, 2); // 6 + len > max → too long
    assert.throws(() => msg.splitFrame(big), msg.MsgError);
    assert.throws(() => msg.splitFrame(Buffer.from([1, 2, 3])), msg.MsgError);
    assert.throws(() => msg.encodeMsg(99), msg.MsgError);
  });

  it('peekHeader returns null until 6 bytes arrive', () => {
    assert.equal(msg.peekHeader(Buffer.alloc(5)), null);
    assert.deepEqual(msg.peekHeader(msg.echoRequest(1)).type !== undefined ? { ok: 1 } : null, {
      ok: 1,
    });
  });
});

describe('builders round-trip through decodeMessage', () => {
  it('PREINIT carries cluster_name + seq', () => {
    const d = msg.decodeMessage(msg.preinit('proxmox-home', 11));
    assert.equal(d.type, MSG.PREINIT);
    assert.equal(d.clusterName, 'proxmox-home');
    assert.equal(d.seq, 11);
  });

  it('PREINIT_REPLY carries tls advertisement', () => {
    const d = msg.decodeMessage(msg.preinitReply(TLS.REQUIRED, true, 11));
    assert.equal(d.type, MSG.PREINIT_REPLY);
    assert.equal(d.tlsSupported, TLS.REQUIRED);
    assert.equal(d.tlsClientCertRequired, 1);
    assert.equal(d.seq, 11);
  });

  it('INIT carries negotiation fields in msg.c order', () => {
    const raw = fullInit();
    const d = msg.decodeMessage(raw);
    assert.equal(d.type, MSG.INIT);
    assert.equal(d.seq, 7);
    assert.ok(d.supportedMessages.includes(MSG.NODE_LIST));
    assert.ok(d.supportedOptions.includes(TLV.RING_ID));
    assert.equal(d.nodeId, 1);
    assert.equal(d.algorithm, ALGO.FFSPLIT);
    assert.equal(d.heartbeatInterval, 10000);
    assert.deepEqual(d.tieBreaker, TB);
    assert.deepEqual(d.ringId, RING);
  });

  it('INIT_REPLY carries limits + algorithms', () => {
    const raw = msg.initReply({
      seq: 7,
      errorCode: REPLY_ERROR.NO_ERROR,
      maxRequest: 32768,
      maxReply: 32768,
      algorithms: [ALGO.FFSPLIT, ALGO.LMS],
    });
    const d = msg.decodeMessage(raw);
    assert.equal(d.type, MSG.INIT_REPLY);
    assert.equal(d.errorCode, REPLY_ERROR.NO_ERROR);
    assert.equal(d.maxRequest, 32768);
    assert.deepEqual(d.supportedAlgorithms, [ALGO.FFSPLIT, ALGO.LMS]);
  });

  it('ECHO_REPLY is a byte copy of the request with type overwritten', () => {
    const req = msg.echoRequest(1234);
    const rep = msg.echoReply(req);
    assert.equal(rep.readUInt16BE(0), MSG.ECHO_REPLY);
    assert.deepEqual(rep.subarray(2), req.subarray(2));
    const d = msg.decodeMessage(rep);
    assert.equal(d.type, MSG.ECHO_REPLY);
    assert.equal(d.seq, 1234);
    assert.throws(() => msg.echoReply(msg.preinit('x')), msg.MsgError);
  });

  it('NODE_LIST carries type/ring/config/quorate/heuristics/nodes', () => {
    const raw = msg.nodeList({
      seq: 9,
      listType: NODE_LIST_TYPE.MEMBERSHIP,
      ringId: RING,
      configVersion: 3n,
      quorate: QUORATE.QUORATE,
      heuristics: HEURISTICS.PASS,
      nodes: [
        { nodeId: 1, dataCenterId: 0, nodeState: NODE_STATE.MEMBER },
        { nodeId: 2, dataCenterId: 0, nodeState: NODE_STATE.DEAD },
      ],
    });
    const d = msg.decodeMessage(raw);
    assert.equal(d.type, MSG.NODE_LIST);
    assert.equal(d.listType, NODE_LIST_TYPE.MEMBERSHIP);
    assert.deepEqual(d.ringId, RING);
    assert.equal(d.configVersion, 3n);
    assert.equal(d.quorate, QUORATE.QUORATE);
    assert.equal(d.heuristics, HEURISTICS.PASS);
    assert.deepEqual(d.nodes, [
      { nodeId: 1, dataCenterId: 0, nodeState: NODE_STATE.MEMBER },
      { nodeId: 2, dataCenterId: 0, nodeState: NODE_STATE.DEAD },
    ]);
  });

  it('vote-bearing replies carry seq + ring + vote', () => {
    const nlr = msg.decodeMessage(msg.nodeListReply(5, NODE_LIST_TYPE.QUORUM, RING, VOTE.ACK));
    assert.deepEqual([nlr.type, nlr.seq, nlr.vote], [MSG.NODE_LIST_REPLY, 5, VOTE.ACK]);
    assert.deepEqual(nlr.ringId, RING);
    const afvr = msg.decodeMessage(msg.askForVoteReply(6, RING, VOTE.NACK));
    assert.deepEqual([afvr.type, afvr.vote], [MSG.ASK_FOR_VOTE_REPLY, VOTE.NACK]);
    const hcr = msg.decodeMessage(
      msg.heuristicsChangeReply(6, RING, HEURISTICS.FAIL, VOTE.ASK_LATER)
    );
    assert.equal(hcr.heuristics, HEURISTICS.FAIL);
    assert.equal(hcr.vote, VOTE.ASK_LATER);
  });

  it('SERVER_ERROR carries reply_error_code', () => {
    const d = msg.decodeMessage(msg.serverError(REPLY_ERROR.TLS_REQUIRED, 3));
    assert.equal(d.type, MSG.SERVER_ERROR);
    assert.equal(d.errorCode, REPLY_ERROR.TLS_REQUIRED);
  });
});

describe('malformed messages (fail-closed)', () => {
  it('ignores unknown TLV options (backward compat by design)', () => {
    const extra = tlv.encodeTlv(65000, Buffer.from([1, 2, 3]));
    const raw = msg.encodeMsg(MSG.ECHO_REQUEST, [tlv.encodeU32(TLV.MSG_SEQ_NUMBER, 1), extra]);
    const d = msg.decodeMessage(raw);
    assert.equal(d.seq, 1);
  });

  it('rejects TLV overrun inside a frame', () => {
    const badPayload = Buffer.from([0x00, 0x09, 0x00, 0x04, 0x01]); // claims 4, has 1
    const frame = Buffer.alloc(6 + badPayload.length);
    frame.writeUInt16BE(MSG.ECHO_REQUEST, 0);
    frame.writeUInt32BE(badPayload.length, 2);
    badPayload.copy(frame, 6);
    assert.throws(() => msg.decodeMessage(frame), tlv.TlvError);
  });

  it('rejects truncated frames and length lies', () => {
    const full = msg.preinit('abc', 1);
    assert.throws(() => msg.decodeMessage(full.subarray(0, full.length - 1)), msg.MsgError);
    const lying = Buffer.from(full);
    lying.writeUInt32BE(1000, 2);
    assert.throws(() => msg.decodeMessage(lying), msg.MsgError);
  });
});
