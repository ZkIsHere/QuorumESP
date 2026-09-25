# QuorumESP — TLS vòng 2 (thiết kế)

> Mục tiêu: mã hóa + mutual auth đúng chuẩn reference, thay plaintext vòng 1.
> Trạng thái: **thiết kế, chưa implement**.

## 1. Behavior reference đã xác minh (không đoán)

- `STARTTLS` không có reply. Server upgrade socket **đồng bộ** ngay khi nhận
  (`nss_sock_start_ssl_as_server`); client upgrade sau khi flush STARTTLS
  (`nss_sock_start_ssl_as_client`). `INIT` là message đầu tiên trong TLS.
  Cả hai dùng lazy handshake (`force=0`).
- Server verify client cert: `SSL_PeerCertificate` + `CERT_VerifyCertName`
  so với **cluster_name** từ `PREINIT`. Fail → ngắt cứng.
- Cipher cụ thể NSS↔mbedTLS: **CHƯA XÁC MINH** — thử `TLS 1.2 +
  ECDHE-RSA-AES128-GCM-SHA256` trước (2 đầu đều hỗ trợ rộng rãi).

## 2. Thiết kế phía ESP32 (mbedTLS trực tiếp, không qua esp-tls wrapper)

Lý do: cần `SSL_VERIFY_REQUIRED` + custom CN check theo cluster_name động
(từng client một cluster khác nhau) — wrapper không đủ control.

- `network/tls.c`: init `mbedtls_ssl_config` server, `authmode=VERIFY_REQUIRED`,
  `ca_chain` = dev CA (verify client), own cert/key = server cert.
- Sau `STARTTLS`: `mbedtls_ssl_setup` + `ssl_set_bio(net_ctx_of_accepted_fd)` +
  handshake loop (WANT_READ/WRITE). Fail → đóng transport, session không tiến.
- Verify callback: parse peer CN, so sánh với `cluster_name` đã nhận ở PREINIT.
  Khác → handshake fail (fail-closed, đúng reference).
- Đọc/ghi sau upgrade đi qua `mbedtls_ssl_read/write` với timeout; framing
  (`qesp_msg_check_header`) giữ nguyên — TLS chỉ thay lớp transport.

## 3. Cert dev (ephemeral, KHÔNG commit — `AGENTS.md` §9)

Hai đường đưa cert vào firmware (Kconfig thắng file nhúng):

**A. Menuconfig** (`QuorumESP → TLS certificates`, local sdkconfig,
git-ignored): 3 ô — server cert, server key, client CA — mỗi ô là
**base64 của DER, 1 dòng** (Kconfig string không mang được newline của
PEM; thử `\n` đã chứng minh gãy — kconfig diễn giải escape rồi header
sinh ra chuỗi rỗng):

```sh
openssl x509 -in server.crt -outform der | base64 -w0
openssl rsa -in server.key -outform der | base64 -w0
```

Firmware base64-decode lúc boot (lọc ký tự lạ, fail-closed nếu rác).
Live-test 2026-09-25: giấu `certs_dev/`, nạp 3 blob qua sdkconfig →
TLS handshake + session OK; mutual mode đá rogue cert như thường.

**B. File nhúng** `firmware/certs_dev/` (git-ignored): `dev-ca.crt`,
`dev-server.crt`, `dev-server.key`. CMake bake thành literal lúc
configure (có `CMAKE_CONFIGURE_DEPENDS` — đổi PEM là tự rebuild, bài học
xương máu: từng flash cert cũ vì cache không nhận file đổi).

Thiếu cả hai → TLS unavailable, server plaintext-only (fail-closed).

**Client cert từng node** (mutual mode cần): portal ngoài
(`host/portal/` → Mint client cert, CN = tên cluster) ký bằng CA local
(`host/pki/ca.key`, git-ignored, KHÔNG BAO GIỜ qua HTTP). Hoặc script
`host/pki/mint-client.sh <CN>` + enroll NSS tay. CA key cũ mất nên
2026-09-25 đã renew toàn bộ PKI dev (hạn 90 ngày).

## 4. Enroll client cho qdevice WSL (NSS DB, theo flow reference)

```sh
export NSS_DB=/etc/corosync/qdevice/net/nssdb   # kiểm tra path thực tế
mkdir -p $NSS_DB
certutil -N -d $NSS_DB --empty-password
# Import CA dev, trust để verify server... (vòng 2a: client tls=on,
# server không yêu cầu verify lẫn? Không — reference default yêu cầu client cert)
```

Chi tiết enroll (CSR bằng `certutil -R -s "CN=interop-test"`, ký bằng
openssl CA, import ngược) sẽ chốt khi implement — flow này là phần rủi ro
nhất vòng 2, cần thử thật mới biết `certutil` chịu PEM ký ngoài hay không.
Fallback nếu NSS khó: vòng 2a cho server **không yêu cầu** client cert
(`VERIFY_OPTIONAL`/tham số dev), verify CN ở vòng 2b.

## 5. Ma trận test vòng 2

| # | Cấu hình | Kỳ vọng |
|---|---|---|
| 2a | client `tls: on`, server có cert, không yêu cầu client cert | STARTTLS → handshake → INIT trong TLS → ACTIVE |
| 2b | + server yêu cầu client cert (CN=cluster) | như 2a + sai CN thì ngắt — **PASS 2026-09-23** |
| 2c | client `tls: required` + server plaintext | client từ chối (đã cover ở harness) |
| 2d | cert hết hạn (days=0..1) | handshake fail, không ACTIVE — **PASS 2026-09-23** (probe openssl, CN đúng nhưng hết hạn → sập ở handshake) |

## 7. Kết quả vòng 2a (client thật, 2026-09-23) — PASS

- ESP32 (mbedTLS qua public esp-tls API) + `corosync-qdevice` 3.1.9 (NSS,
  `tls: on`): `PREINIT → STARTTLS → TLS handshake done → INIT trong TLS →
  ACTIVE node=1 algo=1 hb=8000`. Cipher đàm phán: `TLS 1.2
  ECDHE-RSA-AES256-GCM-SHA384` (đo bằng probe Python độc lập).
- Client NSS verify chain dev-CA + CN=`Qnetd Server`. Cert cũ (`CN=Qnetd Dev`)
  bị từ chối đúng như thiết kế — bắt được nhờ probe, không phải đoán.
- Tool: `host/tools/tls-probe.py` (PREINIT→STARTTLS→đọc cert/cipher, không
  verify — chỉ debug).
- Lưu ý build: cert embed lúc **configure** (`certs_embed.c` generate bởi
  CMake) — copy cert mới xong phải `reconfigure + build + flash`, flash bản
  cũ là serve cert cũ.

## 8. Kết quả vòng 2b + 2d (client thật, 2026-09-23) — PASS

- Mutual TLS end-to-end với `corosync-qdevice` 3.1.9 (NSS, Cluster Cert
  CN=`interop-test` enroll theo flow reference): `STARTTLS → handshake →
  client CN verified (interop-test) → ACTIVE node=1 algo=1 hb=8000`,
  message mã hóa 2 chiều, đóng clean bằng close_notify.
- Cert hết hạn (CN đúng, hết hạn): handshake bị ngắt, không session.
- Bẫy đã gặp và fix (ghi để không tái diễn):
  1. esp-tls **không wire `ca_chain` phía server** → CertificateRequest mang
     CA list rỗng → client không chọn cert gửi. Mutual phải dùng raw mbedTLS
     với `OPTIONAL + ca_chain`, enforcement bằng verify callback + CN check.
  2. IDF tắt `MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` (default n) → post-handshake
     `get_peer_cert()` luôn NULL. Verify phải làm **trong handshake**.
  3. IDF tắt `MBEDTLS_HAVE_TIME_DATE` (default n) → expiry bị bỏ qua hoàn
     toàn. Đã bật trong `sdkconfig.defaults`; cần SNTP đúng giờ
     (`network/time_sync.c`), sai giờ fail-closed là đúng.
   4. mbedTLS server authmode default là NONE (chỉ client default REQUIRED).
   5. `esp_tls_server_session_delete()` KHÔNG đóng socket (chỉ `conn_delete`
      mới đóng qua `mbedtls_net_free`) — dù docstring ghi "close ... and
      free". Mỗi session TLS server rò 1 LWIP socket: pool 10 cạn sau ~9
      session → `accept errno=23 ENFILE` vĩnh viễn (bắt live 2026-09-25).
      Fix phía mình: `network_tls_close()` tự `close(fd)` sau delete
      (xem `firmware/network/tls.c`). Bài học: không tin docstring IDF
      về ownership, đọc implementation.

## 9. Không làm ở vòng 2 (đánh số lại từ §6 cũ)

- Rotation/renew tự động, SNI, session resumption, TLS 1.3-only.
- Web/provisioning cert (để sau cùng với management UI).
