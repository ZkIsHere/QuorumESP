#!/usr/bin/env python3
"""QuorumESP load benchmark. Measures the qnetd-side server under load:
  1. handshake latency (sequential full PREINIT→TLS→INIT, N times)
  2. ECHO throughput (M round-trips on one ACTIVE session)
  3. concurrency (k parallel ACTIVE sessions; >2 transports are REFUSED
     by design — counted separately, not as errors)
  4. sustain (2 sessions x T seconds of ECHO traffic; any mid-run drop,
     wrong reply, or device reboot fails the run)

Bounds are conservative for a dev box (sequential handshakes, k<=4).
Needs both session slots free: stop corosync-qdevice first.
Usage: bench.py HOST PORT CLUSTER [--quick]
Exit 0 = PASS (no unexpected errors), 1 = FAIL.
"""
import socket
import ssl
import statistics
import struct
import sys
import threading
import time

host, port, cluster = sys.argv[1], int(sys.argv[2]), sys.argv[3].encode()
QUICK = "--quick" in sys.argv
N_HS = 5 if QUICK else 20
M_ECHO = 50 if QUICK else 200
SUSTAIN_S = 10 if QUICK else 30
K_CONC = 4

errors = []
refused = [0]


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


def echo_roundtrip(ts, seq):
    """One ECHO round-trip. Server-pushed VOTE_INFO may interleave (votes
    recompute on every join/leave) — answer those, wait for our reply."""
    ts.sendall(frame(8, tlv(0, struct.pack("!I", seq))))
    for _ in range(20):
        mt, body = read_frame(ts)
        if mt == 9:
            return True
        if mt == 14:  # VOTE_INFO: ack with matching seq, keep waiting
            i, sq = 0, None
            while i + 4 <= len(body):
                o, ln = struct.unpack("!HH", body[i:i + 4])
                if o == 0:
                    sq = struct.unpack("!I", body[i + 4:i + 4 + ln])[0]
                i += 4 + ln
            if sq is not None:
                ts.sendall(frame(15, tlv(0, struct.pack("!I", sq))))
            continue
        raise RuntimeError(f"unexpected mt={mt}")
    raise RuntimeError("echo reply never arrived")


def full_session(node):
    """Returns an ACTIVE TLS session (raises on refusal/failure)."""
    s = socket.create_connection((host, port), timeout=10)
    s.sendall(frame(0, tlv(0, struct.pack("!I", 1)) + tlv(1, cluster)))
    if read_frame(s)[0] != 1:
        raise RuntimeError("no PREINIT_REPLY")
    s.sendall(frame(2, tlv(0, struct.pack("!I", 2))))
    ts = tls_wrap(s)
    ts.sendall(
        frame(
            3,
            tlv(0, struct.pack("!I", 3))
            + tlv(4, struct.pack("!18H", *range(18)))
            + tlv(5, struct.pack("!24H", *range(24)))
            + tlv(9, struct.pack("!I", node))
            + tlv(11, struct.pack("!H", 1))
            + tlv(12, struct.pack("!I", 8000))
            + tlv(21, bytes([1]) + struct.pack("!I", 0))
            + tlv(13, struct.pack("!I", node) + struct.pack("!Q", 100)),
        )
    )
    if read_frame(ts)[0] != 4:
        raise RuntimeError("no INIT_REPLY")
    return ts


def phase_handshake():
    lats = []
    for i in range(N_HS):
        t0 = time.monotonic()
        try:
            ts = full_session(300 + i)
            lats.append((time.monotonic() - t0) * 1000)
            ts.close()
        except Exception as e:  # noqa: BLE001 - counted, reported below
            errors.append(f"handshake {i}: {e}")
    if lats:
        print(f"[handshake] n={len(lats)} min={min(lats):.0f}ms "
              f"avg={statistics.mean(lats):.0f}ms "
              f"p95={sorted(lats)[max(0, int(len(lats) * 0.95) - 1)]:.0f}ms "
              f"max={max(lats):.0f}ms", flush=True)
    return lats


def phase_echo():
    try:
        ts = full_session(400)
    except Exception as e:  # noqa: BLE001
        errors.append(f"echo setup: {e}")
        return []
    lats = []
    t0 = time.monotonic()
    try:
        for i in range(M_ECHO):
            s0 = time.monotonic()
            echo_roundtrip(ts, 1000 + i)
            lats.append((time.monotonic() - s0) * 1000)
    except Exception as e:  # noqa: BLE001
        errors.append(f"echo loop: {e}")
    finally:
        ts.close()
    dt = time.monotonic() - t0
    if lats:
        print(f"[echo] n={len(lats)} rate={len(lats) / dt:.1f}/s "
              f"avg={statistics.mean(lats):.1f}ms max={max(lats):.1f}ms",
              flush=True)
    return lats


def phase_concurrency():
    """k parallel sessions; transports beyond 2 are refused by design."""
    results = {}

    def worker(idx):
        try:
            ts = full_session(500 + idx)
            time.sleep(3)
            echo_roundtrip(ts, 1)
            ok = True
            ts.close()
            results[idx] = "ok" if ok else "bad-reply"
        except (ConnectionResetError, BrokenPipeError, OSError):
            results[idx] = "refused"
            refused[0] += 1
        except Exception as e:  # noqa: BLE001
            results[idx] = f"error: {e}"

    for k in (1, 2, 3, 4)[: K_CONC if not QUICK else 2]:
        results.clear()
        refused[0] = 0
        ths = [threading.Thread(target=worker, args=(i,)) for i in range(k)]
        t0 = time.monotonic()
        [t.start() for t in ths]
        [t.join() for t in ths]
        dt = time.monotonic() - t0
        ok = sum(1 for v in results.values() if v == "ok")
        print(f"[concurrency k={k}] ok={ok} refused={refused[0]} "
              f"other={sorted(set(results.values()))} t={dt:.1f}s", flush=True)
        for v in results.values():
            if v not in ("ok", "refused"):
                errors.append(f"concurrency k={k}: {v}")


def phase_sustain():
    """2 sessions x T seconds ECHO. Any drop = FAIL (incl. device reboot)."""
    stop = [False]
    counts = [0, 0]

    def worker(idx):
        try:
            ts = full_session(600 + idx)
            end = time.monotonic() + SUSTAIN_S
            n = 0
            while time.monotonic() < end and not stop[0]:
                echo_roundtrip(ts, n)
                n += 1
                counts[idx] = n
            ts.close()
        except Exception as e:  # noqa: BLE001
            errors.append(f"sustain-{idx}: {e}")
            stop[0] = True

    ths = [threading.Thread(target=worker, args=(i,)) for i in range(2)]
    [t.start() for t in ths]
    [t.join() for t in ths]
    print(f"[sustain] {SUSTAIN_S}s x2 sessions: msgs={counts}", flush=True)


print(f"bench {host}:{port} quick={QUICK}", flush=True)
phase_handshake()
phase_echo()
phase_concurrency()
phase_sustain()
if errors:
    print(f"BENCH: FAIL ({len(errors)} errors)", flush=True)
    for e in errors[:10]:
        print(f"  - {e}", flush=True)
    sys.exit(1)
print("BENCH: PASS", flush=True)
