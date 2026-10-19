/**
 * @file src/handoff/old_side.h
 * @brief The old process's handoff sequence: start of the new process, switch, rollback window, rollback and
 * main-PID handover.
 *
 * See `docs/drain-handoff.md`, steps 3 to 7. Every effect on the rest of Sunshine goes through `old_side_ops_t` and
 * `child_ops_t`, so the order of operations can be tested with fakes and a real socketpair.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>

// local includes
#include "channel.h"
#include "control.h"
#include "end_all.h"
#include "instance_file.h"
#include "protocol.h"
#include "spawn.h"

namespace handoff {
  /**
   * @brief Timeouts of the old side; tests shorten them.
   */
  struct old_side_timeouts_t {
    std::chrono::milliseconds ready {30000};  ///< `HELLO` and `READY` from the new process (`drain_handoff_ready_timeout`).
    std::chrono::milliseconds in_flight {5000};  ///< Switch substep 3.
    std::chrono::milliseconds pairing {10000};  ///< Switch substep 4 pairing wait (`ping_timeout`).
    std::chrono::milliseconds mdns_withdraw {5000};  ///< Switch substep 7.
    std::chrono::milliseconds go_ack {5000};  ///< Switch substep 10, from the `GO` send.
    std::chrono::milliseconds reap {5000};  ///< One kill-first wait; repeated while the child is not gone.
    std::chrono::milliseconds ping_interval {2000};  ///< `PING` period.
    int misses_to_hang {5};  ///< Consecutive misses that mean a hung new process.
    std::chrono::milliseconds rollback_window {60000};  ///< From `COMMIT` to the first handover attempt.
    std::chrono::milliseconds handover_retry {10000};  ///< Period of handover retries.
  };

  /**
   * @brief Effects of the old side on the rest of the process. Implemented by the Sunshine glue; faked in tests.
   *
   * Calls happen on the thread that runs `old_side_t::run()` unless noted.
   */
  class old_side_ops_t {
  public:
    /**
     * @brief Virtual destructor.
     */
    virtual ~old_side_ops_t() = default;

    /**
     * @brief Own `HELLO` (probe results, identity, cooldown deadline).
     *
     * @return Message to send.
     */
    virtual protocol::hello_t hello() = 0;

    /**
     * @brief A pidfd of this process for `HELLO`; the side closes it after sending.
     *
     * @return Pidfd, or -1 (then the start fails).
     */
    virtual int self_pidfd() = 0;

    /**
     * @brief Substep 1: pause accepting on all four sockets; each pause is bounded (at most the 5 s in-flight budget).
     *
     * `old_side_t` treats a non-empty result as a failed switch: it takes the rollback and answers the helper
     * `failed <reason>` (the non-zero exit of the helper) with this reason. `resume_accept()` then runs in the
     * rollback and must be safe on a socket that was never paused.
     *
     * @return An empty string when all four sockets confirmed the pause; otherwise a one-line reason naming the server
     *         that did not confirm.
     */
    virtual std::string pause_accept() = 0;
    virtual void resume_accept() = 0;  ///< Resume accepting on all four sockets.
    virtual void raise_gate() = 0;  ///< Substep 2.
    virtual void lower_gate() = 0;  ///< Lift the 503 gate.

    /**
     * @brief Wait for the in-flight counters to reach zero.
     *
     * @param timeout Longest wait; negative waits without limit (rollback; the implementation logs a Warning every
     *        5 s).
     * @return `true` when zero was reached.
     */
    virtual bool wait_in_flight(std::chrono::milliseconds timeout) = 0;

    /**
     * @brief Substep 4: repeat the step-1 checks, wait up to `pairing_wait` for pairing sessions, drop a pending
     * launch.
     *
     * @param pairing_wait Longest wait for pairing sessions.
     * @return Empty when the switch may continue; otherwise the reason.
     */
    virtual std::string recheck(std::chrono::milliseconds pairing_wait) = 0;

    /**
     * @brief Substep 5: save `sunshine_state.json` under `client_auth_mutex`.
     *
     * @return `true` on success (a failure fails the switch like substep 3).
     */
    virtual bool save_state() = 0;

    virtual void freeze() = 0;  ///< Substep 6: write no state file from now on.
    virtual void unfreeze() = 0;  ///< Rollback: writing state is allowed again.

    /**
     * @brief Substep 7: withdraw mDNS and wait for Avahi to drop the entry.
     *
     * @param timeout Longest wait.
     */
    virtual void withdraw_mdns(std::chrono::milliseconds timeout) = 0;

    virtual void publish_mdns() = 0;  ///< Rollback: republish mDNS.
    virtual void remove_tray() = 0;  ///< Substep 7: remove the tray icon.
    virtual void restore_tray() = 0;  ///< Rollback: restore the tray icon.

    /**
     * @brief Substep 8: the snapshot `GO` carries.
     *
     * @return Snapshot.
     */
    virtual protocol::go_t take_snapshot() = 0;

    /**
     * @brief Substep 9: whether a `SIGTERM` (or other shutdown request) has arrived.
     *
     * @return `true` when shutting down.
     */
    virtual bool shutdown_requested() = 0;

    /**
     * @brief Set or clear the app's release flag.
     *
     * @param released New value.
     */
    virtual void set_released(bool released) = 0;

    virtual void drop_app() = 0;  ///< Rollback after `ENDED` started: drop the released app state, no `undo`.

    /**
     * @brief Rollback: reload both state files and the credentials file and swap in the app list.
     *
     * @return `false` when a reload failed (the frozen state is kept; the implementation logs an Error).
     */
    virtual bool reload_after_rollback() = 0;

    /**
     * @brief After `COMMIT`: become the draining process. The control socket is closed with
     * `control_server_t::close_listener()`, never `stop()` (the helper's handler still waits on the server thread).
     */
    virtual void enter_drain_mode() = 0;

    /**
     * @brief Rollback: become the active process again; the control socket is reopened with
     * `control_server_t::reopen_listener()`.
     */
    virtual void leave_drain_mode() = 0;

    /**
     * @brief Set this process's instance-file role.
     *
     * @param role Role.
     */
    virtual void set_role(role_e role) = 0;

    /**
     * @brief Set this process's handoff state (instance file and, when draining, `HANDOFF_STATE`).
     *
     * @param state State.
     */
    virtual void set_state(state_e state) = 0;

    virtual void remove_new_instance_file() = 0;  ///< Rollback: remove the killed new process's instance file.

    /**
     * @brief Rollback after `COMMIT`: start the best-effort UPnP cleanup of the new base on a background task.
     *
     * Must return at once.
     *
     * @param new_base The killed new process's UDP base.
     */
    virtual void start_upnp_cleanup(int new_base) = 0;

    /**
     * @brief Step 7.2-7.3: `MAINPID=` plus the barrier, then the read-back.
     *
     * @param new_pid New process pid.
     * @return `true` when the read-back equals `new_pid`.
     */
    virtual bool handover(pid_t new_pid) = 0;

    virtual void close_socket_copies() = 0;  ///< Step 7.4.
  };

  /**
   * @brief The child as seen by the old side. Implemented over the pidfd; faked in tests.
   */
  class child_ops_t {
  public:
    /**
     * @brief Virtual destructor.
     */
    virtual ~child_ops_t() = default;

    /**
     * @brief Kill-first: `SIGKILL` through the pidfd and wait for the reap.
     *
     * @param timeout One wait.
     * @return Outcome; `timeout` means try again.
     */
    virtual reap_result_e kill_and_reap(std::chrono::milliseconds timeout) = 0;

    /**
     * @brief Whether the child has exited (pidfd readable).
     *
     * @return `true` when exited.
     */
    virtual bool exited() = 0;

    /**
     * @brief The child's pid (for `MAINPID=` and the helper's `committed` line).
     *
     * @return Pid.
     */
    virtual pid_t pid() = 0;

    /**
     * @brief The child's start time (for the helper's `committed` line).
     *
     * @return Start time.
     */
    virtual std::uint64_t start_time() = 0;
  };

  /**
   * @brief Final outcome of `old_side_t::run()`.
   */
  enum class old_outcome_e {
    handed_over,  ///< `MAIN` sent; this process now drains (step 8).
    rolled_back,  ///< Rolled back; this process is the active process again.
    shutdown,  ///< A shutdown arrived; the run ended without a handover (after a kill-first rollback if needed).
  };

  /**
   * @brief Drives one handoff on the old side.
   *
   * Sequence (each failure goes to the rollback below):
   *
   * 1. Send `HELLO` with `self_pidfd()` attached; wait (`ready` timeout) for the child's `HELLO` and then `READY`.
   *    A failed `READY`, a major-version mismatch, an early close or a timeout is a failed start.
   * 2. Switch substeps in this order: `pause_accept` (a non-empty reason fails the switch: rollback, helper `failed <reason>`), `raise_gate`, `wait_in_flight(in_flight)`,
   *    `recheck(pairing)`, `save_state`, `freeze`, `withdraw_mdns(mdns_withdraw)`, `remove_tray`, `take_snapshot`,
   *    `shutdown_requested` (true → rollback), `set_released(true)`, send `GO`.
   * 3. Wait for `GO_ACK` until `go_ack` after the `GO` send (monotonic). On `ok`, re-check that deadline, send
   *    `COMMIT`, then: `set_role(draining)`, `set_state(draining)`, `enter_drain_mode()`, answer the helper
   *    `committed <child pid> <child start_time>`.
   * 4. Rollback window: send `PING` every `ping_interval` (`ping_monitor_t`); a hung child, a dead child or a channel
   *    error/close is a rollback. When `rollback_window` has passed: `exited()` → rollback; else `handover()`;
   *    success → `close_socket_copies()`, send `MAIN`, return `handed_over`; failure → `set_state(handover_failed)`
   *    and retry every `handover_retry` (pings and the rollback rule continue). A success after failures sets the
   *    state back to `draining`.
   *
   * Rollback (whatever the child does): `end_all.decide_rollback()` first; then `kill_and_reap(reap)` until the
   * child is gone (`reaped` or `reaped_elsewhere`; an `error` counts as gone only when `exited()` is true, otherwise it
   * is handled like `timeout`), and after the first `timeout` `set_state(kill_pending)` (and, if the helper was not yet answered,
   * answer it `kill_pending`); when gone, answer a still-waiting helper `failed <reason>`; then, only for the steps
   * that happened: `wait_in_flight(-1)` and then `reload_after_rollback()` and `unfreeze()` if frozen (the unbounded
   * wait only protects the reload and app-list swap, so a failure before the freeze does not wait),
   * `set_released(false)` for `take_back` or `drop_app()` for `drop` if released, `remove_new_instance_file()`,
   * `start_upnp_cleanup(new_base)` if `COMMIT` was sent, `lower_gate()` if raised, `resume_accept()` if paused,
   * `publish_mdns()` and `restore_tray()` if withdrawn, `leave_drain_mode()` and `set_role(active)` if
   * `enter_drain_mode()` ran, `set_state(none)`. No `resume_accept`, `publish_mdns`, `restore_tray`, `lower_gate`,
   * `set_released(false)` or `drop_app` happens before the child is reaped.
   *
   * A failed start or a failure before `GO` with no helper answer yet answers `failed`; `refused` is never sent here
   * (refusals happen before spawning). The helper is answered exactly once.
   */
  class old_side_t {
  public:
    /**
     * @brief Create the driver.
     *
     * @param ops Process effects.
     * @param child Child.
     * @param channel Old process's end of the channel.
     * @param end_all `END_ALL` state machine shared with the drain-request handler; `reset()` by the caller.
     * @param timeouts Timeouts.
     * @param new_base The child's UDP base.
     * @param answer_helper Called exactly once with the helper's answer, from the `run()` thread.
     */
    old_side_t(old_side_ops_t &ops, child_ops_t &child, channel_t &channel, end_all_machine_t &end_all, old_side_timeouts_t timeouts, int new_base, std::function<void(const control_reply_t &)> answer_helper);

    /**
     * @brief Run the handoff until the handover, a completed rollback, or a shutdown.
     *
     * `END_ALL`, `SESSIONS?`, `CLOSE` and `CLOSE_CERT` are passed to `on_drain_request` on another thread and never
     * block this one; other messages are dropped (their fds closed) with a Warning. At most 1024 messages wait for the
     * handler and at most 1024 for this thread; beyond that the newest is dropped with a Warning. Handled messages
     * never delay the `PING` and handover timers. `run()` returns only after the handler's current call has finished,
     * so the handler must be bounded (the drain glue bounds `END_ALL` by the session join guard).
     *
     * @param on_drain_request Handler for drain requests; it replies on the channel itself.
     * @return Outcome.
     */
    old_outcome_e run(const std::function<void(received_t)> &on_drain_request);

    /**
     * @brief Report a shutdown request (`SIGTERM`); thread-safe. Before `GO` it aborts with a rollback (kill-first) and
     * `run()` returns `shutdown`. After `GO` `run()` returns `shutdown` promptly, whether the child is alive or dies:
     * no handover (no `close_socket_copies`, no `MAIN`), no rollback and no rollback effects. With `systemctl stop`
     * both processes get `SIGTERM`; a `SIGTERM` to the old process alone inside the window ends the unit, as a crash
     * of the main process would (spec step 6).
     */
    void request_shutdown();

  private:
    old_side_ops_t &_ops;  ///< Effects.
    child_ops_t &_child;  ///< Child.
    channel_t &_channel;  ///< Channel.
    end_all_machine_t &_end_all;  ///< `END_ALL` machine.
    old_side_timeouts_t _timeouts;  ///< Timeouts.
    int _new_base;  ///< Child's UDP base.
    std::function<void(const control_reply_t &)> _answer_helper;  ///< Helper answer.
    std::atomic<bool> _shutdown {false};  ///< Shutdown requested.
  };
}  // namespace handoff
