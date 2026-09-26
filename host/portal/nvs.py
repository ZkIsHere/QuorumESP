"""Minimal NVS partition reader (host-side, for the QuorumESP portal).
Parses ACTIVE pages and returns {namespace: {key: value}} for the types
our firmware uses: u8/u16/u32/i32, string (SZ single-page and v2
multipage blobs). CRC-checked; anything doubtful is skipped, never
guessed. Validated by roundtripping generator-made images (test_nvs.py).
Not a general NVS implementation: u64/i64 decode as int, unknown types
and corrupt entries are skipped.
"""
import struct
import zlib

PAGE_SIZE = 4096
HEADER_SIZE = 32
FIRST_ENTRY = 64
ENTRY_SIZE = 32
ACTIVE = 0xFFFFFFFE
FULL = 0xFFFFFFFC

T_U8, T_I8 = 0x01, 0x11
T_U16, T_I16 = 0x02, 0x12
T_U32, T_I32 = 0x04, 0x14
T_U64, T_I64 = 0x08, 0x18
T_SZ = 0x21
T_BLOB, T_BLOB_DATA, T_BLOB_IDX = 0x41, 0x42, 0x48

PRIM = {T_U8: ("<B", 1), T_I8: ("<b", 1), T_U16: ("<H", 2),
        T_I16: ("<h", 2), T_U32: ("<I", 4), T_I32: ("<i", 4),
        T_U64: ("<Q", 8), T_I64: ("<q", 8)}


def _crc_ok(entry):
    stored, = struct.unpack_from("<I", entry, 4)
    calc = zlib.crc32(entry[0:4] + entry[8:32], 0xFFFFFFFF) & 0xFFFFFFFF
    return stored == calc


def _key(entry):
    raw = entry[8:24]
    end = raw.find(b"\x00")
    return raw[:end if end >= 0 else 16].decode("utf-8", "replace")


def _entries(page):
    """Yield (offset, entry) for plausible entries in walk order."""
    off = FIRST_ENTRY
    while off + ENTRY_SIZE <= len(page):
        e = page[off:off + ENTRY_SIZE]
        if e == b"\xff" * ENTRY_SIZE:
            return  # untouched tail
        ns, typ, span = e[0], e[1], e[2]
        if typ == 0xFF or ns == 0xFF:
            off += ENTRY_SIZE  # erased slot: fixed step, keep walking
            continue
        if not _crc_ok(e):
            off += ENTRY_SIZE
            continue
        yield off, e
        span = span if 1 <= span <= 126 else 1
        off += span * ENTRY_SIZE


def parse_partition(buf):
    """Parse an NVS partition image. Returns {ns: {key: value}}.
    Later writes win (walk order). Blob chunks reassembled + CRC-checked.
    Strings come back as str (NUL-trimmed); ints as int."""
    out = {}
    ns_names = {}
    # (page, ns, key) -> list of (chunk_idx, total, bytes) for blobs
    blobs = {}
    blob_meta = {}  # (page_idx, ns, key) -> (size, count)

    npages = len(buf) // PAGE_SIZE
    for pi in range(npages):
        page = buf[pi * PAGE_SIZE:(pi + 1) * PAGE_SIZE]
        state, = struct.unpack_from("<I", page, 0)
        if state not in (ACTIVE, FULL):
            continue
        for off, e in _entries(page):
            ns, typ = e[0], e[1]
            if ns == 0:
                # Namespace definition: index = order of appearance.
                name = _key(e)
                if name and name not in ns_names.values():
                    ns_names[len(ns_names) + 1] = name
                continue
            name = ns_names.get(ns)
            if not name:
                continue
            key = _key(e)
            if typ in PRIM:
                fmt, _ = PRIM[typ]
                out.setdefault(name, {})[key] = struct.unpack_from(fmt, e, 24)[0]
            elif typ in (T_SZ, T_BLOB):
                # Single-page varlen: datalen@24, data follows in entries.
                (dlen,) = struct.unpack_from("<H", e, 24)
                span = e[2] if 1 <= e[2] <= 126 else 1
                raw = b"".join(
                    page[o:o + ENTRY_SIZE]
                    for o in range(off + ENTRY_SIZE,
                                   off + span * ENTRY_SIZE, ENTRY_SIZE))
                data = raw[:dlen]
                if zlib.crc32(data, 0xFFFFFFFF) & 0xFFFFFFFF != \
                        struct.unpack_from("<I", e, 28)[0]:
                    continue
                out.setdefault(name, {})[key] = \
                    data.split(b"\x00")[0].decode("utf-8", "replace") \
                    if typ == T_SZ else data
            elif typ == T_BLOB_IDX:
                size, = struct.unpack_from("<I", e, 24)
                count = e[28]
                blob_meta[(pi, ns, key)] = (size, count, off)
            elif typ == T_BLOB_DATA:
                total, idx = e[2], e[3]
                blobs.setdefault((pi, ns, key), []).append((idx, total, off))

    # Reassemble multipage blobs (firmware nvs_set_str shape: BLOB_IDX
    # index first, then BLOB_DATA chunks; latest index wins).
    for (pi, ns, key), parts in blobs.items():
        page = buf[pi * PAGE_SIZE:(pi + 1) * PAGE_SIZE]
        name = ns_names.get(ns)
        if not name:
            continue
        meta = blob_meta.get((pi, ns, key))
        if meta is None:
            continue
        size, count, idx_off = meta
        # Only chunks written after the latest index (older versions' chunks
        # go stale in place; walk order keeps the newest index).
        payloads = {}
        for idx, total, off in parts:
            if off <= idx_off:
                continue
            hdr = page[off:off + ENTRY_SIZE]
            (csize,) = struct.unpack_from("<H", hdr, 24)
            data = page[off + ENTRY_SIZE:off + ENTRY_SIZE + csize]
            if zlib.crc32(data, 0xFFFFFFFF) & 0xFFFFFFFF != \
                    struct.unpack_from("<I", hdr, 28)[0]:
                continue
            payloads[idx] = data
        if len(payloads) < count:
            continue  # incomplete: never guess partial strings
        data = b"".join(payloads[i] for i in sorted(payloads))[:size]
        out.setdefault(name, {})[key] = data.split(b"\x00")[0].decode(
            "utf-8", "replace")
    return out
