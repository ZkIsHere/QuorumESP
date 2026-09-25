# QuorumESP — LMS vote (port từ reference)

> Nguồn duy nhất: `qdevices/qnetd-algo-lms.c` + `qdevices/qnetd-algo-utils.c`
> (`corosync/corosync-qdevice`). Không suy diễn semantics.

## 1. Luật quyết định (đã port vào `firmware/quorum/lms.c`)

`do_lms` cho 1 client (ring/membership/heuristics của nó đã lưu):

1. Peer cùng partition mà ring khác → `WAIT_FOR_REPLY` (chờ converge).
2. Gom client theo ring thành partition (bỏ qua ring chưa init).
   Score mỗi partition = số client + heuristics (PASS +1, FAIL −1).
3. 0 partition → WAIT. 1 partition → ACK ("votequorum's problem").
4. Newcomer (chưa từng vote) đối diện partition khác đang ACK → NACK
   (KHÔNG lưu — lần sau vẫn là newcomer).
5. Score cao nhất duy nhất → trong đó thì ACK, không thì NACK.
6. Hòa score → partition đông nhất duy nhất thắng.
7. Hòa size → tie-breaker (lowest/highest/NODE_ID — NODE_ID: nominated ACK).

Entry points (khác FFSplit):

| Message | LMS behavior |
|---|---|
| config list | đếm node, luôn `NO_CHANGE` (không quyết định) |
| membership list | validate + lưu → decide → vote **trong reply** |
| quorum list | decide trên ring đã lưu → vote trong reply |
| ask_for_vote | **hỗ trợ** (FFSplit từ chối): decide → vote trong reply |
| heuristics_change | bỏ qua → `NO_CHANGE` (reference) |
| vote_info_reply | no-op |

Không có VOTE_INFO push sequencing như FFSplit. Timer của reference
(re-run waiter) được thay bằng recompute waiter sau mỗi cluster event
(tương đương effect, đã document).

Chia sẻ bảng cluster với FFSplit (`ffsplit.h`); state LMS riêng
(`last_result` mỗi slot, 0 = NEW).

## 2. Trạng thái port

- ✅ Decision core (`lms.c`) + 10 scenario test host (`quorum/test/`).
- ✅ Đấu multi-algorithm vào `server.c`: 1 algorithm/cluster (INIT đầu
  quyết; khác → `ALGORITHM_DIFFERS`), INIT_REPLY advertise cả hai.
- ⏳ Interop live với client `algorithm: lms` (fake-node `--algo lms`).
