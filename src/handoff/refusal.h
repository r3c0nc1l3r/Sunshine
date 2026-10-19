/**
 * @file src/handoff/refusal.h
 * @brief The refusal rules of the handoff verb (spec step 1, "Trigger and checks") and the functions that gather
 * their inputs.
 *
 * `check_refusal()` is a pure function: the coordinator gathers a `refusal_inputs_t`, calls it, and when the result
 * is not empty answers the helper `refused <reason>` (exit 1), logs a Warning that names the reason and the peer pid
 * (`log_refusal()`), and changes nothing else. A refusal does not start the cooldown and is never retried by the
 * coordinator. The switch repeats the app, pairing and pending-launch checks in `old_side_ops_t::recheck()` (substep 4);
 * that path is owned by the coordinator, may reuse the gatherers below, and does not use `check_refusal()`.
 *
 * Thread-safety: `check_refusal()` and the pure helpers are reentrant. The gatherers take no handoff lock, may be
 * called from the coordinator's control-handler thread, and must not be called while holding the slots lock, the clear
 * lock, the gate or `client_auth_mutex()`. `gather_systemd_version()` can block for its timeout (default 2 s).
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

// local includes
#include "instance_file.h"

namespace handoff {
  /**
   * @brief Everything `check_refusal()` looks at. The defaults describe a process that passes every check except the
   * systemd ones, so a test sets only the field under test (and the systemd fields).
   */
  struct refusal_inputs_t {
    bool notify_socket_set {false};  ///< `NOTIFY_SOCKET` is set and not empty (the process is a systemd service with
                                     ///< readiness).
    std::optional<int> systemd_version;  ///< Major version from `sd::parse_systemd_version(systemctl --version)`;
                                         ///< `std::nullopt` when the command failed or its output did not parse.
    bool unit_found {false};  ///< `sd::unit_from_cgroup(/proc/self/cgroup)` found a unit.
    bool in_progress {false};  ///< A handoff started by this process is running (`runtime().handoff_in_progress()`), or
                               ///< another live instance file has role `pending`.
    bool drain_instance_exists {false};  ///< A draining instance exists: the open channel's peer exists
                                         ///< (`drain_peer_t::exists()`), or another live instance file has role
                                         ///< `draining`, or the instance directory cannot be read completely.
    bool cooldown_active {false};  ///< The cooldown deadline has not passed (own attempt, or inherited in `HELLO`).
    bool kill_pending {false};  ///< A live instance file (this process's or another's) has state `kill_pending`.
    bool app_adoptable {true};  ///< `proc::proc.adoptable(&why)`.
    std::string app_reason;  ///< `why` of `adoptable()` when `app_adoptable` is `false`; may be empty (then the generic
                             ///< app reason is used). One line.
    std::size_t pairing_count {0};  ///< `nvhttp::pairing_session_count()` (it expires old pairing sessions first).
    bool pending_launch {false};  ///< `rtsp_stream::has_pending_launch()`: a raised launch session has not started
                                  ///< streaming yet.
  };

  /**
   * @brief The exact `refused <reason>` strings. One line each, no newline.
   */
  namespace refusal_reason {
    /// `kill_pending` is set.
    inline constexpr std::string_view kill_pending = "kill pending: a rollback is waiting for the new process to exit";
    /// A handoff is in progress.
    inline constexpr std::string_view in_progress = "a handoff is already in progress";
    /// A draining instance exists.
    inline constexpr std::string_view drain_exists = "a drain is in progress";
    /// The cooldown has not passed.
    inline constexpr std::string_view cooldown = "the cooldown has not passed";
    /// `NOTIFY_SOCKET` is not set.
    inline constexpr std::string_view no_notify_socket = "not a systemd service with readiness (NOTIFY_SOCKET is not set)";
    /// The systemd version could not be read.
    inline constexpr std::string_view systemd_unknown = "the systemd version could not be determined";
    /// The systemd version is older than 254.
    inline constexpr std::string_view systemd_old = "systemd 254 or newer is required";
    /// No unit was found in `/proc/self/cgroup`.
    inline constexpr std::string_view no_unit = "the systemd unit could not be found from /proc/self/cgroup";
    /// The app is not adoptable and `app_reason` is empty.
    inline constexpr std::string_view app_generic = "an application with a running process is active";
    /// Prefix of the app reason when `app_reason` is not empty: the result is this prefix plus `app_reason`.
    inline constexpr std::string_view app_prefix = "an application with a running process is active: ";
    /// A pairing is in progress.
    inline constexpr std::string_view pairing = "a pairing is in progress";
    /// A launch is pending.
    inline constexpr std::string_view pending_launch = "a launch is pending, retry in a few seconds";
  }  // namespace refusal_reason

  /**
   * @brief Oldest systemd major version that supports the handoff (`MAINPID=` with `BARRIER=1` ordering).
   */
  inline constexpr int minimum_systemd_version = 254;

  /**
   * @brief Decide whether the handoff verb must be refused.
   *
   * Precedence, when several refusals apply: the first row of this list that applies wins, and only its reason is
   * returned. The order is: states that block a retry first (the operator must act, or must not retry), then the
   * permanent properties of the process's environment, then the transient conditions (retry later).
   *
   * 1. `kill_pending`: `refusal_reason::kill_pending`. It comes first because it is the only state in which the
   *    operator must act and tooling must not retry; it must be the reason shown even when a drain, a handoff or the
   *    cooldown also apply (a stuck rollback implies all of them).
   * 2. `in_progress`: `refusal_reason::in_progress`.
   * 3. `drain_instance_exists`: `refusal_reason::drain_exists`.
   * 4. `cooldown_active`: `refusal_reason::cooldown`.
   * 5. `!notify_socket_set`: `refusal_reason::no_notify_socket`.
   * 6. `systemd_version` is `std::nullopt`: `refusal_reason::systemd_unknown`; below `minimum_systemd_version` (254):
   *    `refusal_reason::systemd_old`. Version 254 itself and above pass.
   * 7. `!unit_found`: `refusal_reason::no_unit`.
   * 8. `!app_adoptable`: `refusal_reason::app_generic` when `app_reason` is empty, else
   *    `refusal_reason::app_prefix` followed by `app_reason` after it was sanitised: every CR and LF is replaced with a
   *    space and the text is truncated to 200 bytes (so the result is always one line). If the sanitised text is empty
   *    the generic reason is used.
   * 9. `pairing_count > 0`: `refusal_reason::pairing`.
   * 10. `pending_launch`: `refusal_reason::pending_launch`.
   *
   * `kill_pending` is visible and never retried: the reason is distinct, the coordinator logs it as a Warning with the
   * peer pid like every refusal, the helper exits 1 (`refused`), and nothing in the coordinator retries, resets the
   * cooldown or clears the state because of a refusal. The only things that end the state are the old side finishing
   * its rollback after the reap, or an operator stopping the unit. Tooling treats it as busy (the instance file already
   * says so, see `consumer_busy()`).
   *
   * @param inputs Gathered inputs.
   * @return The reason (one line, no newline) for the first applicable row, or an empty string when the handoff is
   *         allowed.
   */
  std::string check_refusal(const refusal_inputs_t &inputs);

  /**
   * @brief Log a refusal as a Warning: `Drain handoff refused: <reason> (peer pid <pid>)`.
   *
   * @param reason Reason from `check_refusal()`; never logged with any certificate, path or secret.
   * @param peer_pid Pid of the connecting peer (`SO_PEERCRED`), or 0 when unknown (logged as `unknown`).
   */
  void log_refusal(std::string_view reason, pid_t peer_pid);

  /**
   * @brief Read-only facts derived from the instance files, for two inputs.
   */
  struct instance_view_t {
    bool handoff_pending {false};  ///< Another live instance (pid not `self_pid`) has role `pending`.
    bool drain_instance_exists {false};  ///< Another live instance has role `draining`, or the scan is incomplete
                                         ///< (`any_unreadable`). Fail closed: refusing is always safe.
    bool kill_pending {false};  ///< Any live instance, this process's own included, has state `kill_pending`.
  };

  /**
   * @brief Classify a scan of the instance files. Pure.
   *
   * `dir_missing` alone changes nothing (this process's own file may not have been written; the checks that follow do
   * not depend on it). Stale entries are ignored. A live instance with `pid == self_pid` counts only for
   * `kill_pending`.
   *
   * @param scan Result of `scan_instances(dir, read_start_time)`.
   * @param self_pid Pid of this process.
   * @return The three facts.
   */
  instance_view_t classify_instances(const scan_result_t &scan, pid_t self_pid);

  /**
   * @brief Whether the cooldown is active. Pure.
   *
   * @param cooldown_until Deadline (`CLOCK_MONOTONIC`, as `steady_clock` on Linux; the coordinator's own deadline or
   *        the one inherited in `HELLO`); `time_point::min()` means no cooldown.
   * @param now Current time.
   * @return `true` when `now < cooldown_until`.
   */
  bool cooldown_active(std::chrono::steady_clock::time_point cooldown_until, std::chrono::steady_clock::time_point now);

  /**
   * @brief The places `gather_refusal_inputs()` reads. The production values come from
   * `production_refusal_sources()`; tests (and the coordinator's tests) pass fakes. Every member must be set.
   */
  struct refusal_sources_t {
    std::function<std::string()> notify_socket;  ///< Value of `NOTIFY_SOCKET`, empty when unset. Read at each call.
    std::function<std::optional<int>()> systemd_version;  ///< `gather_systemd_version()`.
    std::function<std::optional<std::string>()> cgroup;  ///< Content of `/proc/self/cgroup`, or `std::nullopt` when it
                                                         ///< cannot be read.
    std::function<bool()> handoff_in_progress;  ///< `runtime().handoff_in_progress()`.
    std::function<bool()> peer_exists;  ///< `runtime().peer()` is set and `peer()->exists()` (which must not block).
    std::function<scan_result_t()> scan;  ///< `scan_instances(<runtime dir>/instances, read_start_time)`. A missing
                                          ///< runtime dir gives `dir_missing`.
    std::function<bool(std::string &)> app_adoptable;  ///< `proc::proc.adoptable(&why)`: returns the answer and fills
                                                       ///< `why` when `false`. Never calls `running()`.
    std::function<std::size_t()> pairing_count;  ///< `nvhttp::pairing_session_count()`.
    std::function<bool()> pending_launch;  ///< `rtsp_stream::has_pending_launch()`.
    std::function<pid_t()> self_pid;  ///< `getpid()`.
    std::function<std::chrono::steady_clock::time_point()> now;  ///< `steady_clock::now()`.
  };

  /**
   * @brief The production sources (see `refusal_sources_t`); `systemd_version` is
   * `gather_systemd_version()` with its default runner.
   *
   * @return Sources.
   */
  refusal_sources_t production_refusal_sources();

  /**
   * @brief Run `systemctl --version` and parse it.
   *
   * Cache: only a call with the default (empty) runner reads and fills the cache. The cache holds the first successful
   * parse for the life of the process (the systemd version cannot change under a running service) and is guarded by a
   * mutex (or `call_once`), so concurrent callers are safe and run the command at most once per success. A failure is
   * not cached and is retried at the next default-runner call. A call with an injected runner never reads or writes the
   * cache, so tests do not depend on order or repetition. The default runner executes
   * `/usr/bin/systemctl --version` under the same rules as `sd::read_back_mainpid` (no `PATH` search, no shell, fds 3
   * and up closed, tracked by pidfd only because the control loop reaps with `waitpid(-1)` [C27], output capped at
   * 4096 bytes) with a 2 s timeout.
   *
   * @param runner Returns the command output, or `std::nullopt` on a spawn failure, a timeout, or a non-zero exit;
   *        empty means the default runner (and the cache). A test passes a fake (and bypasses the cache).
   * @return `sd::parse_systemd_version(output)`; `std::nullopt` when the runner failed or the output did not parse.
   */
  std::optional<int> gather_systemd_version(const std::function<std::optional<std::string>()> &runner = {});

  /**
   * @brief Gather every input of `check_refusal()` from the sources.
   *
   * Mapping (the source of each field):
   * - `notify_socket_set`: `sources.notify_socket()` is not empty.
   * - `systemd_version`: `sources.systemd_version()`.
   * - `unit_found`: `sd::unit_from_cgroup(*sources.cgroup())` has a value; `false` when the cgroup cannot be read.
   * - `in_progress`: `sources.handoff_in_progress()` or `classify_instances().handoff_pending`.
   * - `drain_instance_exists`: `sources.peer_exists()` or `classify_instances().drain_instance_exists`.
   * - `cooldown_active`: `cooldown_active(cooldown_until, sources.now())`.
   * - `kill_pending`: `classify_instances().kill_pending`.
   * - `app_adoptable` and `app_reason`: `sources.app_adoptable(why)`; `app_reason` is `why` only when not adoptable.
   * - `pairing_count`: `sources.pairing_count()`.
   * - `pending_launch`: `sources.pending_launch()`.
   *
   * Every source is called exactly once per call (the scan once, not once per field). The function holds no lock of
   * its own and takes no snapshot lock: the checks are advisory, and the switch repeats them (`recheck()`).
   *
   * @param sources Sources.
   * @param cooldown_until The coordinator's cooldown deadline (`time_point::min()` for none).
   * @return Inputs for `check_refusal()`.
   */
  refusal_inputs_t gather_refusal_inputs(
    const refusal_sources_t &sources,
    std::chrono::steady_clock::time_point cooldown_until
  );
}  // namespace handoff
