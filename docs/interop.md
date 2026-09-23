# QuorumESP — Interop với `corosync-qdevice` thật (vòng 1: plaintext)

> Mục tiêu `AGENTS.md` §25 bước 4: chứng minh harness nói được với client thật
> trước khi code ESP32. Vòng 1 chạy **plaintext (`tls: off`)** để xác minh
> framing/handshake; TLS là vòng 2 (xem §TLS cuối file).
> Trạng thái: **Experimental**. Mọi quan sát phải ghi lại vào `docs/protocol.md`.

## 1. Mô hình vòng 1

```text
Linux VM (root)
├── corosync (1 node, votequorum)
├── corosync-qdevice model net ──TCP 5403, tls off──> fake-qnetd (harness)
└── quan sát: frames.jsonl + corosync/qdevice log
```

Máy chạy `fake-qnetd` phải có IP mà VM với tới được (`--host 0.0.0.0`).

## 2. Chuẩn bị phía Linux (Debian/Ubuntu/Proxmox node test)

```sh
sudo apt install corosync corosync-qdevice
corosync -v; corosync-qdevice -v   # ghi lại version vào kết quả
```

Tạo `/etc/corosync/corosync.conf` **tối thiểu** (single node, chỉ cho interop):

```text
totem {
  version: 2
  cluster_name: interop-test
  transport: udpu
}
nodelist {
  node {
    ring0_addr: 127.0.0.1
    nodeid: 1
  }
}
quorum {
  provider: corosync_votequorum
  device {
    model: net
    votes: 1
    net {
      host: <IP-MAY-FAKE-QNETD>
      port: 5403
      tls: off
      algorithm: ffsplit
      tie_breaker: lowest
    }
  }
}
logging {
  to_syslog: yes
}
```

> `tls: off` là mode duy nhất không cần NSS DB (man `corosync-qdevice(8)`),
> phù hợp vòng 1. Không dùng config này cho production.

## 3. Chạy fake-qnetd (máy host, repo này)

```sh
cd host
node tools/fake-qnetd.js --port 5403 --vote ack --log frames.jsonl
# LISTEN port=5403 ...
```

Mở port TCP 5403 trên firewall cho VM.

## 4. Chạy corosync + qdevice (VM, root)

```sh
sudo corosync
sudo corosync-qdevice
# log mặc định ra syslog; theo dõi:
sudo journalctl -u corosync -f &
sudo journalctl -f | grep -i qdevice &
```

Kiểm tra quorum:

```sh
sudo corosync-quorumtool -s
sudo corosync-qdevice-tool -s || true   # nếu tool tồn tại
```

## 5. Tiêu chí pass vòng 1 (ghi lại tất cả)

1. `frames.jsonl` có `PREINIT` (cluster_name `interop-test`) → `PREINIT_REPLY`.
2. Có `INIT` (algorithm ffsplit, heartbeat, tie_breaker, ring_id) →
   `INIT_REPLY` với `error_code NO_ERROR`.
3. Có `ECHO_REQUEST` định kỳ từ client → `ECHO_REPLY` (xem heartbeat bao nhiêu ms).
4. Có `NODE_LIST` (ghi lại `list_type`, số node, quorate, heuristics).
5. `corosync-quorumtool -s` cho thấy qdevice state (Alive? votes?).
6. Không có `SERVER_ERROR` nào từ fake-qnetd (nếu có → copy dòng log lại).

Gửi về: `frames.jsonl` (vài chục dòng đầu là đủ), output `quorumtool -s`,
version `corosync`/`corosync-qdevice`, distro + version. Tôi sẽ đối chiếu với
`session.js` và cập nhật `docs/protocol.md`.

## 6. Ma trận vòng 1 (làm dần, mỗi dòng là 1 lần chạy có log riêng)

| # | Thay đổi | Kỳ vọng |
|---|---|---|
| 1 | baseline trên | handshake + echo + node list |
| 2 | `kill` fake-qnetd 30s rồi chạy lại | client reconnect, handshake lại từ PREINIT |
| 3 | `--vote nack` | reply vote đổi, quan sát `quorumtool` |
| 4 | stop corosync (`corosync-cfgtool -s`?) rồi start lại | session mới, không dính state cũ |

## 7. TLS (vòng 2, chưa làm)

- Client `tls: on/required` cần NSS DB (`corosync-qdevice-net-certutil`,
  CN server `Qnetd Server`, client cert `Cluster Cert`).
- fake-qnetd hiện chưa upgrade socket sau `STARTTLS` (session mới chỉ đánh dấu
  intent). Cần: `tls.TLSServer` + `--cert/--key` PEM phía harness, và enroll
  client cert vào NSS DB phía node. Ghi issue riêng khi bắt đầu vòng 2.
- Cert test **không commit** (`AGENTS.md` §9); tạo ephemeral mỗi lần chạy.

## 8. An toàn

- Chỉ chạy trên VM/node test. Không trỏ production cluster vào fake-qnetd:
  vote của nó là stub (`--vote`), không phải quyết định quorum thật.
- Sau mỗi lần chạy, `corosync-quorumtool -s` phải cho thấy cluster về trạng thái
  cũ khi gỡ qdevice (`quorum.device` xóa khỏi config + restart corosync).
