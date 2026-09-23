'use strict';

/* Session state machine tests — transitions, TLS policy, heartbeat, fail-closed. */

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const {
  MSG,
  TLS,
  REPLY_ERROR,
  ALGO,
  VOTE,
  NODE_LIST_TYPE,
  TIE_BREAKER_MODE,
  HEURISTICS,
  DEFAULTS,
} = require('../src/consts');
const msg = require('../src/msg');
const { QnetSession, SessionError, STATE, clientShouldStartTls } = require('../src/session');

const RING = { nodeId: 1, seq: 1n };
const TB = { mode: TIE_BREAKER_MODE.LOWEST, nodeId: 0 };

function newSession(opts = {}) {
  let now = 1000000;
  const s = new QnetSession({ ...opts, nowMs: () => now });
  return { s, advance: (ms) => (now += ms) };
}

function doHandshake(s, { cluster = 'c1', algorithm = ALGO.FFSPLIT, heartbeat = 10000 } = {}) {
  let r = s.handle(msg.preinit(cluster, 1));
  assert.equal(msg.decodeMessage(r).type, MSG.PREINIT_REPLY);
  r = s.handle(
    msg.init({ seq: 2, algorithm, nodeId: 1, heartbeatInterval: heartbeat, tieBreaker: TB, ringId: RING })
  );
  const d = msg.decodeMessage(r);
  assert.equal(d.type, MSG.INIT_REPLY);
  assert.equal(d.errorCode, REPLY_ERROR.NO_ERROR);
  assert.equal(s.state, STATE.ACTIVE);
  return r;
}

describe('handshake', () => {
  it('PREINIT -> INIT reaches ACTIVE with sane INIT_REPLY', () => {
    const { s } = newSession();
    doHandshake(s);
    assert.equal(s.clusterName, 'c1');
    assert.equal(s.nodeId, 1);
  });

  it('rejects INIT before PREINIT (INIT_REQUIRED path)', () => {
    const { s } = newSession();
    assert.throws(
      () =>
        s.handle(
          msg.init({
            seq: 1,
            algorithm: ALGO.FFSPLIT,
            nodeId: 1,
            heartbeatInterval: 10000,
            tieBreaker: TB,
            ringId: RING,
          })
        ),
      SessionError
    );
    assert.notEqual(s.state, STATE.ACTIVE);
  });

  it('rejects empty cluster_name', () => {
    const { s } = newSession();
    assert.throws(() => s.handle(msg.preinit('', 1)), SessionError);
  });

  it('rejects second PREINIT/INIT once ACTIVE (unexpected message)', () => {
    const { s } = newSession();
    doHandshake(s);
    assert.throws(() => s.handle(msg.preinit('c1', 9)), SessionError);
    assert.throws(
      () =>
        s.handle(
          msg.init({
            seq: 9,
            algorithm: ALGO.FFSPLIT,
            nodeId: 1,
            heartbeatInterval: 10000,
            tieBreaker: TB,
            ringId: RING,
          })
        ),
      SessionError
    );
  });

  it('rejects unsupported algorithm and out-of-range heartbeat', () => {
    const { s: s1 } = newSession();
    s1.handle(msg.preinit('c1', 1));
    assert.throws(
      () =>
        s1.handle(
          msg.init({
            seq: 2,
            algorithm: ALGO.TEST,
            nodeId: 1,
            heartbeatInterval: 10000,
            tieBreaker: TB,
            ringId: RING,
          })
        ),
      SessionError
    );
    const { s: s2 } = newSession();
    s2.handle(msg.preinit('c1', 1));
    assert.throws(
      () =>
        s2.handle(
          msg.init({
            seq: 2,
            algorithm: ALGO.FFSPLIT,
            nodeId: 1,
            heartbeatInterval: 999,
            tieBreaker: TB,
            ringId: RING,
          })
        ),
      SessionError
    );
    const { s: s3 } = newSession();
    s3.handle(msg.preinit('c1', 1));
    assert.throws(
      () =>
        s3.handle(
          msg.init({
            seq: 2,
            algorithm: ALGO.FFSPLIT,
            nodeId: 0,
            heartbeatInterval: 10000,
            tieBreaker: TB,
            ringId: RING,
          })
        ),
      SessionError
    );
  });

  it('rejects garbage bytes (decode failure, never ACTIVE)', () => {
    const { s } = newSession();
    assert.throws(() => s.handle(Buffer.from([0xff, 0xff, 0, 0, 0, 5, 1])), SessionError);
    assert.equal(s.isActive, false);
  });

  it('rejects messages after close', () => {
    const { s } = newSession();
    doHandshake(s);
    s.close('test');
    assert.throws(() => s.handle(msg.echoRequest(1)), SessionError);
  });
});

describe('TLS policy matrix', () => {
  it('server off: STARTTLS is rejected', () => {
    const { s } = newSession({ tlsMode: 'off' });
    s.handle(msg.preinit('c1', 1));
    assert.throws(() => s.handle(msg.starttls(2)), SessionError);
  });

  it('server req: INIT without STARTTLS fails with TLS_REQUIRED', () => {
    const { s } = newSession({ tlsMode: 'req' });
    s.handle(msg.preinit('c1', 1));
    try {
      s.handle(
        msg.init({
          seq: 2,
          algorithm: ALGO.FFSPLIT,
          nodeId: 1,
          heartbeatInterval: 10000,
          tieBreaker: TB,
          ringId: RING,
        })
      );
      assert.fail('expected TLS_REQUIRED');
    } catch (err) {
      assert.equal(err.errorCode, REPLY_ERROR.TLS_REQUIRED);
    }
  });

  it('server req: STARTTLS then INIT succeeds (transport upgrade out of band)', () => {
    const { s } = newSession({ tlsMode: 'req' });
    s.handle(msg.preinit('c1', 1));
    assert.equal(s.handle(msg.starttls(2)), null); // no STARTTLS reply in protocol
    assert.equal(s.state, STATE.TLS_UPGRADED);
    const r = s.handle(
      msg.init({
        seq: 3,
        algorithm: ALGO.FFSPLIT,
        nodeId: 1,
        heartbeatInterval: 10000,
        tieBreaker: TB,
        ringId: RING,
      })
    );
    assert.equal(msg.decodeMessage(r).type, MSG.INIT_REPLY);
    assert.equal(s.isActive, true);
  });

  it('client decision table mirrors qdevice tls on/off/required', () => {
    assert.equal(clientShouldStartTls('off', TLS.REQUIRED), 'abort');
    assert.equal(clientShouldStartTls('off', TLS.SUPPORTED), 'plaintext');
    assert.equal(clientShouldStartTls('on', TLS.SUPPORTED), 'starttls');
    assert.equal(clientShouldStartTls('on', TLS.UNSUPPORTED), 'plaintext');
    assert.equal(clientShouldStartTls('required', TLS.REQUIRED), 'starttls');
    assert.equal(clientShouldStartTls('required', TLS.UNSUPPORTED), 'abort');
  });
});

describe('ACTIVE behavior', () => {
  it('answers ECHO with byte-copy reply preserving seq', () => {
    const { s } = newSession();
    doHandshake(s);
    const rep = s.handle(msg.echoRequest(4242));
    assert.equal(rep.readUInt16BE(0), MSG.ECHO_REPLY);
    assert.equal(msg.decodeMessage(rep).seq, 4242);
  });

  it('answers NODE_LIST / ASK_FOR_VOTE / HEURISTICS_CHANGE with fixed test vote', () => {
    const { s } = newSession({ fixedVote: VOTE.ACK });
    doHandshake(s);
    const nl = msg.decodeMessage(
      s.handle(
        msg.nodeList({
          seq: 10,
          listType: NODE_LIST_TYPE.MEMBERSHIP,
          ringId: RING,
          nodes: [{ nodeId: 1 }],
        })
      )
    );
    assert.equal(nl.type, MSG.NODE_LIST_REPLY);
    assert.equal(nl.vote, VOTE.ACK);
    const afv = msg.decodeMessage(s.handle(msg.askForVote(11)));
    assert.equal(afv.vote, VOTE.ACK);
    const hc = msg.decodeMessage(s.handle(msg.heuristicsChange(12, HEURISTICS.PASS)));
    assert.equal(hc.type, MSG.HEURISTICS_CHANGE_REPLY);
    assert.equal(hc.heuristics, HEURISTICS.PASS);
  });

  it('heartbeat/DPD: alive within interval*1.5, dead after', () => {
    const { s, advance } = newSession();
    doHandshake(s, { heartbeat: 10000 });
    assert.equal(s.isAlive(), true);
    advance(15000);
    assert.equal(s.isAlive(), true); // exactly at boundary
    advance(1);
    assert.equal(s.isAlive(), false);
    // any valid message refreshes liveness
    s.handle(msg.echoRequest(99));
    assert.equal(s.isAlive(), true);
    void DEFAULTS;
  });
});
