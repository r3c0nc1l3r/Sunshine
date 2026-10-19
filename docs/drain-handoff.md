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
- So a process that owns the RTSP listener can put its sessions' UDP media on any ports. Two processes can stream at
  once if they use different UDP bases. The TCP ports stay the same, because the new process inherits the listening
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
other base, or leave 1024-65535, a cooldown shorter than the rollback window, and a deadline shorter than the
rollback window. The feature refuses to enable under the `FRESH_STATE` flag (state is then never saved [C29], so
pairings would not reach the new process).

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
- an application with a running process is active, because the new process cannot adopt processes: the app is not in
  placebo mode, or it is in placebo mode but its process group still has running processes (an `auto-detach` app whose
  launcher exited while the processes it started keep running [C11]). This needs a new read accessor ("adoptable": the
  `placebo` flag is set, and the process group is not valid or `kill(-pgid, 0)` with `pgid > 1` finds no process in it;
  zombies count as present, and a reused pgid gives a false "present", which refuses safely). The accessor reads the
  fields through atomics or a small mutex (the control loop writes them in `running()` and `terminate()`) and never
  calls `running()`, which can itself terminate the app and run `undo` commands
  [C21]. `kill(-pgid, 0)` also sees processes that are no longer Sunshine's children, unlike `process_group_running`
  [C11]; `EPERM` counts as a process present. With no app running there is nothing to adopt (the snapshot says none). A
  process that moves into the adopted app's empty process group after the handoff (with `setpgid`) is not killed by the
  new process's terminate, which has no group; today `terminate_process_group` would kill it [C11]. This is an accepted
  difference. An auto-detach app can keep a live group only with `wait-all: false` (with the default `true`,
  `running()` returns on the group branch first [C11]); the "not adoptable" test uses that setting. A non-placebo app
  whose process has exited but has not been polled yet is refused; the next `running()`
  poll clears it and the helper can retry. A desktop-style app (placebo mode, no process group left) is adopted, see
  "App ownership and prep commands";
- a pairing is in progress: after `expire_pair_sessions` [C14], the pairing session map is not empty (a
  `getservercert` request can wait for its PIN, so the handoff does not wait for it);
- a launch is pending: the RTSP server holds a raised launch session (`launch_event`) whose client has not started
  streaming yet [C5]. The helper is told to retry; such a launch completes or expires within `ping_timeout`;
- the process is not a systemd service with readiness (`NOTIFY_SOCKET` unset), or systemd is older than 254 (checked
  with `systemctl --version`), or the unit name cannot be found from `/proc/self/cgroup`.

The app, pairing and pending-launch checks are repeated at the switch (step 5), because the old process keeps serving
until then.

### 2. Spawn
The old process creates a socketpair (the handoff channel, `SOCK_SEQPACKET | SOCK_CLOEXEC`) and starts
`drain_handoff_binary` with the same arguments, using `posix_spawn` (or `fork` plus only async-signal-safe calls before
`exec`). In the child only, the listening sockets are placed at fds 3-6 and the channel at fd 7, and every other fd is
closed (`close_range`/`closefrom`). To avoid collisions while placing them, the sources are first duplicated to high
fds (`F_DUPFD_CLOEXEC`, 100 or above), then `dup2`'d to 3-7 (which clears `FD_CLOEXEC` on the targets). The child's
environment adds:

- `SUNSHINE_HANDOFF_FDS=https:3,http:4,webui:5,rtsp:6` and `SUNSHINE_HANDOFF_CHANNEL=7`;
- `SUNSHINE_HANDOFF_UDP_BASE` (the base the old process does not use) and `SUNSHINE_HANDOFF_GENERATION` (keeps
  virtual device ids unique). The next launch session id is sent in `GO` (step 5), because the old process keeps
  allocating ids until the switch [C28].

The systemd `LISTEN_FDS`/`LISTEN_PID` convention is not used: the child's pid is not known before the spawn.

The spawn resets the signal mask and dispositions in the child (`POSIX_SPAWN_SETSIGMASK` with an empty mask and
`POSIX_SPAWN_SETSIGDEF`), so a blocked signal in the spawning thread is not inherited. The old process gets a pidfd
for the child from the spawn itself (`pidfd_spawn`, glibc 2.39 or newer; or `clone3`
with `CLONE_PIDFD`), so no pid lookup can race with pid reuse, and uses it to watch the child for the rest of the
handoff. A dead child is reaped with `waitid(P_PIDFD, ...)`. Sunshine already reaps any child with
`waitpid(-1, WNOHANG)` whenever it checks the app [C27], so that call can find the child already reaped (`ECHILD`):
the death is still seen through the pidfd (it becomes readable), and the exit status is then logged as unknown.

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
  `HELLO` also carries its cooldown deadline, which the new process inherits, and a pidfd of the old process itself
  (`pidfd_open(getpid())`, sent with `SCM_RIGHTS` and received with `MSG_CMSG_CLOEXEC`, so no app inherits it), so the
  new process holds the right process without any pid lookup (a pid number can be reused later; the pidfd still refers
  to the old process). A `HELLO` without it is a failed `READY`. The app state is not sent here: the old process keeps
  serving until the switch, so it is sent in `GO`.
- It adopts the sockets instead of binding them (this needs small patches: Simple-Web-Server `start()` must accept an
  existing listening socket, and the RTSP acceptor in `rtsp.cpp` must adopt one instead of `bind`/`listen`), starts its
  servers with accepting paused (see "Pausing accept"), and sends `READY` (or a failed `READY` with a reason).
- In handoff mode it defers everything that announces it or acts as the host until `COMMIT` (step 5): it does not call
  `platf::publish::start()` (mDNS) or `upnp::start()`, which a normal start runs right after `http::init()`, and does
  not call `system_tray::init_tray()`, which a normal start runs after starting the server threads [C37]; so it has no
  tray icon and no tray Quit or restart entries. Its instance file shows role `pending` (see "Active-port discovery").
  On `COMMIT` it starts mDNS, UPnP and the tray and writes role `active`. A new process that exits before `COMMIT` has
  published nothing, mapped no port and shown no icon.
- If the channel reports end-of-file or an error before `GO`, or no `GO` arrives within `ping_timeout` plus 25 s of its
  `READY` (on the monotonic clock; longer than the old process's switch substeps 1-9 can take), the new process exits
  with status 1.

### 4. Failed start
The new process sends a failed `READY`, exits, closes the channel, or does not report within
`drain_handoff_ready_timeout`: the old process kills it and waits for it to be reaped (the kill-first rule, step 6),
logs an Error, answers the helper with a non-zero result, and keeps serving. It never stopped accepting.

### 5. Switch
On `READY` the old process, in this order:

1. pauses accepting on all four sockets (see "Pausing accept"; never closing a server, which would close live
   connections);
2. sets the 503 gate: from now on every request that arrives on an already open (keep-alive) connection, to `/launch`,
   `/resume`, `/cancel`, pairing, `/applist`, `/appasset` and the web UI API, gets 503 and `Connection: close`. The
   gate stays up for as long as the process drains, and during a rollback until the app list has been swapped back
   (step 6), because `/applist` and `/appasset` iterate the app list [C33]. RTSP needs no gate: each RTSP request has
   its own connection [C15], so pausing accept is enough. `/serverinfo` is still answered on keep-alive connections and
   reports the old process's `currentgame` (harmless; documented); it reads the running app, which the swap does not
   touch [C33];
3. waits for the state-changing requests, and the `/applist` and `/appasset` reads, already in progress to finish. These
   are tracked by route, not by method (all client routes are GETs [C13]): `/pair`, `/launch`, `/resume`, `/cancel`,
   `/applist`, `/appasset` and the web UI write APIs each increment an in-flight counter on entry and decrement it on
   exit (an RAII guard, so every return path decrements). Order matters, because the HTTP, HTTPS and web UI servers run
   on separate threads: a handler first increments the counter, then checks the gate (and if it is set, decrements and
   answers 503); the switcher first sets the gate, then waits for zero (sequentially consistent atomics). Only these
   counters are waited on. A launch that answers during this wait raises a launch session (its `do` commands have run);
   switch substep 4 handles it. The wait is bounded (5 s). If it times out, the handoff fails: the old process kills the
   new process and waits for it to be reaped (the kill-first rule, step 6), answers the helper non-zero, removes the new
   process's instance file, lifts the 503 gate, and resumes accepting. Clients that got a 503 during the attempt retry;
4. repeats the step-1 checks that can have changed since then: an app that is not adoptable fails the handoff as in
   switch substep 3. A pairing session (the map holds it while it waits for the PIN, after `/pair` has answered [C14])
   can only end now, because the gate refuses new ones: the old process waits for it to finish or expire, up to
   `ping_timeout`, and fails the handoff as in switch substep 3 if one remains. A pending launch [C5] is dropped at once
   (the launch event is popped and its timer cancelled): with accepting paused it could only expire, its client's RTSP
   connection waits in the shared accept queue and reaches the new process, so the stream fails once and the client
   retries; the app it started is adopted like any other;
5. writes `sunshine_state.json` once more (and `apps.json`, which only the web UI's app save writes, needs no extra
   write), under `client_auth_mutex`, so a pairing that completed
   just before the switch is on disk (`save_state` is called directly; `FRESH_STATE`, which skips it [C29], is refused
   at startup);
6. freezes its state: it writes neither file nor the credentials file again unless it rolls back;
7. withdraws its mDNS service (see "mDNS") and removes its tray icon;
8. takes the snapshot that `GO` carries: its running placebo app or none (see "App ownership and prep commands"), its
   current `host_audio` setting (the value `/resume` uses [C28]), and the next launch session id [C28];
9. checks its shutdown flag (a `SIGTERM` that has arrived aborts the handoff with the kill-first rollback of step 6),
   sets the release flag (see "App ownership and prep commands"), and only then sends `GO`. The handoff worker is
   joined before the `proc` deinit guard runs `terminate()` [C10], so a shutdown never runs `undo` while the snapshot is
   taken or `GO` is in flight;
10. waits for the new process's `GO_ACK` (5 s, on the monotonic clock from the `GO` send; a `GO_ACK` read after that
    deadline, for example because the old process was stopped, counts as a timeout and gets no `COMMIT`). On `ok` it
    re-checks the deadline right before sending `COMMIT` (a miss is a timeout), sends `COMMIT` and answers the helper
    `committed <new pid> <new start_time>`. The helper does not report success yet: it waits up to 10 s for the new
    process's instance file (that pid with that start time) to show role `active`, and only then exits 0. If the old
    process stalls so long that the new process reads `COMMIT` after its own bound, the new process exits without
    writing role `active` and the old process rolls back as for any dead new process before `MAIN` (logged as an Error);
    the helper then sees no `active` file and exits 4 (`unconfirmed`), so exit 0 always means "committed and role
    `active` observed". After `committed` the helper's code is only 0 or 4: a later rollback inside the window (and a
    `kill_pending` it may end in) is seen only in the instance files, which tooling checks after every reload. On a
    failed `GO_ACK` (with a reason), a timeout, a failed `COMMIT` send, a channel error or a dead new process, it rolls
    back with the kill-first rule of step 6 (it is still the main process), which clears the release flag, and answers
    the helper non-zero. Releasing before `GO` means that a stop of both processes in between can at worst lose the
    `undo` commands, never run them twice.

The new process writes no state file before `GO`. On `GO` it reloads `sunshine_state.json` (`load_state` also rebuilds
the trusted client certificate chain [C30]), the credentials file and `apps.json`, all under `client_auth_mutex`. Each
reload must return a status: today `load_state` returns nothing; when the file is missing, or has no `root.uniqueid`, it
generates a new host unique id, and when the file cannot be read it logs an Error and keeps the state it had [C38]. On
`GO` each of these is a failure (the old process wrote the file at switch substep 5, so a missing file or a new unique
id would be a silent identity change): a reload that fails is a failed `GO_ACK` with the reason. After the `apps.json`
reload (a reload replaces the whole `proc_t` [C31]) it checks the snapshot against its own app list and prepares the
adoption, but installs nothing yet. Then it sends `GO_ACK ok` (or a reason) and waits for `COMMIT`:

- On `COMMIT` it installs the adoption (the app, `host_audio`, the session id counter), resumes accepting, publishes
  mDNS, maps its UPnP ports, starts its tray icon (Quit and restart disabled until `MAIN`) and writes role `active` (see
  step 3). Only from here on can it run any `undo` command.
- After a failed `GO_ACK`, or with no `COMMIT` within 5 s of its `GO_ACK`, or on a channel error before `COMMIT`, it
  exits with status 1 by itself and has adopted nothing: no app, no accept, no mDNS, no tray. Its exit runs no `undo`
  (it holds no app). A `COMMIT` read after that bound (on the monotonic clock) is ignored; the old process then sees the
  new process die before `MAIN` and rolls back (step 6). The old process does not rely on any of this: every rollback
  still kills it first.

It sends nothing to systemd yet: the old process stays the unit's main process until the handover (step 7).

**Pausing accept.** Today the RTSP server treats every accept error as fatal: it logs an Error and calls `clear()`,
which ends every session [C23]. Cancelling the acceptor completes the pending accept with `operation_aborted`, so the
change must:

- pause and resume on the RTSP server's own thread (posted to its `io_context`, the switcher waits for confirmation),
  with a flag that makes the `operation_aborted` completion of a pause neither log an Error nor call `clear()`;
- keep a work guard on the `io_context`, because the RTSP loop calls `run_one()` when it has no session [C23]; with
  no pending work that returns at once and leaves the `io_context` stopped, so the loop would spin and a resume would
  need `restart()`;
- check the pause flag on the success path too: an accept that completed just before the pause is handled as today
  (read if a launch is pending, else closed [C23]) but does not re-arm `async_accept`;
- re-arm `async_accept` on resume.

Simple-Web-Server's accept loops (HTTP, HTTPS, web UI) need the same: a pause that neither stops their `io_context`
nor closes the acceptor (to be checked against its source when the patch is written). Simple-Web-Server does not
re-arm its accept after `operation_aborted`, so resuming must start its accept again.

### 6. Rollback window
- The new process serves normally. The old process sends `PING <seq>` over the channel every 2 s from the switch until
  the handover in step 7 is done (or a rollback); then pings stop. In the new process a dedicated reader thread reads
  the channel, answers `PING` inline with `PONG <seq>` (no locks, no other work), and hands every other message to a
  worker thread, so the HTTPS, RTSP and launch paths (a first `/launch` probe can hold those for seconds) never delay a
  `PONG`. A **miss** is a `PING` with no `PONG` of the same `seq` before the next `PING` is due (2 s). Five misses in a
  row (at least 10 s) count as a hung new process: the old process kills it through the pidfd and rolls back. A late
  `PONG` resets the miss count; it is never a reason to kill. An error or end-of-file on the channel before `MAIN`
  counts as a hung new process at once: the old process kills it and rolls back, so a new process never waits forever
  for `MAIN`.
- **Rule:** whenever the old process is still the main process (inside the window, or later while the handover in
  step 7 keeps failing), a dead or hung new process triggers a rollback. No rollback happens after a successful
  read-back (step 7).
- **Kill-first rule.** Every rollback, on every path (a failed start in step 4, a failure in switch substep 3 or 4, a
  failed or late `GO_ACK`, a failed `COMMIT` send, missed `PONG`s, a channel error, a dead new process, the step-7.1
  liveness check failing, a dead or hung new process during step 7), starts the same way, whatever the new process does:
  the old process sends `SIGKILL` through the pidfd (`pidfd_send_signal`; harmless if it is already dead), then waits
  until the pidfd is readable and the child is reaped (`waitid(P_PIDFD)`; `ECHILD` because another `waitpid(-1)` reaped
  it also counts [C27]). The wait is bounded (5 s); if the process is still not gone, the old process leaves the gate
  and accepting as they are (up and paused once the switch has begun; a failed start never paused them, and the new
  process cannot accept before `COMMIT`), logs an Error, sends `SIGKILL` again every 5 s, and makes the stuck kill
  visible (next item). Only once the new process is gone does it continue with the steps below, so a slow or stuck new
  process can never accept, publish mDNS or hold the app next to the old one.
- A kill that does not complete within its 5 s wait is visible: the old process answers a waiting helper non-zero with
  the reason `kill_pending`, shows `handoff` `kill_pending` in its instance file (authoritative) and in `/api/sessions`
  responses (only when its web UI is reachable; after the switch has begun it is gated), and logs an Error every 5 s
  until the reap, after which it continues the rollback and returns to `none`. The state can last indefinitely (a
  process in uninterruptible sleep cannot be reaped); no timeout resumes serving beside it. Once the switch has begun,
  no process accepts on the web UI port, so `/api/sessions` cannot answer; the instance file (and the helper's exit code
  3) is then where tooling reads it. Tooling treats `kill_pending` as busy, alerts, and never retries a reload while it
  is set; a new reload is refused in this state anyway (exit 1).
- If the helper is still waiting (no `committed` sent yet), it is answered non-zero as soon as the kill-first step
  completes (or with `kill_pending` when it does not complete in 5 s), before the in-flight wait, the reload and the
  swap; the rest of the rollback runs without it, so the `ExecReload` bound (see "systemd unit") holds.
- A death of the new process between `READY` and `GO` is handled as a rollback too (of the steps that happened, below).
- After the kill-first step, the old process, in this order: waits for the in-flight counters to reach zero (the gate is
  still up, so they only fall; it keeps the gate up and waits as long as needed, logging a Warning every 5 s, and never
  swaps while a counted request runs), reloads both state files and the credentials file and swaps in the app list (see
  "App ownership and prep commands"; a reload that fails keeps the in-memory state the old process froze at switch
  substep 6 and logs an Error, so the rollback always completes), takes the app back or drops it, then lifts the 503
  gate, resumes accepting on its socket copies, publishes mDNS, restores its tray icon, and leaves drain mode. Nothing
  is reloaded while the gated routes are served, and the routes that stay open do not read the app list. Steps for a
  switch step that never happened are skipped: a failed start (step 4) and a failure in switch substep 3 or 4 come
  before the freeze and the mDNS withdraw, so they skip the reload, the swap, the take-back and the republish, and lift
  the gate and resume accepting only if they were set. systemd sees no main-process exit, so the unit and the old
  process's sessions are not affected. The new process's sessions are lost; apps it launched and `do` commands it ran
  are not undone (logged as a Warning), and its instance file is removed by the old process. After a rollback the old
  process takes back app ownership (see "App ownership and prep commands") and makes its own virtual sink the default
  sink again if it has sessions.
- `END_ALL` against rollback, in the old process: one state, `idle`, `ending`, `ended` or `rolling_back`, changed
  under a mutex that is never held across a join or a channel send (sends use `MSG_DONTWAIT`).
  - `END_ALL` in `idle` moves to `ending`, ends and joins the sessions, sends `ENDED`, and moves to `ended`.
  - `END_ALL` in `ending` (a second terminate while the first still waits) waits for the first and gets the same answer
    (`ENDED`, or `END_REJECTED` if a rollback was decided meanwhile); `END_ALL` in `ended` is answered `ENDED` at once.
  - `END_ALL` in `rolling_back` is answered `END_REJECTED`; the active process treats it like a timeout.
  - The state starts at `idle` for each handoff. An `ENDED` that cannot be sent (`EAGAIN`) is logged; the active
    process then times out and does not terminate, which is safe.
  - Every rollback moves the state to `rolling_back` when it is decided, before the kill-first step. A rollback decided
    in `idle` takes the app back. A rollback decided in `ending` or `ended` drops the app (no `undo`, never taken back):
    the new process may have run, or may be about to run, the `undo` commands, so the claim is given up as soon as
    ending has started. This can lose an `undo`, never double it.
- A death of the new process while the old process is itself shutting down (`SIGTERM`, `systemctl stop`) starts no
  rollback.
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
   "Visibility and control"). The escape hatch is `systemctl restart` (or `stop`) of the unit, which ends both
   processes.

Notifications use raw sockets, not libsystemd: `MAINPID=` and the barrier are sent to `NOTIFY_SOCKET` as datagrams;
`BARRIER=1` carries the write end of a pipe as `SCM_RIGHTS`, and the barrier is complete when the read end reports
end-of-file. The read-back runs `systemctl [--user] show -p MainPID --value <unit>` with `XDG_RUNTIME_DIR` set (user
units) and a 5 s timeout.

If the read-back times out although systemd has already applied `MAINPID=`, the old process still acts as the main
process. A later death of the new process is then a main-process exit for systemd: the unit stops and `Restart=`
applies, as in "the new process dies after the handover" below: the draining process drops its released app
without `undo`. If the new process died abnormally, the adopted app's `undo` commands are lost, exactly as for any
Sunshine crash today; no `do` or `undo` runs twice. The same holds for a
death of the new process between the read-back and `MAIN`: systemd already tracks it as the main process. So once the
read-back has succeeded the old process never rolls back (it has closed its socket copies): a channel error or a dead
new process from then on is only logged, and systemd handles the main-process exit.

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
- `SIGTERM` to the new process alone before `COMMIT`: it holds no app, so it sends no `END_ALL` and runs no `undo`; it
  exits, and the old process rolls back (kill-first), keeps its sessions and takes the app back.
- `SIGTERM` to the new process alone between `COMMIT` and `MAIN`: its shutdown terminates the adopted app, so it sends
  `END_ALL` first, which ends the old process's sessions; the old process then sees the new one die and rolls back with
  no app and no sessions. The `undo` commands run at most once (the shutdown budget is 5 s, and the old process's joins
  can take up to 10 s, so they can be skipped); the old sessions are lost. Use `systemctl reload` again or `systemctl
  restart` instead.
- In-place restart (`/api/restart`, tray restart; both `execv` the binary):
  - in the active role while a draining process exists or a handoff is in progress: refused (409 for the API, a
    Warning in the log). The 409 body names the ways out: wait for the drain, `SIGTERM` to the draining pid (ends its
    sessions), or `systemctl restart` of the unit (ends all sessions);
  - otherwise allowed; the restarted process keeps its UDP base (passed in `SUNSHINE_UDP_BASE`, kept across the
    `execv`) and binds TCP as at a normal start;
  - a draining process has no web UI and ignores tray restart and quit (it disables those menu entries).
- Quit (web UI or tray) in the active role ends the main process, so systemd stops the unit, including any draining
  process (as for `systemctl stop`). Until it is the main process (`MAIN`, step 7), a new process refuses Quit and
  in-place restart (409 for the API, the tray entries disabled), because its exit would be a rollback rather than a
  stop.

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
  an error closes the channel and logs an Error. Before `MAIN` the old process then kills the new process and rolls
  back (step 6); after `MAIN` the rule below applies.
- Messages (requests carry a request id; replies echo it): `HELLO` (version, capabilities, probe results and identity,
  cooldown deadline; the old process's `HELLO` also attaches its own pidfd), `READY` (ok or reason), `MAIN` (handover
  done), `PING`/`PONG`, `SESSIONS?`/`SESSIONS` (list), `CLOSE <id>`/`CLOSED <result>`, `CLOSE_CERT <hash>`/`CLOSED_CERT
  <count>`, `END_ALL`/`ENDED` or `END_REJECTED` (see "App ownership and prep commands"), `GO` (with the
  snapshot)/`GO_ACK <ok|reason>`, `COMMIT`, and the draining process's unsolicited `HANDOFF_STATE <state>`. Requests
  from the active process (`SESSIONS?`, `CLOSE`, `CLOSE_CERT`) time out after 500 ms (late replies are dropped by
  request id), `END_ALL` after 15 s (it waits for session joins, which have a 10 s hang guard); they are sent only from
  request handlers, never while holding a lock.
- When the channel closes after `MAIN`, neither side ends any session: the draining process keeps draining, the active
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
  (or "handover failing" in the `handover_failed` state, "kill pending" in the `kill_pending` state; exit 1). After the
  handover the new process is the main PID and its socket answers (refusing while a draining instance exists).

## Shared resources while two processes run
### GPU
- In handoff mode the new process does not run the startup encoder probe, and opens no capture or encoder before its
  first session. Until then `/serverinfo` reports the probe results received in `HELLO`. Its first `/launch` probes as
  today (the probe opens a capture and an encoder briefly).
- NVENC hosts: the `/launch` probe needs a free encoder session while the draining process still holds its own; hosts
  with a session limit must allow for both processes.
- Capture backends: the feature enables only with wlr screencopy, the backend it has been designed and tested for (no
  exclusive resources, no display-state change), and only when `capture = wlr` is set explicitly: with `capture` empty,
  Sunshine picks backends by what verifies at startup [C36], so the backend in use is not fixed in advance. Any other
  setting (`kms`, `kwin`, `nvfbc`, `portal`, `x11`, or empty) makes the feature refuse to enable, fail closed, with an
  Error naming the setting and saying that drain handoff is supported only with `capture = wlr`. A backend can be added
  after it has been shown to work with two processes. Known reasons to check first: kwin keys its screencast permission
  on the executable path [C34], so a `drain_handoff_binary` at another path can lose it; NvFBC has per-GPU session
  limits; KMS needs DRM master handling.
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

**Today (single process).** `do` commands run only in `proc_t::execute` [C18], which only `/launch` calls, and only when
no app runs [C7]. `/resume` joins the running app and runs no command [C19]. The global prep commands are merged into
every app's prep list (before the app's own, unless the app sets `exclude-global-prep-cmd`) when the apps are loaded
[C20], so one `terminate()` undoes both in reverse [C12]. `undo` commands run only in `terminate()`, called by
`/cancel`, the web UI's close-app, the last client's unpair, unpair-all, the update-client path when no session runs,
and process shutdown, and internally by `execute` (a clean start, and its failure guard, which undoes only the commands
that ran) and by `running()` when the app's own process has exited [C21]. The internal callers never act on an adopted
app: it has no process, and it is never executed again. The last session's end leaves a running app running and runs no
`undo` [C9]. Terminating the app ends every session of the process, because the control loop stops when no app runs
[C22].

**With two processes.** Without the rules below, the new process would start with no app, report `currentgame=0`
[C8], accept a `/launch`, and run the `do` commands again while the old process streams.

- Handoffs are refused while the app is not adoptable (step 1 and switch substep 4): only a placebo app with no process
  left in its group is handed off [C11].
- The snapshot in `GO` (step 5) describes the old process's running placebo app, or none: the app id and name, the
  number of applied prep commands (`_app_prep_it - _app_prep_begin`), a digest of its merged prep list (each entry's
  `do`, `undo` and `elevated`), of the app's `working-dir`, `output` and `exit-timeout`, and of the base environment the
  commands run with (the process environment plus the `apps.json` `env` section [C35], without `SUNSHINE_HANDOFF_*`).
  The digest is computed over an ordered, length-prefixed encoding, so `("a","bc")` and `("ab","c")` differ. The
  snapshot also carries, as data and not in the digest, the launch environment the commands ran with (the `SUNSHINE_*`
  variables set by `execute` [C18]), which the new process uses for the `undo` commands. It is taken after the gate is
  up and the in-flight requests have finished, so no launch or cancel can change it afterwards.
- The new process adopts the app only when its freshly reloaded `apps.json` has the same id and name and its merged
  prep list has the same digest. Adoption is a new `proc_t` method (the fields are private): it sets the app id, the
  app (from its own `apps.json`, which also gives `get_last_run_app_name()`), `_app_prep_begin` and `_app_prep_it`
  (begin plus the applied count), placebo mode, and the environment from the snapshot, and opens the app's `output`
  file for appending if one is set. It runs before the new process accepts any request, so no lock is needed. So
  `currentgame` and the `/launch` refusal behave as before, a second client's `/resume` runs no `do` command, and a
  later terminate runs each applied `undo` once, in reverse, with the same environment as today.
- If the old process has an app and adoption fails (unknown id, other name, other prep list), the new process
  answers `GO_ACK` with the reason ("app or prep commands changed while an app runs; cancel the app first") and the
  old process rolls back (step 6). Without an app, there is nothing to adopt and the old process has no sessions
  [C22].
- When it sends `GO` (step 5) the old process **releases** the app: an atomic flag that `running()` ignores, so its
  control loop still sees the app and its sessions keep streaming [C22]. A `terminate()` of a released app, at a
  terminate or at its exit [C10], only drops the app state: it neither terminates a process group (there is none, as
  only adoptable apps are handed off; `terminate()` today always calls `terminate_process_group` [C12]) nor runs any
  `undo` command, and it sends no tray notification (the icon is removed). A draining process has no route that
  terminates apps (the 503 gate and no web UI).
- Before the active process terminates the app on any path [C21], it sends `END_ALL` over the channel when a draining
  process exists. The draining process ends all its sessions (reason `terminated`, as a terminate ends every
  session today [C22]), waits for them to be joined, gives up its claim on the app, and answers `ENDED`. Only then
  does the active process terminate the app (and run the `undo` commands).
  - **Terminate rule.** While a draining process exists, the active process terminates the app only after `ENDED`, or
    after the draining process's pidfd shows it gone or a zombie (received in `HELLO`, step 3). Nothing else counts: not
    the instance file (its session count can lag), and not a lost channel. If neither arrives within 15 s, the active
    process does not terminate the app: `/cancel` and close-app answer 503 and the app keeps running, unpair and
    unpair-all remove the clients but leave the app running; each logs a Warning. So no `undo` ever runs while the other
    process may still stream, and an old process that never answered `ENDED` and is still alive can roll back and take
    the app back without a second `undo`.
  - `/cancel` sends `END_ALL` and gets `ENDED` (or the pidfd result) before it ends its own sessions; today it ends
    them first [C21]. If the terminate is refused, its own sessions are not ended either.
  - On the shutdown path (`systemctl stop`, Quit) the budget is 5 s, inside the 10 s forced-shutdown limit. With
    `systemctl stop` both processes get `SIGTERM`; the draining process ends its sessions and exits, so its pidfd
    becomes readable within the budget and the `undo` commands run once, as today. On a timeout they are skipped
    (logged), never run twice. The channel is closed only after the `proc` deinit guard [C10] has run.
  - In the draining process, `END_ALL` and `CLOSE_CERT` run on their own threads (they join sessions), so `SESSIONS?`
    and `CLOSE` are still answered while they wait. This relies on lb.2, where list and close do not wait for a join
    [C16]. The active process's request handler waits up to 15 s (documented for `/cancel`). The client HTTPS server
    handles one request at a time, so other HTTPS requests wait meanwhile; an implementation may answer `/cancel` with
    503 early and finish the terminate on a worker, under the same rule.
- The terminate rule applies on every terminate path, also when the active process has forgotten the adopted app after
  an `apps.json` save (below): `END_ALL` is still sent first while a draining process exists, which ends its sessions as
  a terminate does today. `/cancel` sends it even when no app runs any more (today it terminates only when `running() >
  0` [C21]), because it ends sessions in both processes. The old process's claim ends with `ENDED`, so the adopted app's
  `undo` commands are lost, as they are today after such a save.
- The update-client path terminates only when no session runs [C21]; in the active process it uses a new
  `total_session_count()` (own sessions plus the draining process's, from `SESSIONS?`), while `session_count()` stays
  per process (so the new process's first `/launch` or `/resume` still probes its encoders [C4]). If `SESSIONS?`
  times out, the total is treated as non-zero: no terminate, and `/launch` is answered 503.
- As a guard, the active process answers `/launch` with 503 while `total_session_count()` shows draining sessions (an
  adopted app already makes `/launch` refused [C7], and without an app the old process has no sessions; the guard
  matters after an `apps.json` save has made the active process forget the adopted app, see below).
- On rollback the old process reloads `apps.json` without `proc::refresh`, which would replace the whole `proc_t` while
  its control loop polls `running()` [C31] [C22]: a new method swaps in only the app list, under the same rule as
  adoption (no request is served yet). If it has not answered `ENDED`, it takes the app back by clearing the release
  flag; the running app keeps its own copy of its prep list, so a changed `apps.json` does not change what its `undo`
  runs. If it has answered `ENDED`, the new process may already have run the `undo` commands, so the old process drops
  the released app state (no `undo`, no ghost `currentgame`) and logs a Warning. The new process can change its app only
  through a terminate, which needs `ENDED` first, or by starting one when it adopted none, in which case the old process
  had no sessions and no app to take back.
- Today an `apps.json` save through the web UI replaces the whole `proc_t` and so forgets a running app without
  running its `undo` commands [C31]. This spec keeps that behaviour; in the active process the `/launch` guard above
  still stops a `do` command from running while the old process streams.

### Other resources
| Resource | Rule |
|----------|------|
| Audio sinks | The virtual sinks have fixed names; the new process adopts the existing ones. A draining process does not unload sink modules on exit and does not restore the default sink when its sessions end; the active process owns them. |
| Virtual input devices | In handoff mode the new process creates keyboard, mouse, touch and pen devices at its first session, not at startup. Device stable ids and gamepad MAC addresses include the generation, so the two processes never create devices with the same ids. A draining process destroys all of its devices before it exits. A client that reconnects after a handoff gets gamepads with new MAC addresses, so the host may see them as new controllers. |
| App state and prep commands | See "App ownership and prep commands". A draining process never runs `do` or `undo` commands and never terminates apps. |
| `sunshine_state.json`, `apps.json` | Only the active process writes them (the old process stops at the switch, the new one reloads both after `GO`, the old one reloads both on rollback). Writes use a temporary file and rename. |
| Session counts, instance file, `HANDOFF_STATE` | Updated from a separate notifier thread that reads an atomic session count (like `running_sessions` [C25]). Never from inside a session join or while holding the RTSP slots lock or the clear lock (lb.2): those paths would deadlock with `session_count()` or `clear()` [C16]. The new stop reasons need a reason parameter where the code hard-codes `terminated` today [C26]. |
| Login limit | The failed-login table of the login limit is in memory per process, so a handoff clears lockouts. Accepted for the first version. |
| Web UI CSRF tokens | Per process [C32]. After a switch the first web UI write from an already open page fails (403) until the page reloads; documented. |
| Tray icon | Only the active process shows one (removed at the switch, restored on rollback). The tray updates at session start and end [C9] are skipped while the icon is removed. |
| Log file | In handoff mode the new process appends to the log without rotating it. Every line of a draining process carries a `[drain]` tag. |
| mDNS | Only the active process publishes. `publish.cpp` has only start and deinit today; the change adds withdraw and republish. The old process withdraws at the switch and waits up to 5 s for Avahi to drop the entry. If Avahi still renames the new process's service on a collision (for example to "name #2"), the new process resets it to the configured name once the old entry is gone. On rollback the old process republishes. |
| UPnP | Only the active process maps ports. A draining process never unmaps the shared TCP ports, and unmaps only its own UDP base when it exits and the active process does not use that base. A new process maps ports only at `COMMIT`; after a rollback that killed it, the old process removes the new process's UDP-base mapping, best effort, on a background task after the reap (skipped when UPnP is off); it never delays accept, mDNS or the helper's answer. The ports it removes are the new base's UDP ports minus every port the old process itself maps (its own UDP base and the shared TCP ports), so it can never remove a mapping the old process uses, even with a misconfigured offset. |
| File descriptors | Sunshine's sockets are not close-on-exec today (Boost.Asio and ENet create them without `SOCK_CLOEXEC`), but apps and commands are launched with Boost.Process `limit_handles`, which closes every non-stdio fd in the child, and the in-place restart closes all fds above stderr before `execv`. So launched apps inherit no sockets today. The handoff keeps that true: the spawn closes every fd except 0-2 and 3-7 in the child, the new process sets `FD_CLOEXEC` on 3-7 and on the channel, and every new launch path must use `limit_handles` or close fds the same way. `NOTIFY_SOCKET` stays in app environments (harmless with `NotifyAccess=main`). |

## Visibility and control (session API and logs)
- The web UI listener belongs to the active process.
- `GET /api/sessions` there lists its own sessions and the draining process's sessions (`SESSIONS?` over the channel).
  Each entry has one more key, `draining` (bool), always present (`false` when the feature is off). The reply has one
  more top-level key, `handoff`, always present: `"none"` (no handoff state), `"draining"` (a draining process exists),
  `"handover_failed"` (the main-PID handover keeps failing, step 7), or `"kill_pending"` (a rollback waits for the new
  process to be reaped, step 6; seen only while the web UI is reachable, the instance file is authoritative).
  `docs/api.md` and the key-set test change with both. Ids are unique across both processes.
- `POST /api/sessions/<id>/close` closes either kind; for a draining session the request goes over the channel. The
  audit line is unchanged and is written by the active process.
- `/serverinfo` `currentgame` and `/cancel` refer to the active process's app only.
- Revoking a client reaches both processes. Today, disabling a client ends its sessions [C24]. The active process
  also sends `CLOSE_CERT <SHA-256 of the client certificate>` over the channel, and the draining process ends that
  client's sessions (reason `terminated`). As a backstop that needs no channel, the draining process re-reads
  `sunshine_state.json` (read-only) whenever it changes (inotify on its directory, checked at least every 2 s) and
  ends the sessions of any client that is disabled there (`enabled=false`, today's semantics [C24]). A client missing
  from the file ends nothing, because a newer state schema could move the list. An unreadable or partial file, or one
  without a parseable client list, ends no session (logged as a Warning, retried). So a revoked
  client cannot keep streaming on the draining process until the drain deadline.
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
| new process started, until `COMMIT` | `none` | role `pending`, `none` | `none` |
| switch (`COMMIT`) | role `draining`, `draining` | role `active`, `draining` | `draining` |
| rollback kill not yet complete | role `active`, `kill_pending` | unchanged (removed by the old process after the reap) | `kill_pending` (only while the web UI is reachable; the instance file is authoritative) |
| handover attempt fails (step 7) | `handover_failed` (+ `HANDOFF_STATE handover_failed`) | `handover_failed` | `handover_failed` |
| handover succeeds after failures | `draining` (+ `HANDOFF_STATE draining`) | `draining` | `draining` |
| old process exits | file deleted | `none` | `none` |
| rollback | role `active`, `none` | file deleted (by the old process if the new one is dead) | `none` |

The draining process serves no web UI; the active process reports the state from the last `HANDOFF_STATE` message,
or from the draining process's file when the channel is gone.

- `start_time` is field 22 of `/proc/<pid>/stat`; an entry whose pid now has another start time is stale (pid reuse).
- A process writes its file at startup (a new process in handoff mode with role `pending`, which becomes `active` only
  at `COMMIT`) and on every role change and session start or end, and deletes it on exit. No process writes another
  process's file, except that the old process removes a dead new process's file on rollback.
- Consumers read every file in the directory and ignore stale ones. A `pending` file never names the active ports, and
  it makes consumers treat Sunshine as busy (a handoff is in progress).
- Fail-safe rule for consumers: treat Sunshine as busy when the directory is missing while a Sunshine process runs, when
  a running Sunshine process has no file, when any `pending` file exists, when any file is unreadable, when the
  directory exists but cannot be listed completely (an error opening or reading it, also one in the middle of the scan:
  fail closed, never a partial list read as idle), when any instance is in state `kill_pending`, or when any instance
  has sessions above 0 or traffic on its base ports. Between `/launch` and the first media, `sessions` can still be 0,
  so consumers also treat recent RTSP connections as busy.

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
- `ExecReload` exits 0 only when the switch happened and the new process's instance file shows role `active` (step 5),
  and non-zero otherwise (see "Helper exit codes" below), with the reason on stderr (so it lands in the journal and in
  `systemctl reload`'s failure). Its wait is bounded by `drain_handoff_ready_timeout` plus `ping_timeout` plus 35 s (the
  `ping_timeout` term covers the switch substep 4 wait; the 35 s are four 5 s waits, namely the in-flight wait, the mDNS
  withdraw, `GO_ACK` and one kill-first wait, the helper's 10 s wait for role `active`, and 5 s margin; 75 s with the
  defaults of 30 s and 10 s). When the kill is still pending at the end of that wait, the helper exits non-zero with
  `kill_pending`. systemd applies `TimeoutStartSec` to `ExecReload`; the default 90 s is enough for the default timeouts
  (verify on the target version). With larger `drain_handoff_ready_timeout` or `ping_timeout` values, set
  `TimeoutStartSec` to at least the bound plus 10 s.
- Minimum systemd: 254 (tracking a main PID that is not the manager's child through a pidfd). The active process checks
  the version before a handoff (step 1), and the read-back in step 7 guards against a refused `MAINPID`.
- The default `KillMode=control-group` stays.
- Feature enabled but the unit is `Type=simple` (no `NOTIFY_SOCKET`): Sunshine runs normally and refuses every handoff
  with an Error that names the settings above.
- Feature disabled but the unit is `Type=notify`: Sunshine starts normally and sends `READY=1`; the helper fails
  because there is no control socket.

Helper exit codes:

| Code | Meaning | What tooling does |
|------|---------|-------------------|
| 0 | Switched: `COMMIT` sent and the new process's instance file shows role `active`. | Verify the active instance as usual. |
| 1 | Refused before anything started (feature off or no control socket, drain in progress, handover failing, `kill_pending`, cooldown, app process running, pairing, pending launch, not a systemd notify unit). Nothing changed. | Retry later if the reason is transient; never while `kill_pending`. |
| 2 | Failed and rolled back: the new process was killed and reaped; the old process serves alone. | Retry after the cooldown. |
| 3 | `kill_pending`: the rollback could not reap the new process (for example a process in uninterruptible sleep). The old process leaves the gate and accepting as they are and keeps killing. | Treat as busy, alert, never retry automatically. |
| 4 | `unconfirmed`: `COMMIT` was sent but the new process's file did not show `active` within 10 s. | Read the instance files; do not retry until they show one `active` instance and no `kill_pending`. |
| 5 | Usage or internal error of the helper (bad arguments, its own timeout). | Alert. |

## Operator runbook: `kill_pending`
`kill_pending` is fail-safe: a rollback could not reap the new process, so the old process keeps everything as it was
when the kill started and accepts nothing new beside a process that might still run. Once the switch has begun, no new
connection is accepted until it is resolved; the old process's existing sessions keep streaming.

**Recognise it.**

- `systemctl reload` (the helper) exits 3 with `kill_pending` on stderr; a later reload exits 1 ("kill pending").
- The old process's instance file `<runtime dir>/instances/<pid>.json` shows `"handoff": "kill_pending"`; the new
  process's file still exists (role `pending` or `active`). `/api/sessions` does not answer once the switch has begun.
- The journal shows an Error about every 5 s naming the new process's pid and `kill_pending`.

**Check.**

- The new process's state: `ps -o pid,stat,wchan:32,cmd -p <new pid>` and `grep State /proc/<new pid>/status`. `D`
  (uninterruptible sleep) is the expected cause; note the wait channel (`/proc/<new pid>/wchan`; as root,
  `/proc/<new pid>/stack` shows the kernel stack).
- The kernel log (`journalctl -k`) around the time: GPU driver errors, hung-task warnings, or a stalled filesystem
  (network or FUSE mounts) usually explain a `D` state.
- Whether the old process still has sessions (its instance file's `sessions`), to choose the recovery window.

**Recover.**

- Do not retry the reload; it is refused while the state lasts, and tooling must not retry it automatically.
- If the blocking operation completes, the process dies from the pending `SIGKILL`, the old process finishes the
  rollback by itself and serves again: the instance file returns to `none` and the new process's file disappears.
  Wait a few minutes for this first if the wait channel points at a slow but healthy operation.
- Otherwise, in an idle window (or accepting the loss of the old process's sessions): `systemctl stop` the unit, then
  `systemctl start`. systemd kills the whole control group; a process still in `D` state stays until the kernel
  releases it, and while it lives it still holds the inherited listening sockets, so a fresh start fails to bind its
  TCP ports. If the process does not go away, the underlying kernel or driver condition has to be cleared, which can
  need a host reboot.
- Afterwards, check that exactly one instance file shows role `active` and no file shows `kill_pending` before
  allowing reloads again.

## Tests
- **Unit (blind):**
  - config validation of the options;
  - parsing and validation of `SUNSHINE_HANDOFF_FDS` (wrong port, family, not listening);
  - the channel protocol over a socketpair: `HELLO` version mismatch and probe identity mismatch, `READY` failure
    reasons, `GO` with the snapshot and `GO_ACK` (ok, reason, timeout), `PING`/`PONG` with sequence numbers and the
    miss rule, list, close and `CLOSE_CERT` requests with request ids, the 500 ms timeout and dropped late replies,
    `END_ALL`/`ENDED` and its 15 s timeout, `HANDOFF_STATE`, early close, oversized and malformed messages;
  - pausing and resuming the RTSP acceptor ends no session, logs no Error, and leaves the RTSP loop idle (no spin);
  - the in-flight counter order: a handler that enters while the gate is being set is either counted or refused,
    never missed (stress test);
  - the switch re-checks: an app process, a pairing session or a pending launch that appears after step 1 delays or
    fails the switch;
  - the spawn fd placement when the sources already sit at fds 3-7, and a child reaped by another `waitpid(-1)`;
  - the draining process's revocation backstop: a client disabled in a rewritten state file ends its sessions; a
    removed client, or an unreadable or partial file, ends none;
  - app adoption: matching id, name and prep digest adopts in placebo mode with the same number of prep commands applied
    and the received environment; any mismatch is a failed `GO_ACK` and a rollback; a terminate after adoption runs each
    applied `undo` once, in reverse, with the received environment; a released app runs no `undo` at terminate or exit,
    terminates no process group, and keeps `running()` non-zero for the control loop; an `auto-detach` app whose process
    group still runs is not adoptable (step 1 and switch substep 4 refuse);
  - `total_session_count()`, the `/launch` guard (503 while draining sessions exist or `SESSIONS?` times out), and
    update-client, unpair and unpair-all with draining sessions;
  - `END_ALL` on the shutdown path (5 s budget), with a zombie draining pid, and rejected during a rollback;
  - kill-first: every rollback path sends `SIGKILL` through the pidfd and waits for the reap before resuming accept,
    mDNS or the app; a new process that ignores `SIGTERM` or is stopped is still gone before the old one accepts;
  - `COMMIT`: without it within 5 s, or after a failed `GO_ACK`, the new process exits and has adopted, accepted and
    published nothing; before `COMMIT` (at `READY`, at `GO_ACK`, and at the `COMMIT` timeout) a new process has called
    neither `platf::publish::start()` nor `upnp::start()` nor `init_tray()`, and its instance file shows role
    `pending`;
  - a reload that fails on `GO` is a failed `GO_ACK`; a reload that fails on rollback keeps the frozen state;
  - a kill that does not complete in 5 s shows `kill_pending` and answers the helper non-zero;
  - the new process's bound before `GO` (`ping_timeout` plus 25 s from `READY`, monotonic clock) makes it exit with
    status 1 having adopted, accepted and published nothing;
  - a `COMMIT` read after the new process's bound (test hook delaying the old process after its deadline re-check):
    the new process exits, the old process logs an Error and rolls back cleanly;
  - after a rollback that killed a new process past `COMMIT`, the old process removes the new process's UDP-base UPnP
    mapping without delaying accept or the helper's answer; the set of ports it removes never contains a port the old
    process maps (its own UDP base, the TCP ports), also when the two bases overlap;
  - the helper's exit codes 0-5: 0 only after the new process's file shows `active` (pid and start time match), 4 when
    it does not within 10 s; `kill_pending` shown in the instance file, and a new reload refused (exit 1) while it is
    set;
  - `systemctl stop` (both processes get `SIGTERM`) between `GO` and `GO_ACK`: the `undo` commands run at most once;
  - the `END_ALL` state machine: `END_REJECTED` while rolling back, a rollback during `ending` or after `ended` drops
    the app, the mutex is never held across a join or a send;
  - a channel error before `MAIN` makes the old process roll back at once;
  - `/applist` and `/appasset` on a keep-alive connection to a draining or rolling-back process get 503;
  - `END_ALL` in `ending` (a second terminate) and in `ended`; an `ENDED` send that fails with `EAGAIN` is logged and
    the active process does not terminate; `/cancel` with no running app still sends `END_ALL`;
  - a rollback waits for in-flight `/applist` and `/appasset` before swapping the app list; after a successful
    read-back no rollback happens;
  - `SIGTERM` to the new process alone before `COMMIT`: no `END_ALL`, no `undo`, the old process rolls back with its
    sessions and the app; between `COMMIT` and `MAIN`: `undo` at most once, the old process rolls back with no app;
  - a pause that races a successful accept does not re-arm; rollback swaps the app list while the control loop polls
    `running()` and never sees 0;
  - `END_ALL`: every terminate path waits for `ENDED` (or the draining process's pidfd, opened at `HELLO`, showing it
    gone) first; a timeout, a lost channel, or a stale instance file showing 0 sessions leaves the app running and runs
    no `undo`; after `ENDED` a rollback does not take the app back; a lost channel followed by a rollback runs `undo` at
    most once in total; `/cancel` ends its own sessions only after `ENDED`; `systemctl stop` during a drain runs `undo`
    once;
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
  19. garbage or an early close on the channel after `MAIN` ends no session on either side; before `MAIN` it makes the
      old process roll back, and the old process's sessions continue;
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
  31. prep commands run once across a handoff: one global prep command and one app prep command, each with a `do` and an
      `undo` that append a line (`<name> do|undo $PPID`, the Sunshine pid) to a marker file. Client A launches and
      streams; a handoff; client B resumes on the new process; A ends; `/cancel` from B. The marker file must hold
      exactly one `do` and one `undo` line for each command, the `undo` lines in reverse order after both `do` lines,
      the `undo` lines written by the new process's pid while no session of the old process streams, and no line from
      the old process's exit. A variant cancels while A still streams on the old process: A's session ends with
      `terminated` before the `undo` lines appear. A second variant has the global prep command with a `do` only (no
      `undo`): exactly one `do` line, no other line;
  32. a placebo app launched, or cancelled, on the old process after `READY` and before the switch is adopted as it
      is at the switch (or not adopted after a cancel): no second `do`, no second `undo`;
  33. a pairing started after the step-1 check and before the switch either completes on the old process before
      `GO` or makes the switch fail; it is never cut by the switch;
  34. disabling a client on the active process ends its session on the draining process within 2 s, also with the
      channel closed;
  35. the new process cancels the adopted app and then dies inside the window: the old process rolls back without
      taking the app back, and no `undo` line appears twice;
  36. the RTSP loop of a draining process uses no measurable CPU while idle, and a rollback accepts RTSP again;
  37. a new process that delays `GO_ACK` by more than 5 s (a test hook compiled only into test builds, under
      `SUNSHINE_TESTS`, never into a release binary): the old process kills it, waits for the reap, rolls back, and is
      the only process accepting and publishing mDNS; the new process never accepted a connection;
  38. with any `capture` setting other than `wlr` (for example `kms`, `kwin`, or empty), the feature refuses to enable
      with an Error naming the setting, and a reload is refused;
  39. the old process is stopped with `SIGSTOP` right after it sends `GO`: the new process sends `GO_ACK`, gets no
      `COMMIT`, and exits within about 5 s; while it ran it accepted no connection, published no mDNS entry, mapped no
      UPnP port and showed no tray icon, and its instance file never showed role `active`. After `SIGCONT` the old
      process rolls back (kill-first) and serves alone;
  40. the old process is stopped with `SIGSTOP` right after the new process's `READY`: the new process exits within
      `ping_timeout` plus 25 s, having accepted, published and mapped nothing; after `SIGCONT` the old process rolls
      back (kill-first) and serves alone.

## Out of scope
- Moving a live session from one process to another (not possible without client support).
- Windows and macOS; portal and X11 capture.
- Socket activation for the first start.
- Undoing apps or `do` commands of a new process that died inside the rollback window (logged, not undone).

## Appendix: code facts this spec relies on
Base: fork branch `lanternbox` at `75612545`. Line numbers are for that commit.

| Ref | Fact | Where |
|-----|------|-------|
| C1 | `make_settings_manager` returns `nullptr` on Linux (only Windows and macOS build a settings manager). | `src/display_device.cpp:656-680` (`return nullptr;` at 678) |
| C2 | Without a settings manager, revert, configure and the startup revert return early. | `src/display_device.cpp:703-707` (`revert_configuration_unlocked`), `:762-783` (`init` calls it), `:862-867` (`configure_display`), `:893-896` (`revert_configuration`) |
| C3 | `platf::streaming_will_start` / `streaming_will_stop` are empty on Linux. | `src/platform/linux/misc.cpp:568-577` |
| C4 | `/launch` and `/resume` configure the display and reprobe encoders only when `rtsp_stream::session_count() == 0`. | `src/nvhttp.cpp:1380-1399` (launch), `:1489-1512` (resume) |
| C5 | One pending launch at a time: `session_raise` keeps it in `launch_event` and expires it after `ping_timeout`; the RTSP accept path reads it; it is cleared when the control stream connects. | `src/rtsp.cpp:597-616`, `:558`, `:622-634`; `src/stream.cpp:710` |
| C6 | `display_device::init` runs at startup. | `src/main.cpp:297` |
| C7 | `/launch` is refused while an app runs. | `src/nvhttp.cpp:1368-1374` |
| C8 | `/serverinfo` reports the running app as `currentgame` and the busy state. | `src/nvhttp.cpp:1266-1267` |
| C9 | At the last session's end, a running app is left running (only the tray is updated); with no app, gamepads end and the (no-op) revert runs. | `src/stream.cpp:2346-2363`; tray update at session start `:2414-2417` |
| C10 | Process shutdown terminates the app (`proc.terminate()` in the deinit guard). | `src/process.cpp:57-60` |
| C11 | An app without a child process runs in placebo mode; `running()` reports it. An `auto-detach` app whose command exits with 0 within 5 s also becomes placebo, while processes it started can still run in its process group; `terminate()` always calls `terminate_process_group`, which kills a running group. `placebo` is private. | `src/process.cpp:267`, `:287-310` (auto-detach at `:306-310`), `:66-67`, `:326` |
| C12 | `terminate()` runs the applied prep `undo` commands in reverse, from `_app_prep_it` back to `_app_prep_begin`. | `src/process.cpp:322-370` (loop at 330) |
| C13 | Every client route, including the state-changing `/pair`, `/launch`, `/resume`, `/cancel`, is a GET. | `src/nvhttp.cpp:1692-1714` |
| C14 | Pairing sessions live in `map_id_sess`; `expire_pair_sessions` drops expired ones under its mutex. | `src/nvhttp.cpp:186`, `:635-638` |
| C15 | Each RTSP request is handled on its own TCP connection, shut down after the reply. | `src/rtsp.cpp:530-541` |
| C16 | Lock order today: `clear()` joins sessions under the RTSP slots lock, and `rtsp_stream::session_count()` calls `clear(false)`. lb.2 (`fix/lb2-session-api-hardening`, issue #13) moves the join out of the slots lock. This spec adds no lock that is taken while the slots lock is held, and channel requests are never sent while holding any lock. | `src/rtsp.cpp:654-667`, `:827-832` |
| C17 | Sunshine's sockets are created without `SOCK_CLOEXEC`; app launches use Boost.Process `limit_handles`; the in-place restart closes fds above stderr. | Boost.Asio `socket_ops.ipp` (`socket`, `accept`); `src/platform/linux/misc.cpp:443-462`, `:591-594` |
| C18 | `do` commands run only in `proc_t::execute`, in order from `_app_prep_it`, which `execute` sets to the start of the app's prep list; `execute` also sets the `SUNSHINE_*` launch environment the commands run with. Its only caller is `/launch`. | `src/process.cpp:152`, `:165-168`, `:171-193`, `:216`; `src/nvhttp.cpp:1414` |
| C19 | `/resume` needs a running app and runs no `do` command (its `running()` call can still terminate an app whose own process has exited, see C21). | `src/nvhttp.cpp:1449`, `:1465-1471` |
| C20 | When the apps are loaded, the global prep commands are put first in each app's prep list unless the app sets `exclude-global-prep-cmd`, followed by the app's own. | `src/process.cpp:690`, `:702-714`, `:716-731`, `:781` |
| C21 | `proc.terminate()` callers: `/cancel`; the web UI's close-app; update-client when no session runs and an app runs; unpair when no client remains; unpair-all; process shutdown. Internal: `execute()` at its start and in its failure guard, and `running()` when the app's own process has exited. | `src/nvhttp.cpp:1561-1564` (`/cancel` ends its sessions first, at 1561); `src/confighttp.cpp:1198`, `:1321-1322`, `:1372-1373`, `:1403`; `src/process.cpp:57-59`; internal `:154`, `:212-214`, `:313-317` |
| C22 | The control loop stops when no app runs (and no session waits for its peer), then ends every session of the process. | `src/stream.cpp:1404-1407`, `:1411-1440` (reason `terminated`) |
| C23 | Any accept error, including a cancelled accept, makes the RTSP server log an Error and `clear()` all sessions; the RTSP loop calls `run_one_for(500ms)` while `session_count() > 0` and `run_one()` otherwise; without a launch pending, an accepted connection is closed. | `src/rtsp.cpp:547-553`, `:556-571`, `:766-773` |
| C24 | Disabling a client ends its sessions (`terminate_sessions_by_cert` → `clear_by_cert`); unpair does not end sessions unless it removes the last client (then the app is terminated). | `src/confighttp.cpp:1315-1320`, `:1372-1373`; `src/rtsp.cpp:675`, `:850-851` |
| C25 | `running_sessions` is an atomic count of running sessions. | `src/stream.cpp:2223` |
| C26 | `clear()`, `clear_by_cert()` and the control loop's shutdown stop sessions with the hard-coded reason `terminated`. | `src/rtsp.cpp:660`, `:680`; `src/stream.cpp:1439` |
| C27 | `proc_t::running()` reaps any child (`waitpid(-1, WNOHANG)`) on each call. | `src/process.cpp:287-296` |
| C28 | Launch session ids come from a per-process counter; `host_audio` is one per-process value, set by `/launch` and by `/resume` when no session runs, and otherwise kept by `/resume`. | `src/nvhttp.cpp:472`; `:1624`, `:1377`, `:1489-1492` |
| C29 | Under `FRESH_STATE`, state is neither loaded at startup nor saved after a pairing. | `src/nvhttp.cpp:1612-1616`, `:456-457` |
| C30 | `load_state` rebuilds the trusted client certificate chain. | `src/nvhttp.cpp:366`, `:431` |
| C31 | `proc::refresh` replaces the whole `proc_t`; the web UI's app save and delete call it. | `src/process.cpp:801-807`; `src/confighttp.cpp:1169`, `:1245` |
| C32 | CSRF tokens live in a per-process map. | `src/confighttp.cpp:199-200` |
| C33 | `/applist` iterates the app list and `/appasset` searches it; `/serverinfo` calls `running()`, which reads the running app (`_app_id`, `placebo`, the process) and not the app list. | `src/nvhttp.cpp:1313`, `:1581`; `src/process.cpp:384-388`; `src/nvhttp.cpp:1264`; `src/process.cpp:287-318` |
| C34 | KWin grants its screencast interface by a `.desktop` file that names the current executable. | `src/platform/linux/kwingrab.cpp:43-50` |
| C35 | The base environment of app and prep commands is the process environment plus the `apps.json` `env` section, built when the apps are loaded. | `src/process.cpp:674-679`, `:787-788` |
| C36 | Linux capture backends are enabled from the `capture` setting. When it is empty, the first of NvFBC, wlr and KMS that verifies is enabled, X11 and portal are added whenever they verify, and kwin only if nothing else was enabled. | `src/platform/linux/misc.cpp:1376-1406` |
| C37 | A normal start publishes mDNS (`platf::publish::start()`) and maps UPnP ports (`upnp::start()`) on async tasks right after `http::init()`, before the server threads start, and starts the tray (`system_tray::init_tray()`) after them. | `src/main.cpp:476-484`, `:491-493`, `:503-517` |
| C38 | `load_state` returns nothing. A missing state file, or one without `root.uniqueid`, sets a newly generated `http::unique_id` and returns; a file that cannot be read logs an Error and returns, keeping the state it had. | `src/nvhttp.cpp:366-381`, `:383-388` |
