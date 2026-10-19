# Changelog (r3c0nc1l3r/Sunshine fork)

Fork changes on top of the upstream base. Upstream's own changelog is at
https://github.com/LizardByte/Sunshine/releases.

## Unreleased

### Fixed
- Portal capture: a portal helper that never opened a PipeWire remote no longer closes fd 0 when it is destroyed (for
  example at startup when the portal is unavailable). Its fd and D-Bus pointers now start unset (#39).
- The default gamepad setting is copied from one gamepad list call; it was read from a list buffer freed during static
  initialization (#42).

## v2026.914.233613-lb.2
Upstream base: `63d35f702ee9` (nightly `2026.914.233613`). Arch package `sunshine 2026.914.233613.2-1` (CUDA off).

### Changed
- Session close API: a close that finds the session already stopping (or loses the stop race) is answered 200 and
  audited as `result=already-stopping` instead of `closed` (#17).
- Web UI API: the CSRF token is accepted only from the `X-CSRF-Token` header (#19).
- Sessions are joined outside the RTSP slots lock, so the session list and close API never wait behind a session
  teardown (#13). While a teardown is being joined, the session count used by `/launch` and `/resume` still includes
  the session being ended, instead of waiting for the join.

### Fixed
- `authenticate()` no longer throws on a short or malformed `Authorization` header, and compares the password hash in
  constant time (#14).

### Added
- Failed-login limit on the web UI: 10 failures from one address within 5 minutes lock that address out (429) until
  the window ends, with a `Retry-After` header. The address table is bounded; when it is full, expired and then
  unlocked entries are evicted before any locked-out address (#19).
- Tests for the session close API and the login limit, with a test-only seam for fake sessions (#18).

## v2026.914.233613-lb.1
Upstream base: `63d35f702ee9` (nightly `2026.914.233613`). Arch package `sunshine 2026.914.233613.1-1` (CUDA off).

### Added
- `AGENTS.md` fork section: repository facts, branch and tag scheme, checks, rules.
- Web UI API `GET /api/sessions` and `POST /api/sessions/<id>/close` (admin auth, CSRF, audit line at Info) to list
  streaming sessions and close one (#11).
- Info log line `Session ended: reason=<reason>, active sessions=<N>` (#7).
- Info log line `[wayland] capture buffers: ...` about every 60 s while capturing, with live buffer counters (#4).
- `scripts/lanternbox/build_arch_pkg.sh`: builds the Arch package in a disposable container (#9).
- Unit tests for the changes and the e2e leak-cycle script `tests/e2e/leak_cycles.sh` with a moonlight-web streamer
  driver (#8).

### Fixed
- wlr capture no longer leaks GBM buffers and `wl_buffer`s when a screencopy request times out; live buffers are capped
  and the capture reinitializes past the cap (#4).
- The render-node fd is opened with `O_CLOEXEC` and closed with the capture display, so DRM memory no longer grows by
  about 2 MiB per session (#5).
- No busy loop while the capture display is gone; encode retries back off from 100 ms to 2 s; the reinit wait ends
  when capture stops (#6).

### Changed
- Sessions whose video and audio pings stop for `ping_timeout` end (`media-ping-timeout`), and ENet peers that stop
  acknowledging end after `ping_timeout`. Defence in depth only: a client that wedges with its channels alive is ended
  by its own stop watchdog or through the session close API (#3).
