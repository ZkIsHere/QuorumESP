# QuorumESP — OTA (AGENTS.md §12)

> Model: firmware mới → reboot → health check 60s → PASS commit / FAIL rollback
> tự động bởi bootloader. Dev transport là HTTP thường — production cần HTTPS
> + ký image (chưa làm, ghi ở §5).

## 1. Layout flash (4 MB, `firmware/partitions.csv`)

```text
nvs        24K  Wi-Fi/cert blob về sau
otadata     8K  OTA state (bootloader)
ota_0    1.6M   app slot A
ota_1    1.6M   app slot B
storage  ~640K  dự trữ (diagnostics)
```

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`: image mới boot ở trạng thái
pending-verify; crash/reboot trước khi confirm → về image cũ.

## 2. Flow (`firmware/ota/`)

```text
boot → wifi → time → [OTA check] → server
                        │
              QUORUMESP_OTA_CHECK=n → bỏ qua
              URL rỗng → bỏ qua
              GET <url>/version.txt == running → bỏ qua
              khác → GET <url>/firmware.bin → verify → set boot → reboot
                        │
                        v (image mới)
              boot → ... → server up → 60s khỏe → mark valid (commit)
              crash trước 60s → bootloader rollback về image cũ
```

- Version hai bên là `esp_app_desc` version (hiện là git hash ngắn) so chuỗi.
  Guard tự nhiên chống loop: image mới version == server → không tải lại.
- `quorumesp_ota_check_and_update()` không bao giờ trả về khi update (reboot).
  Fail ở bất kỳ bước nào → chạy tiếp image cũ, không chạm boot partition.

## 3. Test với host (không cần tool riêng)

Trên PC, từ `firmware/build/` (sau `idf.py build`):

```powershell
# version mong muốn (hash commit mới, xem App version ở log boot bản mới)
"<HASH-MOI>" | Out-File -NoNewline -Encoding ascii ota-test/version.txt
Copy-Item build/QuorumESP.bin ota-test/firmware.bin
cd ota-test
python -m http.server 8000
```

Trên ESP32 (`menuconfig → QuorumESP dev`): bật `OTA check`, URL
`http://<IP-PC>:8000`, build + flash bản **cũ** (đang chạy). Reboot → log
phải hiện update → reboot vào bản mới → sau 60s `confirming image`.
Kiểm tra `App version` đổi sang hash mới.

Rollback test: tạo image hỏng (VD: build với `while(1)` ở đầu app_main trên
branch test, hoặc flash bản lỗi) → đưa lên OTA → quan sát reboot loop 1 lần
rồi về bản cũ. Ghi kết quả vào đây.

## 4. An toàn

- Không bao giờ set boot partition khi download/verify fail.
- Health 60s bao gồm Wi-Fi + server task (app_main tới được confirm task
  nghĩa là boot path sống).
- Dev dùng HTTP LAN. Production: HTTPS + `cert_pem` trong ota config +
  secure-boot/flash-encryption + ký image (`esp_ota` verify signature khi
  bật secure boot). Chưa implement — không gọi production-ready khi thiếu.

## 5. Không làm ở vòng này

- Polling định kỳ / scheduled window (hiện check 1 lần mỗi boot).
- Delta update, resume download gián đoạn.
- Rollback thủ công từ xa (chưa có management UI).
