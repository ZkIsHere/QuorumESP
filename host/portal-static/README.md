# QuorumESP static portal (`host/portal-static/`)

External management web with **no server**: static files on Cloudflare
Pages + one tiny Worker + browser WebSerial. For other people's boards:
pick ESP32, pick firmware, edit config, flash — no Python install.

```sh
# Deploy (Cloudflare Pages, static dir + one Function):
npx wrangler pages deploy host/portal-static --project-name quorumes-portal
# Local preview of the static part (no WebSerial headless — UI only):
npx wrangler pages dev host/portal-static
```

## What runs where

| Feature | How (no server) |
|---|---|
| Board pick + info | WebSerial `requestPort` + esptool-js `main()` (chip), `readMac` (guarded) |
| App version | WebSerial raw: reset board, regex `App version:` from boot log |
| Firmware list | `fetch` api.github.com (CORS-open) |
| Browser flash | esptool-js `writeFlash` to `0x20000` (CDN bundle; file picker or Worker-proxied release URL). NVS never touched |
| Config read | esptool-js `readFlash(0x9000)` + `nvs.js` parse |
| Config save | `nvs.js` generate (merge over live read) + `writeFlash` NVS |
| Release proxy | `functions/api/dl.js` (Worker: same-repo URLs only, 4MB cap, CORS `*`) |

## `nvs.js` correctness (the load-bearing part)

Parser + generator ported from `host/portal/nvs.py` + the IDF generator.
Proven, not guessed:

- `test/run.cjs` (`node test/run.cjs`): self roundtrips + layout asserts.
- **Byte-identical** to IDF `nvs_partition_gen` output for the qesp key
  set (`cmp` clean on 24KB images).
- Parses the **live board dump** exactly like the Python parser
  (all 8 keys, ssid, pass length, versions).
- NVS CRC quirk documented in code: the generator's
  `zlib.crc32(x, 0xFFFFFFFF)` uses effective init register `0x00000000`.

## Honest limits

- Needs Chrome/Edge (WebSerial) + HTTPS (Pages gives it) + internet once
  for the esptool-js CDN. OS USB-serial driver still required (usually
  automatic) — "no install" means no Python/esptool, not no driver.
- Browser flashing is **not yet exercised against real hardware here**
  (needs a hand on USB + browser); backend USB flow is proven via the
  local portal instead. Report issues.
- Client-cert minting stays on the local portal (`host/portal/`):
  signing needs the CA private key, which must never leave your PC —
  Park it server-side (even Workers) would betray that. Mint locally,
  distribute `.crt/.key` yourself.
- NVS addresses (`0x9000`/`0x6000`) mirror `firmware/partitions.csv`;
  update both if the layout ever changes.
