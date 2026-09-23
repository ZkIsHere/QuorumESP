# QuorumESP — QDevice network protocol (Phase 0)

> Trạng thái: **Experimental, nghiên cứu từ source chính thức**.
> Không suy đoán field/behavior. Cái gì chưa đọc code tới nơi hoặc chưa bắt gói
> với implementation thật đều ghi `CHƯA XÁC MINH`.
> Quy ước fail-closed (`AGENTS.md` §3, §8) áp cho mọi điểm chưa chắc chắn.

## 1. Nguồn tham khảo (theo thứ tự ưu tiên `AGENTS.md` §20)

1. Source chính thức `corosync/corosync-qdevice` (tách từ `corosync/corosync`):
   - `qdevices/tlv.h` — enum TLV opt, error, algorithm, vote, node state:
     <https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/tlv.h>
   - `qdevices/msg.h` — 18 message types + constructor/decode API:
     <https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/msg.h>
   - `qdevices/qnet-config.h` — default port/size/timeout/TLS:
     <https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/qnet-config.h>
   - `qdevices/msgio.h`, `qdevices/corosync-qnetd.c`, `qdevices/corosync-qdevice-net-certutil.sh`
   - Design wiki: <https://github.com/corosync/corosync-qdevice/wiki/Design>
2. Man chính thức: `corosync-qdevice(8)`, `corosync-qnetd(8)`, `corosync-qnetd-tool(8)`.
3. Proxmox: `pvecm.1`, wiki `Cluster_Manager`
   (<https://pve.proxmox.com/pve-docs/pvecm.1.html>,
   <https://pve.proxmox.com/wiki/Cluster_Manager>).
4. Behavior reference implementation (chưa đo ở Phase 0, để Phase 1/4).
5. Tài liệu độc lập: SUSE SLE HA ch.18, RHEL 9/10 ch.28, `go-qnetd` — chỉ tham khảo,
   không phải spec. Blog/forum không dùng làm spec.

## 2. Transport

| Thuộc tính | Giá trị (nguồn) |
|---|---|
| TCP port default | `5403` (`QNETD_DEFAULT_HOST_PORT`, `qnet-config.h`; khớp `pvecm status`/`pcs` docs) |
| TLS mode qnetd (`-s`) | `on` (có TLS nhưng client được phép không STARTTLS) / `off` (tắt hẳn) / `req` (bắt buộc). Default `on` (`TLV_TLS_SUPPORTED`). |
| TLS mode qdevice (`tls=`) | `on` / `off` / `required` (tương ứng, default `on`). `off` là mode duy nhất không cần NSS DB. |
| Client cert (`-c`) | `on`/`off`, default `on`. Chỉ có nghĩa khi TLS bật. |
| TLS stack gốc | NSS (NPR/NSPR socket, `nssdb`, `certutil`). ESP32 sẽ dùng mbedTLS — `CHƯA XÁC MINH` tương đương cipher/cert. |
| Địa chỉ listen | `qnetd -l <addr>`, `-4`/`-6` force IP version; qdevice `force_ip_version 0\|4\|6` (0 = IPv6 trước, fallback IPv4). |
| framing I/O | `msgio_send/msgio_write/msgio_read` trên `PRFileDesc` (`msgio.h`); buffer `dynar`. **CHƯA XÁC MINH** byte layout header trên dây — chỉ biết API `msg_get_header_length()`, `msg_get_len()`, `msg_get_type()`, `msg_is_valid_msg_type()`. Cần đọc `msg.c`/`msgio.c` + bắt gói Phase 1. |

### Behavior quan sát được (từ source, chưa chạy thực tế)

- Design wiki: "Messages are in TLV format", "Certificate is used for authentication of Qdevice-net".
- Man qnetd: "communication protocol... designed to be very simple and allow backwards compatibility" — thể hiện ở cơ chế `SUPPORTED_MESSAGES`/`SUPPORTED_OPTIONS` đàm phán trong PREINIT/INIT.

### Implementation (QuorumESP)

- Phase 1 phải chứng minh framing bằng test với `corosync-qdevice` thật trước khi
  code ESP32. ESP32 không được tự chế header/field.
- TLS fail → không chuyển ACTIVE (fail-closed).

## 3. Message types (18 loại, từ `msg.h`)

| # | Enum | Hướng điển hình | Mục đích (từ tên + constructor) |
|---|---|---|---|
| 0 | `MSG_TYPE_PREINIT` | C→S (`msg_create_preinit(cluster_name, seq)`) | Chào đầu, gửi cluster name |
| 1 | `MSG_TYPE_PREINIT_REPLY` | S→C (`...tls_supported, client_cert_required`) | Advertise TLS + yêu cầu client cert |
| 2 | `MSG_TYPE_STARTTLS` | C→S | Xin nâng lên TLS |
| 3 | `MSG_TYPE_INIT` | C→S (`decision_algorithm, supported_msgs/opts, node_id, heartbeat_interval, tie_breaker, ring_id`) | Đàm phán capability + tham số phiên |
| 4 | `MSG_TYPE_INIT_REPLY` | S→C (`reply_error_code, supported_msgs/opts, server_max_req/reply_size, supported_algorithms`) | Chốt capability + giới hạn size |
| 5 | `MSG_TYPE_SERVER_ERROR` | S→C (`reply_error_code`) | Báo lỗi server |
| 6 | `MSG_TYPE_SET_OPTION` | hai chiều (`heartbeat_interval?, keep_active_partition_tb?`) | Đổi option runtime |
| 7 | `MSG_TYPE_SET_OPTION_REPLY` | hai chiều | Xác nhận option |
| 8 | `MSG_TYPE_ECHO_REQUEST` | hai chiều (`seq`) | Heartbeat / dead-peer detection |
| 9 | `MSG_TYPE_ECHO_REPLY` | hai chiều (echo lại request) | Trả lời heartbeat |
| 10 | `MSG_TYPE_NODE_LIST` | C→S (`seq, type, ring_id?, config_version?, quorate?, heuristics?, nodes`) | Báo config/membership/quorum list |
| 11 | `MSG_TYPE_NODE_LIST_REPLY` | S→C (`seq, type, ring_id, vote`) | Trả vote cho node list |
| 12 | `MSG_TYPE_ASK_FOR_VOTE` | C→S (`seq`) | Xin vote |
| 13 | `MSG_TYPE_ASK_FOR_VOTE_REPLY` | S→C (`seq, ring_id, vote`) | Cho vote |
| 14 | `MSG_TYPE_VOTE_INFO` | S→C? (`seq, ring_id, vote`) | Thông báo vote |
| 15 | `MSG_TYPE_VOTE_INFO_REPLY` | C→S (`seq`) | ACK vote info |
| 16 | `MSG_TYPE_HEURISTICS_CHANGE` | C→S (`seq, heuristics`) | Báo đổi heuristics |
| 17 | `MSG_TYPE_HEURISTICS_CHANGE_REPLY` | S→C (`seq, ring_id, heuristics, vote`) | Vote sau heuristics |

> Thứ tự handshake đầy đủ trên dây **CHƯA XÁC MINH**. Chỉ chắc chắn PREINIT/INIT
> tồn tại từ constructor. Phase 1 phải ghi lại pcap + log `corosync-qdevice -d -f`
> rồi mới chốt state machine.

## 4. TLV options (24 loại, từ `tlv.h`)

`0 MSG_SEQ_NUMBER`, `1 CLUSTER_NAME`, `2 TLS_SUPPORTED`, `3 TLS_CLIENT_CERT_REQUIRED`,
`4 SUPPORTED_MESSAGES`, `5 SUPPORTED_OPTIONS`, `6 REPLY_ERROR_CODE`,
`7 SERVER_MAXIMUM_REQUEST_SIZE`, `8 SERVER_MAXIMUM_REPLY_SIZE`, `9 NODE_ID`,
`10 SUPPORTED_DECISION_ALGORITHMS`, `11 DECISION_ALGORITHM`, `12 HEARTBEAT_INTERVAL`,
`13 RING_ID (node_id:u32 + seq:u64)`, `14 CONFIG_VERSION (u64)`, `15 DATA_CENTER_ID`,
`16 NODE_STATE`, `17 NODE_INFO (node_id + dc_id + state)`, `18 NODE_LIST_TYPE`,
`19 VOTE`, `20 QUORATE`, `21 TIE_BREAKER`, `22 HEURISTICS`, `23 KEEP_ACTIVE_PARTITION_TIE_BREAKER`.

Enum giá trị quan trọng (nguyên văn từ `tlv.h`):

- `tls: UNSUPPORTED=0, SUPPORTED=1, REQUIRED=2`
- `algorithm: TEST=0, FFSPLIT=1, 2NODELMS=2, LMS=3`
- `vote: UNDEFINED=0, ACK=1, NACK=2, ASK_LATER=3, WAIT_FOR_REPLY=4, NO_CHANGE=5`
- `quorate: INQUORATE=0, QUORATE=1`
- `node_state: NOT_SET=0, MEMBER=1, DEAD=2, LEAVING=3`
- `node_list_type: INITIAL_CONFIG=0, CHANGED_CONFIG=1, MEMBERSHIP=2, QUORUM=3`
- `tie_breaker_mode: LOWEST=1, HIGHEST=2, NODE_ID=3`
- `heuristics: UNDEFINED=0, PASS=1, FAIL=2`
- `keep_active_partition_tb: DISABLED=0, ENABLED=1`

`reply_error_code` 0–19 (`tlv.h`): `NO_ERROR`, `UNSUPPORTED_NEEDED_MESSAGE/OPTION`,
`TLS_REQUIRED`, `UNSUPPORTED_MESSAGE`, `MESSAGE_TOO_LONG`, `PREINIT_REQUIRED`,
`DOESNT_CONTAIN_REQUIRED_OPTION`, `UNEXPECTED_MESSAGE`, `ERROR_DECODING_MSG`,
`INTERNAL_ERROR`, `INIT_REQUIRED`, `UNSUPPORTED_DECISION_ALGORITHM`,
`INVALID_HEARTBEAT_INTERVAL`, `UNSUPPORTED_DECISION_ALGORITHM_MESSAGE`,
`TIE_BREAKER_DIFFERS_FROM_OTHER_NODES`, `ALGORITHM_DIFFERS_FROM_OTHER_NODES`,
`DUPLICATE_NODE_ID`, `INVALID_CONFIG_NODE_LIST`, `INVALID_MEMBERSHIP_NODE_LIST`.

### Implementation

- Mọi message không hợp lệ → reject; violation → kết thúc session (fail-closed).
- Không tự thêm field. Backward compat dựa trên `SUPPORTED_*` đã có trong protocol.

## 5. Thuật toán và vote (tóm tắt để trỏ sang architecture)

- Hỗ trợ cứng 4 algorithm (`QNETD_STATIC_SUPPORTED_DECISION_ALGORITHMS_SIZE=4`,
  `qnet-config.h`): `TEST, FFSPLIT, 2NODELMS, LMS`. Production chỉ `FFSPLIT`/`LMS`
  (man qdevice). `TEST` chỉ bật khi build `--enable-debug`.
- `tie_breaker {mode, node_id}`, `ring_id {node_id, seq}`, `heuristics PASS/FAIL`,
  `quorate QUORATE/INQUORATE` là đầu vào của algorithm.
- QuorumESP chỉ tính vote theo algorithm đã đàm phán; quyết định quorate cuối cùng
  thuộc về `votequorum` trên node. Không tự suy diễn semantics ngoài man + source
  algorithm (`qnetd-algo-ffsplit.c`, `qnetd-algo-lms.c` — chưa đọc sâu ở Phase 0).

## 6. Timeout, heartbeat, DPD, giới hạn size (từ `qnet-config.h`)

| Tham số | Default |
|---|---|
| Heartbeat interval chấp nhận (cả 2 phía) | min `1000` ms, max `120000` ms |
| DPD | `enabled`, hệ số `1.5 × heartbeat` (`dpd_interval_coefficient`, min 1, max 1000) |
| qdevice connect timeout | min `1000`, max `120000` ms |
| qnetd max client send | `32` buffer × `32768` B; receive max `32768` B |
| qnetd IPC | max client `10`, recv `4096` B, send `10485760` B |
| qdevice-net initial msg | send/recv `32768` B; max recv `16777216` B; max send buffer `10` |
| qdevice `timeout`/`sync_timeout` | `10000` / `30000` ms (đồng thời là heartbeat base) |
| listen backlog / lock / IPC socket | `10`, `/var/run/corosync-qnetd/*.pid/.sock`, chạy được non-root (`coroqnetd`) |

### Implementation

- Timeout → không giả định peer còn sống; mất TCP → invalidate session; watchdog →
  restart (`AGENTS.md` §8, §11).
- Không giữ vote cũ vô thời hạn sau mất kết nối.

## 7. Session lifecycle (khung để Phase 1 điền, chưa chốt)

```text
TCP connect
  → PREINIT (cluster_name)
  → PREINIT_REPLY (tls_supported, client_cert_required)
  → [STARTTLS nếu đàm phán TLS]        CHƯA XÁC MINH thứ tự bắt buộc
  → INIT (algorithm, node_id, heartbeat, tie_breaker, ring_id, supported_*)
  → INIT_REPLY (error_code, limits, supported_algorithms)
  → SET_OPTION* / ECHO*/NODE_LIST*/ASK_FOR_VOTE*/VOTE_INFO*/HEURISTICS_CHANGE*
  → SERVER_ERROR hoặc timeout/disconnect → invalidate + reconnect (phía qdevice)
```

Nguồn cho reconnect: Design wiki "Reconnect when connection to qnetd is lost";
`qdevice-net-algorithm.h: disconnected(..., *try_reconnect, *vote)`.

## 8. Việc còn lại cho Phase 1 (bắt buộc trước ESP32)

1. Đọc `tlv.c`, `msg.c`, `msgio.c`, `nss-sock.c`, `qdevice-net-socket.c`,
   `qnetd-client-msg-received.c`, `qdevice-net-msg-received.c` từng dòng; ghi lại
   header layout, endian, max size enforce.
2. Dựng harness host: connect, gửi/parse từng message, malformed, TLS on/off/req,
   mất kết nối, reconnect, nhiều client, state machine.
3. Bắt gói + log với `corosync-qdevice`/`corosync-qnetd` thật (package Debian hoặc
   build từ source) để chốt handshake và DPD/heartbeat thực tế.
4. Xác minh mapping NSS→mbedTLS (cipher, cert CN `Qnetd Server`/`Cluster Cert`,
   password file, renew/expire flow).
5. Mọi phát hiện cập nhật vào file này theo mẫu `Nguồn / Behavior quan sát / Implementation`.
