# Drain handoff (spec, Linux, opt-in)

Status: **draft spec, not implemented.** This page is the contract for the implementation, the tests and the review.

## Goal
Upgrade or restart Sunshine while clients are streaming, without ending their streams:

- new sessions go to a new Sunshine process (for example a newly installed binary);
- sessions that are already streaming finish on the old process, which then exits;
- clients notice nothing, except that a request in flight during the switch can fail once and is retried.

The feature is off by default and changes nothing when it is off. It targets Linux with systemd 254 or newer, as a user
or system service.

## Why this works with existing clients
- The client learns the RTSP URL (and so the RTSP port) from `sessionUrl0` in the `/launch` and `/resume` replies.
- For video, audio and control, moonlight-common-c takes the UDP port from each stream's RTSP `SETUP` reply
  (`Transport: server_port=<n>`). The well-known ports (47998, 48000, 47999) are only fallbacks for a missing header.
  Sunshine always sends the header. Clients that do not use moonlight-common-c must be checked separately.
- So a process that owns the RTSP listener can put its sessions' UDP media on any ports. Two processes can stream at once
  if they use different UDP bases. The TCP ports stay the same, because the new process inherits the listening
  sockets and both processes share their accept queues.

## Terms
- **Active process**: the process that accepts new connections.
- **Draining process**: a former active process that keeps its existing sessions and accepts nothing new.
- **Switch**: the moment the old process stops accepting and the new one starts (step 5 below).
- **Rollback window**: `drain_handoff_rollback_window` seconds after the switch, during which the old process is still
  the unit's main process and can take the sockets back.

## Configuration
| Option | Default | Meaning |
|--------|---------|---------|
| `drain_handoff` | `disabled` | `enabled` turns the feature on. |
| `drain_handoff_udp_offset` | `100` | The second UDP base is `port + offset`. Processes alternate between the two bases. |
| `drain_handoff_deadline` | `3600` | Seconds a process may drain, counted from the switch (1 to 86400). At the deadline it ends the remaining sessions (reason `drain-deadline`) and exits. |
| `drain_handoff_ready_timeout` | `30` | Seconds the new process has to report ready, or the handoff fails. |
| `drain_handoff_rollback_window` | `60` | See "Terms". |
| `drain_handoff_cooldown` | `120` | Seconds after a handoff (or a failed attempt) before the next one is accepted. At least the rollback window. |
| `drain_handoff_binary` | path Sunshine was started from | Binary to execute for the new process. |

Config validation rejects an offset that would make any UDP port of either base overlap a TCP port, overlap the
other base, or leave 1024-65535, and a cooldown shorter than the rollback window.

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
already allowed. Reachability depends on the deployment allowing the second UDP base. The operator's deployment notes
must list both port sets.

UDP sockets are bound when a process's first session starts (`start_broadcast`), as today, so a process holds no UDP
ports while it has no session. The UDP port helper takes the process's UDP base as a parameter (today `map_port` adds
offsets to the single configured `port`); TCP ports keep using `port`.

## Sequence
### 1. Trigger and checks
`ExecReload=sunshine --drain-handoff $MAINPID` (see "systemd unit") connects to the active process's control socket
and sends the single handoff verb. The active process refuses (logs a Warning naming the reason and the peer pid,
answers the helper with the reason, keeps running unchanged) when:

- the feature is disabled (then there is no control socket at all, and the helper fails to connect);
- a handoff is in progress, a draining instance exists (known from the open channel and the instance files), or the
  cooldown has not passed;
- an application with a running process is active (`proc_t` not in placebo mode [C11]), because the new process
  cannot adopt a child process. A desktop-style app (placebo mode) is adopted, see "App ownership and prep commands";
- a pairing is in progress: after `expire_pair_sessions` [C14], the pairing session map is not empty (a
  `getservercert` request can wait for its PIN, so the handoff does not wait for it);
- a launch is pending: the RTSP server holds a raised launch session (`launch_event`) whose client has not started
  streaming yet [C5]. The helper is told to retry; such a launch completes or expires within `ping_timeout`;
- the process is not a systemd service with readiness (`NOTIFY_SOCKET` unset), or systemd is older than 254 (checked
  with `systemctl --version`), or the unit name cannot be found from `/proc/self/cgroup`.

### 2. Spawn
The old process creates a socketpair (the handoff channel, `SOCK_SEQPACKET | SOCK_CLOEXEC`) and starts
`drain_handoff_binary` with the same arguments, using `posix_spawn` (or `fork` plus only async-signal-safe calls before
`exec`). In the child only, the listening sockets are placed at fds 3-6 and the channel at fd 7, and every other fd is
closed (`close_range`/`closefrom`). The child's environment adds:

- `SUNSHINE_HANDOFF_FDS=https:3,http:4,webui:5,rtsp:6` and `SUNSHINE_HANDOFF_CHANNEL=7`;
- `SUNSHINE_HANDOFF_UDP_BASE` (the base the old process does not use),
  `SUNSHINE_HANDOFF_NEXT_SESSION_ID` (launch session ids stay unique across both processes), and
  `SUNSHINE_HANDOFF_GENERATION` (keeps virtual device ids unique).

The systemd `LISTEN_FDS`/`LISTEN_PID` convention is not used: the child's pid is not known before the spawn.

Right after the spawn, the old process opens a pidfd for the child (it is the parent and has not reaped it, so the pid
cannot be reused yet) and uses it to watch the child for the rest of the handoff. It keeps reaping the child
(`waitpid`) for as long as it lives, so a dead new process never stays a zombie.

### 3. New process start (handoff mode)
- Before any other thread starts, it sets `FD_CLOEXEC` on fds 3-7 and unsets every `SUNSHINE_HANDOFF_*` variable, so
  apps and commands it launches inherit neither listeners nor the channel.
- It validates the inherited fds: each must be a listening TCP socket (`SO_ACCEPTCONN`) whose local port and address
  family match its own config (`port`, `address_family`, `bind_address`). A mismatch (config or version drift) is
  reported as a failed `READY` with the reason.
- It exchanges `HELLO` with the old process: protocol version and capabilities. An unknown major version is a failed
  `READY`. The old process's `HELLO` carries its encoder probe results (the HEVC, AV1 and 4:4:4 capability flags that
  `/serverinfo` reports) with a probe identity (build id, GPU render node and vendor/device id, driver version). The new
  process uses them only when the identity matches its own, and only for `/serverinfo` (see "GPU"). The old process's
  `HELLO` also carries its cooldown deadline, which the new process inherits.
- It adopts the sockets instead of binding them (this needs small patches: Simple-Web-Server `start()` must accept an
  existing listening socket, and the RTSP acceptor in `rtsp.cpp` must adopt one instead of `bind`/`listen`), starts its
  servers with accepting paused, and sends `READY` (or a failed `READY` with a reason).
- If the channel reports end-of-file or an error before `GO`, the new process exits with status 1.

### 4. Failed start
The new process sends a failed `READY`, exits, closes the channel, or does not report within
`drain_handoff_ready_timeout`: the old process kills it if it still runs (through the pidfd), reaps it, logs an Error,
answers the helper with a non-zero result, and keeps serving. It never stopped accepting.

### 5. Switch
On `READY` the old process, in this order:

1. pauses accepting on all four sockets (`acceptor.cancel()` / pausing the server's accept loop; never closing the
   server, which would close live connections);
2. sets the 503 gate: from now on every request that arrives on an already open (keep-alive) connection, to `/launch`,
   `/resume`, `/cancel`, pairing and the web UI API, gets 503 and `Connection: close`, and new RTSP requests on open
   RTSP connections are refused; RTSP connections of its own live sessions are unaffected. `/serverinfo` is still
   answered on keep-alive connections and reports the old process's `currentgame` (harmless; documented);
3. waits for the state-changing requests already in progress to finish. These are tracked by route, not by method
   (all client routes are GETs [C13]): `/pair`, `/launch`, `/resume`, `/cancel` and the web UI write APIs each
   increment an in-flight counter on entry and decrement it on exit (an RAII guard, so every return path decrements).
   Only these counters are waited on. A launch that answers during this wait raises a launch session whose RTSP
   connection arrives after the switch; it follows the straddle rule (fails once, the client retries). The wait is
   bounded (5 s). If it times out, the handoff fails: the old process lifts the 503 gate, resumes accepting, kills and
   reaps the new process, removes the new process's instance file, and answers the helper non-zero. Clients that got
   a 503 during the attempt retry;
4. writes `sunshine_state.json` and `apps.json` once more, under `client_auth_mutex`, so a pairing that completed
   just before the switch is on disk;
5. freezes its state: it writes neither file again unless it rolls back;
6. withdraws its mDNS service (see "mDNS");
7. sends `GO`, and answers the helper: switched (exit 0).

The new process writes neither state file before `GO`. On `GO` it reloads `sunshine_state.json` and `apps.json` under
`client_auth_mutex` and rebuilds its TLS context's trusted client certificates from the reloaded state, resumes
accepting, and publishes mDNS. It sends nothing to systemd yet: the old process stays the unit's main process until the
handover (step 7).

### 6. Rollback window
- The new process serves normally. The old process sends `PING <seq>` over the channel every 2 s from the switch until
  the handover in step 7 is done (or a rollback); then pings stop. In the new process a dedicated reader thread reads
  the channel, answers `PING` inline with `PONG <seq>` (no locks, no other work), and hands every other message to a
  worker thread, so the HTTPS, RTSP and launch paths (a first `/launch` probe can hold those for seconds) never delay a
  `PONG`. A **miss** is a `PING` with no `PONG` of the same `seq` before the next `PING` is due (2 s). Five misses in a
  row (at least 10 s) count as a hung new process: the old process kills it through the pidfd and rolls back. A late
  `PONG` resets the miss count; it is never a reason to kill.
- **Rule:** whenever the old process is still the main process (inside the window, or later while the handover in
  step 7 keeps failing), a dead or hung new process triggers a rollback.
- A death of the new process between `READY` and `GO` is handled as a full rollback too.
- On a dead or hung new process: the old process resumes accepting on its socket copies,
  answers requests normally again, reloads both state files before any write, publishes mDNS, and leaves drain mode.
  systemd sees no main-process exit, so the unit and the old process's sessions are not affected. The new process's
  sessions are lost; apps it launched and `do` commands it ran are not undone (logged as a Warning), and its instance
  file is removed by the old process. After a rollback the old process takes back app ownership (see "App ownership
  and prep commands") and makes its own virtual sink the default sink again if it has sessions.
- If the old process crashes inside the window: it is the main process, so systemd stops the unit and the new process
  and its sessions end too, then `Restart=` applies. This is the same outcome as a crash without this feature.

### 7. Main PID handover (end of the window)
1. Check through the pidfd that the new process is alive. If not, roll back as in step 6.
2. Send `MAINPID=<new pid>` followed by `BARRIER=1` to systemd (as the main process, which `NotifyAccess=main`
   allows), and wait for the barrier.
3. Read back `systemctl [--user] show -p MainPID --value <unit>` and require it to equal the new pid.
4. Only then close the socket copies, and tell the new process over the channel that it is the main process (`MAIN`).
   (The old process closed its control socket at the switch.)
5. If the barrier or the read-back fails, log an Error, stay the main process, keep the socket copies, and retry the
   handover every 10 s. The rollback rule of step 6 applies throughout. The process does not exit while it is the main
   process: past the drain deadline it keeps retrying, logs an Error every 10 s, and shows the state as
   `handover_failed` in its instance file and in `/api/sessions` responses (the top-level `handoff` field, see
   "Visibility and control"). The escape hatch is `systemctl restart` (or `stop`) of the unit, which ends both processes.

Notifications use raw sockets, not libsystemd: `MAINPID=` and the barrier are sent to `NOTIFY_SOCKET` as datagrams;
`BARRIER=1` carries the write end of a pipe as `SCM_RIGHTS`, and the barrier is complete when the read end reports
end-of-file. The read-back runs `systemctl [--user] show -p MainPID --value <unit>` with `XDG_RUNTIME_DIR` set (user
units) and a 5 s timeout.

From the handover on, the new process is the main process and sends any later notifications (`STATUS=`, `STOPPING=1`).
A dead new process after the handover is a normal main-process exit: with `KillMode=control-group` systemd stops the
unit, which also ends the draining process (its sessions end with `stop`), then `Restart=` applies.

### 8. Drain
The old process keeps its sessions until each ends normally, or until `drain_handoff_deadline` (counted from the
switch). It never exits before the handover in step 7 is done, even when its last session ends earlier. Then it
releases its devices and other resources as described below and exits with status 0. Sessions it ends at the deadline
get reason `drain-deadline`.

### Stopping, restarting and quitting
- `systemctl stop` sends `SIGTERM` to both processes. A draining process ends its sessions at once with reason `stop`
  and exits like a normal shutdown (within the existing 10 s forced-shutdown limit), so stopping does not wait for the
  drain deadline. `TimeoutStopSec=30` is enough.
- `SIGTERM` to the draining process alone (by pid) ends only its sessions, with reason `stop`. This is the way to end
  draining sessions when the channel is lost.
- In-place restart (`/api/restart`, tray restart; both `execv` the binary):
  - in the active role while a draining process exists or a handoff is in progress: refused (409 for the API, a
    Warning in the log). The 409 body names the ways out: wait for the drain, `SIGTERM` to the draining pid (ends its
    sessions), or `systemctl restart` of the unit (ends all sessions);
  - otherwise allowed; the restarted process keeps its UDP base (passed in `SUNSHINE_UDP_BASE`, kept across the
    `execv`) and binds TCP as at a normal start;
  - a draining process has no web UI and ignores tray restart and quit (it disables those menu entries).
- Quit (web UI or tray) in the active role ends the main process, so systemd stops the unit, including any draining
  process (as for `systemctl stop`).

### The launch race and requests that straddle the switch
- A `/launch` that the old process accepted before the switch completes there; the RTSP connection that follows
  reaches the new process, which does not know the launch, so the stream fails once and the client retries.
- A keep-alive request that reaches the old process after the switch gets 503 and `Connection: close`; the client
  reconnects and reaches the new process.
- A pairing that straddles the switch fails (its state lives in the process that started it) and must be retried.
- TLS handshakes in progress on the old process complete there, and their requests get 503.

## The handoff channel
- It is private: only the old process and its spawned child hold it (`SOCK_CLOEXEC` everywhere else). The peer is
  identified by that, and the child by the old process's pidfd; `SO_PEERCRED` is not used on the socketpair (it reports
  the creator's pid on both ends).
- Messages are length-checked (at most 64 KiB) and parsed strictly. Unknown types are ignored. A malformed message or
  an error closes the channel and logs an Error.
- Messages (requests carry a request id; replies echo it): `HELLO` (version, capabilities, probe results and identity,
  cooldown deadline, adopted app), `READY` (ok or reason), `GO`, `MAIN` (handover done), `PING`/`PONG`,
  `SESSIONS?`/`SESSIONS` (list), `CLOSE <id>`/`CLOSED <result>`, `END_ALL`/`ENDED` (see "App ownership and prep
  commands"), and the draining process's unsolicited `HANDOFF_STATE <state>`. Requests from the active process
  (`SESSIONS?`, `CLOSE`) time out after 500 ms, `END_ALL` after 15 s (it waits for session joins, which have a 10 s
  hang guard); they are sent only from request handlers, never while holding a lock.
- When the channel closes after `GO`, neither side ends any session: the draining process keeps draining, the active
  process keeps serving. The active process then no longer lists or closes the draining sessions (it logs a Warning)
  and keeps refusing handoffs while the draining instance file exists.

## The control socket
- Path: `$RUNTIME_DIRECTORY/control/<pid>.sock` when `RUNTIME_DIRECTORY` is set (`RuntimeDirectory=sunshine`), else
  `$XDG_RUNTIME_DIR/sunshine/control/<pid>.sock`. Without either, the feature is refused at startup with an Error.
- Mode 0600; the active process checks the connecting peer with `SO_PEERCRED` (same uid) and logs the peer pid.
- The trust boundary is the uid: any process of the same user is trusted, as it could already reload or signal the
  service. Apps launched through Sunshine cannot trigger a handoff while they run (the refusal rule above).
- It accepts exactly one verb (handoff) with no arguments, once per cooldown. Only the active process has a control
  socket; a draining process has none (the old process closes it at the switch).
- The helper connects to the socket of `$MAINPID`. Until the handover (step 7) the main PID is the old, draining
  process, which has no socket; the helper then reads that pid's instance file and fails with "a drain is in progress"
  (or "handover failing" in the `handover_failed` state). After the handover the new process is the main PID and its
  socket answers (refusing while a draining instance exists).

## Shared resources while two processes run
### GPU
- In handoff mode the new process does not run the startup encoder probe, and opens no capture or encoder before its
  first session. Until then `/serverinfo` reports the probe results received in `HELLO`. Its first `/launch` probes as
  today (the probe opens a capture and an encoder briefly).
- NVENC hosts: the `/launch` probe needs a free encoder session while the draining process still holds its own; hosts
  with a session limit must allow for both processes.
- Supported capture backends: wlr screencopy and KMS (no exclusive resources; KMS drops DRM master). Portal and X11
  capture are not supported in handoff mode (the feature is refused with them).
- The e2e checks the new process's DRM fds and memory before its first session, and peak VRAM during a handoff.

### Display configuration (no cross-process coordination needed on Linux)
On Linux, Sunshine's display-device layer does nothing: `make_settings_manager` returns `nullptr` on Linux [C1], and
`configure_display`, `revert_configuration` and the startup revert in `display_device::init` all return early when no
settings manager exists [C2]. `platf::streaming_will_start` and `streaming_will_stop` are empty on Linux [C3]. So two
processes cannot disturb each other through display configuration, and the `session_count() == 0` blocks in `/launch`
and `/resume` [C4] keep their meaning per process:

- the new process's first `/launch` or `/resume` sees its own count 0 and probes the encoders [C4], which sets its
  encoder even while the old process streams; probe results received in `HELLO` feed `/serverinfo` only and never set
  encoder state;
- the startup `display_device::init` in the new process [C6] is a no-op on Linux.

The feature is Linux-only. If a build ever has a display settings manager (`display_device` initialised with a non-null
settings manager), the feature refuses to enable, with an Error, so this section stays true.

### App ownership and prep commands
**Rule.** Prep commands may change the display mode (for example, a global `do` command that sets the output mode from
`SUNSHINE_CLIENT_WIDTH`, `_HEIGHT` and `_FPS` through the compositor's control tool). So across both processes:

- a `do` command never runs while another Sunshine process streams;
- an `undo` command runs at most once per applied `do`, in one process only, and never while another process
  streams;
- the triggers stay as today: `do` commands run only when an app starts, and `undo` commands only when the app is
  terminated. A session end, including the last one, triggers neither.

**Today (single process).** `do` commands run only in `proc_t::execute` [C18], which only `/launch` calls, and only
when no app runs [C7]. `/resume` joins the running app and runs no command [C19]. The global prep commands are merged
into every app's prep list (before the app's own, unless the app sets `exclude-global-prep-cmd`) when the apps are
loaded [C20], so one `terminate()` undoes both in reverse [C12]. `undo` commands run only in `terminate()`, called by
`/cancel`, the web UI's close-app, the last client's unpair, unpair-all, the update-client path when no session runs,
and process shutdown [C21]. The last session's end leaves a running app running and runs no `undo` [C9]. Terminating
the app ends every session of the process, because the control loop stops when no app runs [C22].

**With two processes.** Without the rules below, the new process would start with no app, report `currentgame=0`
[C8], accept a `/launch`, and run the `do` commands again while the old process streams.

- Handoffs are refused while an app with a real child process runs (step 1). A desktop-style app runs in placebo mode
  (no child process [C11]).
- The old process's `HELLO` describes its running placebo app, or none: the app id and name, the number of applied
  prep commands (`_app_prep_it - _app_prep_begin`), a digest of its merged prep list (each entry's `do`, `undo` and
  `elevated`), and the launch environment the commands ran with (the `SUNSHINE_*` variables set by `execute` [C18]).
- The new process adopts the app only when its own `apps.json` has the same id and name and its merged prep list has
  the same digest. It then marks the app as running in placebo mode with the same number of prep commands applied and
  the received launch environment. So `currentgame` and the `/launch` refusal behave as before, a second client's
  `/resume` runs no `do` command, and a later terminate runs each applied `undo` once, in reverse, with the same
  environment as today.
- If the old process has an app and adoption fails (unknown id, other name, other prep list), the new process sends a
  failed `READY` with the reason ("app or prep commands changed while an app runs; cancel the app first") and the
  handoff fails (step 4). Without an app, there is nothing to adopt and the old process has no sessions [C22].
- At the switch the old process **releases** the app: it keeps reporting the app id to its own control loop, so its
  sessions keep streaming [C22], but it never runs the app's `undo` commands, neither at a terminate nor at its exit
  [C10]. A draining process has no route that terminates apps (the 503 gate and no web UI).
- Before the active process terminates the app on any path [C21], it sends `END_ALL` over the channel when a draining
  process exists. The draining process ends all its sessions (reason `terminated`, as a terminate ends every
  session today [C22]), waits for them to be joined, gives up its claim on the app, and answers `ENDED`. Only then
  does the active process terminate the app (and run the `undo` commands). If `ENDED` does not arrive within 15 s,
  or the channel is lost while a draining instance file shows sessions, the active process does not terminate the
  app: `/cancel` and close-app answer 503 and the app keeps running, unpair and unpair-all remove the clients but leave
  the app running; each logs a Warning. So no `undo` ever runs while the other process may still stream.
- The update-client path terminates only when no session runs [C21]; in the active process that count includes the
  draining process's sessions.
- As a guard, the active process answers `/launch` with 503 while a draining process reports sessions (with the rules
  above this cannot happen: an adopted app makes `/launch` refused [C7], and without an app the old process has no
  sessions).
- On rollback the old process takes the app back (with its prep state) only if it has not answered `ENDED`. If it has,
  the new process may already have run the `undo` commands, so the old process does not take the app back, does not
  run `undo`, and logs a Warning.

### Other resources
| Resource | Rule |
|----------|------|
| Audio sinks | The virtual sinks have fixed names; the new process adopts the existing ones. A draining process does not unload sink modules on exit and does not restore the default sink when its sessions end; the active process owns them. |
| Virtual input devices | In handoff mode the new process creates keyboard, mouse, touch and pen devices at its first session, not at startup. Device stable ids and gamepad MAC addresses include the generation, so the two processes never create devices with the same ids. A draining process destroys all of its devices before it exits. A client that reconnects after a handoff gets gamepads with new MAC addresses, so the host may see them as new controllers. |
| App state and prep commands | See "App ownership and prep commands". A draining process never runs `do` or `undo` commands and never terminates apps. |
| `sunshine_state.json`, `apps.json` | Only the active process writes them (the old process stops at the switch, the new one reloads both after `GO`, the old one reloads both on rollback). Writes use a temporary file and rename. |
| Log file | In handoff mode the new process appends to the log without rotating it. Every line of a draining process carries a `[drain]` tag. |
| mDNS | Only the active process publishes. `publish.cpp` has only start and deinit today; the change adds withdraw and republish. The old process withdraws at the switch and waits up to 5 s for Avahi to drop the entry. If Avahi still renames the new process's service on a collision (for example to "name #2"), the new process resets it to the configured name once the old entry is gone. On rollback the old process republishes. |
| UPnP | Only the active process maps ports. A draining process never unmaps the shared TCP ports, and unmaps only its own UDP base when it exits and the active process does not use that base. |
| File descriptors | Sunshine's sockets are not close-on-exec today (Boost.Asio and ENet create them without `SOCK_CLOEXEC`), but apps and commands are launched with Boost.Process `limit_handles`, which closes every non-stdio fd in the child, and the in-place restart closes all fds above stderr before `execv`. So launched apps inherit no sockets today. The handoff keeps that true: the spawn closes every fd except 0-2 and 3-7 in the child, the new process sets `FD_CLOEXEC` on 3-7 and on the channel, and every new launch path must use `limit_handles` or close fds the same way. `NOTIFY_SOCKET` stays in app environments (harmless with `NotifyAccess=main`). |

## Visibility and control (session API and logs)
- The web UI listener belongs to the active process.
- `GET /api/sessions` there lists its own sessions and the draining process's sessions (`SESSIONS?` over the channel).
  Each entry has one more key, `draining` (bool), always present (`false` when the feature is off). The reply has one
  more top-level key, `handoff`, always present: `"none"` (no handoff state), `"draining"` (a draining process exists)
  or `"handover_failed"` (the main-PID handover keeps failing, step 7). `docs/api.md` and the key-set test change with
  both. Ids are unique across both processes.
- `POST /api/sessions/<id>/close` closes either kind; for a draining session the request goes over the channel. The
  audit line is unchanged and is written by the active process.
- `/serverinfo` `currentgame` and `/cancel` refer to the active process's app only.
- Each process writes its own `Session ended: reason=<r>, active sessions=<N>` line; N counts that process's sessions.
- New stop reasons: `drain-deadline` (ended at the drain deadline) and `stop` (a draining session ended because the
  service or the draining process was stopped).

## Active-port discovery for external idle checks
Each process keeps one file of its own, `<runtime dir>/instances/<pid>.json` (same runtime dir as the control socket),
and writes only that file (a temporary file plus rename):

```json
{"pid": 1234, "start_time": 5551234, "role": "active", "udp_base": 47989, "sessions": 0, "handoff": "none"}
```

`handoff` has the same values as in the session API. Transitions:

| Event | Old process file | New process file | `/api/sessions` `handoff` |
|-------|------------------|------------------|---------------------------|
| normal operation | `none` | (none) | `none` |
| switch (`GO`) | role `draining`, `draining` | role `active`, `draining` | `draining` |
| handover attempt fails (step 7) | `handover_failed` (+ `HANDOFF_STATE handover_failed`) | `handover_failed` | `handover_failed` |
| handover succeeds after failures | `draining` (+ `HANDOFF_STATE draining`) | `draining` | `draining` |
| old process exits | file deleted | `none` | `none` |
| rollback | role `active`, `none` | file deleted (by the old process if the new one is dead) | `none` |

The draining process serves no web UI; the active process reports the state from the last `HANDOFF_STATE` message,
or from the draining process's file when the channel is gone.

- `start_time` is field 22 of `/proc/<pid>/stat`; an entry whose pid now has another start time is stale (pid reuse).
- A process writes its file at startup and on every role change and session start or end, and deletes it on exit. No
  process writes another process's file, except that the old process removes a dead new process's file on rollback.
- Consumers read every file in the directory and ignore stale ones.
- Fail-safe rule for consumers: treat Sunshine as busy when the directory is missing while a Sunshine process runs,
  when a running Sunshine process has no file, when any file is unreadable, or when any instance has sessions above 0
  or traffic on its base ports. Between `/launch` and the first media, `sessions` can still be 0, so consumers also
  treat recent RTSP connections as busy.

## systemd unit
Readiness: whenever `NOTIFY_SOCKET` is set, Sunshine sends `READY=1` once its servers run (TCP listeners bound or
adopted), for a normal start and whether or not the feature is enabled. A process started by a handoff sends nothing
until it is the main process. The encoder probe at a normal start runs before `READY=1`, so the default
`TimeoutStartSec` (90 s) is enough; units that lower it must allow for the probe.

With the feature enabled, the unit needs:
```ini
[Service]
Type=notify
NotifyAccess=main
ExecReload=/usr/bin/sunshine --drain-handoff $MAINPID
TimeoutStopSec=30
# system units only:
RuntimeDirectory=sunshine
```
- `NotifyAccess=main`: only the main process can notify, so apps and commands that Sunshine launches cannot spoof
  `READY` or `MAINPID`.
- `ExecReload` exits 0 when the switch happened (step 5), and non-zero, with the reason on stderr (so it lands in the
  journal and in `systemctl reload`'s failure), when the handoff was refused or failed. Its wait is bounded by
  `drain_handoff_ready_timeout` plus 10 s. systemd applies `TimeoutStartSec` to `ExecReload`; the default 90 s is enough
  (verify on the target version).
- Minimum systemd: 254 (tracking a main PID that is not the manager's child through a pidfd). The active process checks
  the version before a handoff (step 1), and the read-back in step 7 guards against a refused `MAINPID`.
- The default `KillMode=control-group` stays.
- Feature enabled but the unit is `Type=simple` (no `NOTIFY_SOCKET`): Sunshine runs normally and refuses every handoff
  with an Error that names the settings above.
- Feature disabled but the unit is `Type=notify`: Sunshine starts normally and sends `READY=1`; the helper fails
  because there is no control socket.

## Tests
- **Unit (blind):**
  - config validation of the options;
  - parsing and validation of `SUNSHINE_HANDOFF_FDS` (wrong port, family, not listening);
  - the channel protocol over a socketpair: `HELLO` version mismatch, probe identity mismatch and app adoption data,
    `READY` failure reasons, `GO`, `PING`/`PONG` with sequence numbers and the miss rule, list and close requests with
    request ids and the 500 ms timeout, `HANDOFF_STATE`, early close, oversized and malformed messages;
  - app adoption: matching id, name and prep digest adopts in placebo mode with the same number of prep commands
    applied and the received environment; any mismatch is a failed `READY`; a terminate after adoption runs each
    applied `undo` once, in reverse, with the received environment; a released app runs no `undo` at terminate or
    exit and keeps `running()` non-zero for the control loop;
  - `END_ALL`: every terminate path waits for `ENDED` first; a timeout or a lost channel leaves the app running and
    runs no `undo`; after `ENDED` a rollback does not take the app back;
  - the in-flight route counters (every return path decrements) and the refusals in step 1;
  - the notify encoding (`MAINPID=`, `BARRIER=1` with a pipe over `SCM_RIGHTS`) against a test datagram socket;
  - the instance-file writer and the fail-safe reader (stale pid, pid reuse via start time, unreadable file);
  - fd and environment hygiene after adoption;
  - the generation in device ids;
  - the control socket: wrong uid, extra arguments, cooldown, refusal while an app process runs.
- **E2E, on a test host, with a short `drain_handoff_deadline` (for example 60 s):**
  1. handoff during an active stream: the old stream keeps flowing (no gap longer than 1 s), new streams go to the new
     process, and the old process exits after its session ends and the handover;
  2. a launch in flight during the switch fails at most once and the retry succeeds; a keep-alive `/launch` on the old
     process after the switch gets 503;
  3. failed start: the new binary fails before `READY` (crash, wrong config port, version mismatch); the old process
     keeps serving and `systemctl reload` fails;
  4. the new process dies inside the rollback window: the old process takes the sockets back, stays the main PID, its
     sessions continue, and the app is owned by the old process again;
  5. after the window, `systemctl show -p MainPID` equals the new pid; the old process exits after its drain and the
     new one keeps running, keeps its sessions and stays the main PID;
  6. the new process dies after the window: the unit stops cleanly and restarts per `Restart=`; draining sessions end
     with `stop`;
  7. the old process crashes before `GO` (the new one exits), inside the window (the unit restarts), after the window
     (the new one keeps serving);
  8. `systemctl stop` during a handoff and during a drain returns within `TimeoutStopSec`;
  9. deadline expiry ends the old sessions with `drain-deadline`;
  10. `/api/sessions` lists and closes sessions on both processes; after a channel loss, a close of a draining session
      fails cleanly, and `SIGTERM` to the draining pid ends it;
  11. instance files and the idle check stay correct throughout, including the gap between `/launch` and the first
      media;
  12. after the old process exits: input devices, audio sinks and the default sink are as before the handoff;
  13. peak VRAM during the handoff; the new process's DRM usage before its first session; `/serverinfo` codec flags on
      the new process before its first launch match the old process's;
  14. app ownership: after the switch `/serverinfo` on the new process reports the adopted app, a second client's
      `/launch` is refused and `/resume` works, the old process's sessions keep streaming after it releases the app,
      and an `apps.json` or global prep change between the processes makes the handoff fail while an app runs;
  15. a pairing and an `apps.json` edit made during the window survive both a completed handoff and a rollback; a
      pairing that straddles the switch fails once and succeeds on retry;
  16. a reload while a process is draining, within the cooldown, or while an app process runs is refused with a
      Warning, and nothing changes; `systemctl reload` exits non-zero;
  17. `/api/restart` on the active process while a draining process exists is refused; after the drain it works and
      keeps the UDP base;
  18. an app launched by either process holds none of Sunshine's sockets or the channel (`/proc/<pid>/fd`) and has no
      `SUNSHINE_HANDOFF_*` variables;
  19. garbage or an early close on the channel ends no session on either side;
  20. with the feature enabled under a `Type=simple` unit, a reload is refused with an Error and nothing changes;
  21. a client connects to the new process while the old one streams: the new process probes its encoders on its
      first `/resume` (or `/launch` with no adopted app) and the stream starts;
  22. a reload while a launch is pending (raised, RTSP not started) is refused, and succeeds after the stream starts;
  23. a pairing that completes just before the switch (in flight at the freeze) is present on the new process and on
      disk;
  24. the handover keeps failing (simulated read-back mismatch) and the new process dies: the old process rolls back
      and keeps serving; past the deadline the `handover_failed` state is logged and visible, and `systemctl restart`
      recovers;
  25. a hung (SIGSTOPped) new process inside the window is detected by the missing `PONG` and rolled back;
  26. a probe identity mismatch (different build id) makes the new process ignore the `HELLO` results for
      `/serverinfo`;
  27. the cooldown carries over: a reload right after a short drain is refused until the inherited cooldown ends;
  28. a slow `/launch` (an app whose `do` command takes 3 s) in flight at step 3 completes; its stream follows the
      straddle rule;
  29. `PONG` latency stays under 2 s while the new process runs its first `/launch` probe; no false kill;
  30. a reload while a pairing waits for its PIN is refused; a reload whose in-flight wait times out lifts the gate,
      resumes accepting, and the 503s seen in between are retried by the client;
  31. prep commands run once across a handoff: one global prep command and one app prep command, each with a `do`
      and an `undo` that append a line (`<name> do|undo <pid>`) to a marker file. Client A launches and streams; a
      handoff; client B resumes on the new process; A ends; `/cancel` from B. The marker file must hold exactly one
      `do` and one `undo` line for each command, the `undo` lines in reverse order after both `do` lines, the
      `undo` lines written by the new process's pid while no session of the old process streams, and no line from
      the old process's exit. A variant cancels while A still streams on the old process: A's session ends with
      `terminated` before the `undo` lines appear. A second variant has the global prep command with a `do` only (no
      `undo`): exactly one `do` line, no other line.

## Out of scope
- Moving a live session from one process to another (not possible without client support).
- Windows and macOS; portal and X11 capture.
- Socket activation for the first start.
- Undoing apps or `do` commands of a new process that died inside the rollback window (logged, not undone).

## Appendix: code facts this spec relies on
Base: fork branch `lanternbox` at `75612545`. Line numbers are for that commit.

| Ref | Fact | Where |
|-----|------|-------|
| C1 | `make_settings_manager` returns `nullptr` on Linux (only Windows and macOS build a settings manager). | `src/display_device.cpp:656-679` (`return nullptr;` at 678) |
| C2 | Without a settings manager, revert, configure and the startup revert return early. | `src/display_device.cpp:703-707` (`revert_configuration_unlocked`), `:762-783` (`init` calls it), `:862-867` (`configure_display`), `:893-896` (`revert_configuration`) |
| C3 | `platf::streaming_will_start` / `streaming_will_stop` are empty on Linux. | `src/platform/linux/misc.cpp:568-577` |
| C4 | `/launch` and `/resume` configure the display and reprobe encoders only when `rtsp_stream::session_count() == 0`. | `src/nvhttp.cpp:1380-1399` (launch), `:1489-1511` (resume) |
| C5 | One pending launch at a time: `session_raise` keeps it in `launch_event` and expires it after `ping_timeout`; the RTSP accept path reads it; it is cleared when the control stream connects. | `src/rtsp.cpp:597-616`, `:558`, `:622-634`; `src/stream.cpp:710` |
| C6 | `display_device::init` runs at startup. | `src/main.cpp:297` |
| C7 | `/launch` is refused while an app runs. | `src/nvhttp.cpp:1368-1374` |
| C8 | `/serverinfo` reports the running app as `currentgame` and the busy state. | `src/nvhttp.cpp:1266-1267` |
| C9 | At the last session's end, a running app is left running (only the tray is updated); with no app, gamepads end and the (no-op) revert runs. | `src/stream.cpp:2346-2363` |
| C10 | Process shutdown terminates the app (`proc.terminate()` in the deinit guard). | `src/process.cpp:57-60` |
| C11 | An app without a child process runs in placebo mode; `running()` reports it. | `src/process.cpp:267`, `:287-310` |
| C12 | `terminate()` runs the applied prep `undo` commands in reverse, from `_app_prep_it` back to `_app_prep_begin`. | `src/process.cpp:322-369` (loop at 330) |
| C13 | Every client route, including the state-changing `/pair`, `/launch`, `/resume`, `/cancel`, is a GET. | `src/nvhttp.cpp:1691-1712` |
| C14 | Pairing sessions live in `map_id_sess`; `expire_pair_sessions` drops expired ones under its mutex. | `src/nvhttp.cpp:186`, `:635-643` |
| C15 | Each RTSP request is handled on its own TCP connection, shut down after the reply. | `src/rtsp.cpp:530-541` |
| C16 | Lock order today: `clear()` joins sessions under the RTSP slots lock, and `rtsp_stream::session_count()` calls `clear(false)`. lb.2 (`fix/lb2-session-api-hardening`, issue #13) moves the join out of the slots lock. This spec adds no lock that is taken while the slots lock is held, and channel requests are never sent while holding any lock. | `src/rtsp.cpp:654-667`, `:827-832` |
| C17 | Sunshine's sockets are created without `SOCK_CLOEXEC`; app launches use Boost.Process `limit_handles`; the in-place restart closes fds above stderr. | Boost.Asio `socket_ops.ipp` (`socket`, `accept`); `src/platform/linux/misc.cpp:443-462`, `:591-594` |
| C18 | `do` commands run only in `proc_t::execute`, in order from `_app_prep_it`, which `execute` sets to the start of the app's prep list; `execute` also sets the `SUNSHINE_*` launch environment the commands run with. Its only caller is `/launch`. | `src/process.cpp:152`, `:165-168`, `:171-193`, `:216`; `src/nvhttp.cpp:1414` |
| C19 | `/resume` needs a running app and calls no `proc_t` method that runs commands. | `src/nvhttp.cpp:1449`, `:1465-1471` |
| C20 | When the apps are loaded, the global prep commands are put first in each app's prep list unless the app sets `exclude-global-prep-cmd`, followed by the app's own. | `src/process.cpp:690`, `:701-713`, `:715-730`, `:781` |
| C21 | `proc.terminate()` callers: `/cancel`; the web UI's close-app; update-client when no session runs and an app runs; unpair when no client remains; unpair-all; process shutdown. | `src/nvhttp.cpp:1563-1564`; `src/confighttp.cpp:1198`, `:1321-1322`, `:1372-1373`, `:1403`; `src/process.cpp:57-59` |
| C22 | The control loop stops when no app runs (and no session waits for its peer), then ends every session of the process. | `src/stream.cpp:1404-1407`, `:1411-1440` (reason `terminated`) |
