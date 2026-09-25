#!/usr/bin/env python3
"""Wrong-CA probe: handshake presenting a client cert from an UNKNOWN CA
(CN is correct on purpose — only the chain must fail). Server in mutual
mode must reject: TLS alert during handshake or close right after.
Exits 0 when rejection is observed, 1 if the server lets us in.
Usage: rogue-cert-probe.py HOST PORT CLUSTER CERT KEY
"""
import socket
import ssl
import struct
import sys

host, port, cluster = sys.argv[1], int(sys.argv[2]), sys.argv[3].encode()
cert, key = sys.argv[4], sys.argv[5]


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


s = socket.create_connection((host, port), timeout=10)
s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, cluster)))
assert read_frame(s)[0] == 1, "no PREINIT_REPLY"
s.sendall(frame(2, tlv(0, struct.pack("!I", 2))))
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
ctx.minimum_version = ssl.TLSVersion.TLSv1_2
ctx.load_cert_chain(certfile=cert, keyfile=key)
try:
    ts = ctx.wrap_socket(s, server_hostname="Qnetd Server")
except ssl.SSLError as e:
    print(f"ROGUE-CA: handshake aborted by server ({e}) -> REJECTED, PASS", flush=True)
    sys.exit(0)
# Handshake survived at TLS level: server must still refuse INIT.
ts.sendall(
    frame(
        3,
        tlv(0, struct.pack("!I", 3))
        + tlv(4, struct.pack("!18H", *range(18)))
        + tlv(5, struct.pack("!24H", *range(24)))
        + tlv(9, struct.pack("!I", 77))
        + tlv(11, struct.pack("!H", 1))
        + tlv(12, struct.pack("!I", 8000))
        + tlv(21, bytes([1]) + struct.pack("!I", 0))
        + tlv(13, struct.pack("!I", 77) + struct.pack("!Q", 100)),
    )
)
try:
    mt, body = read_frame(ts)
    print(f"ROGUE-CA: server answered INIT mt={mt} body={body.hex()} (expected close) -> FAIL", flush=True)
    sys.exit(1)
except (OSError, struct.error) as e:
    print(f"ROGUE-CA: connection dropped after handshake ({e}) -> REJECTED, PASS", flush=True)
    sys.exit(0)
