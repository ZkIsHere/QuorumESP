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

In ESP-IDF PowerShell, any directory (use `%TEMP%`, never the repo —
the CSV holds the real secret):

```powershell
cd $env:TEMP
@"
key,type,encoding,value
qesp,namespace,,
ssid,data,string,<YOUR-SSID>
pass,data,string,<YOUR-PASSWORD>
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
