#!/usr/bin/env python3
"""Scan a pcap for TLS handshake messages (debug: is CertificateRequest sent?).

Minimal TCP reassembly + TLS record parser. No third-party deps.
Usage: pcap-scan.py FILE [SERVER_IP]
"""
import struct
import sys

path = sys.argv[1]
server_ip = sys.argv[2] if len(sys.argv) > 2 else "192.168.100.219"

HS_NAMES = {
    1: "ClientHello", 2: "ServerHello", 11: "Certificate",
    12: "ServerKeyExchange", 13: "CertificateRequest", 14: "ServerHelloDone",
    16: "ClientKeyExchange",
}


def parse_pcap(data):
    if data[:4] != b"\xd4\xc3\xb2\xa1":
        raise SystemExit("not a little-endian pcap")
    linktype = struct.unpack("<I", data[20:24])[0]
    link = "sll2" if linktype == 276 else "eth"
    off = 24
    pkts = []
    while off + 16 <= len(data):
        ts_s, ts_u, caplen, _ = struct.unpack("<IIII", data[off : off + 16])
        off += 16
        pkts.append(data[off : off + caplen])
        off += caplen
    return pkts, link


def ipv4_tcp(pkt, linktype):
    if linktype == "sll2":
        # Linux cooked v2: 20-byte header, IPv4 starts at offset 20
        if len(pkt) < 21 or (pkt[20] >> 4) != 4:
            return None
        ip = pkt[20:]
    else:  # ethernet
        if len(pkt) < 14 or pkt[12:14] != b"\x08\x00":
            return None
        ip = pkt[14:]
        if len(ip) < 20 or (ip[0] >> 4) != 4:
            return None
    ihl = (ip[0] & 0xF) * 4
    if len(ip) < ihl or ip[9] != 6:
        return None
    src = ".".join(map(str, ip[12:16]))
    tcp = ip[ihl:]
    if len(tcp) < 20:
        return None
    sport, dport = struct.unpack("!HH", tcp[:4])
    seq = struct.unpack("!I", tcp[4:8])[0]
    thl = (tcp[12] >> 4) * 4
    return (src, sport, dport, seq, tcp[thl:])


def main():
    with open(path, "rb") as f:
        pkts, link = parse_pcap(f.read())
    print(f"linktype: {link}, packets: {len(pkts)}")
    streams = {}
    for pkt in pkts:
        r = ipv4_tcp(pkt, link)
        if not r:
            continue
        src, sport, dport, seq, payload = r
        if not payload or dport != 5403 and sport != 5403:
            continue
        frm = (src, sport, dport)
        streams.setdefault(frm, []).append((seq, payload))
    for (src, sport, dport), segs in streams.items():
        direction = "s2c" if src == server_ip else "c2s"
        segs.sort()
        buf = b""
        base = segs[0][0]
        for seq, p in segs:
            rel = (seq - base) % 2**32
            if rel + len(p) > len(buf):
                buf = buf[:rel] + p[len(buf) - rel :] if rel < len(buf) else buf + b"\x00" * (rel - len(buf)) + p
        print(f"== {direction} {src}:{sport} ({len(buf)} bytes reassembled)")
        i = 0
        # skip plaintext prologue: find first 0x16 record
        while i + 5 <= len(buf):
            ctype, ver, rlen = buf[i], struct.unpack("!H", buf[i + 1 : i + 3])[0], struct.unpack("!H", buf[i + 3 : i + 5])[0]
            frag = buf[i + 5 : i + 5 + rlen]
            if ctype != 22 or len(frag) < rlen:
                i += 1
                continue
            j = 0
            while j + 4 <= len(frag):
                htype = frag[j]
                hlen = struct.unpack("!I", b"\x00" + frag[j + 1 : j + 4])[0]
                body = frag[j + 4 : j + 4 + hlen]
                extra = ""
                if htype == 13:
                    # CertificateRequest: cert_types, sigalgs, then CA list
                    try:
                        k = 1 + frag[j + 4]
                        siglen = struct.unpack("!H", frag[j + 4 + k : j + 6 + k])[0]
                        k += 2 + siglen
                        calen = struct.unpack("!H", frag[j + 4 + k : j + 6 + k])[0]
                        k += 2
                        nca, kk = 0, k
                        while kk < k + calen:
                            dl = struct.unpack("!H", frag[j + 4 + kk : j + 6 + kk])[0]
                            kk += 2 + dl
                            nca += 1
                        extra = f" ca_list_len={calen} ca_count={nca}"
                    except Exception as e:
                        extra = f" parse_error={e}"
                print(f"   handshake {htype} ({HS_NAMES.get(htype, '?')}) len={hlen}{extra}")
                j += 4 + hlen
            i += 5 + rlen
            if i > len(buf):
                break


main()
