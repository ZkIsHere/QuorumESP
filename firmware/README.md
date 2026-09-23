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

## CI

Firmware compilation in CI arrives with Phase 2 (needs an ESP-IDF
environment — docker or the self-hosted runner with IDF installed).
Until then: host harness CI only.
