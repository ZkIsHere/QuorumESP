#!/usr/bin/env python3
"""30-min soak watch: 5403 presence + qdevice state every 2 min.
Usage: soakwatch.py (runs ~30 min, prints timestamped lines)"""
import socket
import subprocess
import time

for i in range(16):
    s = socket.socket()
    s.settimeout(4)
    try:
        up = s.connect_ex(("192.168.100.219", 5403)) == 0
    except OSError:
        up = False
    finally:
        s.close()
    try:
        out = subprocess.run(["corosync-qdevice-tool", "-s"],
                             capture_output=True, text=True,
                             timeout=10).stdout
        st = [l for l in out.splitlines() if "State:" in l]
        qd = st[-1].strip() if st else "unknown"
    except Exception as e:  # noqa: BLE001
        qd = f"tool-error {e}"
    print(f"{time.strftime('%H:%M:%S')} 5403={'UP' if up else 'DOWN'} qdevice={qd}",
          flush=True)
    time.sleep(115)
print("SOAKWATCH: DONE", flush=True)
