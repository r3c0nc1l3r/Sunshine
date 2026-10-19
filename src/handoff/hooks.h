/**
 * @file src/handoff/hooks.h
 * @brief Platform-neutral facade that shared code (`nvhttp.cpp`, `rtsp.cpp`, `process.cpp`, `main.cpp`, the audio and
 * input code) calls, so it needs no `#ifdef`s.
 *
 * See `docs/drain-handoff.md`. On Linux every hook reads `handoff::runtime()` (defined in `hooks.cpp`). On other
 * platforms every hook is an inline function with the neutral answer given in its comment, and nothing from the rest
 * of `src/handoff/` is needed, because the handoff sources are compiled only on Linux. Every hook is thread-safe and
 * cheap, and none performs I/O, waits or logs. Most are atomic loads; `restart_blocked()` and `quit_blocked()` take the
 * runtime's mutex and `restart_blocked()` calls `drain_peer_t::exists()`, which must not block. Only `on_sigterm()` is
 * async-signal-safe. Both branches below must keep the same signatures and the
 * same documented meaning. In tests (`SUNSHINE_TESTS`) every hook can be returned to its fresh-process answer with
 * `handoff::runtime().reset_for_test()`, because the hooks keep no state of their own.
 */
#pragma once

// standard includes
#include <optional>
#include <string>
#include <utility>
#include <vector>

// local includes
#include "config.h"
#include "types.h"

#ifdef __linux__
  #include "runtime.h"
#endif

namespace handoff::hooks {
#ifdef __linux__
  /**
   * @brief Whether the feature is on in this process (the mode is anything but `disabled`). `false` until
   * `runtime().configure()` has enabled it or a handoff start has set `new_pending`.
   *
   * @return `runtime().enabled()`. Non-Linux: `false`.
   */
  bool enabled();

  /**
   * @brief Whether this process is a draining process (mode `draining`).
   *
   * Used by the audio code (no default-sink restore, no sink unload), the tray and the stop reasons.
   *
   * @return `true` while draining. Non-Linux: `false`.
   */
  bool draining();

  /**
   * @brief Whether this process was started by a handoff (it lives on after `MAIN`).
   *
   * Used by the input code (create the platform input at the first session, not at startup) and the startup code.
   *
   * @return `runtime().started_by_handoff()`. Non-Linux: `false`.
   */
  bool started_by_handoff();

  /**
   * @brief Handoff generation, for virtual device ids.
   *
   * @return `runtime().generation()`: 0 for a process that was never started by a handoff. Non-Linux: 0.
   */
  unsigned generation();

  /**
   * @brief UDP base of this process: the base for `net::map_udp_port(offset)`.
   *
   * @return `runtime().effective_udp_base(config::sunshine.port)`. Non-Linux: `config::sunshine.port`.
   */
  int udp_base();

  /**
   * @brief The host's own default sink as the old process recorded it, for `sink_info()`.
   *
   * @return `runtime().host_sink()`. Non-Linux: `std::nullopt`.
   */
  std::optional<std::string> host_sink();

  /**
   * @brief What an in-place restart must do to the environment just before `execv`, so the restarted process keeps
   * its UDP base and generation but apps never inherit the variables (they are unset again by `early_init`).
   *
   * Always two entries when the feature is enabled, in this order:
   * - `SUNSHINE_UDP_BASE`: set to the decimal `udp_base()` only when it differs from `config::sunshine.port` (an
   *   inherited or overridden base); otherwise `std::nullopt`, meaning the variable must be unset;
   * - `SUNSHINE_GENERATION`: set to the decimal `generation()` only when it is above 0; otherwise `std::nullopt`
   *   (unset).
   *
   * When the feature is disabled (`enabled()` is false): both entries with `std::nullopt` (unset), so a disabled
   * process that inherited a UDP base override reverts to the configured port on restart.
   *
   * @return Pairs of name and value; a `std::nullopt` value means "unset this variable". Non-Linux: empty (nothing to
   *         set or unset).
   */
  std::vector<std::pair<std::string, std::optional<std::string>>> restart_environment();

  /**
   * @brief Whether an in-place restart (`/api/restart`, the tray's restart entry) must be refused now.
   *
   * Feature disabled: never refused (empty). Otherwise refused (non-empty answer) when, in this order: the mode is
   * `draining` (a draining process ignores restart); the mode is `new_pending` or `new_active` (a new process that
   * is not yet the main process; its exit would be a rollback, not a stop); `runtime().handoff_in_progress()` is true;
   * a draining process exists (`runtime().peer()` is set and `exists()` is true, or the instance state is `draining`,
   * `handover_failed` or `kill_pending`). In the first two cases the text says why only; in the others it names the
   * ways out: wait for the drain, send `SIGTERM` to the draining process (this ends its sessions), or `systemctl
   * restart` of the unit (this ends all sessions).
   *
   * @return Empty when the restart may proceed; otherwise a one-line explanation, used as the 409 body and as the
   *         tray Warning. Non-Linux: empty.
   */
  std::string restart_blocked();

  /**
   * @brief Whether Quit (the web UI's, the tray's) must be refused now.
   *
   * The spec requires a refusal only for a draining process (it ignores Quit) and for a new process that is not yet
   * the main process (`new_pending`, `new_active`; its exit would be a rollback rather than a stop). Quit in the
   * active role is allowed, also while a handoff is in progress and while a draining process exists: the `SIGTERM`
   * that systemd then sends aborts the handoff with the kill-first rollback (switch substep 9) or stops both
   * processes. So this hook does not look at `handoff_in_progress()`. Feature disabled: never refused. The Quit paths
   * (and the `shutdown_event` watcher) call `runtime().request_shutdown()`, not `on_sigterm()`, because Quit ends in
   * `lifetime::exit_sunshine`; the coordinator forwards it to `old_side_t::request_shutdown()`.
   *
   * @return Empty when Quit may proceed; otherwise a one-line explanation. Non-Linux: empty.
   */
  std::string quit_blocked();

  /**
   * @brief Called first in the `SIGTERM` handler, before the shutdown event is raised.
   *
   * Only atomic stores and one eventfd write (`runtime().note_sigterm()`); no locks, no allocation, no logging, so it
   * is safe in a signal handler. Latches the shutdown reason, so a session stop that runs after the event sees it.
   * Non-Linux: does nothing.
   */
  void on_sigterm();

  /**
   * @brief Reason for the session stops of a shutdown: the reason that `rtsp_stream::clear` and the control-loop
   * broadcast pass at shutdown.
   *
   * @return `runtime().shutdown_reason()`: `stop` for a draining process, `terminated` otherwise. Non-Linux:
   *         `shutdown_reason_e::terminated`.
   */
  shutdown_reason_e shutdown_reason();
#else
  /**
   * @brief Whether the feature is on in this process.
   *
   * @return Always `false` on this platform.
   */
  inline bool enabled() {
    return false;
  }

  /**
   * @brief Whether this process is a draining process.
   *
   * @return Always `false` on this platform.
   */
  inline bool draining() {
    return false;
  }

  /**
   * @brief Whether this process was started by a handoff.
   *
   * @return Always `false` on this platform.
   */
  inline bool started_by_handoff() {
    return false;
  }

  /**
   * @brief Handoff generation, for virtual device ids.
   *
   * @return Always 0 on this platform.
   */
  inline unsigned generation() {
    return 0;
  }

  /**
   * @brief UDP base of this process.
   *
   * @return `config::sunshine.port` on this platform.
   */
  inline int udp_base() {
    return config::sunshine.port;
  }

  /**
   * @brief The host's own default sink as the old process recorded it.
   *
   * @return Always `std::nullopt` on this platform.
   */
  inline std::optional<std::string> host_sink() {
    return std::nullopt;
  }

  /**
   * @brief Environment variables an in-place restart must set before `execv`.
   *
   * @return Always empty on this platform.
   */
  inline std::vector<std::pair<std::string, std::optional<std::string>>> restart_environment() {
    return {};
  }

  /**
   * @brief Whether an in-place restart must be refused.
   *
   * @return Always empty (allowed) on this platform.
   */
  inline std::string restart_blocked() {
    return {};
  }

  /**
   * @brief Whether Quit must be refused.
   *
   * @return Always empty (allowed) on this platform.
   */
  inline std::string quit_blocked() {
    return {};
  }

  /**
   * @brief Called first in the `SIGTERM` handler. Does nothing on this platform.
   */
  inline void on_sigterm() {
  }

  /**
   * @brief Reason for the session stops of a shutdown.
   *
   * @return Always `shutdown_reason_e::terminated` on this platform.
   */
  inline shutdown_reason_e shutdown_reason() {
    return shutdown_reason_e::terminated;
  }
#endif
}  // namespace handoff::hooks
