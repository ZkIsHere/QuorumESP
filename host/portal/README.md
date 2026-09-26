# QuorumESP PC portal (`host/portal/`)

External management web — runs on the user's computer, talks to boards
over USB serial. The firmware has NO on-device updater/config editor by
design; this portal is where humans pick boards, flash firmware, edit
config, and mint client certificates.

```sh
# ESP-IDF python env (has esptool + pyserial) + IDF_PATH for NVS tooling:
set IDF_PATH=D:\esp\v6.1\esp-idf
python host/portal/server.py [port]   # http://127.0.0.1:8080
```

## Sections

1. **Board** — serial ports, `chip_id` (chip model + MAC), app version
   read back from the boot log.
2. **Flash firmware** — `.bin` to `0x20000` via esptool (local file or
   GitHub release URL). App partition only: **NVS/config is never
   touched** (no erase; single-partition write).
3. **Flash in browser** — WebSerial + esptool-js CDN, no Python install
   needed (Chrome/Edge + USB cable). Same `0x20000`, NVS untouched.
   Needs internet once for the CDN. Not yet exercised against real
   hardware in CI — report issues.
4. **Config** — read live NVS first (`read_flash` + `nvs.py` parser),
   prefill the form, merge edits over it, regenerate the FULL image.
   Keys the user didn't touch survive byte-identical. Refuses if the
   live read fails (never blind-writes). Changing Wi-Fi needs a board
   reboot (press EN) to take effect.
5. **Mint client certificate** — signs per-node certs with the local dev
   CA (`host/pki/`, git-ignored). Only `client-*.crt/.key` are served;
   `ca.key` returns 403, always.

## `nvs.py` + `test_nvs.py`

Minimal NVS partition reader (ACTIVE pages; u8/u16/u32 + strings via
single-page and v2 multipage blobs; CRC-checked; later writes win).
Proven by roundtripping IDF-generator images (v1+v2, ints, 65-char
strings, overwrites) AND by parsing the live board dump byte-identical
(ssid, 15-char pass, devid, versions).

## Safety rules

- Binds `127.0.0.1` only (localhost tool).
- Flash writes app partition only; provision rewrites NVS only.
- Never serve `ca.key` / `.srl` over HTTP.
- Secrets (Wi-Fi pass, keys) never touch the repo (gitignored).
