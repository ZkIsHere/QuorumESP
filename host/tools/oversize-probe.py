#!/usr/bin/env python3
"""Oversize-frame probe: server must answer MESSAGE_TOO_LONG and stay up."""
import socket
import struct
import sys

host, port = sys.argv[1], int(sys.argv[2])


def frame(t, p):
    return struct.pack("!HI", t, len(p)) + p


def read_frame():
    h = b""
    while len(h) < 6:
        chunk = s.recv(6 - len(h))
        if not chunk:
            raise ConnectionError("EOF in header")
        h += chunk
    mt, ml = struct.unpack("!HI", h)
    b = b""
    while len(b) < ml:
        chunk = s.recv(ml - len(b))
        if not chunk:
            raise ConnectionError("EOF in body")
        b += chunk
    return mt, b


s = socket.create_connection((host, port), timeout=10)
# NODE_LIST header claims 8KB body, send it all
big = b"\x00" * 8192
s.sendall(frame(10, big))
s.settimeout(8)
try:
    mt, b = read_frame()
    print("reply type:", mt)
    i = 0
    while i < len(b):
        o, ln = struct.unpack("!HH", b[i : i + 4])
        if o == 6:
            print("error_code:", struct.unpack("!H", b[i + 4 : i + 6])[0])
        i += 4 + ln
except (socket.timeout, ConnectionError) as e:
    print(f"NO-REPLY ({e}): bad - server must answer with error")
    sys.exit(1)
# connection must still be usable: fresh PREINIT gets a reply
s.sendall(frame(0, struct.pack("!HH", 0, 4) + struct.pack("!I", 99) +
                struct.pack("!HH", 1, 2) + b"ab"))
s.settimeout(8)
try:
    mt, _ = read_frame()
    print("aftermath reply type:", mt, "(1 = PREINIT_REPLY, alive)")
except (socket.timeout, ConnectionError) as e:
    print(f"CONNECTION-DIED ({e}): bad")
    sys.exit(1)
print("OK")
