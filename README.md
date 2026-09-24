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
| OTA update + automatic rollback | ✅ proven (update + rollback paths) |
| GitHub Release OTA channel | ✅ CI builds + publishes |
| Ethernet / Proxmox 2-node / Web UI | ⏳ later (see AGENTS.md §25) |

## Repo layout

```text
AGENTS.md            project constitution (Vietnamese) — read first
docs/                architecture.md, protocol.md, tls.md, ffsplit.md,
                     interop.md, hardware.md, ota.md
host/                Node.js test harness (no deps): TLV/msg/session,
                     fake qnetd + fake 2nd client + TLS/TLS probes
firmware/            ESP-IDF project (C, no malloc in protocol path)
  ├── common/        shared protocol constants
  ├── qdevice/       TLV/msg codec, TCP server, session state machine
  ├── quorum/        FFSplit decision core
  ├── network/      Wi-Fi dev transport, mbedTLS server, SNTP
  ├── ota/           update engine + rollback guard
  └── ...            config, storage, watchdog, web (stubs/scaffold)
```

## Quickstart (developers)

1. Read `AGENTS.md`, then `docs/architecture.md` + `docs/protocol.md`.
2. Host harness: `cd host && npm test` (Node ≥ 20, zero deps).
3. C unit tests (WSL/Debian): `make -C firmware/qdevice/test`,
   `make -C firmware/quorum/test`.
4. Real-client interop: `docs/interop.md` (fake-qnetd + single-node corosync).
5. Firmware: ESP-IDF v6.1, `idf.py set-target esp32 && idf.py build`
   (see `firmware/README.md`, `docs/hardware.md`, `docs/tls.md`).

## Releases & OTA

Pushing a `v*` tag builds firmware in CI (self-hosted runner, cached IDF)
and publishes a GitHub Release with `firmware.bin` + `version.txt`. Devices
with `QUORUMESP_OTA_GITHUB_REPO` set pull updates over HTTPS on boot, with
automatic rollback on failed health checks. Details: `docs/ota.md`.

## Hardware (current)

ESP32 classic (WROOM-32D) + Wi-Fi **dev transport only**. Production wants
Ethernet (see `docs/hardware.md`). No PHY onboard — Ethernet needs an
external PHY board (not yet integrated).

## License

TBD — treat as all rights reserved until a LICENSE file lands.
