/**
 * @file src/handoff/entry.h
 * @brief How `main()` starts, runs and stops a process with the drain handoff: the `--drain-handoff` helper dispatch,
 * the handoff-mode start, the `READY=1` latch, the startup order, the shutdown order, and the overlap rules.
 *
 * See `docs/drain-handoff.md`, steps 3 and 5, "systemd unit", "Stopping, restarting and quitting". Linux only; every
 * call site in `main.cpp` is inside `#ifdef __linux__`. This header is the contract the P18b implementer wires
 * `main.cpp` against; `main.cpp` itself is not edited by the contract. Section numbers below are the contract; the
 * implementer makes no design choice that is not written here.
 *
 * ## 1. `main()` order
 *
 * The anchors are the lines of `main()` at the contract base. "H" marks a step that runs only in a process started by a
 * handoff (`hooks::started_by_handoff()`); "N" marks a step that runs only in any other start. Unmarked steps run in
 * both. Nothing here changes what a process with the feature disabled does: with `runtime().enabled()` false every
 * call below that is not marked "always" does nothing, and the order of the existing statements is unchanged.
 *
 * | # | Where | What |
 * |---|-------|------|
 * | 1 | first statement of `main()`, before the privileged block | `if (const auto code = handoff::entry::maybe_run_helper(argc, argv)) return *code;` (section 2). Always. |
 * | 2 | after `privileged_execution` is computed, before `sanitize_process_environment` | `if (const auto reason = handoff::entry::early_start(privileged_execution); !reason.empty()) { std::cerr << reason << '\n'; return 1; }` (section 3). Always. |
 * | 3 | the privileged block, DRM worker, EGL worker, capability drops | unchanged. |
 * | 4 | `config::parse`, `logging::init`, the version banner, `cmd_to_func` | unchanged. `--drain-handoff` never reaches `config::parse` (step 1). The log is opened for appending in every start (`logging.h`). |
 * | 5 | right after `display_device::init` | `const auto refusal = handoff::entry::apply_startup_policy(privileged_execution);` (section 3). Keep `refusal` for step 14 H. |
 * | 6 | right after step 5, when `handoff::runtime().enabled()` | `handoff::entry::open_instance_file()` (role `pending` in H, `active` in N), then `handoff::session_notifier().start()`. |
 * | 7 | `task_pool.start(1)` and the signal handlers | In the SIGTERM lambda the first statement is `handoff::hooks::on_sigterm()` (before `force_shutdown` is armed and before `shutdown_event->raise`). The SIGINT lambda is unchanged. `main` does not scrub or unset `NOTIFY_SOCKET` anywhere (the refusal gatherer reads it at call time). |
 * | 8 | after the signal handlers | `handoff_process` (section 6) is constructed here, before `proc_deinit_guard` (destruction order, section 7). Starts the shutdown watcher. |
 * | 8b | when `runtime().enabled()`, right after step 8 (so `handoff_process` is declared first and destroyed last, section 7) and before any `READY=1` can be sent (step 15 on) | N only (an H process starts it in `on_main()`): construct `std::optional<handoff::coordinator_t> coordinator` and `control_server_t control` as section 7 orders them (both `std::optional`, emplaced only when the feature is enabled; every later `coordinator` call in this table is guarded by `has_value()`), then `handoff::set_new_ops_coordinator(&*coordinator)` (`new_ops.h`) and `coordinator->start_control(control_socket_path(runtime_dir, getpid()).string())`. The path is `$RUNTIME_DIRECTORY/control/<pid>.sock` when `RUNTIME_DIRECTORY` is set (system unit, `RuntimeDirectory=sunshine`), else `$XDG_RUNTIME_DIR/sunshine/control/<pid>.sock` (user unit). In H the same `coordinator_t` is constructed here, but `start_control` is called by `sunshine_new_ops_t`'s `start_control_server` in `on_main()`. |
 * | 9 | `proc::refresh`, `platf::init`, `proc::init`, `reed_solomon_init`, `input::init` | unchanged; declare `handoff::entry::device_release_t input_deinit_guard {input::init()};` (section 7). |
 * | 10 H | after the gamepad probe | `handoff_process.start_new_side(...)` (section 4): the new-side thread starts and exchanges `HELLO`. |
 * | 11 | the startup encoder probe | In H first `handoff_process.new_ops()->wait_probe_decision(<hello timeout> + 2 s)` (it returns early on a shutdown request; a timeout lets the probe run). Then, in both modes, the probe is wrapped: `if (!handoff::hooks::startup_probe_skipped()) { if (video::probe_encoders()) { ...the existing Error... } }`. So the accepted override is installed before `main` reaches this gate, or the probe runs. In N the `if` is the only change. |
 * | 12 | `http::init()` | unchanged; on failure in H call `handoff_process.new_ops()->servers_failed("HTTP interface failed to initialize")` before `return -1`. |
 * | 13 N | the `mDNS` and `upnp` asyncs | `mDNS = handoff::hooks::mdns_start(false)` replaces `platf::publish::start()`; the `upnp::start(hooks::udp_base())` call (already skipped when `started_by_handoff()`) stays. In H neither async publishes or maps (the `COMMIT` effects of `new_ops.h` do), and `main` takes no mDNS handle. |
 * | 14 H | between `http::init()` and starting the server threads | `switch (new_ops->wait_listeners_validated(<hello timeout> + <ready timeout>))`: `ok` continues; `failed` or `timeout` starts no server thread (they would throw on the inherited fds), skips to step 17 (the new side then calls `exit_failure()` itself; the normal shutdown path runs). |
 * | 15 | `shutdown_event->peek()` check, the three `std::jthread`s | unchanged calls. Before them: `rtsp_stream::set_listening_callback(arrive)`, `nvhttp::set_listening_callback(arrive)` and `confighttp::set_listening_callback(arrive)` (`arrive` is section 5; declared as in `rtsp.h`, `nvhttp.h`, `confighttp.h`); in H `rtsp_stream::adopt_listener(inherited().rtsp, true)`. Then `nvhttp::start()` and `confighttp::start()` as they are declared: in H each adopts the inherited fds paused itself (`inherited().https` and `.http` for nvhttp, `.webui` for confighttp) and registers with `handoff::listeners()`; in N `start()` binds as before and the listening callback only fires. `main` passes no fd and no callback to `start()`. |
 * | 15b | when `runtime().enabled()`, after the RTSP server has started (the `rtsp_stream::start`/adopt step) and after its listener exists | `handoff::listeners().set(handoff::listener_e::rtsp, handoff::rtsp_listener_ops())`; the nvhttp and confighttp `start()` register their own entries (their headers). Before the RTSP server stops (the `rtspThread.join()` of step 18) call `handoff::listeners().clear(handoff::listener_e::rtsp)`, as nvhttp and confighttp do for theirs before they stop. |
 * | 16 | `init_tray()` block | skipped in H (`!hooks::started_by_handoff()` added to the condition). Unchanged in N. |
 * | 17 | `mainThreadLoop` | On Linux it calls `system_tray::run_main_loop([&]{ return shutdown_event->peek(); })` whenever the tray is compiled in and enabled (`tray_is_enabled && config::sunshine.system_tray`), including in H, where no icon exists yet and the loop idles with its 500 ms poll until `request_start()` or shutdown. Without the tray it waits on `shutdown_event->view()` as before. |
 * | 18 | after `mainThreadLoop` returns | `handoff_process.finish_new_side();` then, when the feature is enabled, in this order and BEFORE the `proc` deinit guard (which runs at step 19): `coordinator->request_shutdown()`, `handoff::set_new_ops_coordinator(nullptr)`, `coordinator->stop_control()`, `coordinator->join_attempt()` (bounded, default 8 s; on expiry it logs an Error and `main` goes on; the 10 s forced-shutdown limit of the existing signal handlers still bounds the process, because both `SIGTERM` and Quit reach this point through a signal handler that arms it: Quit goes through `lifetime::exit_sunshine`, which raises `SIGINT`. Time spent before this step counts against that limit, and after an expiry here `coordinator_t::stop()` may wait for the run thread without its own bound, still under that limit); `handoff::listeners().clear(rtsp)`; then the three `join()`s, each preceded by `handoff::note_shutdown_stage(...)` (names in `drain.h`), then `task_pool.stop()` and `task_pool.join()` preceded by `note_shutdown_stage("task pool")`; then, after the joins and the task pool, just before the return, `upnp_unmap.reset()` and `mDNS.reset()` (after `sync_upnp` and `sync_mDNS` completed), which keeps upstream's teardown place before the `proc` guard. |
 * | 19 | `return lifetime::desired_exit_code` | Local destructors run in the order of section 7. |
 *
 * ## 2. The `--drain-handoff <MAINPID>` helper
 *
 * `maybe_run_helper(argc, argv)` runs when `argc >= 2` and `argv[1]` equals `--drain-handoff` exactly. It is the very
 * first statement of `main()`: before the privileged block, before any thread, before `runtime()` is constructed,
 * before the configuration is parsed and before `logging::init` (which would truncate or rotate the service's log
 * file; the helper never opens a log file and never logs). It builds `helper_env_t`:
 * `runtime_dir` from `runtime_dir(getenv("RUNTIME_DIRECTORY"), getenv("XDG_RUNTIME_DIR"))`, and `reply_timeout` from
 * `SUNSHINE_DRAIN_HANDOFF_REPLY_TIMEOUT` when it is a positive decimal integer of seconds (1 to 86400); an invalid
 * value is ignored and one line "ignoring invalid SUNSHINE_DRAIN_HANDOFF_REPLY_TIMEOUT" goes to stderr. The other
 * `helper_env_t` fields keep their defaults. With no runtime directory the helper does not call `run_helper`: it writes
 * "no runtime directory (RUNTIME_DIRECTORY and XDG_RUNTIME_DIR are not set)" and returns 5. Otherwise it calls
 * `run_helper(args, env, err)` with `args` = `argv[2...]`, writes `err` and one newline to stderr when `err` is not
 * empty, writes nothing to stdout, and returns `static_cast<int>(code)`. It reads no other environment variable and
 * leaves `NOTIFY_SOCKET` alone.
 *
 * ## 3. Early start and the startup policy
 *
 * `early_start(privileged_execution)` calls `runtime().early_init(system_environment())` (it must be the first use of
 * `runtime()`: the object is constructed here, before any thread and before the `SIGTERM` handler exists). A non-empty
 * result is returned. If `early_init` succeeded and `started_by_handoff()` is true and `privileged_execution` is true,
 * the result is "drain handoff start is refused with privileged execution" (the sanitising step would drop the
 * `SUNSHINE_HANDOFF_*` variables; `early_init` has already read and scrubbed them, so this is only a refusal to run on).
 *
 * `apply_startup_policy(privileged_execution)`:
 * 1. `runtime().configure(config::drain_handoff)`.
 * 2. `refusal = hooks::enable_refusal(<context>, privileged_execution)` where the context is capture =
 *    `config::video.capture`, `fresh_state` = the `FRESH_STATE` flag, `display_settings_manager` =
 *    `display_device::has_settings_manager()`, `runtime_dir_available` = `runtime_dir(...)` has a value.
 * 3. A non-empty `refusal`: log it as one Error, set `config::drain_handoff.enabled = false`, call `runtime().disable()`
 *    (it returns `false` in a handoff-started process, whose mode `disable()` does not change; the mode is `new_pending`
 *    and the new side reports the refusal, see `new_ops_config_t::startup_refusal`).
 * 4. Return `refusal` (empty when the feature may run, or is off).
 * It logs nothing for a disabled feature. In a handoff-started process `config::drain_handoff.enabled` false (from
 * `config_refusal` in `apply_config`, or step 3) also makes the new side fail with a reason (`validate_listeners()` in
 * `new_ops.h`).
 *
 * `open_instance_file()`: with the runtime enabled, `runtime().open_instance(<runtime dir>/instances, {pid =
 * getpid(), start_time = read_start_time(getpid()), role = pending (H) or active (N), udp_base =
 * runtime().effective_udp_base(config::sunshine.port), sessions = 0, handoff = none})`. Returns the result of
 * `open_instance` (`false` is logged as an Error and the process runs on; the files are advisory for tooling). Does
 * nothing and returns `false` when the runtime is disabled.
 *
 * ## 4. The new-side thread
 *
 * `handoff_process_t::start_new_side(timeouts)`, handoff-started process only: wraps `runtime().inherited()->channel`
 * in a `channel_t` (it takes the fd), builds `sunshine_new_ops_t` (`new_ops_config_t` from the configuration,
 * `startup_refusal` from step 5, the `upnp_unmap` variable of `main` as the slot), and starts one thread (the "new-side
 * thread") plus the shutdown watcher (section 6). The thread body is:
 * 1. `outcome = run_new_side(ops, channel, timeouts)` (`new_side.h`), with the timeouts of `new_side_timeouts_t`, the
 *    `go` timeout set to `ping_timeout + 25 s` and the `hello`/`commit` defaults (spec step 3).
 * 2. `ops.mark_finished()`.
 * 3. `exit_failure`: `ops.exit_failure()` (exits with status 1 through the seam); the thread ends.
 * 4. `committed`: `post_commit_t` is started (below); when `ops.commit_failed()` then `ops.exit_failure()`; the thread
 *    ends.
 * The thread is joined by `finish_new_side()`. It is the only thread that calls `run_new_side` or any
 * `new_side_ops_t` op. `run_new_side` is not run on the main thread: the tray is started by `request_start()`, which
 * the main loop executes (so no op needs the main thread), and the probe decision must be made before the main
 * thread's probe gate (step 11), which a main-thread `run_new_side` could not do.
 *
 * `post_commit_t` (the active side's view of the draining process, built after `committed`, held by
 * `handoff_process_t`; it is the single owner of the peer-link wiring, `coordinator.h` has none):
 * - `peer_link_t(channel, ops.take_old_pidfd())`, then `runtime().set_peer(link)`;
 * - a `channel_reader_t(channel, true, handler, on_close)`: `handler(received)` calls `ops.on_main()` for `MAIN`, and
 *   `link.deliver(message)` for every other message (`SESSIONS`, `CLOSED`, `CLOSED_CERT`, `ENDED`, `END_REJECTED`,
 *   `HANDOFF_STATE`; anything `deliver` does not consume is dropped), and in both cases closes an attached fd; `PING` is
 *   answered by the reader. `on_close(status)` calls `link.on_channel_closed()` (so `on_channel_closed` fires on channel EOF and, below, on pidfd readiness; replies go through `deliver`) and logs a Warning; it does nothing
 *   else: a lost channel ends no session on either side and starts no exit (the old process kills this one before
 *   `MAIN`; after `MAIN` the processes run on, spec "The handoff channel");
 * - a pidfd watcher thread: it polls a `dup` of the old pidfd and a stop eventfd; when the pidfd is readable it calls
 *   `link.on_channel_closed()` once (waiting callers fail promptly; `exists()`/`gone()` already read the pidfd) and ends;
 * - `stop()` stops the reader (so `deliver` and `on_channel_closed` never run on a destroyed link) and the watcher and
 *   joins them, then calls `runtime().set_peer(nullptr)` and releases the link. It does not close the channel (section 7).
 *   `handoff_process_t` calls it only after the `proc` guard has run, because the guard's terminate needs the link.
 * After `MAIN` the same reader keeps serving `deliver` and `PING`.
 *
 * ## 5. The `READY=1` latch
 *
 * `listening_latch_t(count, on_complete)` is a thread-safe countdown. `arrive()` counts one arrival; the call that
 * brings the count to zero calls `on_complete()` once, on the calling thread (a server's io thread), outside any lock;
 * later arrivals do nothing. The four listening callbacks (nvhttp HTTPS, nvhttp HTTP, confighttp, RTSP) all call
 * `arrive()`; `main` builds the latch with count 4.
 * - N: `on_complete` is `notify_ready_if_systemd()`: when `NOTIFY_SOCKET` is set (read at call time),
 *   `sd::notify(NOTIFY_SOCKET, "READY=1")`, whether or not the feature is enabled (spec "systemd unit"). Nothing else.
 * - H: the callbacks call `ops.server_listening()` instead of `arrive()`, and nothing sends `READY=1` from the latch: a
 *   handoff-started process sends nothing to systemd until `MAIN`, where `sunshine_new_ops_t::on_main()` sends the one
 *   `READY=1` (`new_ops.h`).
 * UPnP in H: the handoff-started process starts UPnP at `COMMIT` (`map_upnp()`) with `upnp::set_unmap_scope(udp_only)`
 * until `MAIN`, then `all` (`on_main()`). It never calls `upnp::remove_ports_async`; only the old process does, after a
 * rollback that killed it past `COMMIT` (`coordinator.h`).
 * A server that fails to listen never arrives, so `READY=1` is not sent (as upstream: a failed bind is fatal there).
 * The callbacks never block and never call a pause or resume function.
 *
 * ## 6. Shutdown, ownership and `handoff_process_t`
 *
 * `handoff_process_t` is the one object `main` holds for the process's handoff threads. It owns the shutdown watcher,
 * and in a handoff-started process the new-side thread, its ops and `post_commit_t`.
 * - Shutdown watcher: one thread that waits for `wait_shutdown()` (main passes `[&]{ shutdown_event->view(); }`; the
 *   `SIGTERM` and `SIGINT` handlers, Quit through `lifetime::exit_sunshine` and the drain exit all raise that event) and
 *   then calls `runtime().request_shutdown()` (idempotent; after a `SIGTERM` it changes nothing, since
 *   `hooks::on_sigterm()` already latched the reason) and, when a new side exists, `sunshine_new_ops_t::abort()`. This
 *   is how every shutdown path reaches the code that polls `runtime().shutdown_requested()` (the coordinator, the new
 *   side, the notifier, the drain controller), and what unblocks a new side that waits in `run_new_side` for the old
 *   process. Its destructor calls `raise_shutdown()` (`shutdown_event->raise(true)`, harmless at exit) and joins, so an
 *   early `return` from `main` cannot hang.
 * - `finish_new_side()` (called after `mainThreadLoop`): `ops.abort()` (a no-op once the stage is `committing` or
 *   later), join the new-side thread. After it no op runs. Idempotent. Without a new side it does nothing.
 *
 * ## 7. Destruction order of `main`
 *
 * Locals are destroyed in reverse declaration order, so declare, in this order, from first to last: the
 * `upnp_unmap` and `mDNS` variables (existing), `handoff_process`, then (step 8b) `std::optional<control_server_t> control` and `std::optional<coordinator_t> coordinator` (emplaced only when the feature is enabled),
 * `proc_deinit_guard` (existing), `input_deinit_guard` (existing). `upnp_unmap` and `mDNS` are declared first only so the new side can use the UPnP slot; step 18 resets both explicitly (`upnp_unmap` first, at the end of step 18, before the `proc` guard), so their teardown keeps upstream's place before the app terminate and the device and platform guards. Then the guards run as: input devices, then the `proc`
 * deinit guard, then the coordinator's destructor (`stop()`, then `control`), then `handoff_process`. `request_shutdown()`, `stop_control()` and
 * `join_attempt()` are NOT left to the destructor: they run at step 18, before the `proc` guard; only the responder, the
 * poller, the reader and the channel live past the guard (`coordinator.h` requires `stop()` after it in an active process).
 * - The `proc` guard's terminate (`terminate_app(shutdown_terminate_budget, ..., true)`, `terminate.h`) needs the peer
 *   link, the channel reader and the channel, so `handoff_process` (which owns `post_commit_t`) outlives it. A refused
 *   terminate skips `terminate()` only for an adopted app; an app this process launched is terminated as upstream. The
 *   channel is closed only after the guard has run.
 * - `~handoff_process_t` runs, in this order: `post_commit_t::stop()` (which clears `runtime().set_peer(nullptr)`),
 *   `runtime().set_channel(nullptr)`, `session_notifier().stop()`, `drain_controller().cancel()` (returns `false` and
 *   is harmless when the exit sequence is the thing that is shutting the process down), `mdns_holder().stop()` (in every
 *   process that may have published, also a draining one; idempotent), `runtime().instance_remove()`, the shutdown
 *   watcher. The coordinator's early teardown (`request_shutdown()`, `stop_control()`, the bounded `join_attempt()`) ran at step 18, before the
 *   `proc` guard; its late teardown (`stop()`: revocation poller, channel reader, responder, channel) is in `coordinator.h`; it runs after the `proc` guard
 *   and before `~handoff_process_t`. In a draining process the drain exit has already ended the sessions (`drain.h`) before
 *   `exit_graceful()` starts this order, so `stop()` finds no session to wait for.
 * - `device_release_t` (declared below) replaces the plain `input_deinit_guard` local on Linux: its constructor registers
 *   the release with `register_device_release()` (`drain.h`), so the drain exit may release the devices earlier. Its
 *   destructor releases them at upstream's place in the destruction order, and a later call of the registered closure
 *   does nothing.
 *
 * ## 8. Overlap rules (binding)
 *
 * - A second helper request while one handoff is in progress: the control server serves one connection at a time
 *   (`control.h`); the second waits in the backlog and is answered, if at all, after the first handler returned, by the
 *   coordinator's refusal rules (`in progress`, `a drain is in progress`, cooldown; see `coordinator.h`). A handoff-started
 *   process has no control socket before `MAIN`; `on_main()` starts it, so a helper cannot reach a process that is not yet
 *   the main process (the helper reads the draining process's instance file instead, spec "The control socket").
 * - `SIGTERM` (or any shutdown request) to the new process alone, by stage of `sunshine_new_ops_t::stage()`:
 *
 * | Stage | What happens | Old process | Status |
 * |---|---|---|---|
 * | `starting`, `ready` (before `READY` was sent, or waiting for `GO`) | The watcher calls `abort()`: the channel is half-closed, `run_new_side` returns `exit_failure`, `exit_failure()` exits with 1. No `END_ALL`, no `undo` (no app), nothing published, mapped or shown. | Sees the channel end or the process die and rolls back (kill-first), keeping its sessions and taking the app back. | 1 |
 * | `go` (an op of the `GO` handler may be running) | The running op finishes (they are short and do not wait for the old process); the next channel wait sees the half-closed channel and `run_new_side` returns `exit_failure`. Nothing is installed, so no `END_ALL` and no `undo`. | As above. | 1 |
 * | `committing` (`COMMIT` was read in time) | `abort()` is a no-op (the channel is needed). If the shutdown beat `install_adoption`'s mutex section, nothing is installed and `commit_failed()` is set (exit 1 after the wiring). Otherwise the `COMMIT` effects finish, `run_new_side` returns `committed`, `post_commit_t` starts, and the shutdown guard (section 7) terminates the adopted app: it first sends `END_ALL` (ends the old process's sessions), then `undo` runs at most once. | Sees the new process die before `MAIN` and rolls back with no app and no sessions; after the reap it removes the new base's UDP UPnP mappings on a background task (skipped when UPnP is off). | 0 for the normal shutdown, 1 for `commit_failed()` |
 * | `committed` (waiting for `MAIN`) | Normal shutdown as above. Quit and restart are refused while `new_active` (`hooks`); the tray entries are disabled. | As above. | 0 |
 * | `main` | A normal shutdown of the active process; the draining process, if still there, gets `SIGTERM` from systemd or is asked with `END_ALL` by the guard. | Draining process ends its sessions with `stop` (`drain.h`). | 0 |
 *
 * - Callbacks and locks, new side: the ops run on the new-side thread and take no lock across a seam; the milestone
 *   members are lock-free for the caller's purposes (one short internal mutex); the servers' listening callbacks run on
 *   the servers' io threads and only call `arrive()`/`server_listening()`; the channel reader's handler runs on its
 *   worker thread, takes no lock across `deliver` or `on_main()`, and never calls `abort()`, `stop()` or a pause/resume
 *   function. `post_commit_t::stop()` and `finish_new_side()` join threads: call them only from `main`'s thread (never
 *   from a handler, a callback or under the slots or clear lock).
 * - Drain side, see `drain.h` for the locks the notifier and the controller never take.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "channel.h"
#include "channel_reader.h"
#include "control.h"
#include "drain.h"
#include "new_ops.h"
#include "new_side.h"
#include "peer_link.h"
#include "runtime.h"
#include "settings.h"
#include "src/platform/common.h"

namespace handoff::entry {
  /**
   * @brief Owns the input devices' deinit guard for `main()` and lets the drain exit release the devices early.
   *
   * The constructor keeps the guard in shared state and calls `register_device_release()` (`drain.h`) with a closure
   * that holds that state by value, so the closure stays safe to call after this object is gone. `release()` destroys
   * the guard exactly once: a concurrent second caller blocks until the first has finished, and later calls do
   * nothing. The destructor calls `release()` (so `main()` releases the devices at upstream's place in its
   * destruction order) and then `register_device_release({})`; a copy of the closure that is still called afterwards
   * does nothing and touches no destroyed object. Thread-safe.
   */
  class device_release_t {
  public:
    /**
     * @brief Take the guard and register the release closure.
     *
     * @param guard The guard that `input::init()` returned; may be null (`release()` then only marks it released).
     */
    explicit device_release_t(std::unique_ptr<platf::deinit_t> guard);

    /**
     * @brief Release the devices (once) and register an empty release function.
     */
    ~device_release_t();

    device_release_t(const device_release_t &) = delete;
    device_release_t &operator=(const device_release_t &) = delete;

    /**
     * @brief Destroy the guard exactly once; a concurrent caller waits for the first to finish.
     */
    void release();

  private:
    struct state_t;
    std::shared_ptr<state_t> _state;  ///< Shared with the registered closure.
  };

  /**
   * @brief What the helper dispatch reads and calls; tests pass fakes.
   */
  struct cli_env_t {
    std::function<const char *(const char *)> getenv;  ///< Environment reader. Default: `::getenv`.

    /**
     * @brief `run_helper`. Default: `handoff::run_helper` (`control.h`).
     */
    std::function<helper_exit_e(const std::vector<std::string> &, const helper_env_t &, std::string &)> run;

    std::function<void(const std::string &)> write_err;  ///< Writes one line to stderr (the newline is added by the writer). Default: `std::cerr`.
  };

  /**
   * @brief The helper dispatch with injected environment: section 2 of the file comment, after `argv[1]` matched.
   *
   * @param args The arguments after `--drain-handoff`.
   * @param env Environment and effects.
   * @return The process exit code (`helper_exit_e` as an int; 5 for "no runtime directory").
   */
  int run_drain_handoff_cli(const std::vector<std::string> &args, const cli_env_t &env);

  /**
   * @brief Dispatch `--drain-handoff <MAINPID>`: the first statement of `main()`.
   *
   * @param argc `main`'s `argc`.
   * @param argv `main`'s `argv`.
   * @return The exit code when `argc >= 2` and `argv[1]` is exactly `--drain-handoff` (it runs the helper with the
   *         production `cli_env_t` and `args = argv[2...]`); `std::nullopt` otherwise, with no side effect.
   */
  std::optional<int> maybe_run_helper(int argc, char **argv);

  /**
   * @brief Read the inherited handoff environment (section 3).
   *
   * @param privileged_execution `main`'s `privileged_execution`.
   * @param env Environment to read; tests pass fakes. Default: the real one.
   * @return Empty on success; otherwise the one-line message `main` prints to stderr before it returns 1.
   */
  std::string early_start(bool privileged_execution, const early_init_env_t &env = system_environment());

  /**
   * @brief Apply the configured options and the host refusals (section 3, `apply_startup_policy`).
   *
   * @param privileged_execution `main`'s `privileged_execution`.
   * @return The refusal text that was logged and applied; empty when none.
   */
  std::string apply_startup_policy(bool privileged_execution);

  /**
   * @brief Write this process's first instance file (section 3, `open_instance_file`).
   *
   * @return `true` when written; `false` when the runtime is disabled or the write failed.
   */
  bool open_instance_file();

  /**
   * @brief Send `READY=1` to `NOTIFY_SOCKET` when it is set (read at call time).
   *
   * @return `true` when a datagram was sent.
   */
  bool notify_ready_if_systemd();

  /**
   * @brief Countdown latch for the four listening callbacks (section 5).
   */
  class listening_latch_t {
  public:
    /**
     * @brief Create a latch.
     *
     * @param count Number of arrivals to wait for; 0 calls nothing (use a positive count).
     * @param on_complete Called once when the count reaches zero, on the thread of the last arrival, outside any lock.
     */
    listening_latch_t(int count, std::function<void()> on_complete);

    /**
     * @brief One arrival. Thread-safe, never blocks. Arrivals beyond the count do nothing.
     */
    void arrive();

    /**
     * @brief Arrivals so far.
     *
     * @return The count of arrivals, capped at the construction count.
     */
    int arrived() const;

  private:
    std::function<void()> _on_complete;  ///< Completion action.
    std::atomic<int> _remaining;  ///< Arrivals still missing.
    std::atomic<int> _arrived {0};  ///< Arrivals counted.
    int _count;  ///< Construction count.
  };

#ifdef SUNSHINE_TESTS
  /**
   * @brief Test-only replacements for `post_commit_t`.
   */
  struct post_commit_seams_t {
    std::function<void(std::shared_ptr<peer_link_t>)> set_peer;  ///< `runtime().set_peer`; called with the link at start and with null at `stop()`. Null means production.
    std::function<int()> old_pidfd;  ///< The old process's pidfd (a pollable fake fd in tests; the object owns it). Null means `ops.take_old_pidfd()`.
  };
#endif

  /**
   * @brief The active side's view of the draining process, built after `COMMIT` (section 4).
   *
   * Held by `handoff_process_t`. Tests build it over a socketpair with a fake pidfd.
   */
  class post_commit_t {
  public:
    /**
     * @brief Build and start the link, the reader and the pidfd watcher.
     *
     * @param ops The new-side ops (`take_old_pidfd()` is called once, `on_main()` from the reader).
     * @param channel The process's end of the channel.
     */
    post_commit_t(sunshine_new_ops_t &ops, std::shared_ptr<channel_t> channel);

    /**
     * @brief Calls `stop()`.
     */
    ~post_commit_t();

    post_commit_t(const post_commit_t &) = delete;  ///< Not copyable.
    post_commit_t &operator=(const post_commit_t &) = delete;  ///< Not copyable.

    /**
     * @brief Stop and join the reader and the pidfd watcher, call `runtime().set_peer(nullptr)` (through the seam in
     * tests) and release the link. Idempotent. Does not close the channel.
     */
    void stop();

    /**
     * @brief The link.
     *
     * @return The peer link; valid for the life of this object.
     */
    peer_link_t &link();

#ifdef SUNSHINE_TESTS
    /**
     * @brief Test-only constructor with replaced runtime and pidfd.
     *
     * @param ops The new-side ops (`on_main()` is called from the reader).
     * @param channel The process's end of the channel (a test socketpair end).
     * @param seams Replacements.
     */
    post_commit_t(sunshine_new_ops_t &ops, std::shared_ptr<channel_t> channel, const post_commit_seams_t &seams);
#endif

  private:
    /**
     * @brief Build the link, the reader and the pidfd watcher; shared by both constructors.
     *
     * @param old_pidfd The old process's pidfd (owned by the link from here on), or -1.
     */
    void start(int old_pidfd);

    std::function<void(std::shared_ptr<peer_link_t>)> _set_peer;  ///< Stores or drops the peer (`runtime().set_peer` in production).
    sunshine_new_ops_t &_ops;  ///< Ops (for `on_main()`).
    std::shared_ptr<channel_t> _channel;  ///< The channel.
    std::shared_ptr<peer_link_t> _link;  ///< The link (also stored in the runtime).
    std::unique_ptr<channel_reader_t> _reader;  ///< The reader; answers `PING`.
    int _stop_fd {-1};  ///< Eventfd that stops the pidfd watcher.
    int _watch_pidfd {-1};  ///< `dup` of the old pidfd, polled by the watcher.
    std::thread _watcher;  ///< Pidfd watcher thread.
  };

  /**
   * @brief Seams of `handoff_process_t`; a null member takes its production default.
   */
  struct process_seams_t {
    /**
     * @brief Run the new side. Default: `handoff::run_new_side`.
     */
    std::function<new_outcome_e(new_side_ops_t &, channel_t &, const new_side_timeouts_t &)> run_new_side;

    /**
     * @brief Start the post-`COMMIT` wiring. Default: construct a `post_commit_t`.
     */
    std::function<std::unique_ptr<post_commit_t>(sunshine_new_ops_t &, std::shared_ptr<channel_t>)> make_post_commit;
  };

  /**
   * @brief The handoff threads and teardown of one process (sections 4, 6 and 7).
   */
  class handoff_process_t {
  public:
    /**
     * @brief Create the object and start the shutdown watcher.
     *
     * @param wait_shutdown Blocks until the shutdown event is raised (`[&]{ shutdown_event->view(); }`).
     * @param raise_shutdown Raises the shutdown event (`[&]{ shutdown_event->raise(true); }`).
     */
    handoff_process_t(std::function<void()> wait_shutdown, std::function<void()> raise_shutdown);

#ifdef SUNSHINE_TESTS
    /**
     * @brief As above with injected seams (tests only).
     *
     * @param wait_shutdown Blocks until shutdown.
     * @param raise_shutdown Raises shutdown.
     * @param seams Seams.
     */
    handoff_process_t(std::function<void()> wait_shutdown, std::function<void()> raise_shutdown, const process_seams_t &seams);
#endif

    /**
     * @brief Tear down in the order of section 7 (after `finish_new_side()` if it was not called).
     */
    ~handoff_process_t();

    handoff_process_t(const handoff_process_t &) = delete;  ///< Not copyable.
    handoff_process_t &operator=(const handoff_process_t &) = delete;  ///< Not copyable.

    /**
     * @brief Start the new side (handoff-started process only; section 4).
     *
     * Does nothing and returns `false` when `runtime().started_by_handoff()` is false, when `runtime().inherited()` has
     * no channel, or when it was called before.
     *
     * @param config Configuration facts for the ops.
     * @param upnp_slot `main`'s `upnp_unmap` variable.
     * @param timeouts Timeouts for `run_new_side`.
     * @return `true` when the thread started.
     */
    bool start_new_side(const new_ops_config_t &config, std::unique_ptr<platf::deinit_t> &upnp_slot, const new_side_timeouts_t &timeouts);

#ifdef SUNSHINE_TESTS
    /**
     * @brief As above with a given channel and no use of `runtime().inherited()` (tests only).
     *
     * @param config Configuration facts for the ops.
     * @param channel Channel (a test socketpair end).
     * @param upnp_slot Guard slot.
     * @param timeouts Timeouts.
     * @param seams Seams for the ops.
     * @return `true` when the thread started.
     */
    bool start_new_side_for_test(const new_ops_config_t &config, std::shared_ptr<channel_t> channel, std::unique_ptr<platf::deinit_t> &upnp_slot, const new_side_timeouts_t &timeouts, const new_ops_seams_t &seams);
#endif

    /**
     * @brief The ops of the new side.
     *
     * @return The ops, or null in a process that has no new side. Valid until the destructor.
     */
    sunshine_new_ops_t *new_ops();

    /**
     * @brief After the main loop returned: `abort()` the new side (a no-op from `committing` on) and join its thread.
     * Idempotent; does nothing without a new side.
     */
    void finish_new_side();

  private:
    /**
     * @brief Start the shutdown watcher thread; shared by both constructors.
     */
    void start_watcher();

    /**
     * @brief Build the ops and start the new-side thread; shared by `start_new_side` and its test variant.
     *
     * @param config Configuration facts for the ops.
     * @param channel The channel.
     * @param upnp_slot Guard slot.
     * @param timeouts Timeouts.
     * @param seams Seams for the ops, or null for the production ones (tests only).
     * @return `true` when the thread started.
     */
    bool launch_new_side(const new_ops_config_t &config, std::shared_ptr<channel_t> channel, std::unique_ptr<platf::deinit_t> &upnp_slot, const new_side_timeouts_t &timeouts, const new_ops_seams_t *seams);

    std::function<void()> _wait_shutdown;  ///< Waits for the shutdown event.
    std::function<void()> _raise_shutdown;  ///< Raises the shutdown event.
    process_seams_t _seams;  ///< Seams with defaults.
    std::shared_ptr<channel_t> _channel;  ///< The channel of a handoff-started process.
    std::unique_ptr<sunshine_new_ops_t> _ops;  ///< The ops.
    std::unique_ptr<post_commit_t> _post_commit;  ///< Post-`COMMIT` wiring.
    std::thread _new_side;  ///< The new-side thread.
    std::thread _watcher;  ///< The shutdown watcher.
    std::atomic<bool> _stopping {false};  ///< The destructor started.
    int _stop_fd {-1};  ///< Eventfd that stops the watcher.
    std::mutex _mutex;  ///< Serialises `start_new_side`, `finish_new_side` and the destructor.
  };
}  // namespace handoff::entry
