# QuorumESP — firmware updates (external only)

There is NO on-device updater. No auto-check, no download, no version
memory in firmware (removed; see git history for the old GitHub
auto-update design and its live tests). Updates are performed by the
EXTERNAL PC portal (`host/portal/`) over USB serial:

- pick the board (serial port)
- pick firmware (GitHub release asset or local `.bin`)
- flash app image to `0x20000` via esptool
- provision Wi-Fi (SSID/pass → NVS at `0x9000`)
- read back the running version from the boot log

## Why external

- A quorum box must never surprise-reboot itself from the network.
- USB flashing keeps a human in the loop; every update is deliberate.
- The portal also serves other people's boards (same flow, their PC).

## Release artifacts

GitHub releases carry `firmware.bin` (app image for `0x20000`) +
`version.txt` (exact `PROJECT_VER` baked at build). The portal can pull
`firmware.bin` straight from a release URL. `version.txt` is informational
(the portal reads the real version back from the boot log after flashing).

## Safety notes

- App-only flash (`0x20000`) leaves bootloader + partitions + NVS alone.
  First-ever flash of a blank chip still needs the full `idf.py flash`.
- `write_flash 0x9000` rewrites the whole NVS partition (Wi-Fi creds +
  config). Nothing else uses NVS on current firmware.
- Closing the serial monitor before flashing (COM port is exclusive).
- After flashing, press EN and watch for `App version:` + `wifi up`.

## Live proof (2026-09-25)

Portal flashed `v0.3.0` release `firmware.bin` (downloaded from GitHub
inside the portal) over USB → version read-back `v0.3.0` → board rejoined
Wi-Fi, qdevice `Connected`. No on-device update code involved.
