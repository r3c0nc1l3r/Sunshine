/**
 * @file src/handoff/hooks.h
 * @brief Platform-neutral facade that shared code (`nvhttp.cpp`, `rtsp.cpp`, `process.cpp`, `main.cpp`, the audio and
 * input code) calls, so it needs no `#ifdef`s.
 *
 * See `docs/drain-handoff.md`. On Linux every hook reads `handoff::runtime()` (defined in `hooks.cpp`). On other
 * platforms every hook is an inline function with the neutral answer given in its comment, and nothing from the rest
 * of `src/handoff/` is needed, because the handoff sources are compiled only on Linux. Every hook is thread-safe and
 * cheap (atomic loads), and none of them blocks or logs. Both branches below must keep the same signatures and the
 * same documented meaning.
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
   * @brief Whether the feature is on in this process (the mode is anything but `disabled`).
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
   * @brief Environment variables an in-place restart must set just before `execv`, so the restarted process keeps
   * its UDP base and generation.
   *
   * @return Pairs `{"SUNSHINE_UDP_BASE", <udp_base()>}` and `{"SUNSHINE_GENERATION", <generation()>}`, in this order,
   *         as decimal strings; both only when the feature is enabled, else empty. Non-Linux: empty.
   */
  std::vector<std::pair<std::string, std::string>> restart_environment();

  /**
   * @brief Whether an in-place restart (`/api/restart`, the tray's restart entry) must be refused now.
   *
   * Refused (non-empty answer) when, in this order: the mode is `draining` (a draining process ignores restart); the
   * mode is `new_pending` or `new_active` (a new process that is not yet the main process; its exit would be a
   * rollback, not a stop); a handoff is in progress or a draining process exists (`runtime().peer()` is set and
   * `exists()` is true, or the instance state is `draining`, `handover_failed` or `kill_pending`). In the first two
   * cases the text says why only; in the others it names the ways out: wait for the drain, send `SIGTERM` to the
   * draining process (this ends its sessions), or `systemctl restart` of the unit (this ends all sessions).
   *
   * @return Empty when the restart may proceed; otherwise a one-line explanation, used as the 409 body and as the
   *         tray Warning. Non-Linux: empty.
   */
  std::string restart_blocked();

  /**
   * @brief Whether Quit (the web UI's, the tray's) must be refused now.
   *
   * Refused when the mode is `draining` (ignored) or `new_pending` or `new_active` (the process is not the main
   * process until `MAIN`: `!runtime().is_main()` while the feature is enabled). Quit in the active role is allowed
   * even while a draining process exists (it stops the unit).
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
  inline std::vector<std::pair<std::string, std::string>> restart_environment() {
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
