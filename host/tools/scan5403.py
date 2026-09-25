#!/usr/bin/env python3
"""Scan 192.168.100.200-230 for TCP 5403 (find the ESP32 after DHCP change)."""
import socket
import sys

found = []
for i in range(200, 231):
    ip = f"192.168.100.{i}"
    s = socket.socket()
    s.settimeout(0.5)
    try:
        if s.connect_ex((ip, 5403)) == 0:
            found.append(ip)
            print(f"OPEN {ip}", flush=True)
    except OSError:
        pass
    finally:
        s.close()
if not found:
    print("NO 5403 FOUND", flush=True)
    sys.exit(1)
