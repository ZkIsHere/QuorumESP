'use strict';

/*
 * TLS tests (Phase 1):
 * 1. Protocol-level negotiation matrix — no certificates needed, always runs.
 * 2. Real TLS handshake over loopback with an ephemeral self-signed cert
 *    generated via `openssl` at runtime. Skipped when openssl is missing.
 *    Test certs are never written to the repo (AGENTS.md §9).
 */

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const { execFile } = require('node:child_process');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const tls = require('node:tls');
const { TLS } = require('../src/consts');
const { clientShouldStartTls, QnetSession } = require('../src/session');
const msg = require('../src/msg');

describe('TLS negotiation (protocol level)', () => {
  it('qdevice tls=off never STARTTLS; aborts only against REQUIRED server', () => {
    assert.equal(clientShouldStartTls('off', TLS.UNSUPPORTED), 'plaintext');
    assert.equal(clientShouldStartTls('off', TLS.SUPPORTED), 'plaintext');
    assert.equal(clientShouldStartTls('off', TLS.REQUIRED), 'abort');
  });

  it('qdevice tls=on opportunistically upgrades', () => {
    assert.equal(clientShouldStartTls('on', TLS.UNSUPPORTED), 'plaintext');
    assert.equal(clientShouldStartTls('on', TLS.SUPPORTED), 'starttls');
    assert.equal(clientShouldStartTls('on', TLS.REQUIRED), 'starttls');
  });

  it('qdevice tls=required refuses plaintext-only servers', () => {
    assert.equal(clientShouldStartTls('required', TLS.UNSUPPORTED), 'abort');
    assert.equal(clientShouldStartTls('required', TLS.SUPPORTED), 'starttls');
    assert.equal(clientShouldStartTls('required', TLS.REQUIRED), 'starttls');
  });

  it('server advertises matching TLS mode in PREINIT_REPLY', () => {
    for (const [mode, expected] of [['off', TLS.UNSUPPORTED], ['on', TLS.SUPPORTED], ['req', TLS.REQUIRED]]) {
      const s = new QnetSession({ tlsMode: mode });
      const reply = msg.decodeMessage(s.handle(msg.preinit('c', 1)));
      assert.equal(reply.tlsSupported, expected);
    }
  });
});

function opensslAvailable() {
  return new Promise((resolve) => {
    execFile('openssl', ['version'], (err) => resolve(!err));
  });
}

describe('real TLS handshake (ephemeral self-signed cert)', async () => {
  const hasOpenssl = await opensslAvailable();
  it('wraps a session in TLS with CN verification', { skip: !hasOpenssl }, async () => {
    const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'quorumesp-tls-'));
    try {
      const key = path.join(dir, 'key.pem');
      const cert = path.join(dir, 'cert.pem');
      await new Promise((resolve, reject) => {
        execFile(
          'openssl',
          [
            'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
            '-keyout', key, '-out', cert, '-days', '1',
            '-subj', '/CN=Qnetd Test',
          ],
          (err, stdout, stderr) => (err ? reject(new Error(stderr || err.message)) : resolve())
        );
      });
      const [keyPem, certPem] = await Promise.all([
        fs.readFile(key, 'utf8'),
        fs.readFile(cert, 'utf8'),
      ]);
      const server = tls.createServer({ key: keyPem, cert: certPem }, (sock) => {
        sock.write('PING');
        sock.end();
      });
      await new Promise((res) => server.listen(0, '127.0.0.1', res));
      try {
        const received = await new Promise((resolve, reject) => {
          const sock = tls.connect(
            {
              host: '127.0.0.1',
              port: server.address().port,
              ca: [certPem], // trust ONLY our ephemeral cert — no insecure bypass
              servername: 'Qnetd Test',
              checkServerIdentity: (host, peerCert) => {
                if (!/CN=Qnetd Test/.test(peerCert.subject.CN || peerCert.subject) && peerCert.subject?.CN !== 'Qnetd Test') {
                  return new Error(`unexpected CN: ${JSON.stringify(peerCert.subject)}`);
                }
                return undefined;
              },
            },
            () => sock.on('data', (d) => resolve(d.toString()))
          );
          sock.on('error', reject);
          setTimeout(() => reject(new Error('TLS handshake timeout')), 5000);
        });
        assert.equal(received, 'PING');
      } finally {
        await new Promise((res) => server.close(res));
      }
    } finally {
      await fs.rm(dir, { recursive: true, force: true });
    }
  });
});
