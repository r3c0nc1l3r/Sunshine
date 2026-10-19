/**
 * @file src/handoff/coordinator.h
 * @brief The old side's glue: the control handler, `sunshine_old_ops_t`, `pidfd_child_t`, the spawn request, the
 * cooldown, the post-handover wiring and the listener registry. The new side's peer-link wiring is
 * `post_commit_t` in `entry.h`.
 *
 * See `docs/drain-handoff.md`, steps 1 to 8, "Stopping, restarting and quitting" and "The control socket". Linux only:
 * include it only from Linux code. Everything here is glue. The order of the switch is in `old_side.h`, the refusal
 * rules in `refusal.h`, the process-wide state in `runtime.h`; this header says which Sunshine module each effect
 * calls, in which order, with which timeouts, and under which locks.
 *
 * ## Threads
 *
 * - The control server thread runs `coordinator_t::handle()` (one connection at a time). It blocks inside `handle()`
 *   until the helper's answer is known. It holds no coordinator lock while it waits.
 * - The run thread (one per attempt) runs `old_side_t::run()`, so every `old_side_ops_t` call and every
 *   `child_ops_t` call happens on it. After a handover the same thread does the post-handover wiring and ends.
 * - The shutdown watcher (one per attempt) polls `runtime().shutdown_wake_fd()` and its own stop eventfd. When the
 *   shutdown fd becomes readable it calls `old_side_t::request_shutdown()` once. It covers `SIGTERM`
 *   (`runtime().note_sigterm()`) and every non-signal shutdown (`runtime().request_shutdown()`), which write the same fd.
 * - After a handover the draining side has the channel reader's reader and worker threads (`drain_responder_t::handle`
 *   runs on the worker) and the revocation poller's thread. Their threads are owned by `coordinator_t` until `stop()`.
 *
 * ## Locks (what may re-enter what)
 *
 * - `coordinator_t` has one mutex. It guards the attempt state (claim, cooldown, pointers to the running attempt, the
 *   post-handover objects). It is never held across: a gatherer, `check_refusal`, `log_refusal`, a spawn, a channel send or
 *   receive, any `old_side_ops_t`/`child_ops_t` call, `old_side_t::run()`, `request_shutdown()`, a join, a wait for the
 *   helper's answer, or a call into another module. Code that needs the running `old_side_t` copies its `shared_ptr`
 *   under the mutex and calls it after releasing the mutex.
 * - Nothing in this header calls `check_refusal()`, `gather_refusal_inputs()` or any gatherer while holding the RTSP slots
 *   lock, the RTSP clear lock, the gate, `client_auth_mutex()` or a `proc` lock, and no callback of this header is
 *   called by Sunshine code that holds one of them. `old_side_ops_t::save_state()` calls
 *   `nvhttp::save_state_checked()`, which takes `client_auth_mutex()` itself, so it is called with no lock held.
 * - `control_server_t::close_listener()` and `reopen_listener()` are called from the run thread while the control
 *   thread is inside `handle()`; the control server's contract allows this.
 * - `drain_responder_t::handle` takes the clear lock and the slots lock in its `end_and_join` and `close_cert`
 *   operations, on its own handler threads. `revocation_poller_t`'s callback does the same on the poller thread.
 *   Neither is ever called by the coordinator with such a lock held. `stop()` of either is called with no
 *   Sunshine lock held.
 * - The active process's peer link (`peer_link_t`, its channel reader and pidfd watcher) is not part of this header:
 *   `post_commit_t` of `entry.h` is its single owner.
 *
 * ## Test seams
 *
 * Under `SUNSHINE_TESTS` every clock, spawn, channel creation, module call and thread-start of the handler is
 * injectable (`coordinator_env_t`, `old_ops_seams_t`, `child_seams_t`), so the tests need no real
 * process, systemd, display, GPU, Avahi, IGD or network, and never exit the test binary. The listener registry
 * (`listeners()`) is the seam of the four servers: tests register fakes there.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <vector>

// local includes
#include "channel.h"
#include "channel_reader.h"
#include "control.h"
#include "end_all.h"
#include "instance_file.h"
#include "old_side.h"
#include "peer_link.h"
#include "probe.h"
#include "protocol.h"
#include "refusal.h"
#include "responder.h"
#include "revocation_poll.h"
#include "runtime.h"
#include "spawn.h"
#include "src/upnp.h"

namespace handoff {
  /**
   * @brief Longest wait for one server's pause, resume or listener close (the in-flight budget of the spec).
   *
   * Every call into a server's `pause`, `resume` or `close_listener` made by this header passes this value, or a smaller
   * one, never zero, a negative value or "wait for ever". It equals `old_side_timeouts_t::in_flight` at the defaults.
   */
  inline constexpr std::chrono::milliseconds server_op_timeout {5000};

  /**
   * @brief Timeout of `sd::barrier` and of `sd::read_back_mainpid` in `sunshine_old_ops_t::handover()` (spec step 7).
   */
  inline constexpr std::chrono::milliseconds handover_step_timeout {5000};

  /**
   * @brief Timeout of the tray removal in `sunshine_old_ops_t::remove_tray()` (`system_tray::suspend`).
   */
  inline constexpr std::chrono::milliseconds tray_suspend_timeout {2000};

  /**
   * @brief Poll period of the pairing wait in `sunshine_old_ops_t::recheck()`.
   */
  inline constexpr std::chrono::milliseconds recheck_poll_period {100};

  /**
   * @brief The reasons of this header, in addition to `refusal_reason` (`refusal.h`). One line each, no newline. They are
   * the `reason` of a `refused`, `failed` or `kill_pending` answer, so tests and tooling can identify the path.
   */
  namespace coordinator_reason {
    /// Refused: a shutdown was requested (`SIGTERM`, Quit) or `coordinator_t::stop()` ran.
    inline constexpr std::string_view shutting_down = "the service is shutting down";
    /// Refused: the process is not in the active role (mode `disabled`, `new_pending` or `new_active`).
    inline constexpr std::string_view not_active = "this process is not the active process";
    /// Refused: the process is already draining.
    inline constexpr std::string_view draining = "a drain is in progress";
    /// Refused: a listener of the four is not available; the text is this prefix plus the name from `to_string()`.
    inline constexpr std::string_view listener_missing_prefix = "listener not available: ";
    /// Failed: the child could not be started; the text is this prefix plus the one-line reason of `spawn_child()`.
    inline constexpr std::string_view spawn_failed_prefix = "cannot start the new process: ";
    /// Failed: the channel could not be created.
    inline constexpr std::string_view channel_failed = "cannot create the handoff channel";
    /// Failed: the child's start time could not be read.
    inline constexpr std::string_view start_time_unreadable = "cannot read the start time of the new process";
    /// Failed: `run()` ended without an answer (a shutdown after `GO`, or a defect); the helper is answered once, here.
    inline constexpr std::string_view no_outcome = "the handoff ended without an outcome (shutdown)";
  }  // namespace coordinator_reason

  // ---------------------------------------------------------------------------------------------------------------
  // The four servers
  // ---------------------------------------------------------------------------------------------------------------

  /**
   * @brief The four listening servers whose sockets the new process inherits, in fd order (https 3, http 4, webui 5, rtsp 6).
   */
  enum class listener_e {
    https,  ///< Client HTTPS (`nvhttp`).
    http,  ///< Client HTTP (`nvhttp`).
    webui,  ///< Web UI (`confighttp`).
    rtsp,  ///< RTSP (`rtsp_stream`).
  };

  /**
   * @brief Name of a listener, as in `SUNSHINE_HANDOFF_FDS`: `https`, `http`, `webui`, `rtsp`.
   *
   * It names the server in logged and answered reasons ("the https server did not confirm the pause in time").
   *
   * @param kind Listener.
   * @return Name.
   */
  std::string_view to_string(listener_e kind);

  /**
   * @brief The operations the handoff needs from one server. Each member must be set when the server is registered.
   *
   * The production bindings: for the three Simple-Web-Server servers, the `adoptable_server_t` members `pause(timeout)`,
   * `resume(timeout)`, `close_listener(timeout)` and `listener_fd()` of the server object (`nvhttp.cpp` registers the
   * https and http servers, `confighttp.cpp` the web UI server); for RTSP `rtsp_stream::pause_accept(timeout)`,
   * `resume_accept(timeout)`, `close_listener(timeout)` and `rtsp_stream::listener_fd()` (see `rtsp_listener_ops()`).
   */
  struct listener_ops_t {
    std::function<bool(std::chrono::milliseconds)> pause;  ///< Pause accepting; `true` when confirmed in time.
    std::function<bool(std::chrono::milliseconds)> resume;  ///< Resume accepting; `true` when confirmed in time. Resuming a
                                                            ///< server that is not paused is a no-op that returns `true`.
    std::function<bool(std::chrono::milliseconds)> close_listener;  ///< Close this process's copy of the socket; `true`
                                                                    ///< when done in time.
    std::function<int()> fd;  ///< The listening fd, or -1 when the server has no listener (not started, or closed).
  };

  /**
   * @brief The RTSP server's operations, bound to the `rtsp_stream::` functions.
   *
   * @return Operations for `listeners().set(listener_e::rtsp, ...)`.
   */
  listener_ops_t rtsp_listener_ops();

  /**
   * @brief The process-wide set of the four servers' operations. Thread-safe.
   *
   * A server registers itself once its listener exists (`nvhttp::start()` for https and http, `confighttp::start()` for the
   * web UI, `main` for RTSP through `rtsp_listener_ops()`) and clears itself before it is stopped and destroyed. Only
   * Linux code registers (behind `#ifdef __linux__`); nothing changes for a process that never uses the feature.
   *
   * Call and clear exclude each other per listener: `clear(kind)` returns only after a running `pause`, `resume`,
   * `close_listener` or `fd` call on that listener has returned (at most one `server_op_timeout` later), and a call that
   * starts after `clear()` finds no listener. So a registered function may use the server object it captured. A
   * registered function must not call `set`, `clear` or any other member of the registry.
   */
  class listener_registry_t {
  public:
    /**
     * @brief Register or replace the operations of one listener. A replaced entry is cleared first (waiting as in `clear`).
     *
     * @param kind Listener.
     * @param ops Operations; every member must be set, else the call is ignored with an Error log.
     */
    void set(listener_e kind, listener_ops_t ops);

    /**
     * @brief Remove the operations of one listener; waits for a running call on it. Safe when nothing is registered.
     *
     * @param kind Listener.
     */
    void clear(listener_e kind);

    /**
     * @brief Whether a listener is registered.
     *
     * @param kind Listener.
     * @return `true` when registered.
     */
    bool registered(listener_e kind) const;

    /**
     * @brief Pause one listener.
     *
     * @param kind Listener.
     * @param timeout Longest wait; at most `server_op_timeout`. A larger value is cut to it; zero or less is cut to 1 ms.
     * @return The registered `pause` result; `false` when nothing is registered.
     */
    bool pause(listener_e kind, std::chrono::milliseconds timeout);

    /**
     * @brief Resume one listener; same timeout rule as `pause`.
     *
     * @param kind Listener.
     * @param timeout Longest wait.
     * @return The registered `resume` result; `false` when nothing is registered.
     */
    bool resume(listener_e kind, std::chrono::milliseconds timeout);

    /**
     * @brief Close this process's copy of one listener's socket; same timeout rule as `pause`.
     *
     * @param kind Listener.
     * @param timeout Longest wait.
     * @return The registered `close_listener` result; `true` when nothing is registered (nothing to close).
     */
    bool close_listener(listener_e kind, std::chrono::milliseconds timeout);

    /**
     * @brief The listening fd of one listener.
     *
     * @param kind Listener.
     * @return The fd, or -1 when nothing is registered or the server has no listener.
     */
    int fd(listener_e kind);

#ifdef SUNSHINE_TESTS
    /**
     * @brief Test-only: remove every entry. Not thread-safe: call it when no other thread uses the registry.
     */
    void reset_for_test();
#endif

  private:
    struct entry_t {
      listener_ops_t ops;  ///< Operations.
      bool present {false};  ///< Registered.
      std::mutex call_mutex;  ///< Held by a running call on this listener; `clear` takes it.
    };

    mutable std::mutex _mutex;  ///< Guards `present` and the replacement of `ops`.
    entry_t _entries[4];  ///< One entry per `listener_e`, in enum order.
  };

  /**
   * @brief The process-wide registry.
   *
   * @return The registry.
   */
  listener_registry_t &listeners();

  // ---------------------------------------------------------------------------------------------------------------
  // The child
  // ---------------------------------------------------------------------------------------------------------------

#ifdef SUNSHINE_TESTS
  /**
   * @brief Test-only replacements of the pidfd calls of `pidfd_child_t`. Empty members use the production function.
   */
  struct child_seams_t {
    std::function<reap_result_e(int, std::chrono::milliseconds, int *)> kill_and_reap;  ///< `handoff::kill_and_reap`.
    std::function<bool(int)> exited;  ///< `handoff::pidfd_exited`.
    std::function<std::optional<std::uint64_t>(pid_t)> start_time;  ///< `handoff::read_start_time`.
  };
#endif

  /**
   * @brief `child_ops_t` over the child's pidfd.
   *
   * Owns the pidfd (closed once, in the destructor). All members are called on the run thread; `exited()` and `pid()` are also
   * safe from any thread. The pid is for logs and for `MAINPID=`; every action goes through the pidfd.
   *
   * - `kill_and_reap(timeout)`: `handoff::kill_and_reap(pidfd, timeout, &status)`. Result `reaped` logs one Info line
   *   with the exit status; `reaped_elsewhere` logs one Info line "exit status unknown"; `timeout` logs nothing here (the
   *   old side logs the Error); `error` is returned as is. After a `reaped` or `reaped_elsewhere` result the object
   *   remembers that the child is gone: a later call returns `reaped_elsewhere` at once, without signalling and without
   *   touching the pidfd, so a second kill-first (a rollback after a shutdown) cannot act on a recycled process.
   * - `exited()`: `true` when remembered as gone, else `handoff::pidfd_exited(pidfd)`.
   * - `pid()`: the pid given at construction.
   * - `start_time()`: field 22 of `/proc/<pid>/stat`, read once at construction with `handoff::read_start_time(pid)`;
   *   `0` when it could not be read (the coordinator never builds a `pidfd_child_t` in that case: it fails the start,
   *   see `coordinator_reason::start_time_unreadable`).
   */
  class pidfd_child_t final: public child_ops_t {
  public:
    /**
     * @brief Take over a spawned child.
     *
     * @param child The spawned child; this object owns `child.pidfd` from now on.
     */
    explicit pidfd_child_t(child_t child);

    /**
     * @brief Close the pidfd. Does not kill the child: killing is `kill_and_reap`'s job.
     */
    ~pidfd_child_t() override;

    pidfd_child_t(const pidfd_child_t &) = delete;  ///< Not copyable.
    pidfd_child_t &operator=(const pidfd_child_t &) = delete;  ///< Not copyable.

    /**
     * @brief Kill-first through the pidfd.
     *
     * @param timeout One wait.
     * @return See the class comment.
     */
    reap_result_e kill_and_reap(std::chrono::milliseconds timeout) override;

    /**
     * @brief Whether the child has exited.
     *
     * @return See the class comment.
     */
    bool exited() override;

    /**
     * @brief The child's pid.
     *
     * @return Pid.
     */
    pid_t pid() override;

    /**
     * @brief The child's start time.
     *
     * @return Start time, or 0.
     */
    std::uint64_t start_time() override;

#ifdef SUNSHINE_TESTS
    /**
     * @brief Test-only constructor with replaced pidfd calls.
     *
     * @param child The spawned child (the object still closes `child.pidfd`; a test passes -1 or a real eventfd).
     * @param seams Replacements.
     */
    pidfd_child_t(child_t child, child_seams_t seams);
#endif

  private:
    child_t _child;  ///< Pid and pidfd.
    std::uint64_t _start_time {0};  ///< Read at construction.
    std::atomic<bool> _gone {false};  ///< Reaped (here or elsewhere).
#ifdef SUNSHINE_TESTS
    child_seams_t _seams;  ///< Test replacements.
#endif
  };

  // ---------------------------------------------------------------------------------------------------------------
  // The old side's operations
  // ---------------------------------------------------------------------------------------------------------------

#ifdef SUNSHINE_TESTS
  /**
   * @brief Test-only replacements of every module call of `sunshine_old_ops_t` that is not a listener (those use
   * `listeners()`), so a test needs no GPU, Avahi, systemd, tray or network. An empty member uses the production call
   * named in its comment.
   */
  struct old_ops_seams_t {
    std::function<probe_data_t()> probe;  ///< `gather_probe()`.
    std::function<int()> self_pidfd;  ///< `open_self_pidfd()`.
    refusal_sources_t sources;  ///< `production_refusal_sources()`; only `app_adoptable`, `pairing_count`,
                                ///< `pending_launch` and `now` are read by `recheck()`. Set all members or none (none
                                ///< means production).
    bool sources_set {false};  ///< Whether `sources` is used.
    std::function<bool()> drop_pending_launch;  ///< `rtsp_stream::drop_pending_launch()`.
    std::function<void(std::chrono::milliseconds)> sleep_for;  ///< `std::this_thread::sleep_for` (the pairing poll).
    std::function<bool(std::chrono::milliseconds)> wait_in_flight;  ///< `route_gate()` wait for zero (see the op).
    std::function<void(bool)> gate;  ///< `route_gate().raise()` for `true`, `lower()` for `false`.
    std::function<bool()> save_state;  ///< `nvhttp::save_state_checked()`.
    std::function<void(bool)> freeze;  ///< `nvhttp::set_state_frozen(v)` and `runtime().set_frozen(v)`.
    std::function<void(std::chrono::milliseconds)> mdns_withdraw;  ///< `mdns_holder().withdraw(t)`.
    std::function<bool()> mdns_start;  ///< `mdns_holder().start(false)`.
    std::function<bool(std::chrono::milliseconds)> tray_suspend;  ///< `system_tray::suspend(t)`.
    std::function<bool()> tray_resume;  ///< `system_tray::resume()`.
    std::function<protocol::go_t()> snapshot;  ///< `proc::proc.snapshot()`, `nvhttp::host_audio()` and
                                               ///< `nvhttp::next_launch_session_id()`.
    std::function<void(bool)> set_released;  ///< `proc::proc.set_released(v)`.
    std::function<void()> drop_app;  ///< `proc::proc.drop_released_app()` (the Warning is logged by the op).
    std::function<bool()> reload;  ///< The three reloads of `reload_after_rollback()`, see the op.
    std::function<void(upnp::unmap_scope_e)> set_unmap_scope;  ///< `upnp::set_unmap_scope`.
    std::function<void(std::set<std::uint16_t>)> remove_ports;  ///< `upnp::remove_ports_async`.
    std::function<std::string()> notify_socket;  ///< `getenv("NOTIFY_SOCKET")`, read at each call.
    std::function<std::optional<std::string>()> cgroup;  ///< The content of `/proc/self/cgroup`.
    std::function<bool(const std::string &)> notify;  ///< `sd::notify(<NOTIFY_SOCKET>, payload)`.
    std::function<bool(std::chrono::milliseconds)> barrier;  ///< `sd::barrier(notify_socket, t)`.
    std::function<std::optional<pid_t>(const std::string &, bool, std::chrono::milliseconds)> read_back;  ///<
                                                                                                          ///< `sd::read_back_mainpid(unit, user_unit, t)`.
    std::function<void(std::chrono::steady_clock::time_point)> drain_begin;  ///< `drain_controller().begin(now)`.
    std::function<void()> drain_cancel;  ///< `drain_controller().cancel()`.
    std::function<void()> session_poke;  ///< `session_notifier().poke()`.
  };
#endif

  /**
   * @brief What each `old_side_ops_t` operation does in production. Runs on the run thread, one call at a time.
   *
   * Nothing here throws to the caller: an exception from a module is caught, logged as an Error, and mapped to the
   * "failure" value of that operation (`false`, a non-empty reason, or nothing for `void` operations).
   *
   * Operations, in the order of the switch:
   *
   * - `hello()`: `HELLO` with the protocol version of `protocol.h`, no capabilities, `gather_probe()`'s results and
   *   identity (read when called; the old side always has probed), and `cooldown_deadline_ns` = the cooldown deadline
   *   given at construction as nanoseconds since the epoch of `std::chrono::steady_clock` (which is `CLOCK_MONOTONIC` on
   *   Linux, the clock the new process compares with). The deadline is always present.
   * - `self_pidfd()`: `open_self_pidfd()`; the old side closes it.
   * - `pause_accept()`: pauses the listeners one after another in the order https, http, webui, rtsp with
   *   `listeners().pause(kind, server_op_timeout)`. The first `false` stops the sequence and returns the reason
   *   `the <name> server did not confirm the pause in time` (the server named by `to_string(listener_e)`), logged as an
   *   Error; the listeners after it are not touched. Returns an empty string when all four confirmed. The old side
   *   treats a non-empty result as a failed switch: the rollback resumes every listener (`resume_accept()` is idempotent
   *   on a listener that was never paused) and the helper is answered `failed <reason>`. The worst wait of a failed
   *   pause is one `server_op_timeout`. `old_side_ops_t::pause_accept()` returns this reason string (`old_side.h`).
   * - `resume_accept()`: `listeners().resume(kind, server_op_timeout)` for all four, in the same order, never stopping at
   *   a failure. A `false` logs an Error naming the server ("the <name> server did not confirm the resume in time") and the
   *   loop goes on; nothing is thrown and the rollback continues. Worst wait: four timeouts, only when all four servers
   *   are unresponsive.
   * - `raise_gate()`: `route_gate().raise()`. `lower_gate()`: `route_gate().lower()`.
   * - `wait_in_flight(timeout)`: waits until `route_gate().in_flight()` is 0, polling at most every 10 ms; a non-negative
   *   `timeout` is the longest wait (`false` at the end); a negative one waits without limit and logs a Warning every 5 s
   *   ("waiting for N requests in flight"). It never swaps or reloads anything itself.
   * - `recheck(pairing_wait)`: in this order, (1) `proc::proc.adoptable(&why)`: not adoptable gives `refusal_reason::
   *   app_generic`, or `app_prefix` plus the sanitised `why` as `check_refusal` builds it; (2) while
   *   `nvhttp::pairing_session_count()` is above 0, wait in steps of `recheck_poll_period` until it is 0 or
   *   `pairing_wait` has passed (monotonic); still above 0 gives `refusal_reason::pairing`; (3) if
   *   `rtsp_stream::has_pending_launch()`, `rtsp_stream::drop_pending_launch()` and log an Info line (this is not a
   *   failure). The first failing step decides; an empty result lets the switch go on.
   * - `save_state()`: `nvhttp::save_state_checked()`; `false` fails the switch.
   * - `freeze()`: `nvhttp::set_state_frozen(true)`, `runtime().set_frozen(true)`, and the UPnP scope of this process is set
   *   to `upnp::unmap_scope_e::none` when the new base equals this process's own base (`hooks::udp_base()`), else
   *   `udp_only`, with `upnp::set_unmap_scope`. The scope is therefore set before `COMMIT` can be sent, so it holds on
   *   every later path in which this process exits (see the UPnP rule below). `unfreeze()`: the reverse
   *   (`set_state_frozen(false)`, `set_frozen(false)`) and the scope back to `all`. Whatever fails or is skipped in
   *   the rollback (for example `reload_after_rollback()` returning `false`), `unfreeze()` and `leave_drain_mode()` each
   *   restore `all` unconditionally (idempotent); and the failed-attempt cleanup of the run thread (below) sets `all`
   *   once more for the outcome `rolled_back` and for a failure before `COMMIT` (the process is the only active one), but not
   *   for `handed_over` and not for a `shutdown` after `COMMIT` (a new process may hold the shared mappings), so no
   *   rollback path leaves `udp_only` behind.
   * - `withdraw_mdns(timeout)`: `mdns_holder().withdraw(timeout)`. `publish_mdns()`: `mdns_holder().start(false)`; a `false`
   *   logs a Warning.
   * - `remove_tray()`: `system_tray::suspend(tray_suspend_timeout)`; `false` logs a Warning and the switch goes on.
   *   `restore_tray()`: `system_tray::resume()`; `false` logs a Warning.
   * - `take_snapshot()`: `go_t{proc::proc.snapshot(), nvhttp::host_audio(), nvhttp::next_launch_session_id()}`.
   * - `shutdown_requested()`: `runtime().shutdown_requested()`.
   * - `set_released(v)`: `proc::proc.set_released(v)`. `drop_app()`: `proc::proc.drop_released_app()`, and a Warning when it
   *   dropped an app.
   * - `reload_after_rollback()`: in this order, `nvhttp::reload_state()` (any status but success counts as failure),
   *   `http::reload_user_creds(config::sunshine.credentials_file)` (non-zero is a failure), then `proc::parse(
   *   config::stream.file_apps)` and, on a value, `proc::proc.swap_app_list(...)` with its list (never `proc::refresh`).
   *   A failure of one step is logged as an Error and the next steps still run; the result is `false` when any step
   *   failed. The 503 gate is up and the in-flight counters are zero when it runs (the old side guarantees it).
   * - `enter_drain_mode()`: (1) `runtime().transition(mode_e::normal, mode_e::draining)` (an Error log when it is refused;
   *   the steps go on); (2) `logging::set_process_tag(runtime().process_tag())`; (3) the control socket is closed with
   *   `control.close_listener()` (never `stop()`); (4) the UPnP scope is set again as in `freeze()` (idempotent);
   *   (5) `drain_controller().begin(now())` of `drain.h` (the deadline, `runtime().settings().deadline_s` seconds, is
   *   counted from this call, the switch), which starts the deadline timer and forbids the exit until
   *   `drain_controller().handed_over()`; then `session_notifier().poke()` so the instance file shows the draining state
   *   at once. `leave_drain_mode()`: (1) `drain_controller().cancel()` (joins the controller thread; it returns `true`
   *   because the exit cannot have started before the handover); (2) `runtime().transition(mode_e::draining, mode_e::normal)`;
   *   (3) the process tag is cleared; (4) the UPnP scope is `all`; (5) `control.reopen_listener()`; a failure is an Error
   *   log, never a failure of the rollback.
   * - `set_role(r)`: `runtime().instance_set_role(r)`. `set_state(s)`: `runtime().instance_set_state(s)`, and, when the
   *   instance file's role is `draining` (the old side calls `set_role(draining)` first), a best-effort
   *   `HANDOFF_STATE <name>` on the channel (`runtime().channel()->send`; a failed send is a Debug line).
   * - `remove_new_instance_file()`: `remove_instance_file(<instances dir>, <child pid>)`.
   * - `start_upnp_cleanup(new_base)`: called by the old side only in the rollback, after `COMMIT` was sent and after the
   *   killed new process was reaped (spec, "Other resources", UPnP row). This OLD process (the only active one again)
   *   calls `upnp::remove_ports_async(handoff::upnp_ports_to_remove(new_base, own_base, config::sunshine.port))`, which
   *   runs the removal on a background task. It returns at once and never delays accept, mDNS or the helper's answer.
   *   It is skipped (a Debug line) when UPnP is off (`config::sunshine.flags[config::flag::UPNP]` unset) or when the set
   *   is empty. The active process never calls `remove_ports_async` after `MAIN`, and the new process never calls it.
   * - `handover(new_pid)`: reads `NOTIFY_SOCKET` now (the process environment is never scrubbed of it), and
   *   `sd::notify(socket, sd::mainpid_payload(new_pid))`; then `sd::barrier(socket, handover_step_timeout)`; then
   *   `sd::read_back_mainpid(unit, sd::is_user_unit(cgroup), handover_step_timeout)` with the unit from
   *   `sd::unit_from_cgroup` over `/proc/self/cgroup`. `true` only when all three succeed and the read-back equals
   *   `new_pid`; a failure logs an Error naming the step.
   * - `close_socket_copies()`: `listeners().close_listener(kind, server_op_timeout)` for all four; a `false` logs a Warning
   *   naming the server.
   *
   * UPnP rule (binding). This process never removes a mapping it shares with the active process. From `freeze()` on, its
   * scope is `udp_only` (or `none` when the bases are equal), so its guard never removes the shared TCP mappings whenever
   * and however it exits (a drain exit, a `SIGTERM` after `GO`, a shutdown in the window). The scope returns to `all` only
   * in `unfreeze()` and `leave_drain_mode()` (the rollback, where this process is the only active one again). The new
   * process's own scope is the new side's business (`new_ops.h`): it sets `udp_only` when it starts UPnP at `COMMIT`, so
   * that its graceful exit between `COMMIT` and `MAIN` (the rollback path where it exits after a `SIGTERM`) never removes
   * the shared TCP mappings either, and `all` at `MAIN`.
   */
  class sunshine_old_ops_t final: public old_side_ops_t {
  public:
    /**
     * @brief Create the operations for one attempt.
     *
     * @param control The control server (for `close_listener()` and `reopen_listener()`).
     * @param new_base The child's UDP base.
     * @param child_pid The child's pid (for `remove_new_instance_file()`).
     * @param cooldown_until The cooldown deadline of this attempt (for `hello()`).
     * @param instances_dir The instances directory.
     */
    sunshine_old_ops_t(control_server_t &control, int new_base, pid_t child_pid, std::chrono::steady_clock::time_point cooldown_until, std::filesystem::path instances_dir);

    protocol::hello_t hello() override;  ///< See the class comment.
    int self_pidfd() override;  ///< See the class comment.
    std::string pause_accept() override;  ///< See the class comment; empty when all four confirmed.
    void resume_accept() override;  ///< See the class comment.
    void raise_gate() override;  ///< See the class comment.
    void lower_gate() override;  ///< See the class comment.
    bool wait_in_flight(std::chrono::milliseconds timeout) override;  ///< See the class comment.
    std::string recheck(std::chrono::milliseconds pairing_wait) override;  ///< See the class comment.
    bool save_state() override;  ///< See the class comment.
    void freeze() override;  ///< See the class comment.
    void unfreeze() override;  ///< See the class comment.
    void withdraw_mdns(std::chrono::milliseconds timeout) override;  ///< See the class comment.
    void publish_mdns() override;  ///< See the class comment.
    void remove_tray() override;  ///< See the class comment.
    void restore_tray() override;  ///< See the class comment.
    protocol::go_t take_snapshot() override;  ///< See the class comment.
    bool shutdown_requested() override;  ///< See the class comment.
    void set_released(bool released) override;  ///< See the class comment.
    void drop_app() override;  ///< See the class comment.
    bool reload_after_rollback() override;  ///< See the class comment.
    void enter_drain_mode() override;  ///< See the class comment.
    void leave_drain_mode() override;  ///< See the class comment.
    void set_role(role_e role) override;  ///< See the class comment.
    void set_state(state_e state) override;  ///< See the class comment.
    void remove_new_instance_file() override;  ///< See the class comment.
    void start_upnp_cleanup(int new_base) override;  ///< See the class comment.
    bool handover(pid_t new_pid) override;  ///< See the class comment.
    void close_socket_copies() override;  ///< See the class comment.

#ifdef SUNSHINE_TESTS
    /**
     * @brief Test-only constructor with replaced module calls.
     *
     * @param control The control server.
     * @param new_base The child's UDP base.
     * @param child_pid The child's pid.
     * @param cooldown_until The cooldown deadline of this attempt.
     * @param instances_dir The instances directory.
     * @param own_base This process's own UDP base (replaces `hooks::udp_base()`).
     * @param seams Replacements; an empty member uses the production call.
     */
    sunshine_old_ops_t(control_server_t &control, int new_base, pid_t child_pid, std::chrono::steady_clock::time_point cooldown_until, std::filesystem::path instances_dir, int own_base, old_ops_seams_t seams);
#endif

  private:
    /**
     * @brief The UPnP scope of this process while it shares the active role's mappings.
     *
     * @return `none` when `_new_base == _own_base`, else `udp_only`.
     */
    upnp::unmap_scope_e drain_scope() const;

    control_server_t &_control;  ///< Control server.
    int _new_base;  ///< The child's UDP base.
    int _own_base;  ///< This process's UDP base.
    pid_t _child_pid;  ///< The child's pid.
    std::chrono::steady_clock::time_point _cooldown_until;  ///< Cooldown deadline of the attempt.
    std::filesystem::path _instances_dir;  ///< Instances directory.
#ifdef SUNSHINE_TESTS
    old_ops_seams_t _seams;  ///< Test replacements.
#endif
  };

  // ---------------------------------------------------------------------------------------------------------------
  // The coordinator
  // ---------------------------------------------------------------------------------------------------------------

#ifdef SUNSHINE_TESTS
  /**
   * @brief Test-only environment of `coordinator_t`. Every member is optional; an empty one uses the production value
   * named in its comment.
   */
  struct coordinator_env_t {
    runtime_t *runtime {nullptr};  ///< `&handoff::runtime()`. A test passes its own `runtime_t`.
    refusal_sources_t sources;  ///< `production_refusal_sources()`. Set all members or none.
    bool sources_set {false};  ///< Whether `sources` is used.
    std::function<std::chrono::steady_clock::time_point()> now;  ///< `std::chrono::steady_clock::now()` (the cooldown
                                                                 ///< clock; the refusal sources have their own `now`).
    std::function<std::optional<std::pair<int, int>>()> make_channel_pair;  ///< `handoff::make_channel_pair()`.
    std::function<std::optional<child_t>(const spawn_request_t &, std::string *)> spawn;  ///< `handoff::spawn_child`.
    std::function<std::unique_ptr<child_ops_t>(child_t)> make_child;  ///< `std::make_unique<pidfd_child_t>`.
    std::function<std::optional<std::uint64_t>(pid_t)> start_time;  ///< `read_start_time`, used to check the child's start
                                                                    ///< time before the old side starts.
    std::function<std::unique_ptr<old_side_ops_t>(pid_t child_pid, int new_base, std::chrono::steady_clock::time_point cooldown_until)> make_ops;  ///< `std::make_unique<sunshine_old_ops_t>`.
    std::function<void(old_side_timeouts_t &)> adjust_timeouts;  ///< Applied to the timeouts built from the settings.
    std::function<std::string()> host_sink;  ///< The recorded host sink (see `coordinator_t`).
    std::function<std::vector<std::string>()> argv;  ///< `lifetime::get_argv()` as a vector.
    std::filesystem::path instances_dir;  ///< `<runtime dir>/instances`; empty means no instance file is touched.
    std::function<std::unique_ptr<drain_responder_t>(channel_t &, end_all_machine_t &)> make_responder;  ///<
                                                                                                         ///< `std::make_unique<drain_responder_t>` (production operations).
    std::function<std::unique_ptr<revocation_poller_t>()> make_poller;  ///< `std::make_unique<revocation_poller_t>`.
    std::function<void()> drain_handed_over;  ///< `drain_controller().handed_over()`.
    std::function<void(upnp::unmap_scope_e)> set_unmap_scope;  ///< `upnp::set_unmap_scope`.
  };
#endif

  /**
   * @brief The old side's coordinator: owns the control server's request handler, one attempt at a time, and what runs
   * after a handover.
   *
   * ## `handle()`: the verb, step by step
   *
   * Called by the control server with the peer pid (0 when unknown). It returns the reply the server writes. It performs
   * these steps in this order and stops at the first that ends the request:
   *
   * 1. **Kill pending, then claim.** First, before the claim is looked at: when the instance state of this process is
   *    `kill_pending` (`runtime().instance_current()->handoff`; a rollback is waiting for the new process to exit and still
   *    holds the claim), the answer is `refused` `refusal_reason::kill_pending` (never `in_progress`), logged as a
   *    Warning with the peer pid. It is never retried by the coordinator and nothing clears it automatically; the state
   *    ends only when the rollback's kill completes (the pending `SIGKILL` takes effect), or by the operator path of the
   *    spec's runbook "Operator runbook: `kill_pending`": in an idle window `systemctl stop` the unit and then
   *    `systemctl start` it (a process in `D` state stays until the kernel releases it). Then the claim: an atomic claim of "an attempt is running" is taken. If it is already taken (another `handle()` call
   *    overlaps this one, or an attempt's run thread has not finished, or the process has handed over), the answer is
   *    `refused` with `refusal_reason::in_progress` (`coordinator_reason::draining` after a handover) at once. No
   *    gatherer is called, the cooldown is not touched, and the Warning of `log_refusal()` is written. The claim is
   *    released again on every path that ends without an attempt.
   * 2. **Shutdown.** If `runtime().shutdown_requested()` or `stop()` has begun: `refused` with
   *    `coordinator_reason::shutting_down`.
   * 3. **Mode.** `runtime().mode()` must be `normal`: `draining` gives `refused` `coordinator_reason::draining`; `new_pending`
   *    and `new_active` give `refused` `refusal_reason::in_progress`; `disabled` gives `refused`
   *    `coordinator_reason::not_active`.
   * 4. **Refusals.** `gather_refusal_inputs(sources, cooldown_until())` is called once, then `check_refusal()` once on its
   *    result, both with no coordinator lock held. Every source is read before `check_refusal()`; nothing is gathered
   *    after it. When the result is not empty: `log_refusal(reason, peer_pid)` (a Warning with the peer pid), release the
   *    claim and answer `refused <reason>` once. A `kill_pending` refusal is answered with its own reason
   *    (`refusal_reason::kill_pending`), which `check_refusal` ranks first. The coordinator never retries a refusal, never
   *    starts or resets the cooldown because of one, and never clears `kill_pending`: only the old side's rollback ends
   *    that state, after the reap.
   * 5. **Listeners.** The fds of `listeners().fd(https|http|webui|rtsp)` are read; a missing one (-1) gives `refused`
   *    `coordinator_reason::listener_missing_prefix` plus the name, with a Warning. Nothing has started yet.
   * 6. **Cooldown armed.** `cooldown_until = now() + settings().cooldown_s` seconds (see below). From here the attempt
   *    counts, whatever its outcome.
   * 7. **Attempt state.** `runtime().set_handoff_in_progress(true)`; `end_all.reset()` (the previous attempt's responder
   *    was stopped before its claim was released); `make_channel_pair()` (a failure gives `failed`
   *    `coordinator_reason::channel_failed`, see "Failed attempts" below); the old process's end is wrapped in a
   *    `channel_t` and stored with `runtime().set_channel()`.
   * 8. **Spawn.** `spawn_request_t{binary, argv, extra_env, fds}`: `binary` is `settings().binary`, else `/proc/self/exe`;
   *    `argv` is `lifetime::get_argv()` unchanged; `extra_env` is `child_environment(other_udp_base(config::sunshine.port,
   *    settings().udp_offset, hooks::udp_base()), runtime().generation() + 1)` followed, when the recorded host sink is
   *    not empty and does not start with `sink-sunshine-`, by `SUNSHINE_HANDOFF_HOST_SINK=<name>`; `fds` are the four
   *    listener fds and the child's end of the channel. The parent's copy of the child's channel end is closed after the
   *    spawn, success or not. A spawn failure gives `failed` `coordinator_reason::spawn_failed_prefix` plus the reason.
   *    The environment never contains `NOTIFY_SOCKET` changes: it is inherited as is.
   * 9. **Child.** The child's start time is read (`read_start_time(pid)`); unreadable gives a kill-first
   *    (`kill_and_reap` once, 5 s) and `failed` `coordinator_reason::start_time_unreadable`. Otherwise a `pidfd_child_t`
   *    takes the pidfd. If a shutdown was requested meanwhile, the child is killed first (`kill_and_reap`, one
   *    `reap` wait) and the answer is `failed` `coordinator_reason::shutting_down`.
   * 10. **Run.** `sunshine_old_ops_t` and `old_side_t` are constructed (timeouts: `ready` = `ready_timeout_s`,
   *    `rollback_window` = `rollback_window_s`, `pairing` = `config::stream.ping_timeout`, the rest the defaults of
   *    `old_side_timeouts_t`; `new_base` as above; `answer_helper` = the one-shot answer below). One `drain_responder_t` is
   *    constructed over the channel and the machine and `responder.handle` is the `on_drain_request` of `run()`, so a
   *    `END_ALL` that arrives in the rollback window (a new process shut down after `COMMIT`) is answered by the same
   *    code that serves the drain afterwards. The run thread and the shutdown watcher start. The control thread then
   *    waits for the answer.
   * 11. **Answer.** The reply is the first call of `answer_helper` (see "Exactly once"). `handle()` returns it.
   *
   * ## Exactly once
   *
   * The helper is answered exactly once per request: the first of (a) a refusal (steps 1 to 5), (b) a failure before the
   * run thread starts (steps 7 to 9), (c) the first call of the one-shot `answer_helper` (`committed` after `COMMIT`
   * was sent, or `failed` / `kill_pending` as `old_side_t` reports), or (d) the coordinator's own `failed`
   * `coordinator_reason::no_outcome` when `run()` returned without any answer. A later call of `answer_helper` is ignored
   * (a Debug line) and never reaches the socket. `committed` is never produced before `COMMIT` has been sent; the
   * coordinator adds no `committed` of its own, it passes `old_side_t`'s reply (child pid and start time from
   * `pidfd_child_t`) unchanged.
   *
   * ## A second request
   *
   * The control server serves one connection at a time, so a second helper waits in the backlog while `handle()` blocks.
   * It is then answered in order: if the first attempt has not finished its run thread (a rollback tail, the rollback
   * window), the second gets `refused` `refusal_reason::in_progress`; after the process drained, the control socket has
   * been closed and the queued connection fails at the helper (exit 5); after a rollback, the cooldown (and a drain
   * instance, and `kill_pending`) decide as in step 4. A second call of `handle()` that overlaps the first on another
   * thread (tests, or a future server) gets `refused` `refusal_reason::in_progress` at step 1 without waiting.
   *
   * ## Cooldown
   *
   * The length is `runtime().settings().cooldown_s` seconds. It is armed at step 6 as `now() + length`, so it covers the
   * attempt from its start (and `cooldown_s` is at least the rollback window), and goes into `HELLO` through
   * `sunshine_old_ops_t::hello()`, from where the new process inherits it (`set_cooldown_until()`). When an attempt ends
   * in `rolled_back`, or fails before or at the run, it is re-armed once to `max(current, now() + length)`, so that the
   * time after a failed attempt is also `length` (spec: "after a handoff (or a failed attempt)"). A `shutdown` outcome
   * does not re-arm. A refusal never arms it. The clock is `std::chrono::steady_clock`; tests inject `coordinator_env_t::now`.
   *
   * ## Failed attempts
   *
   * After any failure after step 7 the coordinator, in this order, with no lock held: stops the responder (it is discarded: a later attempt constructs a new one,
   * and `end_all.reset()` runs only after it was stopped) and the shutdown
   * watcher, closes the channel and `runtime().set_channel(nullptr)`, re-arms the cooldown, `runtime().
   * set_handoff_in_progress(false)`, sets the UPnP scope `all` where `freeze()` describes (rolled back or failed before `COMMIT`), releases the claim. The old side has already done the rollback effects
   * (`old_side_t::run()` does them before it returns). A failure before the run thread starts answers the helper
   * `failed <reason>` and logs an Error with the same reason.
   *
   * ## After `handed_over`
   *
   * `run()` returned `old_outcome_e::handed_over` (it sent `MAIN`, closed the socket copies and the process now drains).
   * The run thread then, in this order, with no lock held: (1) asserts the UPnP scope of `freeze()` again (`udp_only`, or
   * `none` when the bases are equal) with `upnp::set_unmap_scope`, so the scope holds before anything else can end this
   * process; (2) `runtime().set_handoff_in_progress(false)`; (3) stops the shutdown watcher; (4) starts a
   * `channel_reader_t(channel, answer_pings=false, responder.handle, on_close)` where `on_close` logs a Warning ("the
   * channel to the active process closed; this process keeps draining") and ends no session; (5)
   * `revocation_poller_t::start()` (`false` is an Error log; the drain goes on); (6) calls `drain_controller().handed_over()` of
   * `drain.h`, which allows the drain exit. The claim is never released: later `handle()` calls are refused with
   * `coordinator_reason::draining`. The reader, responder and poller live in the coordinator until `stop()`.
   *
   * ## Shutdown
   *
   * `request_shutdown()` (called by the shutdown watcher, or directly by `main`) is idempotent. By stage:
   * - Before the spawn (steps 1 to 8): `handle()` answers `refused` `coordinator_reason::shutting_down` (step 2) or, if
   *   the request arrived after step 2, kills the child first (step 9) and answers `failed`.
   * - During the switch (run thread started, before `GO`): the watcher calls `old_side_t::request_shutdown()`; the old side
   *   rolls back kill-first, answers the helper `failed` if it is still waiting, and `run()` returns `shutdown`.
   * - While a rollback is already running (a failure, a hung child or a lost channel started it, in any stage): the
   *   shutdown does not interrupt it. The rollback finishes, kill-first and reap included, with its effects, and only
   *   then does `run()` return (`old_side.h`: a shutdown request makes `run()` return `shutdown`, never in the middle of a
   *   rollback that has begun). `join_attempt()` bounds the wait.
   * - In the rollback window (after `GO`): `old_side_t::request_shutdown()` makes `run()` return `shutdown` promptly, with
   *   no handover and no rollback; the helper was already answered `committed` (or, if not, gets
   *   `coordinator_reason::no_outcome`). The run thread then only does the failed-attempt cleanup without re-arming. The
   *   process shuts down as a draining process would (`runtime().shutdown_reason()` is `stop`), and the unit's stop
   *   ends both processes.
   * - After the handover: nothing is running; the draining process ends its sessions with `stop` (`drain.h`), and the
   *   responder keeps answering `END_ALL` and the others until `stop()` (which `main`'s shutdown order calls, `entry.h`
   *   section 7, after the drain exit ended the sessions, released the devices and started `exit_graceful()`, and after the
   *   `proc` guard).
   * A refused terminate permission (`terminate.h`) skips `terminate()` only for an adopted app; this header changes
   * nothing there. `stop()` first marks the coordinator stopping (new `handle()` calls are refused), then forwards the
   * shutdown to a running attempt and joins its threads (normally already done by `join_attempt()`, which `main` calls
   * before the `proc` guard). The caller stops the control server (`stop_control()`) after `request_shutdown()` and before
   * `stop()`, as `control_server_t::stop()` requires.
   *
   * ## Recorded host sink
   *
   * `SUNSHINE_HANDOFF_HOST_SINK` carries the host's own default sink as this process recorded it. Production reads it with
   * `audio::recorded_host_sink()` (declared in `audio.h`), which returns the `sink_t::host` of the last stream
   * start, else `hooks::host_sink()` for a process that was itself started by a handoff, else `std::nullopt`; the
   * coordinator treats `std::nullopt` and an empty name alike (no variable).
   *
   * ## Thread-safety
   *
   * `handle()` is called by one thread at a time in production (the control server thread); an overlapping call is
   * answered as above. `request_shutdown()`, `stop()`, `set_cooldown_until()`, `cooldown_until()`, `handed_over()` and
   * `attempt_running()` are safe from any thread, including a signal-watcher thread but not a signal handler. `stop()`
   * must not be called from the run thread, the shutdown watcher or a callback of this header, and not twice
   * concurrently (the second call waits for the first).
   */
  class coordinator_t {
  public:
    /**
     * @brief Create the production coordinator.
     *
     * @param control The control server whose handler this object provides. It must outlive this object's use of it; stop
     *        the server (`control_server_t::stop()`) before destroying the coordinator.
     */
    explicit coordinator_t(control_server_t &control);

    /**
     * @brief Calls `stop()`.
     */
    ~coordinator_t();

    coordinator_t(const coordinator_t &) = delete;  ///< Not copyable.
    coordinator_t &operator=(const coordinator_t &) = delete;  ///< Not copyable.

    /**
     * @brief The handler to give to `control_server_t::start()`; it calls `handle()` and captures this object.
     *
     * @return Handler. Valid for the life of this object.
     */
    control_server_t::handler_t handler();

    /**
     * @brief Serve one `handoff` verb; see the class comment for the steps. Blocks until the helper's answer is known; the
     * wait has no timeout of its own (it is bounded by the old side's timeouts).
     *
     * @param peer_pid Pid of the connecting peer (`SO_PEERCRED`), or 0 when unknown.
     * @return The reply for the helper: `refused`, `failed`, `kill_pending` or `committed`.
     */
    control_reply_t handle(pid_t peer_pid);

    /**
     * @brief Start the control server on `path` with `handler()`. Both a normal start and the new side's `on_main()`
     * (`sunshine_new_ops_t`'s `start_control_server`) use it.
     *
     * Called only when the feature is enabled (`runtime().enabled()`), never in a handoff-started process before
     * `MAIN`. In a normal start `main` calls it after `open_instance_file()` and before `READY=1` (`entry.h`). The path is
     * `control_socket_path(runtime_dir, getpid())` (`control.h`), which is `$RUNTIME_DIRECTORY/control/<pid>.sock` when
     * `RUNTIME_DIRECTORY` is set (system unit, `RuntimeDirectory=sunshine`), else
     * `$XDG_RUNTIME_DIR/sunshine/control/<pid>.sock` (user unit). Idempotent: a second call while running returns `true`
     * and changes nothing. A failure to listen is logged as an Error and does not stop the process (the helper then
     * fails to connect).
     *
     * @param path Socket path.
     * @return `true` when the server is listening; `false` after a failure or after `request_shutdown()`/`stop()`.
     */
    bool start_control(const std::string &path);

    /**
     * @brief Stop the control server (`control_server_t::stop()`). Idempotent; safe without `start_control()`. Call it
     * after `request_shutdown()` (which makes a running handler return) and before `stop()`.
     */
    void stop_control();

    /**
     * @brief Forward a shutdown to a running attempt (`old_side_t::request_shutdown()`) and refuse new requests.
     * Idempotent; does not wait.
     */
    void request_shutdown();

    /**
     * @brief Wait for the current attempt's run thread to end, bounded.
     *
     * `main`'s shutdown calls it right after `request_shutdown()` and `stop_control()`, BEFORE the `proc` deinit guard
     * (`entry.h`, step 18), so no old-side operation (kill-first, rollback effects, the app take-back) can run
     * concurrently with that guard. Only the responder, the revocation poller, the channel reader and the channel
     * live past the guard (until `stop()`).
     *
     * @param timeout Longest wait; the default is 8 s: the kill-first reap (5 s) plus the rollback effects, below the 10 s
     *        limit that the `SIGTERM` handler armed.
     * @return `true` when no attempt was running or it ended in time. `false` on expiry: an Error is logged ("the
     *         handoff attempt did not finish within <n> s at shutdown") and the caller goes on (the 10 s limit still
     *         applies); the run thread is then joined by `stop()`.
     */
    bool join_attempt(std::chrono::milliseconds timeout = std::chrono::milliseconds {8000});

    /**
     * @brief Stop everything this object runs, in this order: refuse new requests, forward the shutdown, join the run thread
     * and the shutdown watcher, stop the revocation poller, stop the channel reader, stop the responder (waiting for its
     * handler threads, bounded by the session join guard of 10 s), close the channel. Idempotent.
     *
     * Must be called after the sessions of a draining process have been ended and joined (so that a pending `END_ALL` is
     * answered), and after `main`'s `proc` shutdown guard in an active process. The run thread was already joined by
     * `join_attempt()`; what lives past the guard is the responder, the poller, the reader and the channel.
     */
    void stop();

    /**
     * @brief The cooldown deadline.
     *
     * @return The deadline (`time_point::min()` when none was ever armed or inherited).
     */
    std::chrono::steady_clock::time_point cooldown_until() const;

    /**
     * @brief Raise the cooldown deadline to at least `deadline` (never lowers it). The new side calls it with the deadline
     * received in `HELLO` so that the active process inherits the cooldown.
     *
     * @param deadline Deadline on the `steady_clock`.
     */
    void set_cooldown_until(std::chrono::steady_clock::time_point deadline);

    /**
     * @brief Whether an attempt's run thread is running (the claim is held and the process has not handed over).
     *
     * @return `true` while an attempt runs.
     */
    bool attempt_running() const;

    /**
     * @brief Whether `old_side_t::run()` returned `handed_over` in this process.
     *
     * @return `true` after a handover.
     */
    bool handed_over() const;

#ifdef SUNSHINE_TESTS
    /**
     * @brief Test-only constructor.
     *
     * @param control The control server.
     * @param env Environment with the replacements.
     */
    coordinator_t(control_server_t &control, coordinator_env_t env);

    /**
     * @brief Test-only: the `END_ALL` machine of the current or last attempt, for assertions.
     *
     * @return The machine.
     */
    end_all_machine_t &end_all_for_test();
#endif

  private:
    /**
     * @brief The state of one attempt, shared by the handler, the run thread and the shutdown watcher.
     */
    struct attempt_t;

    control_server_t &_control;  ///< Control server.
    mutable std::mutex _mutex;  ///< Guards the members below; see "Locks".
    std::condition_variable _cv;  ///< Signals the end of an attempt and the helper's answer.
    std::chrono::steady_clock::time_point _cooldown_until {std::chrono::steady_clock::time_point::min()};  ///< Deadline.
    bool _claimed {false};  ///< An attempt holds the claim.
    bool _stopping {false};  ///< `request_shutdown()` or `stop()` ran.
    bool _handed_over {false};  ///< `handed_over` happened.
    bool _control_started {false};  ///< True once start_control() started the control server; guarded by _mutex.
    end_all_machine_t _end_all;  ///< Shared by `old_side_t` and the responder.
    std::shared_ptr<attempt_t> _attempt;  ///< The current or last attempt.
    std::unique_ptr<channel_reader_t> _reader;  ///< After a handover.
    std::unique_ptr<revocation_poller_t> _poller;  ///< After a handover.
    std::thread _run_thread;  ///< The run thread of the current or last attempt.
#ifdef SUNSHINE_TESTS
    coordinator_env_t _env;  ///< Test replacements.
#endif
  };
}  // namespace handoff
