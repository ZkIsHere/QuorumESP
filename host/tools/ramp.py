#!/usr/bin/env python3
"""Ramp: sequential PREINIT connects 2s apart, counting accepts-until-RST.
Shows whether accept capacity recovers and after how many connects it dies.
Usage: ramp.py HOST [N]"""
import socket
import struct
import sys
import time

host = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 15


def frame(t, p):
    return struct.pack("!HI", t, len(p)) + p


def tlv(o, v):
    return struct.pack("!HH", o, len(v)) + v


ok = rst = 0
for i in range(n):
    try:
        s = socket.create_connection((host, 5403), timeout=8)
        s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, b"interop-test")))
        h = s.recv(6)
        s.close()
        if h and len(h) == 6:
            ok += 1
            print(f"{i}: OK {h.hex()}", flush=True)
        else:
            rst += 1
            print(f"{i}: EOF", flush=True)
    except Exception as e:
        rst += 1
        print(f"{i}: RST {type(e).__name__}", flush=True)
    time.sleep(2)
print(f"RAMP: ok={ok} rst={rst}", flush=True)
