#!/usr/bin/env python3
"""QuorumESP interop debug: full TLS handshake as a qdevice client.

PREINIT -> STARTTLS -> TLS (with client cert) -> INIT -> print INIT_REPLY.
Proves the server side independent of the NSS client. Debug tool only.
Usage: tls-client.py HOST PORT CLUSTER CLIENT_CRT CLIENT_KEY
"""
import socket
import ssl
import struct
import sys

host, port = sys.argv[1], int(sys.argv[2])
cluster = sys.argv[3].encode()
crt, key = sys.argv[4], sys.argv[5]


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


def dec_u16array(buf):
    return [struct.unpack("!H", buf[i : i + 2])[0] for i in range(0, len(buf), 2)]


s = socket.create_connection((host, port), timeout=10)
s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, cluster)))
print("preinit_reply:", read_frame(s)[0])
s.sendall(frame(2, tlv(0, struct.pack("!I", 2))))
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE  # debug only: server auth proven separately
ctx.minimum_version = ssl.TLSVersion.TLSv1_2
ctx.load_cert_chain(certfile=crt, keyfile=key)
ts = ctx.wrap_socket(s, server_hostname="Qnetd Server")
print("tls cipher:", ts.cipher()[0])


def get(path, data):
    return data


# INIT: seq2 algo1 node1 hb8000 tb{lowest} ring{1,30}
payload = (
    tlv(0, struct.pack("!I", 2))
    + tlv(4, struct.pack("!18H", *range(18)))
    + tlv(5, struct.pack("!24H", *range(24)))
    + tlv(9, struct.pack("!I", 1))
    + tlv(11, struct.pack("!H", 1))
    + tlv(12, struct.pack("!I", 8000))
    + tlv(21, bytes([1]) + struct.pack("!I", 0))
    + tlv(13, struct.pack("!I", 1) + struct.pack("!Q", 30))
)
ts.sendall(frame(3, payload))
mt, body = read_frame(ts)
print("init_reply type:", mt)
# find error code TLV (opt 6)
i = 0
while i < len(body):
    o, ln = struct.unpack("!HH", body[i : i + 4])
    v = body[i + 4 : i + 4 + ln]
    if o == 6:
        print("error_code:", struct.unpack("!H", v)[0])
    i += 4 + ln
ts.close()
print("OK")
