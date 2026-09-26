#!/usr/bin/env python3
"""4-session concurrency proof: 4 simultaneous full sessions, all ACTIVE at
once (barrier), ECHO while all connected, always close (try/finally — an
orphaned client looks like a ghost slot to the next run).
Usage: four.py HOST PORT CLUSTER
"""
import socket
import ssl
import struct
import sys
import threading
import time

host, port, cluster = sys.argv[1], int(sys.argv[2]), sys.argv[3].encode()
T = 30


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


def full_session(node):
    s = socket.create_connection((host, port), timeout=T)
    s.settimeout(T)
    s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, cluster)))
    if read_frame(s)[0] != 1:
        raise RuntimeError("no PREINIT_REPLY")
    s.sendall(frame(2, tlv(0, struct.pack("!I", 2))))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ts = ctx.wrap_socket(s, server_hostname="Qnetd Server")
    ts.settimeout(T)
    ts.sendall(
        frame(3, tlv(0, struct.pack("!I", 3))
              + tlv(4, struct.pack("!18H", *range(18)))
              + tlv(5, struct.pack("!24H", *range(24)))
              + tlv(9, struct.pack("!I", node))
              + tlv(11, struct.pack("!H", 1))
              + tlv(12, struct.pack("!I", 8000))
              + tlv(21, bytes([1]) + struct.pack("!I", 0))
              + tlv(13, struct.pack("!I", node) + struct.pack("!Q", 100))))
    mt, _ = read_frame(ts)
    if mt != 4:
        raise RuntimeError(f"INIT mt={mt}")
    return ts


res = {}
barrier = threading.Barrier(4, timeout=90)


def worker(idx):
    t0 = time.monotonic()
    ts = None
    try:
        time.sleep(idx * 0.5)  # desync SYN burst, still fully overlapping
        ts = full_session(950 + idx)
        dt = time.monotonic() - t0
        barrier.wait()
        for _ in range(6):  # ECHO, tolerating interleaved VOTE_INFO pushes
            ts.sendall(frame(8, tlv(0, struct.pack("!I", 1))))
            mt, _ = read_frame(ts)
            if mt == 9:
                break
            if mt != 14:
                raise RuntimeError(f"echo mt={mt}")
        else:
            raise RuntimeError("echo never answered")
        res[idx] = f"ACTIVE hs={dt:.1f}s echo=ok"
    except (ConnectionResetError, BrokenPipeError, OSError) as e:
        res[idx] = f"REFUSED/TIMEOUT {type(e).__name__} after {time.monotonic() - t0:.1f}s"
    except Exception as e:  # noqa: BLE001 - includes broken barrier
        res[idx] = f"ERROR {e}"
    finally:
        if ts is not None:
            try:
                ts.close()
            except OSError:
                pass


ths = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
[t.start() for t in ths]
[t.join() for t in ths]
ok = sum(1 for v in res.values() if v.startswith("ACTIVE"))
for i in range(4):
    print(i, res.get(i), flush=True)
print(f"FOUR: {ok}/4 ACTIVE", flush=True)
sys.exit(0 if ok == 4 else 1)
