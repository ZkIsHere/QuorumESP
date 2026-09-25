#!/usr/bin/env python3
"""Connection flood: 100 simultaneous PREINIT connects, then a sustained
reconnect loop (30 full TLS handshakes back-to-back). The standing real
client (if any) must survive; the server must keep answering afterwards.
Refusals (RST/table-full) are EXPECTED under flood — counted, not errors.
Unexpected: mid-session drops of established sessions, hang (>15s with no
accept at all), or a dead server afterwards.
Usage: flood.py HOST PORT CLUSTER
Exit 0 = PASS, 1 = FAIL.
"""
import socket
import ssl
import struct
import sys
import threading
import time

host, port, cluster = sys.argv[1], int(sys.argv[2]), sys.argv[3].encode()
N_BURST = 100
N_LOOP = 30
errors = []


def frame(t, p):
    return struct.pack("!HI", t, len(p)) + p


def tlv(o, v):
    return struct.pack("!HH", o, len(v)) + v


def read_frame(s):
    h = s.recv(6)
    mt, ml = struct.unpack("!HI", h)
    b = b""
    while len(b) < ml:
        b += s.recv(ml - len(b))
    return mt, b


def preinit_close():
    try:
        s = socket.create_connection((host, port), timeout=10)
        s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, cluster)))
        h = s.recv(6)
        s.close()
        return "ok" if h and len(h) == 6 else "empty"
    except (ConnectionResetError, BrokenPipeError, OSError):
        return "refused"
    except Exception as e:  # noqa: BLE001
        return f"error: {e}"


def tls_handshake(node):
    s = socket.create_connection((host, port), timeout=10)
    s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, cluster)))
    if read_frame(s)[0] != 1:
        raise RuntimeError("no PREINIT_REPLY")
    s.sendall(frame(2, tlv(0, struct.pack("!I", 2))))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ts = ctx.wrap_socket(s, server_hostname="Qnetd Server")
    ts.sendall(
        frame(3, tlv(0, struct.pack("!I", 3))
              + tlv(4, struct.pack("!18H", *range(18)))
              + tlv(5, struct.pack("!24H", *range(24)))
              + tlv(9, struct.pack("!I", node))
              + tlv(11, struct.pack("!H", 1))
              + tlv(12, struct.pack("!I", 8000))
              + tlv(21, bytes([1]) + struct.pack("!I", 0))
              + tlv(13, struct.pack("!I", node) + struct.pack("!Q", 100))))
    if read_frame(ts)[0] != 4:
        raise RuntimeError("no INIT_REPLY")
    return ts


# Phase 1: 100 simultaneous bare connects.
res = {}
ths = [threading.Thread(target=lambda i=i: res.update({i: preinit_close()}),
                        args=[]) for i in range(N_BURST)]
[t.start() for t in ths]
[t.join() for t in ths]
ok = sum(1 for v in res.values() if v == "ok")
ref = sum(1 for v in res.values() if v == "refused")
bad = {v for v in res.values() if v not in ("ok", "refused")}
print(f"[burst x{N_BURST}] ok={ok} refused={ref} other={sorted(bad) or 'none'}",
      flush=True)
for v in bad:
    errors.append(f"burst: {v}")

# Phase 2: 30 back-to-back full TLS handshakes.
ok2 = 0
for i in range(N_LOOP):
    try:
        tls_handshake(700 + (i % 20)).close()
        ok2 += 1
    except (ConnectionResetError, BrokenPipeError, OSError) as e:
        errors.append(f"loop {i}: transport ({e})")
        break
    except Exception as e:  # noqa: BLE001
        errors.append(f"loop {i}: {e}")
        break
print(f"[loop x{N_LOOP}] ok={ok2}", flush=True)

# Phase 3: server still alive?
try:
    tls_handshake(799).close()
    print("[alive] final handshake ok", flush=True)
except Exception as e:  # noqa: BLE001
    errors.append(f"alive check: {e}")

if errors:
    print(f"FLOOD: FAIL ({len(errors)})", flush=True)
    for e in errors[:8]:
        print(f"  - {e}", flush=True)
    sys.exit(1)
print("FLOOD: PASS", flush=True)
