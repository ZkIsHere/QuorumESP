#!/usr/bin/env python3
"""Reconnect storm: N sequential full handshakes (PREINIT→STARTTLS→TLS→INIT→close)
then a burst of concurrent half-open connections. Server must stay up and
answer ECHO afterwards. Prints heap before/after via /api/status (needs web UI).
Usage: storm.py HOST PORT CLUSTER N
"""
import socket
import ssl
import struct
import sys
import threading
import urllib.request
import json

host, port, cluster = sys.argv[1], int(sys.argv[2]), sys.argv[3].encode()
n = int(sys.argv[4]) if len(sys.argv) > 4 else 50


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


def one_handshake(node):
    s = socket.create_connection((host, port), timeout=10)
    s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, cluster)))
    assert read_frame(s)[0] == 1
    s.sendall(frame(2, tlv(0, struct.pack("!I", 2))))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ts = ctx.wrap_socket(s, server_hostname="Qnetd Server")
    ts.sendall(
        frame(
            3,
            tlv(0, struct.pack("!I", 3))
            + tlv(4, struct.pack("!18H", *range(18)))
            + tlv(5, struct.pack("!24H", *range(24)))
            + tlv(9, struct.pack("!I", node))
            + tlv(11, struct.pack("!H", 1))
            + tlv(12, struct.pack("!I", 8000))
            + tlv(21, bytes([1]) + struct.pack("!I", 0))
            + tlv(13, struct.pack("!I", node) + struct.pack("!Q", 100)),
        )
    )
    mt, _ = read_frame(ts)
    assert mt == 4, f"INIT failed mt={mt}"
    ts.close()


def heap():
    try:
        with urllib.request.urlopen(f"http://{host}/api/status", timeout=8) as r:
            return json.load(r)["firmware"]["heap_free"]
    except Exception as e:
        return f"n/a ({e})"


h0 = heap()
print(f"heap before: {h0}", flush=True)
for i in range(n):
    one_handshake(100 + (i % 50))
    if (i + 1) % 10 == 0:
        print(f"  {i + 1}/{n} sequential ok", flush=True)

errs = []


def half_open(_):
    try:
        s = socket.create_connection((host, port), timeout=10)
        s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, cluster)))
        s.close()
    except OSError as e:
        errs.append(str(e))


ths = [threading.Thread(target=half_open, args=(i,)) for i in range(20)]
[t.start() for t in ths]
[t.join() for t in ths]
print(f"concurrent burst done, transport errors: {len(errs)}", flush=True)

# server still alive? full handshake + INIT + ECHO (ECHO needs ACTIVE)
s = socket.create_connection((host, port), timeout=10)
s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, cluster)))
assert read_frame(s)[0] == 1
s.sendall(frame(2, tlv(0, struct.pack("!I", 2))))
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
ts = ctx.wrap_socket(s, server_hostname="Qnetd Server")
ts.sendall(
    frame(
        3,
        tlv(0, struct.pack("!I", 3))
        + tlv(4, struct.pack("!18H", *range(18)))
        + tlv(5, struct.pack("!24H", *range(24)))
        + tlv(9, struct.pack("!I", 200))
        + tlv(11, struct.pack("!H", 1))
        + tlv(12, struct.pack("!I", 8000))
        + tlv(21, bytes([1]) + struct.pack("!I", 0))
        + tlv(13, struct.pack("!I", 200) + struct.pack("!Q", 100)),
    )
)
mt, _ = read_frame(ts)
assert mt == 4, f"final INIT failed mt={mt}"
ts.sendall(frame(8, tlv(0, struct.pack("!I", 777))))
mt, _ = read_frame(ts)
assert mt == 9, f"ECHO failed mt={mt}"
ts.close()
h1 = heap()
print(f"heap after: {h1}", flush=True)
print("STORM: PASS", flush=True)
