# QuorumESP — FFSplit vote (port từ reference)

> Nguồn duy nhất: `qdevices/qnetd-algo-ffsplit.c`
> (`corosync/corosync-qdevice`). Không suy diễn semantics.

## 1. Luật quyết định (đã port vào `firmware/quorum/ffsplit.c`)

`partition_cmp` (partition nào "tốt" hơn):

1. Config lẻ: membership > nửa config → thắng.
2. Config chẵn: membership > nửa → thắng; < nửa → thua.
3. Hòa 50:50: score = `active_clients + (heur_pass − heur_fail)`
   (số học unsigned y hệt reference, kể cả wrap khi fail > pass).
4. Hòa score: nhiều active client hơn thắng.
5. Hòa tiếp: keep-active-partition (nếu mọi client bật) — bên thuộc partition
   quorate trước thắng.
6. Cuối cùng: tie_breaker (`lowest`/`highest`/node-id) — partition chứa node
   ưu tiên thắng.

`select_partition`: so từng client với best hiện tại, giữ best.
`is_membership_stable`: mọi client đồng ý config set; trong cùng partition
phải đồng ý ring + membership set. Không stable → `WAIT_FOR_REPLY`.
`decide`: stable → chọn best → trong best ACK, ngoài NACK; không chọn được
ai → NACK tất cả.

## 2. Trạng thái port

- ✅ Decision core (`ffsplit.c`) + 9 scenario test trên host (`quorum/test/`).
- ✅ Đấu vào `server.c`: multi-client tasks, reply trạng thái, VOTE_INFO push
  NACK-trước-ACK-sau, track seq, task watchdog, session cap 2 (RAM).
- ✅ Live 2-client (node1 thật + node2 giả `host/tools/fake-node.py`,
  2026-09-23): tie→lowest thắng (node1 ACK/node2 NACK); kill node1→node2
  flip ACK; node1 quay lại khi node2 còn sống→vẫn NACK (keep-active dính);
  kill node2→node1 ACK lại. Khớp reference semantics.
- ⏳ LMS chưa port (chỉ FFSplit production lúc này).

## 3. Câu hỏi mở (cần interop 2 node thật)

- `ring_id.node_id` hai node cùng partition có giống hệt nhau không? Port hiện
  so sánh cả struct (đúng reference), nhưng mới chỉ quan sát 1 node. Cần 2
  node cùng partition để xác nhận giá trị thực tế trên dây.
