#!/usr/bin/env node
'use strict';

/*
 * QuorumESP interop tool — fake qnetd that logs every frame.
 *
 * Purpose: receive a REAL corosync-qdevice client (Linux corosync cluster)
 * so we can verify byte-compatibility of the protocol implementation and
 * record the on-wire handshake. See docs/interop.md for the full procedure.
 *
 * Every frame is logged as one JSON line:
 *   { t, dir: "c2s"|"s2c", type, len, hex, decoded }
 * decoded is null when the frame does not parse (never trust it).
 *
 * Usage:
 *   node host/tools/fake-qnetd.js --port 5403 --vote ack --log frames.jsonl
 *
 * Notes:
 * - Plaintext only (guides the first interop round with `tls: off`).
 *   TLS interop needs a real socket upgrade + certs — follow-up work,
 *   see docs/interop.md §TLS.
 * - The reply vote (--vote) is a TEST STUB, not a quorum decision.
 */

const fs = require('node:fs');
const net = require('node:net');
const msg = require('../src/msg');
const { QnetSession } = require('../src/session');
const { VOTE } = require('../src/consts');

function parseArgs(argv) {
  const out = { port: 5403, host: '0.0.0.0', vote: 'ack', log: null };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--port') {
      out.port = Number(argv[++i]);
    } else if (a === '--host') {
      out.host = argv[++i];
    } else if (a === '--vote') {
      out.vote = argv[++i];
    } else if (a === '--log') {
      out.log = argv[++i];
    } else if (a === '--help' || a === '-h') {
      out.help = true;
    } else {
      throw new Error(`unknown arg ${a}`);
    }
  }
  return out;
}

const VOTES = { ack: VOTE.ACK, nack: VOTE.NACK, 'ask-later': VOTE.ASK_LATER, 'no-change': VOTE.NO_CHANGE };

function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (opts.help) {
    console.log('usage: fake-qnetd.js [--port N] [--host ADDR] [--vote ack|nack|ask-later|no-change] [--log FILE]');
    process.exit(0);
  }
  if (!Number.isInteger(opts.port) || opts.port < 0 || opts.port > 65535) {
    throw new Error(`bad --port ${opts.port}`);
  }
  if (!(opts.vote in VOTES)) {
    throw new Error(`bad --vote ${opts.vote} (want ${Object.keys(VOTES).join('|')})`);
  }
  const logStream = opts.log ? fs.createWriteStream(opts.log, { flags: 'a' }) : null;
  const log = (obj) => {
    const line = JSON.stringify({ t: new Date().toISOString(), ...obj });
    console.log(line);
    if (logStream) {
      logStream.write(`${line}\n`);
    }
  };

  const server = net.createServer((sock) => {
    const peer = `${sock.remoteAddress}:${sock.remotePort}`;
    const session = new QnetSession({ tlsMode: 'off', fixedVote: VOTES[opts.vote] });
    log({ event: 'connect', peer });
    let acc = Buffer.alloc(0);
    let dead = false;
    const kill = (reason) => {
      if (dead) {
        return;
      }
      dead = true;
      log({ event: 'close', peer, reason, state: session.state });
      session.close(reason);
      sock.destroy();
    };

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
        if (h.type > 17 || msg.HEADER_LEN + h.len > session.maxReceiveSize) {
          log({ event: 'framing-violation', peer, type: h.type, len: h.len });
          kill('framing violation');
          return;
        }
        if (acc.length < msg.HEADER_LEN + h.len) {
          return;
        }
        const frame = Buffer.from(acc.subarray(0, msg.HEADER_LEN + h.len));
        acc = acc.subarray(msg.HEADER_LEN + h.len);
        let decoded = null;
        try {
          decoded = msg.decodeMessage(frame, session.maxReceiveSize);
          // BigInt (ring seq, config version) is not JSON-serializable.
          decoded = JSON.parse(JSON.stringify(decoded, (_, v) => (typeof v === 'bigint' ? `bigint:${v}` : v)));
        } catch (err) {
          decoded = { _decodeError: err.message };
        }
        log({ dir: 'c2s', peer, type: h.type, len: h.len, hex: frame.toString('hex'), decoded });
        let reply = null;
        try {
          reply = session.handle(frame);
        } catch (err) {
          kill(`session: ${err.message}`);
          return;
        }
        if (session.isClosed) {
          kill('session closed');
          return;
        }
        if (reply) {
          const rh = msg.peekHeader(reply);
          log({ dir: 's2c', peer, type: rh.type, len: rh.len, hex: reply.toString('hex') });
          sock.write(reply);
        }
      }
    });
    sock.on('error', (err) => kill(`socket: ${err.message}`));
    sock.on('close', () => {
      if (!dead) {
        dead = true;
        log({ event: 'close', peer, reason: 'transport closed', state: session.state });
        session.close('transport closed');
      }
    });
  });

  server.listen(opts.port, opts.host, () => {
    console.log(`LISTEN port=${server.address().port} host=${opts.host} vote=${opts.vote}`);
  });
  server.on('error', (err) => {
    console.error(`server error: ${err.message}`);
    process.exit(1);
  });
}

main();
