#!/usr/bin/env python3
"""Fetch the server certificate (PREINIT→STARTTLS→TLS) and print dates."""
import socket
import ssl
import struct
import sys

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.100.219"


def frame(t, p):
    return struct.pack("!HI", t, len(p)) + p


def tlv(o, v):
    return struct.pack("!HH", o, len(v)) + v


s = socket.create_connection((host, 5403), timeout=10)
s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, b"interop-test")))
h = s.recv(6)
mt, ml = struct.unpack("!HI", h)
print("preinit:", h.hex())
body = b""
while len(body) < ml:  # drain the full reply: leftover bytes would corrupt TLS
    body += s.recv(ml - len(body))
s.sendall(frame(2, tlv(0, struct.pack("!I", 2))))
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
ts = ctx.wrap_socket(s, server_hostname="Qnetd Server")
c = ts.getpeercert(binary_form=True)
import subprocess
open("/tmp/srv.der", "wb").write(c)
print(subprocess.run(["openssl", "x509", "-in", "/tmp/srv.der", "-inform",
                      "der", "-noout", "-subject", "-dates"],
                     capture_output=True, text=True).stdout)
