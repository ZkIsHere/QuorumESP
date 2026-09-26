# QuorumESP static portal (`host/portal-static/`)

External management web with **no server**: static files on Cloudflare
Pages + one tiny Worker + browser WebSerial. For other people's boards:
pick a board card, see firmware version, flash a release.

```sh
# Deploy (Cloudflare Pages, static dir + one Function):
npx wrangler pages deploy host/portal-static --project-name quorumes-portal
# Local preview:
python -m http.server 8000 --directory host/portal-static
# then open http://localhost:8000 in Chrome/Edge
```

## UI (deliberately minimal)

- Top: project name + firmware version of the connected board.
- Board grid: static 2D art + model per supported board (`boards.js`
  catalog — add future hardware there). Click = WebSerial connect.
- Release select + Flash: release `firmware.bin` → app slot `0x20000`.
  No file picker, no config UI, no status page.

## What runs where

| Feature | How (no server) |
|---|---|
| Board pick | WebSerial `requestPort` (per-card USB filters) |
| Board model | Static catalog (`boards.js`) + chip name from esptool-js |
| App version | WebSerial raw: reset board, regex `App version:` from boot |
| Firmware list | `fetch` api.github.com (CORS-open) |
| Browser flash | esptool-js `writeFlash` to `0x20000` (CDN bundle) |
| Release proxy | `functions/api/dl.js` (Worker: same-repo URLs only, 4MB cap) |

## NVS note

An earlier revision had a byte-exact JS NVS reader/generator for
in-browser config editing (proven `cmp`-clean vs the IDF generator).
Removed with the config UI (git history keeps it). Config provisioning
lives in `docs/provisioning.md` (NVS partition via esptool) instead.

## Honest limits

- Needs Chrome/Edge (WebSerial) + HTTPS (Pages gives it) + internet once
  for the esptool-js CDN. OS USB-serial driver still required (usually
  automatic) — "no install" means no Python/esptool, not no driver.
- Browser flashing is **not yet exercised against real hardware here**
  (needs a hand on USB + browser). Report issues.
- Client-cert minting is `host/pki/mint-client.sh` on your own PC:
  signing needs the CA private key, which must never leave it.
