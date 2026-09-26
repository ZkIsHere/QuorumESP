# Web diagnostic UI (AGENTS.md §13)

Read-only diagnostics on the device. No quorum control, no config edit,
no firmware update — those live on the static portal
(`host/portal-static/`), never on-device.

## Enable + auth (dev only, default OFF)

`QUORUMESP_WEB_ENABLE=n` (default): `web_api_init()` is a no-op.
Production stays OFF (smaller attack surface).

Enable in menuconfig (`QuorumESP → Web UI`):

- `QUORUMESP_WEB_ENABLE=y`
- `QUORUMESP_WEB_USER` / `QUORUMESP_WEB_PASSWORD` (local sdkconfig,
  git-ignored — same secret handling as Wi-Fi)

No user/pass → the UI refuses to start (never an open page).
Every handler requires HTTP Basic (browser's own login dialog,
constant-time compare). Missing creds → `401` challenge; wrong creds →
`401` and counted. Logout is `GET /logout` → `401` (browser re-prompts;
true logout = close the browser). POST was never implemented → `405`.

## Rate limit

`QUORUMESP_WEB_LOGIN_MAX_FAIL` wrong passwords (default 5) from one IP →
the IP goes fully silent: connections drop with NO response until
`QUORUMESP_WEB_LOGIN_BLOCK_S` (default 60s) expires. Unknown peers share
one fallback bucket so limiting never silently disables itself.

Proven failure mode (2026-09-25): logging from inside an httpd handler
overflowed the default 4K worker stack → abort in newlib locks →
crash-reboot that looked like "silence". Fixed with 8K worker stacks
(`hcfg.stack_size`). Lesson: keep handler logging minimal.

## Display (deliberately minimal, one page)

- Project name + device id, version, uptime (`22s` … `2d 3h 4m 5s` … `y`)
- Connected-client table (node, algo)
- Recent log (never contains secrets — the project never logs secrets)
- Debug mode (`QUORUMESP_DEBUG`) adds heap + session count to the stream

Single interface: `GET /` renders the shell, `GET /events` pushes the
live values over Server-Sent Events (2s ticks, 60s stream, EventSource
reconnects). Snapshot via `qdevice_status_snapshot()` (locked copy,
200 ms timeout — never blocks the vote path).

## Known limits

- Log ring starts at web init (earlier boot lines are not kept).
- One httpd worker is held up to 60s per open stream; fine for a dev box.
- No HTTPS/rate-distributed auth for the UI — trusted lab LAN only,
  turn off when done. (qdevice TLS is a separate channel.)
- user/pass live plaintext in local sdkconfig — dev-acceptable.

## Live test (WROOM-32D)

- No/wrong auth → `401`; 5 wrong → silence (connection drops, no bytes).
- Correct auth → page renders, SSE ticks update uptime/clients/log.
- `/logout` → `401` re-prompt.
- After tests: flashed back to the web-OFF build, board to steady state.
