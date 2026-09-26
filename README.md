# QuorumESP

> **EXPERIMENTAL — do not deploy to production clusters.** Interop and failure
> behavior are still being proven. See [AGENTS.md](AGENTS.md) §23.

QuorumESP is an ESP32-based network quorum device that provides a **QDevice
vote** compatible with Corosync QDevice for Proxmox VE clusters. Corosync
decides quorum; this firmware only ever supplies a vote — never cluster
management, fencing, or health verdicts.

```text
Proxmox Node A (corosync-qdevice) ──┐
                                    ├── TLS / QDevice protocol ──> QuorumESP (qnetd-side)
Proxmox Node B (corosync-qdevice) ──┘                              FFSplit vote
```

## Status

| Area | State |
|---|---|
| QDevice framing/handshake (plaintext + TLS mutual) | ✅ proven vs real `corosync-qdevice` |
| FFSplit voting (tie / failover / keep-active) | ✅ proven 2-client live |
| 4 concurrent sessions (self-assessed admission) | ✅ proven live (`FOUR` 4/4) |
| Updates | ✅ external only — USB/portal flash, no on-device updater |
| GitHub Release channel | ✅ CI builds + publishes `firmware.bin` for the portal |
| Ethernet / Web UI | ⏳ later (see AGENTS.md §25) |

## Repo layout

```text
AGENTS.md            project constitution (Vietnamese) — read first
docs/                architecture.md, protocol.md, tls.md, ffsplit.md,
                     interop.md, hardware.md, ota.md
host/                Node.js test harness (no deps): TLV/msg/session,
                     fake qnetd + fake 2nd client + TLS/TLS probes
firmware/            ESP-IDF project (C)
  ├── common/        shared protocol constants
  ├── qdevice/       TLV/msg codec + multi-client TCP server
  ├── quorum/        FFSplit + LMS decision cores
  ├── network/       Wi-Fi dev transport, TLS (menuconfig certs), SNTP
  ├── ...            config (NVS), watchdog, web (read-only UI)
host/                Node harness + Python probes + static web portal
  portal-static/    Cloudflare Pages portal (WebSerial, no server)
  pki/              local CA + mint-client.sh (client certs, dev only)
```

## Quickstart (developers)

1. Read `AGENTS.md`, then `docs/architecture.md` + `docs/protocol.md`.
2. Host harness: `cd host && npm test` (Node ≥ 20, zero deps).
3. C unit tests (WSL/Debian): `make -C firmware/qdevice/test`,
   `make -C firmware/quorum/test`.
4. Real-client interop: `docs/interop.md` (fake-qnetd + single-node corosync).
5. Firmware: ESP-IDF v6.1, `idf.py set-target esp32 && idf.py build`
   (see `firmware/README.md`, `docs/hardware.md`, `docs/tls.md`).

## Releases & updates

Pushing a `v*` tag builds firmware in CI (self-hosted runner, cached IDF)
and publishes a GitHub Release with `firmware.bin` + `version.txt`. There
is NO on-device auto-updater by design — flash via USB (`idf.py flash`)
or the static portal (`host/portal-static/`). Details:
`docs/ota.md`.

## Hardware (current)

ESP32 classic (WROOM-32D) + Wi-Fi **dev transport only**. Production wants
Ethernet (see `docs/hardware.md`). No PHY onboard — Ethernet needs an
external PHY board (not yet integrated).

## License

TBD — treat as all rights reserved until a LICENSE file lands.
