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
