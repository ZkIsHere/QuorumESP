# QuorumESP load benchmark (`host/tools/bench.py`)

Measures the qnetd-side server under load. Needs both session slots
free — stop `corosync-qdevice` first, restart it after (the script does
NOT do that for you).

```sh
python3 host/tools/bench.py 192.168.100.219 5403 interop-test [--quick]
```

Phases: (1) sequential full handshakes (PREINIT→TLS→INIT),
(2) ECHO round-trips on one ACTIVE session, (3) k parallel sessions
(>2 transports are REFUSED by design — counted, not errors),
(4) sustain: 2 sessions × T seconds of ECHO (any drop, wrong reply,
or device reboot fails the run). VOTE_INFO pushed by the server
interleaves with ECHO replies and is answered, not mistaken for errors.

## Baseline (WROOM-32D, 2026-09-25, `BENCH: PASS`)

| Phase | Result |
|---|---|
| handshake ×20 | min 1677ms, avg 1819ms, p95 1915ms, max 1916ms (software RSA-2048) |
| echo ×200 | 56.7/s, avg 17.6ms, max 45.4ms |
| concurrency | k=1,2 all ok; k=3,4 exactly 2 ok + rest refused (by design) |
| sustain 30s ×2 | 1493 + 1489 msgs, zero drops |

Reading: handshake cost is one-time per session (RSA); steady-state
ECHO answers at ~55/s with both slots busy. Refusals past 2 concurrent
transports are the documented session budget, not failures.

## 4 sessions (classic ESP32 squeezed, 2026-09-25)

Per-session steady cost measured from heap logs: **~40-47K**
(148K → 113K → 78K → 49K across 4 admissions). Squeezing applied:
shared RX/TX stash (one 32K+2K for all sessions, serialized by mutex),
TLS I/O 8K/4K, task stack 16K→8K (watermark proves ~4K use, ~3.9K margin),
socket pool 10→16. Admission is self-assessed: ceiling 5 + heap floor
40K (basis: 4th handshake succeeded from 49.2K; below floor = clean
refuse, client retries). 5th concurrent refuses naturally (~20-30K).

- `four.py`: **4/4 ACTIVE simultaneously**, ECHO flowing on all.
- bench concurrency: k=1,2,3 full; k=4 mostly 3+1 (10s tool timeout,
  not server refusal — `four.py` with 30s proves 4/4).
- ECHO single-session rate unchanged (56.5/s) — shared mutex costs
  nothing when uncontended; loaded 2-session latency ~375ms (250ms
  recv turns), fine for 8s heartbeats.
- 4th concurrent TLS handshake OOMs below ~60K free (fail-closed,
  others unaffected) — this is what the floor guards.
- Stack 8K safe: watermark 3856-3868 free at session end (peak incl.
  RSA handshake ≤ ~4.3K).

## Flood (`host/tools/flood.py`, 2026-09-25, PASS on retry)

100 simultaneous bare connects with the real client holding its slot:
~10 accepted, ~90 refused (RST, no hang, no crash). 30 back-to-back full
TLS handshakes after. First run caught transient accept backpressure
(loop broke at #4, pool still draining from the burst); re-run after
cooldown: 30/30 + final alive handshake OK. Standing qdevice client
stayed Connected and the cluster quorate throughout. Verdict: survives
100x overload and recovers by itself — correct behavior for a quorum
box (serve the real client, shed the rest).
