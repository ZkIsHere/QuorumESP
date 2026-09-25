# Web diagnostic UI (AGENTS.md §13)

Read-only diagnostics. Không điều khiển quorum, không đổi config,
không trigger OTA. Hiển thị sau khi QDevice core đã ổn định.

## Bật (dev only, default OFF)

```text
idf.py menuconfig  ->  QuorumESP dev  ->  Diagnostic web UI
```

`QUORUMESP_WEB_ENABLE=n` (default): `web_api_init()` là no-op, không
link httpd vào đường chạy. Production để OFF (giảm attack surface).

## Endpoints (chỉ GET — POST trả 405 do không có handler đăng ký)

| Method | Path         | Nội dung                                             |
| ------ | ------------ | ---------------------------------------------------- |
| GET    | `/`          | Trang HTML: version, id, uptime, heap, net, cluster, bảng clients |
| GET    | `/api/status`| JSON snapshot: firmware / net / cluster / clients    |
| GET    | `/api/log`   | JSON array 40 dòng log gần nhất (không chứa secret)  |

Snapshot lấy qua `qdevice_status_snapshot()` — copy có khóa, timeout
200 ms, không bao giờ block đường vote.

Vote hiển thị là vote đã decide (FFSplit: trong quorate → ack;
LMS: vote đã lưu), không phải trạng thái quorum của cluster.

## Giới hạn đã biết

- Log ring chỉ giữ từ lúc web init (dòng boot trước đó không có).
  Muốn giữ log boot: chuyển `esp_log_set_vprintf` sớm hơn trong
  `app_main` (chưa làm — không ảnh hưởng đường vote).
- Không auth. Chỉ dùng trong mạng lab tin cậy, tắt khi xong việc.
- Chưa có HTTPS cho UI (qdevice TLS là kênh riêng, không liên quan).

## Live test (đã chạy, board WROOM-32D)

- `GET /` render đúng: version/id/uptime/heap/ip/rssi/tls/cluster/bảng node.
- `GET /api/status` JSON hợp lệ, khớp session thật
  (node 1, ffsplit, tls 1, vote ack, mutual CN verified).
- `GET /api/log` chứa dòng boot + session (`TLS handshake done`,
  `VOTE_INFO node=1 vote=1`).
- `POST /` và `POST /api/status` → `405` (read-only đúng thiết kế).
- Sau test: flash lại bản default (web OFF), board về steady state.
