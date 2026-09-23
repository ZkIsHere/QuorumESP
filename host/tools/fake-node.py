#!/usr/bin/env python3
"""Fake second qdevice client for split-brain testing (debug tool).

Presents a diverging membership view (node 2 alone) to exercise the
server's FFSplit decision with 2 concurrent clients. Full mutual TLS
with an openssl client cert. Logs every VOTE_INFO received.
Usage: fake-node.py HOST PORT CLUSTER NODE_ID CFG_NODES MEMB_NODES [--cert C --key K]
Example: fake-node.py 192.168.100.219 5403 interop-test 2 1,2 2
"""
import socket
import ssl
import struct
import sys
import threading
import time

host, port = sys.argv[1], int(sys.argv[2])
cluster = sys.argv[3].encode()
node_id = int(sys.argv[4])
cfg_nodes = [int(x) for x in sys.argv[5].split(",")]
memb_nodes = [int(x) for x in sys.argv[6].split(",")]
cert = key = None
for i, a in enumerate(sys.argv):
    if a == "--cert":
        cert = sys.argv[i + 1]
    if a == "--key":
        key = sys.argv[i + 1]

seq = [0]


def nxt():
    seq[0] += 1
    return seq[0]


def frame(t, p):
    return struct.pack("!HI", t, len(p)) + p


def tlv(o, v):
    return struct.pack("!HH", o, len(v)) + v


def ring(node, sq):
    return struct.pack("!I", node) + struct.pack("!Q", sq)


def node_info(nid):
    # NODE_INFO (17) wrapping NODE_ID (9) — a bare NODE_ID decodes as a
    # top-level field, leaving the node list empty (server rightly rejects).
    return tlv(17, tlv(9, struct.pack("!I", nid)))


def read_frame(s):
    h = s.recv(6)
    mt, ml = struct.unpack("!HI", h)
    b = b""
    while len(b) < ml:
        b += s.recv(ml - len(b))
    return mt, b


def dec_u16array(buf):
    return [struct.unpack("!H", buf[i : i + 2])[0] for i in range(0, len(buf), 2)]


s = socket.create_connection((host, port), timeout=10)
s.sendall(frame(0, tlv(0, struct.pack("!I", nxt())) + tlv(1, cluster)))
assert read_frame(s)[0] == 1, "no PREINIT_REPLY"
s.sendall(frame(2, tlv(0, struct.pack("!I", nxt()))))
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE  # debug tool: server auth proven separately
ctx.minimum_version = ssl.TLSVersion.TLSv1_2
if cert:
    ctx.load_cert_chain(certfile=cert, keyfile=key)
ts = ctx.wrap_socket(s, server_hostname="Qnetd Server")
print("TLS up:", ts.cipher()[0], flush=True)

# INIT: node, ffsplit, hb 8000, tie lowest, ring(node,100)
ts.sendall(
    frame(
        3,
        tlv(0, struct.pack("!I", nxt()))
        + tlv(4, struct.pack("!18H", *range(18)))
        + tlv(5, struct.pack("!24H", *range(24)))
        + tlv(9, struct.pack("!I", node_id))
        + tlv(11, struct.pack("!H", 1))
        + tlv(12, struct.pack("!I", 8000))
        + tlv(21, bytes([1]) + struct.pack("!I", 0))
        + tlv(13, ring(node_id, 100)),
    )
)
mt, body = read_frame(ts)
assert mt == 4, f"no INIT_REPLY, got {mt}"
print("INIT_REPLY ok", flush=True)
# SET_OPTION kap=1
ts.sendall(frame(6, tlv(0, struct.pack("!I", nxt())) + tlv(23, bytes([1]))))
print("set_option_reply:", read_frame(ts)[0], flush=True)


def send_node_list(list_type, nodes, extra=b""):
    ts.sendall(
        frame(
            10,
            tlv(0, struct.pack("!I", nxt()))
            + tlv(18, bytes([list_type]))
            + tlv(13, ring(node_id, 100))
            + extra
            + b"".join(node_info(n) for n in nodes),
        )
    )


VOTE_NAMES = {1: "ACK", 2: "NACK", 3: "ASK_LATER", 4: "WAIT", 5: "NO_CHANGE"}


def handle_frame(mt, body):
    """Decode + print one frame. Returns True if it was a NODE_LIST_REPLY."""
    if mt == 14:  # VOTE_INFO
        i, vote, sq = 0, None, None
        while i < len(body):
            o, ln = struct.unpack("!HH", body[i : i + 4])
            v = body[i + 4 : i + 4 + ln]
            if o == 0:
                sq = struct.unpack("!I", v)[0]
            elif o == 19:
                vote = v[0]
            i += 4 + ln
        print(f"VOTE_INFO vote={VOTE_NAMES.get(vote, vote)} seq={sq}", flush=True)
        ts.sendall(frame(15, tlv(0, struct.pack("!I", sq))))
        return False
    if mt == 11:
        i, v = 0, None
        while i < len(body):
            o, ln = struct.unpack("!HH", body[i : i + 4])
            if o == 19:
                v = body[i + 4]
            i += 4 + ln
        print(f"NODE_LIST_REPLY vote={VOTE_NAMES.get(v, v)}", flush=True)
        return True
    print(f"msg type={mt} len={len(body)}", flush=True)
    return False


def send_and_await(payload, what):
    ts.sendall(payload)
    while True:
        mt, body = read_frame(ts)
        if handle_frame(mt, body):
            print(f"{what} reply ok", flush=True)
            return


# INITIAL_CONFIG (no ring, like the real client)
send_and_await(
    frame(
        10,
        tlv(0, struct.pack("!I", nxt()))
        + tlv(18, bytes([0]))
        + b"".join(node_info(n) for n in cfg_nodes),
    ),
    "initial_config",
)
send_and_await(
    frame(
        10,
        tlv(0, struct.pack("!I", nxt()))
        + tlv(18, bytes([2]))
        + tlv(13, ring(node_id, 100))
        + b"".join(node_info(n) for n in memb_nodes),
    ),
    "membership",
)
send_and_await(
    frame(
        10,
        tlv(0, struct.pack("!I", nxt()))
        + tlv(18, bytes([3]))
        + b"".join(node_info(n) for n in memb_nodes),
    ),
    "quorum",
)


def echo_loop():
    n = 0
    while True:
        time.sleep(5)
        n += 1
        try:
            ts.sendall(frame(8, tlv(0, struct.pack("!I", n))))
        except OSError:
            return


threading.Thread(target=echo_loop, daemon=True).start()

while True:
    mt, body = read_frame(ts)
    handle_frame(mt, body)
