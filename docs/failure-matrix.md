# QuorumESP — Failure recovery matrix (live evidence)

> Mỗi dòng: điều kiện → hành vi kỳ vọng → bằng chứng. "Live" = đo trên ESP32
> + client thật trừ khi ghi khác. Cập nhật khi thêm case.

## 1. Transport / session

| Case | Expected (fail-closed) | Evidence |
|---|---|---|
| TCP peer đóng đột ngột | invalidate session, recompute vote cho phần còn lại | live nhiều lần (`client session end`, recompute log) |
| Mất Wi-Fi (AP/client rớt) | DPD hb×1.5 đóng session; client reconnect khi mạng về | live (`dead peer, closing` khi kill corosync) |
| ESP32 reboot / mất nguồn | client reconnect từ PREINIT; NVS/config persistent | live (reboot EN + brownout, `config v1 id=` giữ nguyên) |
| ESP32 brownout (nguồn yếu) | reset, boot lại sạch (không half-state) | live (đầu project, fix bằng nguồn ngoài) |
| Oversize frame (>32K) | drain + `MESSAGE_TOO_LONG`, giữ kết nối | code + drain path; live 8K garbage → `ERROR_DECODING_MSG` + alive (oversize-probe) |
| Malformed TLV/header | reject + error reply, giữ kết nối; framing sai thì drop | host tests + live (`framing reject`, `decode failed`) |
| Cluster name lạ giữa chừng | đóng kết nối (single-cluster scope) | live (probe cluster `ab` bị refuse đúng) |
| Reconnect storm | mỗi session độc lập, table tối đa 2 (+8 vote slots), đầy thì refuse log rõ | code; chưa đo tải (TODO soak) |

## 2. TLS / auth

| Case | Expected | Evidence |
|---|---|---|
| TLS handshake fail (cert lạ) | đóng, không ACTIVE | live (`TLS handshake failed`, NSS alert -0x7280) |
| Client không gửi cert (mutual) | đóng sau handshake | live (`no client certificate presented`) |
| CN != cluster | đóng (verify callback) | live (`client CN mismatch` path; đúng CN pass) |
| Cert hết hạn | handshake fail | live probe cert hết hạn (2d) |
| Chain sai CA | handshake fail | unit (flags path); live chưa có CA lạ (TODO khi có PKI thật) |
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
| Algorithm khác nhau | `ALGORITHM_DIFFERS` ở INIT | code; live chưa có 2 algo cùng lúc (cần 2 client khác algo — TODO) |

## 5. OTA / update

| Case | Expected | Evidence |
|---|---|---|
| Update tốt | tải → reboot → confirm 60s | live (v0.2.x nhiều vòng) |
| Image crash trước confirm | rollback về bản cũ ở reboot tới | live (`bad-ota-test` → về bản tốt) |
| Server serve bản hỏng liên tục | lặp update/crash/rollback (chưa backoff!) | live quan sát; **TODO: retry counter + backoff** |
| Mất mạng giữa download | giữ image cũ, thử lại reboot sau | code path; live chưa kéo dây mạng giữa chừng (TODO) |
| USB flash đè ota_0 | mất khả năng rollback (đúng semantics IDF) | live (bài học, đã ghi docs/ota.md) |

## 6. Watchdog

| Case | Expected | Evidence |
|---|---|---|
| Task WDT armed, feed mỗi vòng | reset khi task kẹt >10s | code (component + wire); **live proof CHƯA** (cần patch treo thử) |
| Task session chết | accept loop + client khác sống, chip không treo toàn cục | không đo trực tiếp (TODO cùng WDT proof) |

## 7. Chưa làm (ghi nợ rõ ràng)

- WDT timeout live proof (patch treo tạm thời → quan sát reset → revert).
- Mixed-algorithm live (2 client khác algo cùng lúc).
- Wi-Fi drop vật lý (tắt AP thật) + reconnect storm đếm được.
- OTA retry/backoff + secure boot/ký image.
- Soak test nhiều ngày (RAM/session leak).
