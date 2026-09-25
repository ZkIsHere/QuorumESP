# Web diagnostic UI (AGENTS.md §13)

Read-only diagnostics. Không điều khiển quorum, không đổi config,
không trigger OTA. Hiển thị sau khi QDevice core đã ổn định.

## Bật + auth (dev only, default OFF)

`QUORUMESP_WEB_ENABLE=n` (default): `web_api_init()` là no-op.
Production để OFF (giảm attack surface).

Bật trong menuconfig (`QuorumESP dev`):

- `QUORUMESP_WEB_ENABLE=y`
- `QUORUMESP_WEB_USER` / `QUORUMESP_WEB_PASSWORD` (local sdkconfig,
  git-ignored — giống cách giữ Wi-Fi secret)

Không user/pass → UI từ chối khởi động (`ESP_FAIL`, không có trang mở).
Mọi handler kiểm tra `Authorization: Basic` (so sánh constant-time);
thiếu/sai → `401` + `WWW-Authenticate`. POST vốn đã 405 (không handler).

## Hiển thị (cố ý tối giản)

- Project name + device id, version, uptime (`22s`, `1h 1m 1s`,
  `2d 3h 4m 5s`, lên tới `y`)
- Bảng clients đang kết nối (node, algo)
- Log gần nhất (không chứa secret — project không bao giờ log secret)

Endpoints (đều GET + auth):

| Method | Path         | Nội dung                              |
| ------ | ------------ | ------------------------------------- |
| GET    | `/`          | Trang HTML: tên/version/uptime/clients/log |
| GET    | `/api/status`| JSON cùng tập field                   |
| GET    | `/api/log`   | JSON array 40 dòng log gần nhất       |

Snapshot lấy qua `qdevice_status_snapshot()` — copy có khóa, timeout
200 ms, không bao giờ block đường vote.

## Giới hạn đã biết

- Log ring chỉ giữ từ lúc web init (dòng boot trước đó không có).
- Không auth session/rate-limit, không HTTPS cho UI — chỉ dùng trong
  mạng lab tin cậy, tắt khi xong việc. (qdevice TLS là kênh riêng,
  không liên quan.)
- user/pass nằm plaintext trong sdkconfig/NVS build — chấp nhận được
  cho dev, production cần provisioning riêng (chưa thiết kế).

## Live test (board WROOM-32D, đã chạy)

- Không auth → `401` cả 3 endpoint; sai pass → `401`.
- Đúng auth → `/api/status` JSON gọn (`project/version/uptime/clients`),
  `/` render bảng node + `<pre>` log khớp session thật.
- Sau test: flash lại bản default (web OFF), board về steady state.
