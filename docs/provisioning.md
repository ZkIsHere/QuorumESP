# QuorumESP — provisioning (Wi-Fi credentials into NVS)

> Release builds carry NO credentials (`sdkconfig.release` has none on
> purpose). A release image without provisioned NVS fails Wi-Fi and keeps
> running old code — fail-closed. Device write-up: `firmware/README.md`.

## 1. Why NVS (not Kconfig)

Kconfig credentials bake secrets into every developer's build and can leak
into CI artifacts. NVS keeps one secret store on the device, written once
over USB, surviving app flashes and OTA updates (app partitions never touch
the `nvs` partition at 0x9000).

Lookup order (`config_get_wifi`): NVS `qesp/ssid` → Kconfig fallback (dev).

## 2. Provision (USB, once per device)

Namespace `qesp`, format version key `ver` (current v1). Full key table:

| Key | Type | Meaning |
|---|---|---|
| `ver` | u32 | format version (written by firmware) |
| `host` | str | DHCP/hostname (default `quorumesp`) |
| `wssid` / `wpass` | str | Wi-Fi credentials |
| `netmode` | u8 | 0 DHCP, 1 static |
| `ip`/`msk`/`gw`/`dns1`/`dns2` | u32 | static net, network byte order (set via Web UI later; CSV needs decimal conversion, not documented yet — DHCP for now) |
| `devid` | str | stable device id (MAC-derived on first boot) |
| `logl` | u8 | log level 0..5 |
| `qdport` | u16 | qdevice listen port |

Minimal Wi-Fi provisioning CSV (only what `nvs_partition_gen` handles well):

In ESP-IDF PowerShell, any directory (use `%TEMP%`, never the repo —
the CSV holds the real secret):

```powershell
cd $env:TEMP
@"
key,type,encoding,value
qesp,namespace,,
wssid,data,string,<YOUR-SSID>
wpass,data,string,<YOUR-PASSWORD>
"@ | Out-File -Encoding ascii wifi.csv
python $env:IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py generate wifi.csv nvs-wifi.bin 0x6000
python -m esptool --chip esp32 -p COM3 write_flash 0x9000 nvs-wifi.bin
Remove-Item wifi.csv, nvs-wifi.bin
```

Close the serial monitor first (COM port is exclusive). Reboot: log must
show `wifi creds from NVS` instead of the Kconfig fallback warning.

## 3. Safety notes

- `write_flash 0x9000` rewrites the whole NVS partition (24K). Nothing else
  on this firmware uses NVS yet — safe today; re-read this when storage
  grows (§10 work will namespace everything under versioned keys).
- Never commit `wifi.csv` / `nvs-wifi.bin` (gitignored patterns cover keys
  and bins; the commands above run outside the repo anyway).
- Factory reset = `idf.py erase-flash` (wipes creds too — reprovision after).
- Open networks: omit the `pass` row (empty password is accepted).
