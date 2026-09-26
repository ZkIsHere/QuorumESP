# QuorumESP — Failure recovery matrix (live evidence)

> Mỗi dòng: điều kiện → hành vi kỳ vọng → bằng chứng. "Live" = đo trên ESP32
> + client thật trừ khi ghi khác. Cập nhật khi thêm case.

## 1. Transport / session

| Case | Expected (fail-closed) | Evidence |
|---|---|---|
| TCP peer đóng đột ngột | invalidate session, recompute vote cho phần còn lại | live nhiều lần (`client session end`, recompute log) |
| TLS session rò socket (LWIP pool 10) | đóng session phải giải phóng socket, accept tiếp tục | **live 2026-09-25 (bug thật)**: sau ~9 session TLS, `accept failed errno=23 (ENFILE)` vĩnh viễn, `sessions=0 socks=0 heap` ổn định. Nguyên nhân: `esp_tls_server_session_delete` của IDF KHÔNG đóng fd (chỉ `conn_delete` mới đóng). Fix: `network_tls_close` tự `close(fd)` + fail path. Chứng minh: storm 50 PASS sau fix |
| Mất Wi-Fi (AP/client rớt) | DPD hb×1.5 đóng session; client reconnect khi mạng về; **device retry vô hạn sau boot** (`s_boot_done`) | live (`dead peer, closing` khi kill corosync); reconnect-vô-hạn: code + build OK, **live-deferred (board brownout nguồn 2026-09-25, xem docs/hardware.md §7)** |
| ESP32 reboot / mất nguồn | client reconnect từ PREINIT; NVS/config persistent | live (reboot EN + brownout, `config v1 id=` giữ nguyên) |
| ESP32 brownout (nguồn yếu) | reset, boot lại sạch (không half-state) | live (đầu project, fix bằng nguồn ngoài) |
| Oversize frame (>32K) | drain + `MESSAGE_TOO_LONG`, giữ kết nối | code + drain path; live 8K garbage → `ERROR_DECODING_MSG` + alive (oversize-probe) |
| Malformed TLV/header | reject + error reply, giữ kết nối; framing sai thì drop | host tests + live (`framing reject`, `decode failed`) |
| Cluster name lạ giữa chừng | đóng kết nối (single-cluster scope) | live (probe cluster `ab` bị refuse đúng) |
| Reconnect storm | session độc lập, refuse sạch khi hết slot, đầy thì refuse log rõ | **live 2026-09-25**: `storm.py` 50 handshake TLS liên tiếp + burst 20 concurrent + INIT/ECHO cuối → `STORM: PASS`, 0 lỗi (qdevice tắt trước để khỏi tranh slot). Budget hiện tại: ceiling 5 + heap floor (xem hàng dưới), chứng minh 4 ACTIVE (`four.py`) |
| Hết slot / hết heap | refuse sạch + log rõ (`table full` / `low heap (N)`), client retry | **live 2026-09-25**: session thứ 5 refuse ở heap ~20-30K; handshake TLS thứ 4 concurrent OOM dưới ~60K (fail-closed, các session kia bình thường). Không crash, không ENFILE, tự hồi |

## 2. TLS / auth

| Case | Expected | Evidence |
|---|---|---|
| TLS handshake fail (cert lạ) | đóng, không ACTIVE | live (`TLS handshake failed`, NSS alert -0x7280) |
| Client không gửi cert (mutual) | đóng sau handshake | live (`no client certificate presented`) |
| CN != cluster | đóng (verify callback) | live (`client CN mismatch` path; đúng CN pass) |
| Cert hết hạn | handshake fail | live probe cert hết hạn (2d) |
| Chain sai CA | handshake fail | **live 2026-09-25**: `rogue-cert-probe.py` với client cert do CA lạ ký (CN đúng `interop-test` để cô lập lỗi chain) → server mutual mode abort handshake `access_denied` (`ROGUE-CA PASS`). Control dương mutual mode không chạy được: dev PKI 2-ngày đã hết hạn sáng nay (CA + Cluster Cert `Not After Sep 25 07:45/08:07`) — bản thân việc hết hạn cũng là fail-closed đúng (qdevice kẹt `Connecting` ở mutual, vẫn `Connected` ở 2a) |
| Time sai (1970) | chain fail-closed (với TIME_DATE on) | code + log cảnh báo wall-clock; live SNTP fail → mutual từ chối đúng |

## 3. Config / storage

| Case | Expected | Evidence |
|---|---|---|
| NVS corrupt hoàn toàn | `nvs_flash_init` fail → erase → safe defaults (Kconfig) → boot + log rõ | **live 2026-09-25**: ghi rác 24K → boot `fresh` + Wi-Fi lên (xác minh bằng parse NVS sau đó) |
| Format mới hơn (downgrade) | từ chối, safe defaults, KHÔNG tin field lạ | unit test (`migrate refuses`); live chưa (cần bump version test) |
| Format cũ v0 (chỉ ssid/pass) | migrate giữ Wi-Fi, điền defaults | unit test; live-deferred (thiết bị duy nhất đã ở v1) |
| NVS trống hoàn toàn | fresh defaults + device id từ MAC | live (boot đầu sau erase) |
| Reboot giữ config + id | `config v1 id=` giống nhau mọi boot | live (`qesp-c04268` ổn định qua nhiều reboot) |

## 4. Quorum semantics

| Case | Expected | Evidence |
|---|---|---|
| Split-brain 2 partition | đúng 1 bên ACK (tie_break/score/kap) | live fake-node + FFSplit unit |
| Node chết → failover | bên còn lại flip NACK→ACK | live (kill node1) |
| Node quay lại (keep-active) | bên đang giữ KHÔNG mất vote | live (node1 về vẫn NACK) |
| Lone survivor | ACK | live + unit |
| Config lệch giữa client | WAIT_FOR_REPLY, không vote bừa | unit (`unstable`) |
| Algorithm khác nhau | `ALGORITHM_DIFFERS` ở INIT | **live 2026-09-25**: `mixed-algo-probe.py` — conn A ffsplit ok, conn B lms bị `INIT_REPLY error=16`, transport giữ, conn A ECHO vẫn đáp (`MIXED-ALGO: PASS`) |

## 5. OTA / update

| Case | Expected | Evidence |
|---|---|---|
| Update tốt | tải → reboot → confirm 60s | live (v0.2.x nhiều vòng) |
| Image crash trước confirm | rollback về bản cũ ở reboot tới | live (`bad-ota-test` → về bản tốt) |
| Server serve bản hỏng liên tục | quá 3 lần thì bỏ qua tới khi có version khác (NVS fail-memory) | **live 2026-09-25**: release `v0.0.0-ota-fail-test2` chứa image `abort()` → đúng 3 vòng download/reboot/crash/rollback rồi `failed 3 times before, skipping`, server lên ổn định (serial log). Download hỏng (404) cũng tính vào counter. Release test đã xóa, v0.2.4 lại là Latest |
| Mất mạng giữa download | giữ image cũ, thử lại reboot sau | code path; live chưa kéo dây mạng giữa chừng (TODO) |
| USB flash đè ota_0 | mất khả năng rollback (đúng semantics IDF) | live (bài học, đã ghi docs/ota.md) |

## 6. Watchdog

| Case | Expected | Evidence |
|---|---|---|
| Task kẹt (treo accept không feed) | WDT trigger | **live 2026-09-25**: patch treo tạm → `Task watchdog got triggered` lặp mỗi 10s đúng hẹn |
| Vận hành bình thường | im lặng, không reset giả | live 100s sau revert: boot sạch, ACTIVE, không trigger |
| Timeout phải reset (panic) | `CONFIG_ESP_TASK_WDT_PANIC=y` đã bật (trước đó chỉ warn) | config; vòng panic-reset full chưa quan sát trực tiếp (deferred — là code IDF chuẩn) |
| Task session chết | accept loop + client khác sống, chip không treo toàn cục | thiết kế (task riêng + WDT riêng); chưa kill đơn lẻ live (TODO cùng soak) |

## 7. Chưa làm (ghi nợ rõ ràng)

- Wi-Fi drop vật lý (tắt AP thật) — user không muốn đụng router chính; hotspot path bỏ dở.
- Soak test dài (ngày): khung đã có (`status:` mỗi 60s + qdevice Connected), chưa chạy.
- Chain sai CA live (cần PKI lạ — xuất client cert từ NSS DB hoặc CA mới).
