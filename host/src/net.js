'use strict';

/*
 * QuorumESP Phase 1 — socket helpers + fake qnetd server for tests.
 *
 * Framing follows msgio_read (msgio.c): read the 6-byte header first,
 * validate type + size, then read the rest. Oversize/invalid frames are
 * rejected before use (fail-closed). stdlib `net` only.
 *
 * Source: https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/msgio.c
 */

const net = require('node:net');
const { DEFAULTS } = require('./consts');
const msg = require('./msg');
const { QnetSession } = require('./session');

class NetError extends Error {
  constructor(message) {
    super(message);
    this.name = 'NetError';
  }
}

function connectWithTimeout(host, port, timeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    const sock = net.connect(port, host);
    const timer = setTimeout(() => {
      sock.destroy();
      reject(new NetError(`connect timeout after ${timeoutMs}ms`));
    }, timeoutMs);
    sock.once('connect', () => {
      clearTimeout(timer);
      resolve(sock);
    });
    sock.once('error', (err) => {
      clearTimeout(timer);
      reject(err);
    });
  });
}

function sendAll(sock, buf) {
  return new Promise((resolve, reject) => {
    sock.write(buf, (err) => (err ? reject(err) : resolve()));
  });
}

/* Validate a parsed header against policy (mirror msgio_read checks). */
function checkHeader(h, maxSize) {
  if (h.type > 17) {
    throw new NetError(`invalid message type ${h.type}`);
  }
  if (msg.HEADER_LEN + h.len > maxSize) {
    throw new NetError(`message too long: ${msg.HEADER_LEN + h.len} > ${maxSize}`);
  }
}

/*
 * Read exactly one complete message frame.
 * Resolves { frame } or null on clean EOF / timeout (caller treats as
 * dead peer — never assume the peer is alive). Rejects NetError on
 * malformed header, invalid type, or oversize length.
 */
function readOneMessage(sock, { maxSize = DEFAULTS.QNETD_MAX_RECEIVE_SIZE, timeoutMs = 5000 } = {}) {
  return new Promise((resolve, reject) => {
    let acc = Buffer.alloc(0);
    let settled = false;
    const timer = setTimeout(() => {
      if (!settled) {
        settled = true;
        cleanup();
        resolve(null);
      }
    }, timeoutMs);

    function cleanup() {
      clearTimeout(timer);
      sock.removeListener('data', onData);
      sock.removeListener('end', onEnd);
      sock.removeListener('error', onError);
      sock.removeListener('close', onClose);
    }
    function done(value) {
      if (!settled) {
        settled = true;
        cleanup();
        resolve(value);
      }
    }
    function fail(err) {
      if (!settled) {
        settled = true;
        cleanup();
        reject(err);
      }
    }

    function onData(chunk) {
      acc = Buffer.concat([acc, chunk]);
      if (acc.length < msg.HEADER_LEN) {
        return;
      }
      let h;
      try {
        h = msg.peekHeader(acc);
        checkHeader(h, maxSize);
      } catch (err) {
        fail(err instanceof NetError ? err : new NetError(`bad header: ${err.message}`));
        return;
      }
      if (acc.length >= msg.HEADER_LEN + h.len) {
        done({ frame: Buffer.from(acc.subarray(0, msg.HEADER_LEN + h.len)) });
      }
    }
    function onEnd() {
      done(null);
    }
    function onClose() {
      done(null);
    }
    function onError(err) {
      fail(new NetError(`socket error: ${err.message}`));
    }

    sock.on('data', onData);
    sock.once('end', onEnd);
    sock.once('error', onError);
    sock.once('close', onClose);
  });
}

/*
 * Minimal fake qnetd server: one QnetSession per socket, replies per the
 * state machine, destroys the socket fail-closed on any violation.
 * Returns { server, port, sessions, close() }.
 */
function startFakeQnetd({ port = 0, host = '127.0.0.1', sessionOptions = {}, onSession = null } = {}) {
  const sessions = [];
  const server = net.createServer((sock) => {
    const session = new QnetSession(sessionOptions);
    sessions.push({ session, sock });
    if (onSession) {
      onSession(session, sock);
    }
    let acc = Buffer.alloc(0);
    let dead = false;

    function kill(reason) {
      if (dead) {
        return;
      }
      dead = true;
      session.close(reason);
      sock.destroy();
    }

    sock.on('data', (chunk) => {
      if (dead) {
        return;
      }
      acc = Buffer.concat([acc, chunk]);
      for (;;) {
        if (acc.length < msg.HEADER_LEN) {
          return;
        }
        const h = msg.peekHeader(acc);
        try {
          checkHeader(h, session.maxReceiveSize);
        } catch (err) {
          kill(`framing: ${err.message}`);
          return;
        }
        if (acc.length < msg.HEADER_LEN + h.len) {
          return; // wait for rest of body
        }
        const frame = Buffer.from(acc.subarray(0, msg.HEADER_LEN + h.len));
        acc = acc.subarray(msg.HEADER_LEN + h.len);
        let reply = null;
        try {
          // Protocol errors come back as SERVER_ERROR/INIT_REPLY replies;
          // only misuse (use-after-close) throws → destroy transport.
          reply = session.handle(frame);
        } catch (err) {
          kill(err.message);
          return;
        }
        if (session.isClosed) {
          kill('session closed');
          return;
        }
        if (reply) {
          sock.write(reply);
        }
      }
    });
    sock.on('error', () => kill('socket error'));
    sock.on('close', () => session.close('transport closed'));
  });

  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, () => {
      resolve({
        server,
        port: server.address().port,
        sessions,
        close: () => new Promise((res) => server.close(res)),
      });
    });
  });
}

module.exports = { NetError, connectWithTimeout, sendAll, readOneMessage, startFakeQnetd };
