# Drain handoff (spec, Linux, opt-in)

Status: **draft spec, not implemented.** This page is the contract for the implementation, the tests and the review.

## Goal
Upgrade or restart Sunshine while clients are streaming, without ending their streams:

- new sessions go to a new Sunshine process (for example a newly installed binary);
- sessions that are already streaming finish on the old process, which then exits;
- clients notice nothing, except that a launch in flight during the handoff can fail once and is retried.

The feature is off by default and changes nothing when it is off. It targets Linux with systemd (user or system unit).

## Why this works with existing clients
- The client learns the RTSP URL (and so the RTSP port) from `sessionUrl0` in the `/launch` and `/resume` replies.
- For video, audio and control, the client takes the UDP port from each stream's RTSP `SETUP` reply
  (`Transport: server_port=<n>`). The well-known ports (47998, 48000, 47999) are only fallbacks for a missing header.
  This holds for moonlight-common-c, which Moonlight's clients use. Sunshine always sends the header.
- So a process that owns the RTSP listener can put its sessions' UDP media on any ports. Two processes can stream at once
  if they use different UDP bases. The TCP ports (HTTPS, HTTP, web UI, RTSP) stay the same, because the new process
  inherits the listening sockets.

## Configuration
| Option | Default | Meaning |
|--------|---------|---------|
| `drain_handoff` | `disabled` | `enabled` turns the feature on. |
| `drain_handoff_udp_offset` | `100` | The second UDP base is `port + offset`. Processes alternate between the two bases. |
| `drain_handoff_deadline` | `3600` | Seconds an old process may drain (1 to 86400). At the deadline it ends the remaining sessions (reason `drain-deadline`) and exits. |
| `drain_handoff_ready_timeout` | `30` | Seconds the new process has to report ready, or the handoff is rolled back. |
| `drain_handoff_rollback_window` | `60` | Seconds after the switch during which the old process keeps the listening sockets and takes them back if the new process dies. |
| `drain_handoff_binary` | path Sunshine was started from | Binary to execute for the new process. |

Config validation rejects an offset that would make any UDP port of either base overlap a TCP port, overlap the
other base, or leave 1024-65535.

## Sequence
Trigger: `SIGUSR2` to the running process (the unit's `ExecReload=kill -USR2 $MAINPID`).

1. **Old process: checks.** It refuses the handoff (logs a Warning, keeps running unchanged) when:
   - the feature is disabled;
   - a handoff is already in progress, or this process is itself draining;
   - an application with a running process (non-empty `cmd`, not detached) is active, because the new process cannot
     adopt it. Apps without a process (desktop-style) are fine.
2. **Old process: spawn.** It creates a Unix socketpair (the handoff channel) and `fork`+`exec`s `drain_handoff_binary`
   with the same arguments. The new process receives:
   - the four listening TCP sockets, in the systemd `LISTEN_FDS` / `LISTEN_FDNAMES` / `LISTEN_PID` convention, with
     the names `https`, `http`, `webui`, `rtsp`;
   - `SUNSHINE_HANDOFF_FD` (its end of the channel), `SUNSHINE_HANDOFF_UDP_BASE` (the base that the old process does
     not use), `SUNSHINE_HANDOFF_NEXT_SESSION_ID` (so launch session ids stay unique across both processes), and
     `SUNSHINE_HANDOFF_GENERATION` (a counter used to keep virtual device ids unique).
3. **New process: start in handoff mode.** It adopts the inherited sockets instead of binding them, binds its UDP media
   ports at the given base, and starts its servers, but does **not accept** connections yet. In handoff mode it skips
   the startup actions that would disturb the old process's streams (see "Shared resources"), including the startup
   encoder probe. It then sends `READY` on the channel.
4. **Old process: switch.** On `READY` within `drain_handoff_ready_timeout`, the old process stops accepting on all
   four sockets at once (no more `/launch`, `/resume`, pairing, web UI or RTSP), withdraws its mDNS service, and sends
   `GO`. From that moment it answers nothing new. It keeps its copies of the sockets until the rollback window ends.
5. **New process: take over.** On `GO` it starts accepting, publishes mDNS, and sends `MAINPID=<its pid>` and
   `READY=1` to systemd (`NotifyAccess=all`).
6. **Old process: drain.** It keeps its sessions until each ends normally, or until `drain_handoff_deadline`. Then it
   releases its devices, sinks and other resources as described below, and exits with status 0. A drained session ends
   with its normal stop reason; at the deadline the reason is `drain-deadline`.

### Rollback
- The new process does not send `READY` in time, exits, or closes the channel before `GO`: the old process kills it if
  it is still running, logs an Error, and keeps serving as if nothing happened (it never stopped accepting).
- The new process dies within `drain_handoff_rollback_window` after `GO`: the old process resumes accepting on its
  socket copies, publishes mDNS again, sends `MAINPID=<old pid>` to systemd, and leaves drain mode. Its sessions are not
  affected.
- After the rollback window the old process closes its socket copies. A later failure of the new process is handled by
  systemd as a normal service failure.

### The launch race
A `/launch` that the old process accepted before the switch completes on the old process. Its RTSP connection then
reaches the new process, which does not know the launch, so the stream fails once; the client retries and the retry
goes to the new process. The old process stops answering `/launch` at the switch (step 4), so only requests already in
flight are affected. The e2e covers this case.

## Shared resources while two processes run
| Resource | Rule |
|----------|------|
| GPU (capture, encode) | The new process opens no capture or encoder before its first session: the startup encoder probe is skipped in handoff mode (the `/launch` probe still runs). The e2e checks this from the new process's DRM fds and memory, and measures peak VRAM during the handoff. |
| Audio sinks | The virtual sinks have fixed names, and the new process adopts the existing ones. A draining process does not unload sink modules on exit and does not restore the default sink when its sessions end; the new process owns them after the switch. |
| Virtual input devices | In handoff mode the new process creates keyboard, mouse, touch and pen devices at its first session, not at startup. Device stable ids and gamepad MAC addresses include the generation, so the two processes never create devices with the same ids. The old process destroys all of its devices before it exits. |
| Display configuration | In handoff mode the new process does not revert display changes at startup. A draining process does not revert them when it exits. |
| App state and prep commands | The new process starts with no running app; its first launch runs the app's and the global `do` commands again (they must be idempotent). A draining process does not run `undo` commands and does not terminate apps on exit. |
| `sunshine_state.json`, `apps.json` | Only the process that accepts connections handles pairing and the web UI, so only it writes these files. Writes use a temporary file and rename. |
| Log file | In handoff mode the new process appends to the log without rotating it. Every line of a draining process carries a `[drain]` tag. |
| mDNS | Only the process that accepts connections publishes. |

## Visibility and control (session API and logs)
- The web UI listener belongs to the process that accepts connections (the new one after the switch).
- `GET /api/sessions` there lists its own sessions and the draining process's sessions, which it queries over the
  handoff channel. Each entry gets one more key, `draining` (bool). Ids are unique across both processes.
- `POST /api/sessions/<id>/close` closes either kind; for a draining session the request goes over the channel. The
  audit line is unchanged and is written by the process that serves the request.
- Each process writes its own `Session ended: reason=<r>, active sessions=<N>` line; N counts that process's sessions.

## Active-port discovery for external idle checks
External tools that detect streams by Sunshine's UDP ports need to know every base that is in use. Each process keeps
`$XDG_RUNTIME_DIR/sunshine/instances.json` up to date, by writing a temporary file and renaming it:

```json
{"instances": [
  {"pid": 1234, "role": "active", "udp_base": 47989, "sessions": 0},
  {"pid": 1200, "role": "draining", "udp_base": 48089, "sessions": 1}
]}
```

- A process adds itself at startup, updates its entry on every role change and session start or end, and removes it on
  exit. A stale entry (its pid gone) is ignored and cleaned up by the next writer.
- Fail-safe rule for consumers: treat Sunshine as busy when the file is missing or unreadable while a Sunshine process
  runs, when more Sunshine processes run than the file lists, or when any listed instance has sessions above 0 or
  traffic on its base ports.

## systemd unit (when the feature is enabled)
```ini
[Service]
Type=notify
NotifyAccess=all
ExecReload=kill -USR2 $MAINPID
```
The default `KillMode=control-group` stays: stopping the unit ends both processes.

## Tests
- **Unit (blind):**
  - config validation of the options;
  - the `LISTEN_FDS` parsing and the socket-name mapping;
  - the handoff channel protocol (`READY`, `GO`, list and close requests) over a socketpair, including timeouts and
    early close;
  - the `instances.json` writer, with stale entries and the atomic write;
  - the generation in device ids.
- **E2E, on a test host:**
  1. handoff during an active stream: the old stream keeps flowing (no gap longer than 1 s), new streams go to the new
     process, and the old process exits after its session ends;
  2. a launch in flight during the handoff fails at most once, and the retry succeeds;
  3. rollback when the new binary fails before `READY`, and when it dies inside the rollback window;
  4. deadline expiry ends the old sessions with `drain-deadline`;
  5. `/api/sessions` lists and closes sessions on both processes;
  6. `instances.json` and the idle check stay correct throughout;
  7. after the old process exits: input devices, audio sinks and the default sink are as before the handoff;
  8. peak VRAM during the handoff and the new process's DRM usage before its first session.

## Out of scope
- Moving a live session from one process to another (not possible without client support).
- Windows and macOS.
- Socket activation for the first start (the inherited-socket code accepts `LISTEN_FDS` from systemd too, but no
  `.socket` unit ships with this change).
