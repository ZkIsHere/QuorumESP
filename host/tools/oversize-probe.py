#!/usr/bin/env python3
"""Oversize-frame probe: server must answer MESSAGE_TOO_LONG and stay up."""
import socket
import struct
import sys

host, port = sys.argv[1], int(sys.argv[2])


def frame(t, p):
    return struct.pack("!HI", t, len(p)) + p


s = socket.create_connection((host, port), timeout=10)
# NODE_LIST header claims 8KB body, send it all
big = b"\x00" * 8192
s.sendall(frame(10, big))
s.settimeout(8)
try:
    h = s.recv(6)
    mt, ml = struct.unpack("!HI", h)
    b = b""
    while len(b) < ml:
        b += s.recv(ml - len(b))
    print("reply type:", mt)
    i = 0
    while i < len(b):
        o, ln = struct.unpack("!HH", b[i : i + 4])
        if o == 6:
            print("error_code:", struct.unpack("!H", b[i + 4 : i + 6])[0])
        i += 4 + ln
except socket.timeout:
    print("TIMEOUT: no reply (bad - connection should stay up with error)")
    sys.exit(1)
# connection must still be usable: fresh PREINIT gets a reply
s.sendall(frame(0, struct.pack("!HH", 0, 4) + struct.pack("!I", 99) +
                struct.pack("!HH", 1, 2) + b"ab"))
s.settimeout(8)
try:
    h = s.recv(6)
    mt, ml = struct.unpack("!HI", h)
    print("aftermath reply type:", mt, "(1 = PREINIT_REPLY, alive)")
except socket.timeout:
    print("TIMEOUT: connection died (bad)")
    sys.exit(1)
print("OK")
