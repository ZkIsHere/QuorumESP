'use strict';

/*
 * Connection lifecycle tests over real loopback TCP:
 * handshake, malformed → fail-closed, disconnect/reconnect,
 * multiple concurrent clients, oversize rejection, read timeout.
 */

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const {
  MSG,
  REPLY_ERROR,
  ALGO,
  VOTE,
  NODE_LIST_TYPE,
  TIE_BREAKER_MODE,
  DEFAULTS,
} = require('../src/consts');
const msg = require('../src/msg');
const net = require('../src/net');

const RING = { nodeId: 1, seq: 1n };
const TB = { mode: TIE_BREAKER_MODE.LOWEST, nodeId: 0 };

function initMsg(seq = 2, extra = {}) {
  return msg.init({
    seq,
    algorithm: ALGO.FFSPLIT,
    nodeId: 1,
    heartbeatInterval: 10000,
    tieBreaker: TB,
    ringId: RING,
    ...extra,
  });
}

async function handshake(sock, cluster = 'test-cluster') {
  await net.sendAll(sock, msg.preinit(cluster, 1));
  const pre = await net.readOneMessage(sock);
  assert.ok(pre, 'expected PREINIT_REPLY');
  assert.equal(msg.decodeMessage(pre.frame).type, MSG.PREINIT_REPLY);
  await net.sendAll(sock, initMsg());
  const init = await net.readOneMessage(sock);
  assert.ok(init, 'expected INIT_REPLY');
  assert.equal(msg.decodeMessage(init.frame).type, MSG.INIT_REPLY);
}

describe('fake qnetd over TCP', () => {
  let qnetd;
  before(async () => {
    qnetd = await net.startFakeQnetd({});
  });
  after(async () => {
    await qnetd.close();
  });

  it('completes PREINIT -> INIT handshake on a live connection', async () => {
    const sock = await net.connectWithTimeout('127.0.0.1', qnetd.port);
    try {
      await handshake(sock);
      await net.sendAll(sock, msg.echoRequest(7));
      const echo = await net.readOneMessage(sock);
      assert.equal(msg.decodeMessage(echo.frame).type, MSG.ECHO_REPLY);
      assert.equal(msg.decodeMessage(echo.frame).seq, 7);
    } finally {
      sock.destroy();
    }
  });

  it('answers node list and vote requests after handshake', async () => {
    const sock = await net.connectWithTimeout('127.0.0.1', qnetd.port);
    try {
      await handshake(sock);
      await net.sendAll(
        sock,
        msg.nodeList({ seq: 20, listType: NODE_LIST_TYPE.MEMBERSHIP, ringId: RING, nodes: [{ nodeId: 1 }] })
      );
      const nl = await net.readOneMessage(sock);
      assert.equal(msg.decodeMessage(nl.frame).type, MSG.NODE_LIST_REPLY);
      await net.sendAll(sock, msg.askForVote(21));
      const afv = await net.readOneMessage(sock);
      assert.equal(msg.decodeMessage(afv.frame).vote, VOTE.ACK);
    } finally {
      sock.destroy();
    }
  });

  it('destroys the transport on framing-level garbage (invalid type)', async () => {
    const sock = await net.connectWithTimeout('127.0.0.1', qnetd.port);
    try {
      // Type 99 is outside 0–17: rejected before any session exists.
      await net.sendAll(sock, Buffer.from([0x00, 0x63, 0x00, 0x00, 0x00, 0x02, 0xde, 0xad]));
      const res = await net.readOneMessage(sock, { timeoutMs: 1500 });
      assert.equal(res, null, 'framing violation must drop the connection with no reply');
      const sess = qnetd.sessions[qnetd.sessions.length - 1].session;
      assert.equal(sess.isActive, false);
    } finally {
      sock.destroy();
    }
  });

  it('answers session-level violations with SERVER_ERROR and stays usable', async () => {
    const sock = await net.connectWithTimeout('127.0.0.1', qnetd.port);
    try {
      // Valid frame, wrong state: ECHO before any handshake.
      await net.sendAll(sock, msg.echoRequest(1));
      const res = await net.readOneMessage(sock, { timeoutMs: 1500 });
      assert.ok(res, 'server must answer with SERVER_ERROR, not drop');
      const d = msg.decodeMessage(res.frame);
      assert.equal(d.type, MSG.SERVER_ERROR);
      assert.equal(d.errorCode, REPLY_ERROR.PREINIT_REQUIRED);
      // Same connection recovers with a valid handshake (reference stays up).
      await handshake(sock, 'recovered-after-error');
    } finally {
      sock.destroy();
    }
  });

  it('answers INIT-before-PREINIT with error and the same connection recovers', async () => {
    const sock = await net.connectWithTimeout('127.0.0.1', qnetd.port);
    try {
      await net.sendAll(sock, initMsg(1));
      const res = await net.readOneMessage(sock, { timeoutMs: 1500 });
      assert.ok(res, 'expected SERVER_ERROR for out-of-order INIT');
      assert.equal(msg.decodeMessage(res.frame).type, MSG.SERVER_ERROR);
      await handshake(sock, 'reconnect-cluster');
    } finally {
      sock.destroy();
    }
  });

  it('isolates multiple concurrent clients', async () => {
    const socks = await Promise.all(
      ['cluster-a', 'cluster-b', 'cluster-c'].map((c) =>
        net.connectWithTimeout('127.0.0.1', qnetd.port).then(async (s) => ({ s, c }))
      )
    );
    try {
      await Promise.all(socks.map(({ s, c }) => handshake(s, c)));
      await Promise.all(
        socks.map(async ({ s }, i) => {
          await net.sendAll(s, msg.echoRequest(100 + i));
          const r = await net.readOneMessage(s);
          assert.equal(msg.decodeMessage(r.frame).seq, 100 + i);
        })
      );
      const names = qnetd.sessions.slice(-3).map((e) => e.session.clusterName).sort();
      assert.deepEqual(names, ['cluster-a', 'cluster-b', 'cluster-c']);
    } finally {
      socks.forEach(({ s }) => s.destroy());
    }
  });

  it('rejects oversize frames without reading the body', async () => {
    const small = await net.startFakeQnetd({ sessionOptions: { maxReceiveSize: 64 } });
    const sock = await net.connectWithTimeout('127.0.0.1', small.port);
    try {
      await net.sendAll(sock, msg.preinit('x'.repeat(200), 1));
      const res = await net.readOneMessage(sock, { timeoutMs: 1500 });
      assert.equal(res, null, 'oversize PREINIT must be dropped');
    } finally {
      sock.destroy();
      await small.close();
    }
  });

  it('read timeout returns null instead of assuming the peer alive', async () => {
    const sock = await net.connectWithTimeout('127.0.0.1', qnetd.port);
    try {
      await handshake(sock);
      const res = await net.readOneMessage(sock, { timeoutMs: 300 });
      assert.equal(res, null);
      // Connection still usable afterwards — timeout was local, not a verdict.
      await net.sendAll(sock, msg.echoRequest(55));
      const echo = await net.readOneMessage(sock);
      assert.equal(msg.decodeMessage(echo.frame).seq, 55);
    } finally {
      sock.destroy();
    }
  });

  it('server observes disconnect and invalidates the session', async () => {
    const seen = [];
    const srv = await net.startFakeQnetd({ onSession: (session) => seen.push(session) });
    const sock = await net.connectWithTimeout('127.0.0.1', srv.port);
    try {
      await handshake(sock);
    } finally {
      sock.destroy(); // abrupt close, no goodbye in protocol
    }
    await new Promise((r) => setTimeout(r, 300));
    assert.equal(seen.length, 1);
    assert.equal(seen[0].isClosed, true);
    assert.match(seen[0].closedReason, /transport closed/);
    await srv.close();
  });

  it('split writes (partial frames) are reassembled', async () => {
    const sock = await net.connectWithTimeout('127.0.0.1', qnetd.port);
    try {
      const frame = msg.preinit('split-cluster', 1);
      sock.write(frame.subarray(0, 3));
      await new Promise((r) => setTimeout(r, 50));
      sock.write(frame.subarray(3));
      const pre = await net.readOneMessage(sock);
      assert.equal(msg.decodeMessage(pre.frame).type, MSG.PREINIT_REPLY);
    } finally {
      sock.destroy();
    }
    void DEFAULTS;
  });
});
