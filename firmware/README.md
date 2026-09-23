# QuorumESP firmware

ESP-IDF project scaffold. **No QDevice code yet** — init order and module
boundaries only (Phase 2 starts here, Phase 3 adds the protocol).

## Hardware

See `docs/hardware.md`. Target: classic ESP32 (`idf.py set-target esp32`).

## Build (needs ESP-IDF v5.x installed)

```sh
# ESP-IDF PowerShell/terminal with idf.py on PATH:
cd firmware
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

`sdkconfig.defaults` is the committed template; the generated `sdkconfig`
and `build/` are git-ignored.

## Layout (AGENTS.md §6)

```text
firmware/
├── main/          app_main (boot log only for now)
├── qdevice/       protocol / session / state  (Phase 3)
├── quorum/        vote / membership            (Phase 3)
├── network/       ethernet / tls               (Phase 2)
├── config/        versioned persistent config  (Phase 2)
├── storage/       NVS wrapper                  (Phase 2)
├── watchdog/      HW + task watchdog           (Phase 2)
├── web/           diagnostic API, post-core    (Phase 10)
└── diagnostics/   logging categories           (Phase 2)
```

## Dev bring-up over Wi-Fi (NON-PRODUCTION, Ethernet skipped for now)

> Wi-Fi is a dev transport only. Production needs Ethernet
> (`docs/hardware.md`, `AGENTS.md` §5). This deviation is recorded here.

```sh
# In ESP-IDF PowerShell, from firmware/:
idf.py set-target esp32
idf.py menuconfig   # QuorumESP dev -> Wi-Fi SSID + password (local sdkconfig only!)
idf.py build
idf.py -p COM3 flash monitor
```

On boot the log prints the STA IP. Point a real `corosync-qdevice` at
`IP:5403` with `tls: off` (same `corosync.conf` as `docs/interop.md`,
`host:` = ESP32 IP). Expected serial output:

```text
I (xxx) BOOT: QuorumESP boot (EXPERIMENTAL, Wi-Fi dev transport)
I (xxx) NETWORK: wifi up, ip=192.168.x.x
I (xxx) QDEVICE: qnetd-side listening on port 5403 (TEST STUB vote=ACK)
I (xxx) STATE: CONNECTED -> PREINIT_DONE
I (xxx) STATE: PREINIT_DONE -> ACTIVE node=1 algo=1 hb=8000
```

Vote is a fixed-ACK stub; cluster integration verdicts come later (Phase 4).

## CI

Firmware compilation in CI arrives with Phase 2 (needs an ESP-IDF
environment — docker or the self-hosted runner with IDF installed).
Until then: host harness CI only.
