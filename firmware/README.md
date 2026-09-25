# QuorumESP firmware

ESP-IDF (v6.1) qnetd-side implementation: real FFSplit + LMS vote paths
against `corosync-qdevice`, fail-closed. See `docs/` for protocol,
interop evidence, and the failure matrix.

## Hardware

See `docs/hardware.md`. Target: classic ESP32 (`idf.py set-target esp32`).
Current dev board: WROOM-32D over Wi-Fi (NON-PRODUCTION transport).

## Build

```sh
# ESP-IDF PowerShell/terminal with idf.py on PATH:
cd firmware
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

`sdkconfig.defaults` is the committed template; the generated `sdkconfig`
and `build/` are git-ignored. Menuconfig lives under `QuorumESP`
(Network → Wi-Fi, Web UI, TLS certificates, debug).

## Layout (AGENTS.md §6)

```text
firmware/
├── main/          app_main boot order
├── common/        portable protocol constants (no IDF dep)
├── qdevice/       TLV/msg codec + multi-client server (server.c)
├── quorum/        FFSplit + LMS decision cores
├── network/       Wi-Fi STA, TLS (mbedTLS/esp-tls), SNTP time sync
├── config/        versioned NVS config + migration (+host unit tests)
├── watchdog/      task watchdog, panic-on-timeout
└── web/           read-only diagnostic UI (Basic auth + SSE, default OFF)
```

Dead Phase-2 scaffold stubs (`protocol/session/state`, `vote/membership`,
`ethernet`, `storage`, `diagnostics`, on-device OTA) were deleted once the
real implementations landed — history is in git.

## Dev bring-up over Wi-Fi (NON-PRODUCTION, Ethernet skipped for now)

> Wi-Fi is a dev transport only. Production needs Ethernet
> (`docs/hardware.md`, `AGENTS.md` §5). This deviation is recorded here.

```sh
# In ESP-IDF PowerShell, from firmware/:
idf.py set-target esp32
idf.py menuconfig   # QuorumESP -> Network -> Wi-Fi SSID + password (local sdkconfig only!)
idf.py build
idf.py -p COM3 flash monitor
```

(Credentials can also be provisioned straight to NVS — see
`docs/provisioning.md` — which wins over menuconfig.)

On boot the log prints the STA IP. Point a real `corosync-qdevice` at
`IP:5403` (see `docs/interop.md`). Expected serial output:

```text
I (xxx) BOOT: QuorumESP boot (EXPERIMENTAL, Wi-Fi dev transport)
I (xxx) NETWORK: wifi up, ip=192.168.x.x
I (xxx) QDEVICE: qnetd-side listening on port 5403 (FFSplit+LMS, lwip_socks=10)
I (xxx) STATE: CONNECTED -> PREINIT_DONE
I (xxx) STATE: PREINIT_DONE -> ACTIVE node=1 algo=1 hb=8000
```

Votes are real decisions (FFSplit/LMS, reference-faithful); live evidence
in `docs/interop.md` + `docs/failure-matrix.md`.

## Updates

No on-device updater. Flash via USB (`idf.py flash`) or the external PC
portal (`host/portal/` — release `firmware.bin` to `0x20000`). See
`docs/ota.md`.

## CI

`.github/workflows/ci.yml`: host protocol tests, C unit tests (gcc),
firmware compilation. Release tags (`v*`) build + publish `firmware.bin`
+ `version.txt` via `.github/workflows/release.yml`.
