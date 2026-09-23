'use strict';

/* Smoke test for the interop CLI: spawn, handshake, verify log output. */

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { MSG } = require('../src/consts');
const msg = require('../src/msg');
const net = require('../src/net');

function startCli(extraArgs, logFile) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, ['tools/fake-qnetd.js', ...extraArgs, '--log', logFile], {
      cwd: path.join(__dirname, '..'),
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let out = '';
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error(`CLI did not print LISTEN in time. stdout=${out}`));
    }, 8000);
    child.stdout.on('data', (d) => {
      out += d.toString();
      const m = out.match(/LISTEN port=(\d+)/);
      if (m) {
        clearTimeout(timer);
        resolve({ child, port: Number(m[1]), output: () => out });
      }
    });
    child.on('error', (err) => {
      clearTimeout(timer);
      reject(err);
    });
  });
}

describe('fake-qnetd CLI', () => {
  it('serves a handshake and logs JSON lines', async () => {
    const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'quorumesp-cli-'));
    const logFile = path.join(dir, 'frames.jsonl');
    const { child, port } = await startCli(['--port', '0', '--vote', 'ack'], logFile);
    try {
      const sock = await net.connectWithTimeout('127.0.0.1', port);
      try {
        await net.sendAll(sock, msg.preinit('cli-test', 1));
        const pre = await net.readOneMessage(sock);
        assert.equal(msg.decodeMessage(pre.frame).type, MSG.PREINIT_REPLY);
      } finally {
        sock.destroy();
      }
      await new Promise((r) => setTimeout(r, 300));
      const lines = (await fs.readFile(logFile, 'utf8')).trim().split('\n').map(JSON.parse);
      const dirs = lines.map((l) => l.dir).filter(Boolean);
      assert.ok(dirs.includes('c2s'), 'client frame must be logged');
      assert.ok(dirs.includes('s2c'), 'server reply must be logged');
      const preinit = lines.find((l) => l.dir === 'c2s');
      assert.equal(preinit.decoded.clusterName, 'cli-test');
      assert.match(preinit.hex, /^[0-9a-f]+$/);
    } finally {
      child.kill();
    }
  });

  it('rejects bad CLI args with non-zero exit', async () => {
    const code = await new Promise((resolve) => {
      const child = spawn(process.execPath, ['tools/fake-qnetd.js', '--vote', 'bogus'], {
        cwd: path.join(__dirname, '..'),
        stdio: 'ignore',
      });
      child.on('exit', resolve);
    });
    assert.notEqual(code, 0);
  });
});
