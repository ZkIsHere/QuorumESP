#!/usr/bin/env python3
"""Check NTP reachability (debug network path for ESP32 SNTP)."""
import socket
import struct
import sys
import time

for host in ["pool.ntp.org", "time.google.com", "vn.pool.ntp.org"]:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(4)
        s.sendto(b"\x1b" + 47 * b"\x00", (host, 123))
        d, _ = s.recvfrom(512)
        t = struct.unpack("!I", d[40:44])[0] - 2208988800
        print(f"{host}: NTP-OK {time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(t))}")
    except Exception as e:
        print(f"{host}: FAIL {type(e).__name__} {e}")
    finally:
        try:
            s.close()
        except Exception:
            pass
