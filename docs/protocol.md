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
| framing I/O | ĐÃ XÁC MINH từ source (xem §2.1): header 6 B + TLV stream; đọc theo `msgio_read` (`msgio.c`), buffer `dynar` giới hạn bởi max receive size. |

### 2.1. Wire format — ĐÃ XÁC MINH từ `msg.c`, `tlv.c`, `msgio.c`

Nguồn:

- <https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/msg.c>
- <https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/tlv.c>
- <https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/msgio.c>

**Message header (6 byte, `MSG_TYPE_LENGTH=2` + `MSG_LENGTH_LENGTH=4`):**

```text
offset 0: u16 type  (big-endian, htons/ntohs)
offset 2: u32 len   (big-endian, htonl/ntohl) = số byte TLV đứng sau header
offset 6: TLV stream (len byte)
```

`msg_set_len()` ghi `dynar_size - 6`; `msg_get_len()` đọc u32 BE tại offset 2.
Type hợp lệ duy nhất là 0–17 (`msg_is_valid_msg_type`).

**TLV (`TLV_TYPE_LENGTH=2` + `TLV_LENGTH_LENGTH=2`):**

```text
offset 0: u16 opt_type (big-endian)
offset 2: u16 opt_len  (big-endian) = số byte value
offset 4: value[opt_len]
```

- Mọi multi-byte integer đều big-endian: u16 `htons`, u32 `htonl`, u64 `htobe64`.
- u16 array: từng phần tử BE. String (`CLUSTER_NAME`): byte thô, **không** NUL terminator
  (`tlv_add_string` dùng `strlen`).
- `RING_ID` = u32 node_id BE + u64 seq BE = **12 B** (`memcpy` vào `tmp_buf[12]`).
- `TIE_BREAKER` = u8 mode + u32 node_id BE = **5 B**; node_id = 0 trừ khi mode = NODE_ID.
- `NODE_INFO` = TLV lồng nhau: `NODE_ID` bắt buộc (≠ 0, nếu không decode lỗi -4);
  `DATA_CENTER_ID` chỉ khi ≠ 0; `NODE_STATE` chỉ khi ≠ NOT_SET.
- `HEURISTICS_UNDEFINED` không bao giờ được encode (`tlv_add_heuristics` trả -1).
- `ECHO_REPLY` = copy nguyên byte request rồi ghi đè type (`msg_set_type`).
- Decoder **bỏ qua** TLV type không biết (switch không có `default` xử lý) → backward compat.

**Đọc message (`msgio_read`, non-blocking):**

1. Đọc đủ 6 B header trước.
2. Khi đủ header: kiểm tra type hợp lệ, kiểm tra `6 + len ≤ dynar_max_size`
   (max receive size đã đàm phán). Vi phạm → bật cờ `skipping_msg`, vẫn đọc tiếp
   tới hết frame rồi báo lỗi (không lệch stream).
3. Mã trả về: `1` đủ message / `0` đang dở / `-1` EOF / `-2` lỗi socket /
   `-3` không lưu nổi header / `-4` không lưu nổi body / `-5` type sai /
   `-6` message quá dài.

**Giải mã (`msg_decode`):** `0` ok / `-1` sai độ dài option / `-2` hết bộ nhớ /
`-3` TLV tràn khỏi message / `-4` nội dung option không hợp lệ
(enum ngoài miền, `node_id == 0`, ...).

**Hệ quả fail-closed cho QuorumESP:** `-1/-3/-4` khi decode, `-3..-6` khi đọc,
type ngoài 0–17, `len` vượt max → reject/kết thúc session, không dùng nội dung.

> Còn lại `CHƯA XÁC MINH` trên dây thật: thứ tự handshake bắt buộc, timeout/retry/
> DPD thực tế, mapping NSS→mbedTLS. Để Phase 1 (harness) và Phase 4 (interop).

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

## 7. Session lifecycle — ĐÃ XÁC MINH từ source (chưa bắt gói thật)

Nguồn (đọc code, chưa chạy runtime):

- Client connect + gửi: `qdevices/qdevice-net-socket.c`
  (`non_blocking_client_socket_write_cb`, `qdevice_net_send_preinit`,
  `qdevice_net_send_init`, `qdevice_net_socket_write_finished`)
- Client nhận: `qdevices/qdevice-net-msg-received.c`
  (`..._preinit_reply`, `..._check_tls_compatibility`, `..._init_reply`)
- Server nhận: `qdevices/qnetd-client-msg-received.c`
  (`qnetd_client_msg_received_{preinit,starttls,init}`, `..._check_tls`)
- TLS upgrade: `qdevices/nss-sock.c`
  (`nss_sock_start_ssl_as_server/client`, lazy handshake `force=0`)

### 7.1. Thứ tự bắt buộc trên dây

```text
TCP connect (client non-blocking; thành công ở non_blocking_client_socket_write_cb)
  → C: PREINIT(cluster_name)            luôn là message đầu tiên
  → S: PREINIT_REPLY(tls_supported, client_cert_required)
  → правил TLS (check_tls_compatibility):
       dùng TLS  khi (server,client) ∈ {(SUP,SUP),(SUP,REQ),(REQ,SUP),(REQ,REQ)}
       plaintext khi còn lại TRỪ 2 cặp incompatible:
         server UNSUPPORTED + client REQUIRED → client từ chối kết nối
         server REQUIRED + client UNSUPPORTED → client từ chối kết nối
       (tương đương ma trận tls on/off/required trong man page)
  → nếu TLS:
       C: STARTTLS (seq tăng)  →  S: KHÔNG trả lời (không tồn tại STARTTLS_REPLY)
       client flush xong STARTTLS → nss_sock_start_ssl_as_client (lazy handshake)
       server nhận STARTTLS → nss_sock_start_ssl_as_server (đồng bộ, silent)
       C: INIT ...               là message đầu tiên TRONG TLS
  → nếu plaintext: C: INIT ngay sau PREINIT_REPLY
  → S: INIT_REPLY(error_code, limits, algos)   LUÔN gửi, kể cả khi validation fail
       (client check error_code==NO_ERROR + sizes + algorithms mới đi tiếp;
        xong thì xóa connect_timer, bật echo-request timer)
  → steady state (hướng đã xác minh, trigger chi tiết vẫn UNVERIFIED):
       C→S ECHO_REQUEST (timer phía client) → S→C ECHO_REPLY (byte copy)
       C→S NODE_LIST(INITIAL_CONFIG/CHANGED_CONFIG/MEMBERSHIP/QUORUM)
         → S→C NODE_LIST_REPLY(seq, type, ring_id, vote)
       C→S ASK_FOR_VOTE → S→C ASK_FOR_VOTE_REPLY(seq, ring_id, vote)
       S→C VOTE_INFO → C→S VOTE_INFO_REPLY (client cập nhật cast-vote timer)
       C→S HEURISTICS_CHANGE → S→C HEURISTICS_CHANGE_REPLY
       hai chiều SET_OPTION ↔ SET_OPTION_REPLY
```

### 7.2. Chính sách lỗi phía server (đã căn harness theo)

- Sai thứ tự/thiếu field/enum lạ/decode fail → **trả error reply, GIỮ kết nối**,
  session không tiến (không vote). Mã đã thấy trong code:
  `PREINIT_REQUIRED`, `INIT_REQUIRED`, `TLS_REQUIRED`, `UNEXPECTED_MESSAGE`,
  `ERROR_DECODING_MSG`, `DOESNT_CONTAIN_REQUIRED_OPTION`,
  `INVALID_HEARTBEAT_INTERVAL`, `UNSUPPORTED_DECISION_ALGORITHM`,
  `TIE_BREAKER/ALGORITHM_DIFFERS_FROM_OTHER_NODES`, `DUPLICATE_NODE_ID`,
  `INTERNAL_ERROR`, `UNSUPPORTED_MESSAGE`.
- Ngắt cứng (`-1`, đóng socket) chỉ khi: verify client cert fail
  (`CERT_VerifyCertName` vs cluster_name), lỗi alloc, lỗi transport.
- Framing (type ngoài 0–17, vượt max size) bị loại trước khi vào session.
- Fail-closed của QuorumESP = không vote khi handshake chưa sạch, đúng như trên.

### 7.4. Quan sát interop vòng 1 (client thật, corosync 3.1.9, 2026-09-23)

- Client gửi `PREINIT` **có** seq (=1); `INIT` đầy đủ 18 msg + 24 opt,
  heartbeat 8000; `SET_OPTION` chỉ mang `kapTb=1` (không heartbeat).
- `NODE_LIST INITIAL_CONFIG` và `QUORUM` **không có `ring_id`**; chỉ
  `MEMBERSHIP` có. Server phải fallback ring đã biết (INIT → list mới nhất)
  cho field ring bắt buộc trong reply — harness đã implement theo.
- Harness từng trả error 7 cho 2 list thiếu ring → client ngắt
  (`Received server error 7. Disconnecting`). Sau fix, cần chạy lại để xem
  tiếp ECHO timer + vote flow phía client.
- Node state trong config list là `NOT_SET` (field vắng mặt), trong quorum
  list là `MEMBER`.
- Log thô: `host/frames.jsonl` vòng chạy tương ứng (không commit file log).

### 7.5. Quan sát interop vòng 1b (sau fix ring fallback, cùng client)

- `INITIAL_CONFIG` thiếu ring → reply type 11 với ring từ `INIT`. Client chấp
  nhận, đi tiếp `MEMBERSHIP` → `QUORUM(quorate=0)` → `QUORUM(quorate=1)`.
- Cluster 1 node chuyển inquorate→quorate ngay sau vote ACK của harness:
  **đường vote → cast → quorum hoạt động end-to-end** với client thật.
- `ECHO_REQUEST` xuất hiện sau ~8s = đúng `heartbeatInterval` đã đàm phán;
  echo dùng **bộ đếm seq riêng** (bắt đầu từ 1), không chung với message seq.
- Không còn `SERVER_ERROR` nào trong suốt phiên. Vòng 1 coi như pass ở mức
  framing/handshake/steady-state plaintext.

### 7.6. Còn UNVERIFIED (cần interop `docs/interop.md` + đọc tiếp)

- Reconnect backoff phía client (không nằm trong 3 file đã đọc, nghi ở
  `qdevice-net-instance.c`).
- Trigger gửi từng `NODE_LIST` type phía client (nghi ở
  `qdevice-net-votequorum.c` / algorithm files).
- Khi nào server đẩy `VOTE_INFO` vs chờ `ASK_FOR_VOTE`; đường vote vào votequorum.
- Hành vi PREINIT trùng lặp trên cùng kết nối.
- Giá trị số của timer (connect/echo/DPD/cast-vote) — chỉ mới thấy tên.

## 8. Việc còn lại cho Phase 1 (bắt buộc trước ESP32)

1. ~~Đọc `tlv.c`, `msg.c`, `msgio.c`... ghi lại header layout, endian, max size enforce.~~
   HOÀN THÀNH (xem §2.1). Còn lại: `nss-sock.c`, `qdevice-net-socket.c`,
   `qnetd-client-msg-received.c`, `qdevice-net-msg-received.c` — để Phase 1 khi cần
   chốt thứ tự handshake trên dây thật.
2. ~~Dựng harness host...~~ HOÀN THÀNH (`host/`, 58 test pass — xem `host/README.md`).
   Harness đã căn error policy theo §7.2 (error reply + giữ kết nối).
3. Bắt gói + log với `corosync-qdevice`/`corosync-qnetd` thật — ĐANG TIẾN HÀNH,
   xem `docs/interop.md` (vòng 1 plaintext + `host/tools/fake-qnetd.js`).
4. Xác minh mapping NSS→mbedTLS (cipher, cert CN `Qnetd Server`/`Cluster Cert`,
   password file, renew/expire flow).
5. Mọi phát hiện cập nhật vào file này theo mẫu `Nguồn / Behavior quan sát / Implementation`.
