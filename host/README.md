# QuorumESP — Host-side protocol test harness (Phase 1)

> Chỉ dùng để phát triển và kiểm thử trên máy tính.
> KHÔNG phải implementation thứ hai: mọi byte format đều mirror từ
> reference `corosync/corosync-qdevice` (`tlv.c`, `msg.c`, `msgio.c`).
> Nguồn và mọi điểm chưa chắc chắn ghi trong `docs/protocol.md`.

## Chạy test

```sh
cd host
npm test        # node --test test/  (stdlib only, không cần npm install)
```

Yêu cầu: Node.js ≥ 20. Không có dependency ngoài.

## Cấu trúc

```text
host/
├── src/
│   ├── consts.js   # enum mirror tlv.h / msg.h + default qnet-config.h
│   ├── tlv.js      # TLV encode/decode (u16 type BE + u16 len BE + value)
│   ├── msg.js      # frame 6 B header + 18 builders + generic decoder
│   ├── session.js  # qnetd-side state machine CONNECTED→PREINIT→ACTIVE→CLOSED
│   └── net.js      # socket helpers + fake qnetd (fail-closed)
└── test/
    ├── tlv.test.js         # parser/serializer + malformed TLV
    ├── msg.test.js         # framing + builders + malformed frame
    ├── session.test.js     # state machine + TLS policy + heartbeat/DPD
    ├── connection.test.js  # TCP loopback, disconnect/reconnect, multi-client
    └── tls.test.js         # ma trận đàm phán + TLS handshake thật (cần openssl)
```

## Bao phủ theo AGENTS.md §16 / Phase 1

| Yêu cầu | Test |
|---|---|
| connection | `connection.test.js` handshake |
| protocol message | `msg.test.js` builders round-trip |
| malformed message | `tlv.test.js`, `msg.test.js`, `connection.test.js` (fail-closed) |
| TLS | `tls.test.js` (matrix + handshake với cert ephemeral) |
| mất connection | `connection.test.js` server observes disconnect |
| reconnect | `connection.test.js` reconnect sau violation |
| nhiều client | `connection.test.js` 3 client đồng thời |
| state machine | `session.test.js` transition + invalid rejection |
| timeout | `connection.test.js` read timeout → null, DPD boundary |

## Lưu ý an toàn

- `QnetSession.fixedVote` là **stub cho test**, không phải thuật toán quorum
  (FFSplit/LMS cần chứng minh interop ở Phase 4 mới được kết luận).
- Cert TLS trong test được tạo ephemeral bằng `openssl` lúc chạy, không commit.
- Mọi violation → đóng session, không vào ACTIVE (fail-closed).
