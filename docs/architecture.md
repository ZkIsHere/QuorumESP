# QuorumESP — Kiến trúc QDevice / QNetD (Phase 0)

> Trạng thái: **Experimental**. Tài liệu nghiên cứu, chưa phải spec để implement.
> Mọi behavior dưới đây đều có nguồn. Phần chưa chắc chắn được đánh dấu
> `CHƯA XÁC MINH` và mặc định fail-closed theo `AGENTS.md` §3, §8.

## 1. Phân biệt QDevice và QNetD (bắt buộc, `AGENTS.md` §21)

Không dùng lẫn hai khái niệm này trong code/tài liệu.

| Thành phần | Tên đầy đủ | Chạy ở đâu | Vai trò |
|---|---|---|---|
| `corosync-qdevice` | QDevice daemon | **Trên mỗi node** Proxmox/Corosync | Client. Gọi `votequorum API`, gửi heartbeat, nhận vote, không tự quyết quorum. Bản thân nó không làm gì nếu không có model/plugin. |
| `corosync-qdevice model net` | qdevice-net | Trên mỗi node, là 1 model của qdevice | Client TCP/TLS kết nối tới qnetd. Xử lý reconnect, voting, heuristics. |
| `corosync-qnetd` | QNet daemon | **Ngoài cluster**, 1 host độc lập | Server/trọng tài. Phục vụ nhiều cluster, gần như stateless, không có config file, cluster mới được handle động. Chỉ cho vote **một partition tại một thời điểm**. |
| `votequorum` | Corosync quorum provider + API | Trong corosync trên mỗi node | Nơi quyết định quorum cuối cùng. QDevice chỉ cung cấp vote. |
| `heuristics` | Script kiểm tra cục bộ | Trên mỗi node | Tie-breaker phụ. Chạy khi start/membership change/kết nối qnetd thành công (hoặc định kỳ ở mode `on`). Tất cả pass mới tính là pass. |

Nguồn:

- `corosync/corosync-qdevice` README: "qdevice is a daemon running on each node... qnetd is a daemon running outside of the cluster... support multiple clusters and be almost configuration and state free"
  <https://github.com/corosync/corosync-qdevice>
- Design wiki: "Qdevice = Daemon (not part of corosync process) running on every node... Qnetd = QDevice Network Daemon runs only on one node... able to provide arbiter function for multiple clusters"
  <https://github.com/corosync/corosync-qdevice/wiki/Design>
- Proxmox `pvecm` docs / Cluster Manager wiki: "Currently, only QDevice Net is supported... It will only give votes to one partition of a cluster at any time"
  <https://pve.proxmox.com/pve-docs/pvecm.1.html>
  <https://pve.proxmox.com/wiki/Cluster_Manager>

## 2. Vị trí của QuorumESP

```text
Proxmox Node A (qdevice-net client) ──┐
                                      ├──TCP/TLS──> QuorumESP (= qnetd-side)
Proxmox Node B (qdevice-net client) ──┘                    │
                                              quyết định vote theo algorithm
                                                          │
                                              Corosync/votequorum trên mỗi node
                                              quyết định quorum cuối cùng
```

- QuorumESP triển khai **phía network-side (qnetd-side)** để nói chuyện với
  `corosync-qdevice model net` thật.
- QuorumESP **KHÔNG** là cluster manager, không điều khiển/fence node,
  không thay Corosync quyết định healthy/quorate (`AGENTS.md` §1, §2, §22).
- Quy tắc vàng: `QuorumESP cung cấp một QDevice vote. Corosync quyết định quorum.`

## 3. Mô hình `net` — tham số vận hành (từ man page + source)

`corosync.conf` phía node (`quorum.device.net`):

| Key | Ý nghĩa | Default (nguồn) |
|---|---|---|
| `host` | IP/hostname qnetd server. Bắt buộc. | — |
| `port` | TCP port qnetd. | `5403` (`QNETD_DEFAULT_HOST_PORT`, `qnet-config.h`) |
| `tls` | `on` (thử TLS trước, fallback nếu server không advertise) / `off` (không TLS, không cần NSS DB) / `required` (bắt buộc TLS, lỗi nếu server không hỗ trợ). | `on` |
| `algorithm` | `ffsplit` / `lms` (còn `test`, `2nodelms` chỉ cho dev, không dùng production). | `ffsplit` |
| `tie_breaker` | `lowest` / `highest` / node-id cụ thể. Fallback khi partition bằng nhau tuyệt đối. | `lowest` |
| `connect_timeout` | Timeout kết nối tới qnetd. | `0.8 * quorum.sync_timeout` |
| `timeout` / `sync_timeout` | Chu kỳ `votequorum_poll`, đồng thời điều chỉnh heartbeat timeout của model net. | `10000` / `30000` ms |
| `votes` | Số vote qdevice cung cấp. `ffsplit` yêu cầu `1`; `lms` dùng default (`nodes-1`, tức xóa key để dùng default). | xem man `corosync-qdevice(8)` |

Nguồn:

- Man `corosync-qdevice(8)` (Debian): mô tả đầy đủ các key trên
  <https://manpages.debian.org/stretch/corosync-qdevice/corosync-qdevice.8.en.html>
- Man `corosync-qnetd(8)` (Ubuntu): `Connection... can be optionally configured with TLS client certificate checking. The communication protocol... is designed to be very simple and allow backwards compatibility`
  <https://manpages.ubuntu.com/manpages/resolute/man8/corosync-qnetd.8.html>
- `qdevices/qnet-config.h` (default hằng số):
  <https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/qnet-config.h>

## 4. Thuật toán vote (mức kiến trúc, chưa phải pseudocode để code)

- **FFSplit (fifty-fifty split, default):** cho cluster chẵn node. Cho đúng **1 vote**
  cho partition có nhiều active node nhất. Nếu bằng nhau thì so số client kết nối
  tới qnetd, rồi tới `tie_breaker`. Có thể chuyển vote nếu partition đang active bị
  chia tiếp nhưng partition khác còn ≥50% active node. Yêu cầu kết nối qnetd active,
  nếu mất thì không cấp vote.
- **LMS (last-man-standing):** node duy nhất còn thấy qnetd thì được vote. Cho phép
  cluster còn đúng 1 node vẫn quorate, nhưng sức nặng vote = `nodes-1` nên mất kết
  nối qnetd là mất `nodes-1` vote → chỉ cluster full node mới quorate được (overvote).
- `keep_active_partition_tie_breaker`: chỉ ảnh hưởng FFSplit (LMS hard-code giữ
  partition active cũ). Default qnetd `off`, qdevice-net `enabled` — xem `qnet-config.h`.

Nguồn:

- Man `corosync-qdevice(8)` §MODEL NET ALGORITHMS.
- SUSE SLE HA 15 SP7 ch.18 (mô tả FFSplit/LMS/heuristics/tie-breaker):
  <https://documentation.suse.com/sle-ha/15-SP7/html/SLE-HA-all/cha-ha-qdevice.html>
- RHEL 9/10 ch.28 (LMS = `nodes-1` vote, mất qnetd là mất từng đó vote):
  <https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/9/html/configuring_and_managing_high_availability_clusters/assembly_configuring-quorum-devices-configuring-and-managing-high-availability-clusters>

## 5. TLS và xác thực (mô hình gốc dùng NSS)

- Cả hai phía dùng **NSS DB** (`certutil`, `pk12util`), không phải PEM trực tiếp.
  - qnetd: `/etc/corosync/qnetd/nssdb`, nickname `QNetd Cert`, CA tự tạo qua
    `corosync-qnetd-certutil -i`, ký cert per-cluster (`-s -c <crq> -n <cluster>`).
  - qdevice: `/etc/corosync/qdevice/net/nssdb`, nickname `Cluster Cert`, CN = cluster name,
    CN server kỳ vọng `Qnetd Server`. Tool `corosync-qdevice-net-certutil.sh`
    (`-i/-r/-M/-m/-Q`).
- `qnetd -s on/off/req`, `-c on/off` (có yêu cầu client cert không, default on).
  `qdevice tls on/off/required` tương ứng.
- Proxmox yêu cầu traffic mã hóa (`pvecm qdevice setup` tự copy SSH key, tạo CA/cert).

Hệ quả cho ESP32 (`AGENTS.md` §9):

- Production **bắt buộc TLS** khi QDevice network model yêu cầu. Không tắt verify
  vĩnh viễn, không hard-code key, không commit credential/cert thật.
- Gốc dùng NSS; ESP32 dùng mbedTLS → **CHƯA XÁC MINH** mapping cipher/cert (NSS policy
  có thể reject cipher/key ngắn; có biến `NSS_IGNORE_SYSTEM_POLICY=1` phía gốc nhưng
  ESP32 không có). Phải có test interop Phase 4 mới kết luận. Trước đó fail-closed:
  TLS fail → không sang ACTIVE.

Nguồn:

- `qdevices/corosync-qdevice-net-certutil.sh`:
  <https://github.com/corosync/corosync-qdevice/blob/main/qdevices/corosync-qdevice-net-certutil.sh>
- Man `corosync-qnetd(8)` §TLS CONFIGURATION + `corosync-qnetd.c` CLI parse (`-s`, `-c`).
- Proxmox Cluster Manager: "The traffic between the daemon and the cluster must be encrypted".

## 6. Tích hợp Proxmox (luồng chuẩn, để biết boundary)

1. `apt install corosync-qnetd` trên host ngoài; `apt install corosync-qdevice` trên mọi node.
2. Mọi node online → trên 1 node chạy `pvecm qdevice setup <QDEVICE-IP>`.
3. Kiểm tra `pvecm status`: `Expected votes 3, Quorum 2, Flags Quorate Qdevice`;
   membership flag mỗi node `A,V,NMW` (Alive, Vote, NoMasterWins). `NA` = mất kết nối
   tới qnetd (kiểm tra TCP 5403). `NR` = chưa register.
4. Gỡ: `pvecm qdevice remove`.

QuorumESP thay bước 1 (host ngoài) nhưng phải tương thích bước 2–3.

Nguồn: Proxmox wiki `Cluster_Manager` + `pvecm.1` (mục QDevice Status Flags, port 5403).

## 7. Ràng buộc áp cho QuorumESP (`AGENTS.md` §7, §8, §11)

- State machine rõ ràng, transition không hợp lệ phải reject. Tên state thực tế tuân
  protocol (chi tiết message ở `docs/protocol.md`).
- Fail-closed: mất TCP → invalidate session; TLS fail → không ACTIVE; protocol
  violation/message invalid → kết thúc/reject session; timeout → không giả định peer
  còn sống; watchdog timeout → restart; config lỗi → safe mode.
- Không giữ quorum state cũ vô thời hạn sau mất kết nối; không báo hợp lệ khi chưa
  xác minh; không vô hiệu safety check để test pass.
- Ethernet ưu tiên hơn Wi-Fi; core không phụ thuộc cứng 1 controller.
- Web/UI chỉ diagnostic sau khi core ổn định; không cho đổi quorum state tùy ý.

## 8. Điều chưa biết (không đoán, để Phase 1/4 chứng minh)

1. Byte layout chính xác của msg header + TLV encoding (endian, padding) — mới đọc
   `tlv.h`/`msg.h`, chưa đọc `tlv.c`/`msg.c`/`msgio.c` từng dòng. `CHƯA XÁC MINH`.
2. Thứ tự handshake bắt buộc và timeout/ retry/ DPD chính xác trên dây — cần bắt gói
   với `corosync-qdevice` thật.
3. Mapping NSS cipher/cert sang mbedTLS trên ESP32.
4. Behavior `2nodelms`/`test` algorithm — không dùng, không implement trừ khi cần.
5. `go-qnetd` (Go reimpl, claim 100% protocol compat, 18 msg types, 4 algorithms) chỉ
   là tài liệu độc lập tham khảo, **không phải spec**:
   <https://github.com/benjaminbear/go-qnetd>

## 9. Kết luận Phase 0

- Đã xác định boundary: QuorumESP = **qnetd-side**, nói TCP/TLS + TLV protocol với
  `qdevice-net` trên node, phục vụ Proxmox qua `pvecm` flow chuẩn.
- Đủ cơ sở sang **Phase 1: host-side test harness** (connection/message/malformed/
  TLS/disconnect/reconnect/multi-client/state machine) trước khi đụng ESP32.
- Mọi quyết định protocol tiếp theo phải cập nhật `docs/protocol.md` theo mẫu
  `Nguồn / Behavior quan sát / Implementation`.
