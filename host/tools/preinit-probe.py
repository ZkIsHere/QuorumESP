#!/usr/bin/env python3
"""Single PREINIT probe: prints what the server answers (or RST/EOF)."""
import socket
import struct
import sys

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.100.219"


def frame(t, p):
    return struct.pack("!HI", t, len(p)) + p


def tlv(o, v):
    return struct.pack("!HH", o, len(v)) + v


try:
    s = socket.create_connection((host, 5403), timeout=8)
    s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, b"interop-test")))
    h = s.recv(6)
    print("got", h.hex() if h else "EOF-empty", flush=True)
    s.close()
except Exception as e:
    print("ERR", type(e).__name__, e, flush=True)
