#!/usr/bin/env python3
"""QuorumESP interop debug: read the server certificate presented after STARTTLS.

Speaks just enough of the protocol (PREINIT -> STARTTLS) to trigger the TLS
upgrade, then prints the peer cert + negotiated cipher. No verification —
this is a debug probe, never a security decision.
Usage: tls-probe.py HOST [PORT]
"""
import socket
import ssl
import struct
import sys

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.100.219"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 5403


def frame(msg_type, payload: bytes) -> bytes:
    return struct.pack("!HI", msg_type, len(payload)) + payload


def tlv(opt: int, val: bytes) -> bytes:
    return struct.pack("!HH", opt, len(val)) + val


def read_frame(s: socket.socket):
    hdr = b""
    while len(hdr) < 6:
        chunk = s.recv(6 - len(hdr))
        if not chunk:
            raise RuntimeError("EOF in header")
        hdr += chunk
    mtype, mlen = struct.unpack("!HI", hdr)
    body = b""
    while len(body) < mlen:
        chunk = s.recv(mlen - len(body))
        if not chunk:
            raise RuntimeError("EOF in body")
        body += chunk
    return mtype, body


s = socket.create_connection((host, port), timeout=10)
# PREINIT with cluster name + seq 1
s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, b"probe")))
print("preinit_reply type:", read_frame(s)[0])
# STARTTLS (no reply expected)
s.sendall(frame(2, tlv(0, struct.pack("!I", 2))))
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
ctx.minimum_version = ssl.TLSVersion.TLSv1_2
ts = ctx.wrap_socket(s, server_hostname="Qnetd Server")
print("negotiated:", ts.version(), ts.cipher()[0])
cert = ts.getpeercert(binary_form=False)
print("subject:", cert.get("subject"))
print("issuer:", cert.get("issuer"))
print("SAN:", cert.get("subjectAltName"))
ts.close()
