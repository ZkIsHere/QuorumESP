#!/usr/bin/env python3
"""Poll 5403 UP/DOWN N times (detect reboot windows). Usage: poll5403.py N"""
import socket
import sys
import time

n = int(sys.argv[1]) if len(sys.argv) > 1 else 6
for _ in range(n):
    s = socket.socket()
    s.settimeout(3)
    try:
        up = s.connect_ex(("192.168.100.219", 5403)) == 0
    except OSError:
        up = False
    finally:
        s.close()
    print(f"{time.strftime('%H:%M:%S')} {'UP' if up else 'DOWN'}", flush=True)
    time.sleep(3)
