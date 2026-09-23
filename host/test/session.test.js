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

  it('answers INIT before PREINIT with PREINIT_REQUIRED and stays CONNECTED', () => {
    const { s } = newSession();
    const r = s.handle(
      msg.init({
        seq: 1,
        algorithm: ALGO.FFSPLIT,
        nodeId: 1,
        heartbeatInterval: 10000,
        tieBreaker: TB,
        ringId: RING,
      })
    );
    const d = msg.decodeMessage(r);
    assert.equal(d.type, MSG.SERVER_ERROR);
    assert.equal(d.errorCode, REPLY_ERROR.PREINIT_REQUIRED);
    assert.equal(s.state, STATE.CONNECTED);
    // Recovery on the same connection still works (reference stays up).
    doHandshake(s);
  });

  it('answers empty cluster_name with DOESNT_CONTAIN_REQUIRED_OPTION', () => {
    const { s } = newSession();
    const d = msg.decodeMessage(s.handle(msg.preinit('', 1)));
    assert.equal(d.type, MSG.SERVER_ERROR);
    assert.equal(d.errorCode, REPLY_ERROR.DOESNT_CONTAIN_REQUIRED_OPTION);
    assert.equal(s.state, STATE.CONNECTED);
  });

  it('answers second PREINIT/INIT once ACTIVE with UNEXPECTED_MESSAGE', () => {
    const { s } = newSession();
    doHandshake(s);
    for (const f of [
      msg.preinit('c1', 9),
      msg.init({
        seq: 9,
        algorithm: ALGO.FFSPLIT,
        nodeId: 1,
        heartbeatInterval: 10000,
        tieBreaker: TB,
        ringId: RING,
      }),
    ]) {
      const d = msg.decodeMessage(s.handle(f));
      assert.equal(d.type, MSG.SERVER_ERROR);
      assert.equal(d.errorCode, REPLY_ERROR.UNEXPECTED_MESSAGE);
    }
    assert.equal(s.state, STATE.ACTIVE);
  });

  it('answers bad INIT with INIT_REPLY carrying the error code (reference behavior)', () => {
    const cases = [
      [{ algorithm: ALGO.TEST }, REPLY_ERROR.UNSUPPORTED_DECISION_ALGORITHM],
      [{ heartbeatInterval: 999 }, REPLY_ERROR.INVALID_HEARTBEAT_INTERVAL],
      [{ nodeId: 0 }, REPLY_ERROR.DOESNT_CONTAIN_REQUIRED_OPTION],
    ];
    for (const [override, expectedCode] of cases) {
      const { s } = newSession();
      s.handle(msg.preinit('c1', 1));
      const d = msg.decodeMessage(
        s.handle(
          msg.init({
            seq: 2,
            algorithm: ALGO.FFSPLIT,
            nodeId: 1,
            heartbeatInterval: 10000,
            tieBreaker: TB,
            ringId: RING,
            ...override,
          })
        )
      );
      assert.equal(d.type, MSG.INIT_REPLY);
      assert.equal(d.errorCode, expectedCode);
      assert.equal(s.state, STATE.PREINIT_DONE);
    }
  });

  it('answers garbage bytes with ERROR_DECODING_MSG and never goes ACTIVE', () => {
    const { s } = newSession();
    const d = msg.decodeMessage(s.handle(Buffer.from([0xff, 0xff, 0, 0, 0, 5, 1])));
    assert.equal(d.type, MSG.SERVER_ERROR);
    assert.equal(d.errorCode, REPLY_ERROR.ERROR_DECODING_MSG);
    assert.equal(s.isActive, false);
    assert.equal(s.state, STATE.CONNECTED);
  });

  it('rejects messages after close', () => {
    const { s } = newSession();
    doHandshake(s);
    s.close('test');
    assert.throws(() => s.handle(msg.echoRequest(1)), SessionError);
  });
});

describe('TLS policy matrix', () => {
  it('server off: STARTTLS gets UNSUPPORTED_MESSAGE, connection stays up', () => {
    const { s } = newSession({ tlsMode: 'off' });
    s.handle(msg.preinit('c1', 1));
    const d = msg.decodeMessage(s.handle(msg.starttls(2)));
    assert.equal(d.type, MSG.SERVER_ERROR);
    assert.equal(d.errorCode, REPLY_ERROR.UNSUPPORTED_MESSAGE);
    assert.equal(s.state, STATE.PREINIT_DONE);
  });

  it('server req: INIT without STARTTLS gets TLS_REQUIRED, then recovers', () => {
    const { s } = newSession({ tlsMode: 'req' });
    s.handle(msg.preinit('c1', 1));
    const d = msg.decodeMessage(
      s.handle(
        msg.init({
          seq: 2,
          algorithm: ALGO.FFSPLIT,
          nodeId: 1,
          heartbeatInterval: 10000,
          tieBreaker: TB,
          ringId: RING,
        })
      )
    );
    assert.equal(d.type, MSG.SERVER_ERROR);
    assert.equal(d.errorCode, REPLY_ERROR.TLS_REQUIRED);
    assert.equal(s.state, STATE.PREINIT_DONE);
    // Client does STARTTLS and retries INIT on the same connection.
    assert.equal(s.handle(msg.starttls(3)), null);
    const ok = msg.decodeMessage(
      s.handle(
        msg.init({
          seq: 4,
          algorithm: ALGO.FFSPLIT,
          nodeId: 1,
          heartbeatInterval: 10000,
          tieBreaker: TB,
          ringId: RING,
        })
      )
    );
    assert.equal(ok.type, MSG.INIT_REPLY);
    assert.equal(ok.errorCode, REPLY_ERROR.NO_ERROR);
    assert.equal(s.isActive, true);
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

  it('answers ring-less INITIAL_CONFIG/QUORUM with the last known ring (real-client behavior)', () => {
    const { s } = newSession({ fixedVote: VOTE.ACK });
    doHandshake(s);
    // INITIAL_CONFIG without ring → falls back to INIT ring.
    const nl = msg.decodeMessage(
      s.handle(
        msg.nodeList({ seq: 10, listType: NODE_LIST_TYPE.INITIAL_CONFIG, nodes: [{ nodeId: 1 }] })
      )
    );
    assert.equal(nl.type, MSG.NODE_LIST_REPLY);
    assert.deepEqual(nl.ringId, RING);
    // MEMBERSHIP with a new ring updates the fallback.
    const ring2 = { nodeId: 1, seq: 99n };
    s.handle(
      msg.nodeList({ seq: 11, listType: NODE_LIST_TYPE.MEMBERSHIP, ringId: ring2, nodes: [{ nodeId: 1 }] })
    );
    const q = msg.decodeMessage(
      s.handle(
        msg.nodeList({
          seq: 12,
          listType: NODE_LIST_TYPE.QUORUM,
          quorate: 1,
          nodes: [{ nodeId: 1, nodeState: 1 }],
        })
      )
    );
    assert.deepEqual(q.ringId, ring2);
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
