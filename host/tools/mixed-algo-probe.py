#!/usr/bin/env python3
"""Mixed-algorithm probe: register ffsplit on conn A, then attempt LMS
INIT on conn B for the SAME cluster. Expect INIT_REPLY carrying
TLV_REPLY_ERROR_CODE=16 (ALGORITHM_DIFFERS), transport stays up
(ECHO still answered on A). Exits 0 on expected behavior.
Usage: mixed-algo-probe.py HOST PORT CLUSTER
"""
import socket
import ssl
import struct
import sys

host, port, cluster = sys.argv[1], int(sys.argv[2]), sys.argv[3].encode()
seq = [0]


def nxt():
    seq[0] += 1
    return seq[0]


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


def tls_conn():
    s = socket.create_connection((host, port), timeout=10)
    s.sendall(frame(0, tlv(0, struct.pack("!I", nxt())) + tlv(1, cluster)))
    assert read_frame(s)[0] == 1, "no PREINIT_REPLY"
    s.sendall(frame(2, tlv(0, struct.pack("!I", nxt()))))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    return ctx.wrap_socket(s, server_hostname="Qnetd Server")


def send_init(ts, node, algo):
    ts.sendall(
        frame(
            3,
            tlv(0, struct.pack("!I", nxt()))
            + tlv(4, struct.pack("!18H", *range(18)))
            + tlv(5, struct.pack("!24H", *range(24)))
            + tlv(9, struct.pack("!I", node))
            + tlv(11, struct.pack("!H", algo))
            + tlv(12, struct.pack("!I", 8000))
            + tlv(21, bytes([1]) + struct.pack("!I", 0))
            + tlv(13, struct.pack("!I", node) + struct.pack("!Q", 100)),
        )
    )
    return read_frame(ts)


def find_opt(body, opt):
    i = 0
    while i + 4 <= len(body):
        o, ln = struct.unpack("!HH", body[i : i + 4])
        if o == opt:
            return body[i + 4 : i + 4 + ln]
        i += 4 + ln
    return None


def err_code(body):
    """INIT_REPLY always carries TLV 6; 0 = NO_ERROR (success)."""
    raw = find_opt(body, 6)
    if raw is None:
        return None
    if len(raw) == 2:
        return struct.unpack("!H", raw)[0]
    return struct.unpack("!I", raw)[0]


a = tls_conn()
mt, body = send_init(a, 9, 1)  # ffsplit decides the cluster algorithm
assert mt == 4 and err_code(body) == 0, f"conn A INIT failed: mt={mt}"
print("conn A ffsplit INIT_REPLY ok", flush=True)

b = tls_conn()
mt, body = send_init(b, 10, 3)  # lms must be refused
assert mt == 4, f"conn B dropped (mt={mt}), expected error reply"
assert err_code(body) == 16, (
    f"expected ALGORITHM_DIFFERS(16), got mt={mt} err={err_code(body)}"
)
print("conn B lms INIT_REPLY error=16 ALGORITHM_DIFFERS (transport kept)", flush=True)

a.sendall(frame(8, tlv(0, struct.pack("!I", 4242))))
mt, _ = read_frame(a)
assert mt == 9, f"conn A ECHO_REPLY missing (mt={mt})"
print("conn A ECHO_REPLY ok — first client unaffected", flush=True)
print("MIXED-ALGO: PASS", flush=True)
