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
Trigger: the helper `sunshine --drain-handoff <pid>` (the unit's `ExecReload`, see "systemd unit"). It connects to the
running process's control socket and waits for the outcome, so a refused or failed handoff is reported as a non-zero
exit. There is no signal trigger.

1. **Old process: checks.** It refuses the handoff (logs a Warning, answers the helper with the reason, keeps running
   unchanged) when:
   - the feature is disabled;
   - a handoff is already in progress, this process is itself draining, or **any** draining instance still exists
     (the active process knows this from its open handoff channel to the draining process, and also checks the
     instance files). A third process would need the base the draining process still holds;
   - an application with a running process (non-empty `cmd`, not detached) is active, because the new process cannot
     adopt it. Apps without a process (desktop-style) are fine;
   - systemd readiness is not available (no `NOTIFY_SOCKET`, see "systemd unit"): without it the main PID cannot move to
     the new process, and systemd would end the service when the old process exits. The log names the unit settings
     that are needed.
2. **Old process: spawn.** It creates a Unix socketpair (the handoff channel, `SOCK_SEQPACKET`, both ends
   `FD_CLOEXEC`) and starts `drain_handoff_binary` with the same arguments, using `posix_spawn` (or `fork` plus only
   async-signal-safe calls before `exec`). The listening sockets are placed at fds 3-6 and the channel at fd 7 in the
   child only; every other fd of the old process is closed in the child. The new process receives:
   - the four listening TCP sockets, in the systemd `LISTEN_FDS` / `LISTEN_FDNAMES` / `LISTEN_PID` convention, with
     the names `https`, `http`, `webui`, `rtsp`;
   - `SUNSHINE_HANDOFF_FD` (its end of the channel), `SUNSHINE_HANDOFF_UDP_BASE` (the base that the old process does
     not use), `SUNSHINE_HANDOFF_NEXT_SESSION_ID` (so launch session ids stay unique across both processes), and
     `SUNSHINE_HANDOFF_GENERATION` (a counter used to keep virtual device ids unique).
3. **New process: start in handoff mode.** It adopts the inherited sockets instead of binding them, sets `FD_CLOEXEC`
   on them and on the channel, and unsets `LISTEN_FDS`, `LISTEN_FDNAMES`, `LISTEN_PID` and every `SUNSHINE_HANDOFF_*`
   variable, so apps and prep commands it launches inherit neither listeners nor the channel. It binds its UDP media
   ports at the given base, and starts its servers, but does **not accept** connections yet. In handoff mode it skips
   the startup actions that would disturb the old process's streams (see "Shared resources"), including the startup
   encoder probe. It then sends `READY` on the channel.
4. **Old process: switch.** On `READY` within `drain_handoff_ready_timeout`, the old process stops accepting on all
   four sockets at once (no more `/launch`, `/resume`, pairing, web UI or RTSP), withdraws its mDNS service, and sends
   `GO`. From that moment it answers nothing new. It keeps its copies of the sockets until the rollback window ends.
   It answers the helper: switched (exit 0).
5. **New process: take over.** On `GO` it starts accepting and publishes mDNS. It sends nothing to systemd yet: the old
   process stays the unit's main PID for the whole rollback window.
6. **Old process: hand over the main PID.** When the rollback window ends with the new process alive, the old process
   closes its socket copies and sends `MAINPID=<new pid>` to systemd itself (it is still the main process, so
   `NotifyAccess=main` allows it). From then on the new process is the main process and sends any later notifications.
   The old process does not exit before this point, even when its sessions end earlier.
7. **Old process: drain.** It keeps its sessions until each ends normally, or until `drain_handoff_deadline`. Then it
   releases its devices, sinks and other resources as described below, and exits with status 0. A drained session ends
   with its normal stop reason; at the deadline the reason is `drain-deadline`.

### Stopping the service
`systemctl stop` sends `SIGTERM` to both processes. A draining process does not wait for its deadline: it ends its
sessions at once with reason `stop` and exits like a normal shutdown (within the existing 10 s forced-shutdown limit).
So the stop timeout does not depend on `drain_handoff_deadline`; `TimeoutStopSec=30` is enough.

### Process tree
The new process starts as a child of the old one. It must not set `PR_SET_PDEATHSIG`, and the old process never
signals it after `GO` (its shutdown path skips the child). When the old process exits, the service manager (PID 1, or
the user manager, which is a subreaper) adopts the new process, which keeps running, keeps its sessions and stays the
unit's main PID. Before `GO`, the old process watches its child (`SIGCHLD`, `waitpid`) to detect a failed start.

### The handoff channel
Messages on the channel are untrusted input:
- each end checks the peer's credentials (`SO_PEERCRED`): same uid, and the pid of the expected child or parent;
- the control socket used by the helper is `$XDG_RUNTIME_DIR/sunshine/control/<pid>.sock`, mode 0600, and it accepts
  only peers with the same uid; it exists only while the feature is enabled;
- messages are length-checked (at most 64 KiB) and parsed strictly; unknown message types are ignored;
- a malformed message, a peer-credential mismatch or an error closes the channel and logs an Error.

When the channel closes after `GO`, neither side ends any session: the draining process keeps draining until its
sessions end or its deadline, and the active process keeps serving. The active process then no longer lists or closes
the draining sessions (it logs a Warning), and it keeps refusing new handoffs while the draining process's instance file
exists.

### Rollback
- The new process does not send `READY` in time, exits, or closes the channel before `GO`: the old process kills it if
  it is still running, logs an Error, and keeps serving as if nothing happened (it never stopped accepting).
- The new process dies within `drain_handoff_rollback_window` after `GO`: the old process (still the main PID) resumes
  accepting on its socket copies, publishes mDNS again, and leaves drain mode. systemd sees no main-process exit, so the
  unit and the old process's sessions are not affected. Sessions that had started on the new process are lost.
- The new process dies after the window (it is the main PID by then): systemd sees the main process exit. With
  `KillMode=control-group` it stops the unit, which also ends the draining process and its sessions (they end with
  reason `stop`, see "Stopping the service"). `Restart=` then applies as for any crash (for example `on-failure`
  starts a fresh process). This is the same outcome as a crash without this feature.

Why not `ExitType=cgroup` instead: it would keep the unit up when the new main process dies after the window, but then
only a draining process remains, which accepts nothing, so the service would look up while serving no one. Keeping
the old process as main for the rollback window gives a real rollback and works with any systemd version that
supports `Type=notify`.

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
| Virtual input devices | In handoff mode the new process creates keyboard, mouse, touch and pen devices at its first session, not at startup. Device stable ids and gamepad MAC addresses include the generation, so the two processes never create devices with the same ids. The old process destroys all of its devices before it exits. A client that reconnects after a handoff gets gamepads with new MAC addresses, so the host may see them as new controllers (for example, per-controller settings keyed by MAC do not carry over). |
| Display configuration | In handoff mode the new process does not revert display changes at startup. A draining process does not revert them when it exits. |
| App state and prep commands | The new process starts with no running app; its first launch runs the app's and the global `do` commands again (they must be idempotent). A draining process does not run `undo` commands and does not terminate apps on exit. |
| `sunshine_state.json`, `apps.json` | Only the process that accepts connections handles pairing and the web UI, so only it writes these files. Writes use a temporary file and rename. |
| Log file | In handoff mode the new process appends to the log without rotating it. Every line of a draining process carries a `[drain]` tag. |
| mDNS | Only the process that accepts connections publishes. |

## Visibility and control (session API and logs)
- The web UI listener belongs to the process that accepts connections (the new one after the switch).
- `GET /api/sessions` there lists its own sessions and the draining process's sessions, which it queries over the
  handoff channel. Each entry gets one more key, `draining` (bool), always present (`false` when the feature is off).
  `docs/api.md` and the key-set test change with it. Ids are unique across both processes.
- `POST /api/sessions/<id>/close` closes either kind; for a draining session the request goes over the channel. The
  audit line is unchanged and is written by the process that serves the request.
- Each process writes its own `Session ended: reason=<r>, active sessions=<N>` line; N counts that process's sessions.
- Two stop reasons are added: `drain-deadline` (a draining session ended at the deadline) and `stop` (a draining
  session ended because the service was stopped).

## Active-port discovery for external idle checks
External tools that detect streams by Sunshine's UDP ports need to know every base that is in use. Each process keeps
one file of its own, `$XDG_RUNTIME_DIR/sunshine/instances/<pid>.json`, and writes only that file (a temporary file plus
rename):

```json
{"pid": 1234, "role": "active", "udp_base": 47989, "sessions": 0}
```

- A process writes its file at startup and on every role change and session start or end, and deletes it on exit.
  No process ever writes another process's file, so updates are never lost.
- Consumers read every file in the directory. A file whose pid no longer runs is ignored (and may be deleted by the
  next Sunshine start).
- Fail-safe rule for consumers: treat Sunshine as busy when the directory is missing while a Sunshine process runs,
  when a running Sunshine process has no file, when any file is unreadable, or when any instance has sessions above 0
  or traffic on its base ports.

## Ports
The TCP ports do not change (the sockets are inherited). Only the UDP media ports move. With the default `port` 47989
and `drain_handoff_udp_offset` 100:

| Protocol | Service | First base (offset from `port`) | Second base |
|----------|---------|---------------------------------|-------------|
| TCP | client HTTPS | 47984 (-5) | same socket |
| TCP | client HTTP | 47989 (0) | same socket |
| TCP | web UI | 47990 (+1) | same socket |
| TCP | RTSP | 48010 (+21) | same socket |
| UDP | video | 47998 (+9) | 48098 |
| UDP | control (ENet) | 47999 (+10) | 48099 |
| UDP | audio | 48000 (+11) | 48100 |

The second base must be reachable wherever the first is. Check host and network firewalls (and any port forwarding)
before enabling the feature; `drain_handoff_udp_offset` can be set so the second base falls inside a range that is
already allowed. UPnP mappings, when UPnP is enabled, are added for the base in use.

## systemd unit
Readiness: whenever `NOTIFY_SOCKET` is set, Sunshine sends `READY=1` once its TCP listeners and UDP sockets are bound
and its servers run (for a normal start as well as for a handoff start's first process, and whether or not the
feature is enabled). The encoder probe at a normal start runs before that point, so the default `TimeoutStartSec`
(90 s) is enough; units that lower it must allow for the probe.

With the feature enabled, the unit needs:
```ini
[Service]
Type=notify
NotifyAccess=main
ExecReload=/usr/bin/sunshine --drain-handoff $MAINPID
TimeoutStopSec=30
```
- `NotifyAccess=main`: only the main process can notify, so apps and commands that Sunshine launches cannot spoof
  `READY` or `MAINPID`. The old process moves the main PID itself (step 6); the new process notifies only once it is
  main.
- `ExecReload` exits 0 when the switch happened, and non-zero (with the reason on stderr, so it lands in the journal
  and in `systemctl reload`'s failure) when the feature is off (no control socket), a handoff or a drain is in
  progress, an app with a process is running, readiness is unavailable, or the new process failed and was rolled
  back. Its wait is bounded by `drain_handoff_ready_timeout` plus 10 s; that must stay below the timeout systemd
  applies to `ExecReload` on the target version.
- The default `KillMode=control-group` stays: stopping the unit ends both processes.
- Feature enabled but the unit is `Type=simple` (no `NOTIFY_SOCKET`): Sunshine runs normally and refuses every handoff
  with an Error that names the settings above.
- Feature disabled but the unit is `Type=notify`: Sunshine starts normally and sends `READY=1`; the helper fails
  because there is no control socket.

## Tests
- **Unit (blind):**
  - config validation of the options;
  - the `LISTEN_FDS` parsing and the socket-name mapping;
  - the handoff channel protocol (`READY`, `GO`, list and close requests) over a socketpair, including timeouts,
    early close, oversized and malformed messages, and a peer-credential mismatch;
  - the instance-file writer (own file only, atomic write, removal on exit) and a reader that applies the fail-safe
    rules, with stale and unreadable files;
  - the fd and environment hygiene after adoption;
  - the generation in device ids.
- **E2E, on a test host:**
  0. all e2e runs use a short `drain_handoff_deadline` (for example 60 s);
  1. handoff during an active stream: the old stream keeps flowing (no gap longer than 1 s), new streams go to the new
     process, and the old process exits after its session ends;
  2. a launch in flight during the handoff fails at most once, and the retry succeeds;
  3. rollback when the new binary fails before `READY`, and when it dies inside the rollback window;
  4. deadline expiry ends the old sessions with `drain-deadline`;
  5. `/api/sessions` lists and closes sessions on both processes;
  6. `instances.json` and the idle check stay correct throughout;
  7. after the old process exits: input devices, audio sinks and the default sink are as before the handoff;
  8. peak VRAM during the handoff and the new process's DRM usage before its first session;
  9. a reload while a process is still draining is refused with a Warning, and nothing changes;
  10. an app launched by the new process holds none of the listening sockets or the channel (`/proc/<pid>/fd`), and its
      environment has no `LISTEN_*` or `SUNSHINE_HANDOFF_*` variables;
  11. the old process exits after its drain; the new process keeps running, keeps its sessions and stays the unit's
      main PID;
  12. garbage or an early close on the channel ends no session on either side;
  13. with the feature enabled under a `Type=simple` unit, a reload is refused with an Error and nothing changes;
  14. the new process dies inside the rollback window: the old process takes the sockets back, stays the main PID,
      and its sessions continue; `systemctl reload` reported success at the switch, and the rollback is logged;
  15. the new process dies after the window: the unit restarts per `Restart=`, the draining sessions end with `stop`;
  16. `systemctl reload` exits non-zero when the feature is off, and during a drain;
  17. `systemctl stop` during a drain returns within `TimeoutStopSec` and ends the draining sessions with `stop`.

## Out of scope
- Moving a live session from one process to another (not possible without client support).
- Windows and macOS.
- Socket activation for the first start (the inherited-socket code accepts `LISTEN_FDS` from systemd too, but no
  `.socket` unit ships with this change).
