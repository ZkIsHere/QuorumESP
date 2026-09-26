/* NVS partition reader + generator (mirrors host/portal/nvs.py and the IDF
 * generator byte-for-byte for our key set). Single page, ACTIVE state,
 * namespace + u8/u16/u32 + v2 strings. CRC32 standard. No dependencies.
 * Usable in browser and node (CommonJS/ESM-agnostic: attaches to globalThis
 * when no module system is detected).
 */
(function (root, factory) {
  if (typeof module !== "undefined" && module.exports) module.exports = factory();
  else root.QespNvs = factory();
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  const PAGE_SIZE = 4096, FIRST_ENTRY = 64, ENTRY_SIZE = 32;
  const ACTIVE = 0xfffffffe, FULL = 0xfffffffc;
  const T_U8 = 0x01, T_U16 = 0x02, T_U32 = 0x04, T_SZ = 0x21;
  const VER2 = 0xfe;

  const CRC_T = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      t[n] = c >>> 0;
    }
    return t;
  })();

  /* NOTE: this matches zlib.crc32(data, 0xFFFFFFFF) as used by the IDF
   * generator — zlib XORs the start value first, so the effective initial
   * register is 0x00000000 (NOT standard CRC-32 init). Verified
   * byte-identical against real generator output. */
  function crc32(bytes) {
    let c = 0x00000000;
    for (let i = 0; i < bytes.length; i++)
      c = CRC_T[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  }

  function entryCrcOk(e) {
    const stored = e[4] | (e[5] << 8) | (e[6] << 16) | (e[7] << 24);
    const buf = new Uint8Array(28);
    buf.set(e.subarray(0, 4), 0);
    buf.set(e.subarray(8, 32), 4);
    return stored >>> 0 === crc32(buf);
  }

  function keyOf(e) {
    let end = 0;
    while (end < 16 && e[8 + end] !== 0) end++;
    return String.fromCharCode.apply(null, e.subarray(8, 8 + end));
  }

  function u32le(b, o) {
    return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
  }

  function parsePartition(buf) {
    const out = {};
    const nsNames = {};
    const npages = Math.floor(buf.length / PAGE_SIZE);
    for (let pi = 0; pi < npages; pi++) {
      const page = buf.subarray(pi * PAGE_SIZE, (pi + 1) * PAGE_SIZE);
      const state = u32le(page, 0);
      if (state !== ACTIVE && state !== FULL) continue;
      let off = FIRST_ENTRY;
      while (off + ENTRY_SIZE <= page.length) {
        const e = page.subarray(off, off + ENTRY_SIZE);
        let empty = true;
        for (let i = 0; i < ENTRY_SIZE; i++) if (e[i] !== 0xff) { empty = false; break; }
        if (empty) break;
        const ns = e[0], typ = e[1];
        let span = e[2];
        if (typ === 0xff || ns === 0xff) { off += ENTRY_SIZE; continue; }
        if (!entryCrcOk(e)) { off += ENTRY_SIZE; continue; }
        if (!(span >= 1 && span <= 126)) span = 1;
        if (ns === 0) {
          const name = keyOf(e);
          let known = false;
          for (const k in nsNames) if (nsNames[k] === name) known = true;
          if (name && !known) nsNames[Object.keys(nsNames).length + 1] = name;
        } else {
          const name = nsNames[ns];
          const key = keyOf(e);
          if (name && key) {
            if (typ === T_U8) (out[name] = out[name] || {})[key] = e[24];
            else if (typ === T_U16) (out[name] = out[name] || {})[key] = e[24] | (e[25] << 8);
            else if (typ === T_U32) (out[name] = out[name] || {})[key] = u32le(e, 24);
            else if (typ === T_SZ) {
              const dlen = e[24] | (e[25] << 8);
              let raw = new Uint8Array(0);
              const parts = [];
              for (let o = off + ENTRY_SIZE; o < off + span * ENTRY_SIZE; o += ENTRY_SIZE)
                parts.push(page.subarray(o, o + ENTRY_SIZE));
              const flat = new Uint8Array(parts.reduce((a, p) => a + p.length, 0));
              let at = 0;
              for (const p of parts) { flat.set(p, at); at += p.length; }
              raw = flat.subarray(0, dlen);
              const want = u32le(e, 28);
              if (crc32(raw) === want) {
                let end = 0;
                while (end < raw.length && raw[end] !== 0) end++;
                const s = String.fromCharCode.apply(null, raw.subarray(0, end));
                (out[name] = out[name] || {})[key] = s;
              }
            }
          }
        }
        off += span * ENTRY_SIZE;
      }
    }
    return out;
  }

  function wrU32(buf, o, v) {
    buf[o] = v & 0xff; buf[o + 1] = (v >>> 8) & 0xff;
    buf[o + 2] = (v >>> 16) & 0xff; buf[o + 3] = (v >>> 24) & 0xff;
  }

  /* mapping: {key: [encoding, value]}, encoding in u8/u16/u32/string.
   * Returns a 4096-byte (padSize or full partition) Uint8Array, single page.
   * Byte layout mirrors IDF nvs_partition_gen v2 for short values. */
  function generate(mapping, order, padSize) {
    const page = new Uint8Array(PAGE_SIZE).fill(0xff);
    wrU32(page, 0, ACTIVE);
    wrU32(page, 4, 0); // seq/page_num
    page[8] = VER2;
    const crcH = crc32(page.subarray(4, 28));
    wrU32(page, 28, crcH);
    let entryNum = 0;
    const keys = order || Object.keys(mapping);

    function useSlot() {
      const bit = entryNum * 2;
      page[32 + (bit >> 3)] &= ~(1 << (bit & 7));
      const off = FIRST_ENTRY + entryNum * ENTRY_SIZE;
      entryNum++;
      return off;
    }
    function putEntry(ns, typ, span, key, data24, dataLen, dataCrc) {
      const off = useSlot();
      const e = page.subarray(off, off + ENTRY_SIZE);
      e[0] = ns; e[1] = typ; e[2] = span; e[3] = 0xff;
      for (let i = 0; i < 16; i++)
        e[8 + i] = i < key.length ? key.charCodeAt(i) & 0xff : 0;
      if (data24) e.set(data24, 24);
      if (dataLen !== undefined) { e[24] = dataLen & 0xff; e[25] = (dataLen >>> 8) & 0xff; }
      if (dataCrc !== undefined) wrU32(e, 28, dataCrc);
      const hb = new Uint8Array(28);
      hb.set(e.subarray(0, 4), 0);
      hb.set(e.subarray(8, 32), 4);
      wrU32(e, 4, crc32(hb));
      return off;
    }
    // Namespace first (index 1), like the generator.
    const NS = 1;
    const nsOff = useSlot();
    {
      const e = page.subarray(nsOff, nsOff + ENTRY_SIZE);
      e[0] = 0; e[1] = T_U8; e[2] = 1; e[3] = 0xff;
      const nm = "qesp";
      for (let i = 0; i < 16; i++) e[8 + i] = i < nm.length ? nm.charCodeAt(i) : 0;
      e[24] = NS;
      const hb = new Uint8Array(28);
      hb.set(e.subarray(0, 4), 0);
      hb.set(e.subarray(8, 32), 4);
      wrU32(e, 4, crc32(hb));
    }
    const enc = new TextEncoder();
    for (const k of keys) {
      const [t, v] = mapping[k];
      if (t === "u8" || t === "u16" || t === "u32") {
        const data24 = new Uint8Array(8).fill(0xff);
        if (t === "u8") data24[0] = v & 0xff;
        else if (t === "u16") { data24[0] = v & 0xff; data24[1] = (v >>> 8) & 0xff; }
        else wrU32(data24, 0, v);
        const code = t === "u8" ? T_U8 : t === "u16" ? T_U16 : T_U32;
        putEntry(NS, code, 1, k, data24);
      } else if (t === "string") {
        const raw = enc.encode(v + "\0");
        const nData = Math.ceil(raw.length / 32);
        putEntry(NS, T_SZ, nData + 1, k, null, raw.length,
                 crc32(raw));
        for (let i = 0; i < nData; i++) {
          const off = useSlot();
          const chunk = raw.subarray(i * 32, (i + 1) * 32);
          page.set(chunk, off);
        }
      }
    }
    if (padSize && padSize > PAGE_SIZE) {
      const full = new Uint8Array(padSize).fill(0xff);
      full.set(page, 0);
      return full;
    }
    return page;
  }

  return { parsePartition, generate, crc32 };
});
