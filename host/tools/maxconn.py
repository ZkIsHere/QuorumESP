#!/usr/bin/env python3
"""Max-capacity probe: how many simultaneous connections / concurrent
requesting devices can the board take?
  A. concurrent ACTIVE sessions k=1..8 (hold 10s, ECHO every 2s)
  B. ECHO latency with 2 ACTIVE busy sessions (50 each)
  C. accept churn: sequential PREINIT+close for 20s (connects/sec)
  D. SYN burst ceiling: N=20/40/60 simultaneous bare connects
Needs both slots free: stop corosync-qdevice first.
Usage: maxconn.py HOST PORT CLUSTER
"""
import socket
import ssl
import struct
import sys
import threading
import time

host, port, cluster = sys.argv[1], int(sys.argv[2]), sys.argv[3].encode()


def frame(t, p):
    return struct.pack("!HI", t, len(p)) + p


def tlv(o, v):
    return struct.pack("!HH", o, len(v)) + v


def read_frame(s):
    h = s.recv(6)
    mt, ml = struct.unpack("!HI", h)
    b = b""
    while len(b) < ml:
        b += s.recv(ml - len(b))
    return mt, b


def tls_wrap(s):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    return ctx.wrap_socket(s, server_hostname="Qnetd Server")


def full_session(node):
    s = socket.create_connection((host, port), timeout=10)
    s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, cluster)))
    if read_frame(s)[0] != 1:
        raise RuntimeError("no PREINIT_REPLY")
    s.sendall(frame(2, tlv(0, struct.pack("!I", 2))))
    ts = tls_wrap(s)
    ts.sendall(
        frame(3, tlv(0, struct.pack("!I", 3))
              + tlv(4, struct.pack("!18H", *range(18)))
              + tlv(5, struct.pack("!24H", *range(24)))
              + tlv(9, struct.pack("!I", node))
              + tlv(11, struct.pack("!H", 1))
              + tlv(12, struct.pack("!I", 8000))
              + tlv(21, bytes([1]) + struct.pack("!I", 0))
              + tlv(13, struct.pack("!I", node) + struct.pack("!Q", 100))))
    if read_frame(ts)[0] != 4:
        raise RuntimeError("no INIT_REPLY")
    return ts


def echo_ok(ts, seq):
    ts.sendall(frame(8, tlv(0, struct.pack("!I", seq))))
    for _ in range(20):
        mt, body = read_frame(ts)
        if mt == 9:
            return True
        if mt == 14:
            i = 0
            while i + 4 <= len(body):
                o, ln = struct.unpack("!HH", body[i:i + 4])
                if o == 0:
                    sq = struct.unpack("!I", body[i + 4:i + 4 + ln])[0]
                    ts.sendall(frame(15, tlv(0, struct.pack("!I", sq))))
                i += 4 + ln
    return False


print(f"maxconn {host}:{port}", flush=True)

# A. concurrent ACTIVE sessions
for k in range(1, 9):
    res = {}

    def worker(idx):
        try:
            ts = full_session(800 + idx)
            end = time.monotonic() + 10
            n = 0
            ok = True
            while time.monotonic() < end:
                if not echo_ok(ts, n):
                    ok = False
                    break
                n += 1
                time.sleep(2)
            ts.close()
            res[idx] = f"active({n})" if ok else "dropped"
        except (ConnectionResetError, BrokenPipeError, OSError):
            res[idx] = "refused"
        except Exception as e:  # noqa: BLE001
            res[idx] = f"error:{e}"

    ths = [threading.Thread(target=worker, args=(i,)) for i in range(k)]
    [t.start() for t in ths]
    [t.join() for t in ths]
    nact = sum(1 for v in res.values() if v.startswith("active"))
    nref = sum(1 for v in res.values() if v == "refused")
    print(f"[active k={k}] active={nact} refused={nref} other={sorted(set(res.values()))}",
          flush=True)
    time.sleep(8)  # let TIME_WAIT drain between rounds

# B. latency with 2 busy sessions
lats = []
s1, s2 = full_session(900), full_session(901)
t0 = time.monotonic()
for i in range(50):
    a = time.monotonic()
    echo_ok(s1, i)
    b = time.monotonic()
    echo_ok(s2, i)
    lats.append(((b - a) / 2) * 1000)
s1.close()
s2.close()
import statistics
print(f"[loaded-latency] n={len(lats)} avg={statistics.mean(lats):.1f}ms "
      f"max={max(lats):.1f}ms", flush=True)
time.sleep(8)

# C. accept churn 20s
n = 0
t0 = time.monotonic()
while time.monotonic() - t0 < 20:
    try:
        s = socket.create_connection((host, port), timeout=5)
        s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, cluster)))
        if s.recv(6):
            n += 1
        s.close()
    except OSError:
        pass
print(f"[churn] {n} accepts in 20s = {n / 20:.1f}/s", flush=True)
time.sleep(8)

# D. burst ceiling
for burst in (20, 40, 60):
    res = {}

    def hitter(idx):
        try:
            s = socket.create_connection((host, port), timeout=8)
            s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, cluster)))
            h = s.recv(6)
            s.close()
            res[idx] = "ok" if h and len(h) == 6 else "empty"
        except (ConnectionResetError, BrokenPipeError, OSError):
            res[idx] = "refused"
        except Exception as e:  # noqa: BLE001
            res[idx] = f"error:{e}"

    ths = [threading.Thread(target=hitter, args=(i,)) for i in range(burst)]
    [t.start() for t in ths]
    [t.join() for t in ths]
    ok = sum(1 for v in res.values() if v == "ok")
    print(f"[burst x{burst}] ok={ok} refused={burst - ok}", flush=True)
    time.sleep(15)

print("MAXCONN: DONE", flush=True)
