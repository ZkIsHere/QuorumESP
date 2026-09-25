# QuorumESP — Hardware (proposal, Phase 2 input)

> Trạng thái: **đề xuất, chưa chốt mua**. Core firmware không phụ thuộc cứng
> controller nào (`network/ethernet` qua `esp_eth`/`esp_netif`).

## 1. Ràng buộc từ AGENTS.md §5

- ESP32 có **Ethernet** (ưu tiên hơn Wi-Fi — infrastructure component).
- Không phụ thuộc cứng một Ethernet controller duy nhất.

## 2. Thực tế ESP-IDF cần biết

- Chỉ ESP32 **classic** có Ethernet MAC tích hợp (RMII). Các dòng S2/S3/C3/C6
  **không** có EMAC → bắt buộc Ethernet SPI ngoài (nhiều driver code hơn).
- Vì vậy target là ESP32 classic + PHY RMII ngoài. `sdkconfig.defaults` đã
  ghim `CONFIG_IDF_TARGET="esp32"`.

## 3. Đề xuất chính: WT32-ETH01 (hoặc tương đương LAN8720)

| Tiêu chí | WT32-ETH01 |
|---|---|
| MCU | ESP32 classic (có EMAC) |
| PHY | LAN8720A, RMII — `esp_eth` hỗ trợ native trong IDF |
| Giá/tình trạng | rẻ, phổ biến, nhiều seller |
| GPIO | đủ cho LED/button/OLED sau này (§14) |

Board tương đương chấp nhận được: bất kỳ board ESP32 + LAN8720/TL8201 RMII
nào có schematic rõ ràng (VD: LilyGO T-ETH, Espressif ETH devkit).

## 4. Phương án dự phòng: W5500 qua SPI

- Chạy được cả trên ESP32-S3/C3 (không có EMAC).
- Tốn thêm code SPI + `esp_eth` SPI driver; throughput thấp hơn RMII nhưng
  đủ cho QDevice (message nhỏ, heartbeat giây).
- Chỉ làm khi board RMII không mua được. Abstraction `network/ethernet`
  giữ nguyên interface để đổi PHY không chạm core.

## 5. Không chọn lúc này

- Wi-Fi làm uplink chính (chỉ dùng cho setup/diagnostic nếu cần).
- PoE/custom carrier board (để sau khi core ổn định).
- Pin/button/buzzer chi tiết (§14) — sau Phase 2.

## 6. Việc cần làm khi có board (Phase 2)

1. `idf.py set-target esp32` + `menuconfig` chỉnh chân RMII theo schematic.
2. Up link, DHCP/static IP, ping soak test 24h.
3. Mất link / rút cáp → `ethernet` báo sự kiện, session invalidate (fail-closed).
4. Ghi kết quả vào file này (board thực tế + rev + chân pin).

## 7. Dev board hiện tại (WROOM-32D, Wi-Fi) — bài học nguồn

> 2026-09-25: board boot-loop `E BOD: Brownout detector was triggered`
> ngay tại `phy_init` (RF calibration burst), `rst:0x3 (SW_RESET)`,
> không bao giờ tới `wifi up`. Trước đó cùng ngày board chạy ổn định
> nhiều giờ (flash, web UI live, vote thật) rồi rớt dần: PREINIT còn
> trả lời nhưng TLS handshake (CPU + TX burst) làm sập nguồn giữa chừng.
> Đây là lỗi nguồn USB/cáp, KHÔNG phải firmware (TX đã cap 13 dBm trong
> `wifi.c`, nhưng không cứu được supply đã yếu).
>
> Quy tắc:
>
> - Mọi rớt mạng/reboot khó hiểu → nghi nguồn TRƯỚC khi nghi code.
>   Dấu hiệu: `E BOD`, `rst:0x3` ngay sau `phy_init`, hoặc chết giữa
>   TLS handshake/OTA download.
> - Dùng nguồn ngoài/cáp tốt, không đổi cổng USB giữa chừng khi test.
> - Mở COM3 (kể cả passive snoop) RESET board qua mạch DTR/RTS
>   (bootloader `rst:0x1` ngay khi open). Tuyệt đối không mở serial
>   trong lúc chạy live test mạng — quan sát qua log mạng/probe.
