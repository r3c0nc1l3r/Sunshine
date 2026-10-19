/**
 * @file src/handoff/drain.h
 * @brief The draining process's life after the switch: the session notifier, the drain deadline, and the bounded exit.
 *
 * See `docs/drain-handoff.md`, "8. Drain", "Stopping, restarting and quitting", "Other resources" (session counts,
 * instance file, `HANDOFF_STATE`) and "Active-port discovery". Linux only.
 *
 * Two independent parts, each with its own thread, so a slow session join can never delay the instance file:
 * - `session_notifier_t` runs in every process that has an instance file (normal, new, draining). It keeps the
 *   `sessions` field of the instance file current, sends `HANDOFF_STATE` when this process drains, and keeps the
 *   `handoff` field of an active process that has a draining peer in step with the peer.
 * - `drain_controller_t` runs only while this process drains: the deadline timer and the exit.
 *
 * ## Entry points for the coordinator (`coordinator.h`)
 *
 * | Where | Call |
 * |---|---|
 * | `sunshine_old_ops_t::enter_drain_mode()` (right after `COMMIT`) | `drain_controller().begin(now)` |
 * | after `old_side_t::run()` returned `handed_over` | `drain_controller().handed_over()` |
 * | `sunshine_old_ops_t::leave_drain_mode()` (rollback), and the shutdown path | `drain_controller().cancel()` |
 * | `main()` after the instance file was opened | `session_notifier().start()` (see `entry.h`) |
 * | stream code on a session start or end (optional) | `session_notifier().poke()` through `hooks::sessions_changed()` |
 *
 * ## Locks and re-entry (binding)
 *
 * - The notifier thread never takes the RTSP slots lock, the clear lock, the retained-input lock or any lock of the
 *   channel besides `channel_t::send` (which is non-blocking). It reads the session count from an atomic
 *   (`stream::session::running_session_count()`, a lock-free accessor to the `running_sessions` counter) and never
 *   calls `rtsp_stream::session_count()` (it calls `clear(false)`) or anything that joins a session. It therefore
 *   cannot deadlock with a session join or with `clear()`, and a stuck join cannot stop the instance file from being
 *   updated.
 * - `poke()` is lock-free (an atomic store and one `write` to an eventfd) and may be called from any thread under any
 *   lock, including the slots lock, the clear lock and a stream's own lock.
 * - The controller's callbacks (`end_sessions`, `release_devices`, `arm_limit`, `exit_graceful`, `force_exit`,
 *   `log_error`) run on the controller thread (or, for the `arm_limit` expiry, on the task pool). `end_sessions` takes
 *   the clear lock and the slots lock and joins (bounded by the 10 s hang guard of a session join); no other callback
 *   takes them. No callback is ever called while the controller's own mutex is held, so a callback may call `begin`,
 *   `handed_over`, `phase()` or `poke()`; it must not call `cancel()` (it would wait for itself; `cancel()` called on the
 *   controller thread returns `false` at once).
 * - `cancel()` and `stop()` join threads: never call them under the slots lock or the clear lock, and not from a
 *   `session_notifier_t`/`drain_controller_t` callback.
 *
 * ## SIGTERM and stop reasons
 *
 * A `SIGTERM` to a draining process (alone, or with `systemctl stop`, which also signals the active process) takes the
 * normal shutdown path: `hooks::on_sigterm()` latches the reason `stop`, the handler arms its own 10 s limit and
 * raises the shutdown event, and the RTSP loop and the control loop end the sessions with `stop` (`rtsp_stream::clear`
 * and the broadcast in `stream.cpp` pass `to_stop_reason(hooks::shutdown_reason())`). The drain does not wait for the
 * deadline. The controller sees `runtime().shutdown_requested()` and does nothing more (it never runs its own exit
 * sequence after a shutdown was requested, and never arms a second limit); a `cancel()` from the shutdown path joins its
 * thread. The reason `drain_deadline` exists only for the deadline path below; `terminated` is never used by a drain
 * (a draining process never terminates apps, and `END_ALL` is a separate request that ends sessions with `terminated`,
 * see `responder.h`).
 *
 * ## In-place restart and Quit refusals (existing hooks, nothing new to declare)
 *
 * A draining process, and a new process before `MAIN`, ignore Quit and in-place restart. The refusals already exist as
 * `hooks::quit_blocked()` and `hooks::restart_blocked()` (`hooks.h` gives the full rules and texts). What this
 * contract adds is who must consult them:
 * - `system_tray::tray_quit_cb` and `tray_restart_cb` (exist): a non-empty text is logged as a Warning, nothing else
 *   happens (the tray entries are also disabled, see `system_tray::set_lifecycle_entries_enabled`);
 * - `confighttp::restart` (`POST /api/restart`, to be changed by P18c): after authentication and the CSRF check, and
 *   before `platf::restart()`, call `hooks::restart_blocked()`; a non-empty text is answered with HTTP 409 and the JSON
 *   body `{"status": false, "status_code": 409, "error": <text>}` (the headers of `bad_request`, with the status
 *   `client_error_conflict`), logs the text as a Warning, and does not restart. The text names the three ways out for
 *   the "draining peer exists" and "handoff in progress" cases, as `hooks.h` specifies. The web UI of a draining process
 *   does not exist (its listener is closed), so in practice the 409 is answered by the active process;
 * - `POST /api/restart` has no Quit counterpart in this tree; if one is added it must call `hooks::quit_blocked()` the
 *   same way (409).
 * Tests: with `runtime().reset_for_test()` plus `transition`/`set_peer`/`set_handoff_in_progress`, the hooks give the
 * text and the endpoint answers 409 with a positive marker (the text in the body) and `platf::restart` is not called;
 * with no blocker the request gets past the check (a seam observes `platf::restart` being reached).
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

// local includes
#include "instance_file.h"
#include "runtime.h"
#include "src/stream_liveness.h"

namespace handoff {
  /**
   * @brief Timing of the notifier; tests shorten it.
   */
  struct notifier_timing_t {
    std::chrono::milliseconds period {500};  ///< Longest time between two passes when nothing pokes the notifier.
  };

  /**
   * @brief The effects of the notifier. A null member takes its production default; tests pass fakes.
   *
   * Every function runs on the notifier thread and must not take the slots lock, the clear lock or block for long.
   */
  struct notifier_ops_t {
    /**
     * @brief Number of running sessions of this process. Default: `stream::session::running_session_count()` (the
     * lock-free accessor to `running_sessions`, to be added to `stream.h`).
     */
    std::function<int()> sessions;

    /**
     * @brief Write the session count to the instance file. Default: `runtime().instance_set_sessions(n)`.
     */
    std::function<void(int)> write_sessions;

    /**
     * @brief Current mode. Default: `runtime().mode()`.
     */
    std::function<mode_e()> mode;

    /**
     * @brief Handoff state this process has in its instance file. Default: `runtime().instance_current()->handoff`, or
     * `state_e::none` when there is no instance writer.
     */
    std::function<state_e()> current_state;

    /**
     * @brief Write the handoff state to the instance file. Default: `runtime().instance_set_state(state)`.
     */
    std::function<void(state_e)> write_state;

    /**
     * @brief Send `HANDOFF_STATE <state>` to the active process over the channel without blocking. Default: the
     * channel of `runtime().channel()` (`MSG_DONTWAIT`); a missing channel or a failed send is ignored (the active
     * process falls back to this process's instance file).
     */
    std::function<void(state_e)> send_state;

    /**
     * @brief The draining peer's state as seen from the active process, or `std::nullopt` when this process has no
     * draining peer. Default: when `runtime().peer()` is set, `peer->exists() ? peer->state() : state_e::none`;
     * otherwise `std::nullopt`.
     */
    std::function<std::optional<state_e>()> peer_state;

    /**
     * @brief Whether a shutdown was requested. Default: `runtime().shutdown_requested()`.
     */
    std::function<bool()> shutdown_requested;
  };

  /**
   * @brief Keeps the instance file's `sessions` and `handoff` fields current and sends `HANDOFF_STATE`.
   *
   * One pass, in this order, on every wake (a `poke()`, the end of `timing.period`, or a shutdown request):
   * 1. `n = ops.sessions()`. When `n` differs from the last value written (the first pass always writes),
   *    `ops.write_sessions(n)`.
   * 2. `mode = ops.mode()`. Draining: `state = ops.current_state()`; when `n` changed or `state` differs from the state
   *    last sent (nothing was sent yet counts as different), `ops.send_state(state)` and remember it. The draining
   *    process never mirrors a peer.
   * 3. Not draining: `peer = ops.peer_state()`. When it has a value that differs from `ops.current_state()`,
   *    `ops.write_state(*peer)`. With no peer (`std::nullopt`) nothing is written, so the state the old side set itself
   *    (`kill_pending`, `handover_failed` while it still is the active process) is never overwritten. A peer that
   *    vanished reads `state_e::none`, which is how the new process's file returns to `none` when the old process
   *    exits (spec table "old process exits").
   * No pass writes a role, never removes the file, and does nothing when there is no instance writer (the runtime's
   * `instance_*` functions already do that). After `runtime().instance_remove()` later passes write nothing.
   */
  class session_notifier_t {
  public:
    /**
     * @brief Construct with the production effects and the default timing. Nothing runs until `start()`.
     */
    session_notifier_t();

#ifdef SUNSHINE_TESTS
    /**
     * @brief Construct with injected effects and timing (tests only).
     *
     * @param ops Effects; a null member takes its production default.
     * @param timing Timing.
     */
    session_notifier_t(const notifier_ops_t &ops, const notifier_timing_t &timing);
#endif

    /**
     * @brief Calls `stop()`.
     */
    ~session_notifier_t();

    session_notifier_t(const session_notifier_t &) = delete;  ///< Not copyable.
    session_notifier_t &operator=(const session_notifier_t &) = delete;  ///< Not copyable.

    /**
     * @brief Start the thread (idempotent: a second call while it runs does nothing). The first pass runs at once.
     *
     * Call after `runtime().open_instance()` (a pass without a writer writes nothing, so an earlier start is harmless).
     * The thread waits with `poll` on its own eventfd and `runtime().shutdown_wake_fd()`; a readable shutdown fd makes
     * it do one last pass and end (it stays ended; the instance file is removed by the shutdown path).
     */
    void start();

    /**
     * @brief Ask for a pass now.
     *
     * Lock-free: one atomic store and one `write` to the eventfd, no allocation, no logging. Safe under any lock and
     * from any thread, also before `start()` (the pass happens when the thread starts) and after `stop()` (does
     * nothing). Many pokes before a pass cost one pass.
     */
    void poke();

    /**
     * @brief Stop and join the thread. Idempotent; safe without `start()`. Must not be called under the slots lock or
     * the clear lock, or from a notifier callback.
     */
    void stop();

    /**
     * @brief Number of passes completed since construction (tests wait for it to advance).
     *
     * @return The count; lock-free.
     */
    unsigned passes() const;

  private:
    notifier_ops_t _ops;  ///< Effects with defaults filled in.
    notifier_timing_t _timing;  ///< Timing.
    int _wake_fd {-1};  ///< Eventfd written by `poke()`.
    std::atomic<bool> _stop {false};  ///< `stop()` was called.
    std::atomic<unsigned> _passes {0};  ///< Completed passes.
    std::thread _thread;  ///< The notifier thread.
    std::mutex _start_mutex;  ///< Serialises `start()` and `stop()`; never taken by the thread.
  };

  /**
   * @brief Where the drain controller is. Only moves forward, except `cancelled`/`idle` as described.
   */
  enum class drain_phase_e {
    idle,  ///< `begin()` was not called (or `cancel()` ended a drain that had not started its exit).
    draining,  ///< `begin()` was called; the timer and the exit conditions are watched.
    exiting,  ///< The exit sequence started. `cancel()` no longer stops it.
    done,  ///< The exit sequence handed the process to `exit_graceful` (or the limit expired).
  };

  /**
   * @brief Timing of the controller; tests shorten it.
   */
  struct drain_timing_t {
    std::chrono::milliseconds tick {100};  ///< How often the controller thread re-reads the clock and the session count.
    std::chrono::milliseconds zero_grace {1000};  ///< The session count must read 0 continuously this long before the exit starts.
    std::chrono::milliseconds forced_shutdown {10000};  ///< The bounded-exit limit; the same 10 s as the signal handlers.
  };

  /**
   * @brief The effects of the controller. A null member takes its production default; tests pass fakes.
   */
  struct drain_ops_t {
    /**
     * @brief Monotonic clock. Default: `std::chrono::steady_clock::now`. A test sets a fake clock; the controller
     * thread re-reads it every `tick` and on every `poke()`/`handed_over()`/`begin()`.
     */
    std::function<std::chrono::steady_clock::time_point()> now;

    /**
     * @brief Number of running sessions. Default: `stream::session::running_session_count()` (lock-free).
     */
    std::function<int()> sessions;

    /**
     * @brief Whether a shutdown was requested. Default: `runtime().shutdown_requested()`.
     */
    std::function<bool()> shutdown_requested;

    /**
     * @brief End and join every session of this process with the given reason. Default:
     * `rtsp_stream::terminate_sessions(reason)`.
     *
     * Called on the controller thread, under no controller lock. Bounded by the session join guard (10 s per session
     * that does not stop), which is why the forced-shutdown limit is armed only after it returned.
     */
    std::function<void(stream::stop_reason_e)> end_sessions;

    /**
     * @brief Release the devices: destroy the virtual input devices (the `input::init()` guard) and everything
     * else this process holds that the active process must be able to take (capture and encoder sessions end with the
     * sessions). Default: the function registered with `register_device_release()`, or nothing when none was
     * registered. Must be idempotent and thread-safe, because `main()`'s own shutdown also runs it. The audio sinks,
     * the default sink, UPnP TCP mappings and mDNS are untouched (the active process owns them).
     */
    std::function<void()> release_devices;

    /**
     * @brief Make sure the UPnP unmap scope protects the shared mappings before the process can exit.
     *
     * Default: when `upnp::unmap_scope()` is still `all`, set it to `udp_only` (to `none` when the UDP base of this
     * process equals `other_udp_base(...)` of the configuration, that is, when both processes share one base), and log
     * a Warning "UPnP unmap scope was not set at the switch". It never changes a scope that is already `udp_only` or
     * `none`. The coordinator's `enter_drain_mode()` sets the scope at the switch; this is the second guard.
     */
    std::function<void()> ensure_unmap_scope;

    /**
     * @brief Arm the forced-shutdown limit: after `limit`, run `on_expiry` once on another thread. Default:
     * `task_pool.pushDelayed` (the mechanism of the signal handlers' 10 s task). Armed at most once per controller.
     */
    std::function<void(std::chrono::milliseconds limit, std::function<void()> on_expiry)> arm_limit;

    /**
     * @brief Start the normal shutdown: `lifetime::exit_sunshine(0, true)`. Returns at once. Tests must replace it.
     */
    std::function<void()> exit_graceful;

    /**
     * @brief Leave the process now, after the limit expired. Default: flush the log (`logging::log_flush()`) and
     * `_exit(1)`. It does not call `debug_trap` (the draining process holds nothing that needs a core dump, and the
     * handoff must not leave a crash report behind). Tests must replace it: it must never really exit.
     */
    std::function<void(int)> force_exit;

    /**
     * @brief Log one line at Error level. Default: `BOOST_LOG(error)`.
     */
    std::function<void(const std::string &)> log_error;
  };

  /**
   * @brief The drain deadline timer and the bounded exit of a draining process.
   *
   * ## What it watches
   *
   * `begin(switch_time)` records the switch (`COMMIT`) time and starts one thread (the controller thread). The
   * deadline is `switch_time + deadline_s` (`runtime().settings().deadline_s`, read at `begin()`), counted from the
   * switch, not from the handover. The thread, every `tick`, evaluates:
   * - `deadline_passed = ops.now() >= deadline`;
   * - `zero_stable`: `ops.sessions()` has read 0 on every tick for at least `zero_grace` (any non-zero reading restarts
   *   the grace; `zero_grace` covers a session that was still being set up when the count first read 0);
   * - `handed_over` (set by `handed_over()`).
   * It never starts the exit before `handed_over` is true, even past the deadline and with no sessions (spec: "never
   * exits before the handover in step 7 is done"). The deadline expiry is latched: when the deadline passes while the
   * main-PID handover is still failing (or before the rollback window ended), nothing happens yet, and the exit starts
   * at the first tick after `handed_over()` (so the sessions are not ended by a process that may still roll back and
   * become the active process again). The exit starts when `handed_over && (deadline_passed || zero_stable)`; when
   * both hold, the reason is `drain_deadline`.
   *
   * ## The exit sequence (binding, bounded)
   *
   * Runs once, on the controller thread, phase `exiting`. If `ops.shutdown_requested()` is true at the start, the
   * sequence does nothing and the controller ends (the signal path owns the exit and its own limit). Otherwise, in this
   * order:
   * 1. `ensure_unmap_scope()`.
   * 2. `end_sessions(reason)`: `drain_deadline` when the deadline passed, else `stop` (with zero sessions it only
   *    makes sure none remain and finishes the gamepad teardown). A session joins within its hang guard.
   * 3. `release_devices()`.
   * 4. Only now arm the limit: `arm_limit(forced_shutdown, on_limit_expired)`. Ending sessions and releasing devices
   *    are therefore outside the limit, and the limit covers exactly the part that can wait on something outside this
   *    process: the stop of the servers.
   * 5. `exit_graceful()` (the normal shutdown: servers stop and are joined, `main()` returns, guards run). Phase
   *    `done`.
   *
   * The limit's expiry (`on_limit_expired`, public so the task pool can call it, and so a test can) does, once:
   * `log_error("Drain exit did not finish within <n> s: <stage> did not finish")` where `<stage>` is the last name given
   * to `note_shutdown_stage()` (`unknown stop` when none), then `force_exit(1)`. The text is neutral: it names the
   * stop that did not finish and nothing else; it states no cause. `force_exit` ends the process, so nothing after it
   * runs. If the process already left (the normal shutdown completed) the task never fires.
   *
   * ## Overlap rules
   *
   * - `begin()` twice: the second call changes nothing (the first switch time wins).
   * - `handed_over()` before `begin()`: remembered; takes effect at `begin()`.
   * - `cancel()` while `phase()` is `draining`: stops the thread and returns it to `idle` (the rollback and the shutdown
   *   path). While `exiting` or `done`: returns `false` at once and stops nothing (the exit is committed).
   * - A rollback after `handed_over()` is impossible (the old side never rolls back after a successful read-back), so
   *   `handed_over()` followed by `cancel()` happens only at shutdown.
   * - The notifier is independent: a long `end_sessions` does not delay it.
   */
  class drain_controller_t {
  public:
    /**
     * @brief Construct with the production effects and the default timing. Nothing runs until `begin()`.
     */
    drain_controller_t();

#ifdef SUNSHINE_TESTS
    /**
     * @brief Construct with injected effects and timing (tests only).
     *
     * @param ops Effects; a null member takes its production default.
     * @param timing Timing.
     */
    drain_controller_t(const drain_ops_t &ops, const drain_timing_t &timing);
#endif

    /**
     * @brief Calls `cancel()`, then joins the thread if it is still running.
     */
    ~drain_controller_t();

    drain_controller_t(const drain_controller_t &) = delete;  ///< Not copyable.
    drain_controller_t &operator=(const drain_controller_t &) = delete;  ///< Not copyable.

    /**
     * @brief The switch happened: start the deadline timer and the exit watch.
     *
     * @param switch_time The monotonic time of `COMMIT` (the coordinator takes `now()` in `enter_drain_mode()`). The
     *        deadline is this plus `runtime().settings().deadline_s`.
     */
    void begin(std::chrono::steady_clock::time_point switch_time);

    /**
     * @brief The main-PID handover is done (`old_side_t::run()` returned `handed_over`): the process may now exit.
     * Wakes the thread.
     */
    void handed_over();

    /**
     * @brief Stop the controller before its exit started.
     *
     * @return `true` when the controller is stopped and its thread joined (also when it was never begun);
     *         `false` when the exit sequence has started (or finished), or when called from the controller thread. Does
     *         not wait for a running exit sequence.
     */
    bool cancel();

    /**
     * @brief The phase.
     *
     * @return The phase; lock-free.
     */
    drain_phase_e phase() const;

    /**
     * @brief The forced-shutdown limit expired. Public for the task pool and for tests; runs at most once (a second
     * call does nothing). See the class comment for what it does.
     */
    void on_limit_expired();

  private:
    drain_ops_t _ops;  ///< Effects with defaults filled in.
    drain_timing_t _timing;  ///< Timing.
    std::mutex _mutex;  ///< Guards the members below; never held across an `_ops` call.
    std::condition_variable _cv;  ///< Wakes the thread on `begin`, `handed_over`, `cancel`.
    std::thread _thread;  ///< The controller thread.
    std::chrono::steady_clock::time_point _deadline {};  ///< `switch_time + deadline_s`.
    std::atomic<drain_phase_e> _phase {drain_phase_e::idle};  ///< Phase.
    bool _handed_over {false};  ///< `handed_over()` was called.
    bool _cancel {false};  ///< `cancel()` asked the thread to end.
    bool _limit_armed {false};  ///< The limit was armed.
    std::atomic<bool> _expired {false};  ///< `on_limit_expired()` ran.
    std::thread::id _thread_id;  ///< Id of the controller thread, for the self-call check in `cancel()`.
  };

  /**
   * @brief The process-wide notifier (production effects).
   *
   * @return The notifier. Created on first use; `main()`'s shutdown path calls `stop()` (see `entry.h`).
   */
  session_notifier_t &session_notifier();

  /**
   * @brief The process-wide drain controller (production effects).
   *
   * @return The controller. Created on first use; `main()`'s shutdown path calls `cancel()` (see `entry.h`).
   */
  drain_controller_t &drain_controller();

  /**
   * @brief Register the function that releases this process's devices (`main()` passes a closure that resets the
   * `input::init()` guard).
   *
   * Called once from `main()` before any thread that may drain starts. The function must be idempotent and thread-safe:
   * the drain exit calls it from the controller thread and the normal shutdown destroys the same guard later, so the
   * production closure holds the guard in a `std::unique_ptr` protected by a `std::once_flag`/mutex and resets it at
   * most once. A second registration replaces the first.
   *
   * @param release The function; may be empty (nothing to release).
   */
  void register_device_release(std::function<void()> release);

  /**
   * @brief Record the shutdown step that is about to run, for the forced-shutdown message.
   *
   * `main()`'s shutdown path calls it before each step that can wait on something outside the process, with these
   * names (the strings are the contract): `"client server thread (nvhttp)"` before `httpThread.join()`,
   * `"web UI server thread (confighttp)"` before `configThread.join()`, `"RTSP server thread"` before
   * `rtspThread.join()`, `"task pool"` before `task_pool.join()`. Thread-safe and lock-free for readers (an atomic
   * `shared_ptr<const std::string>`); cheap; callable from any thread; does nothing observable besides the stored name.
   *
   * @param name Stage name.
   */
  void note_shutdown_stage(std::string_view name);

  /**
   * @brief The last name given to `note_shutdown_stage()`.
   *
   * @return The name, or an empty string when none was given.
   */
  std::string shutdown_stage();

#ifdef SUNSHINE_TESTS
  /**
   * @brief Test-only: forget the registered device release and the shutdown stage.
   */
  void reset_drain_for_test();
#endif
}  // namespace handoff
