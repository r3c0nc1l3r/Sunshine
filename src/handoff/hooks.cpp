/**
 * @file src/handoff/hooks.cpp
 * @brief Definitions for the Linux half of `src/handoff/hooks.h`.
 */
#ifdef __linux__
  // local includes
  #include "hooks.h"

  #include "src/config.h"

namespace handoff::hooks {
  bool enabled() {
    return runtime().enabled();
  }

  bool draining() {
    return runtime().mode() == mode_e::draining;
  }

  bool started_by_handoff() {
    return runtime().started_by_handoff();
  }

  unsigned generation() {
    return runtime().generation();
  }

  int udp_base() {
    return runtime().effective_udp_base(config::sunshine.port);
  }

  std::optional<std::string> host_sink() {
    return runtime().host_sink();
  }

  std::vector<std::pair<std::string, std::optional<std::string>>> restart_environment() {
    std::vector<std::pair<std::string, std::optional<std::string>>> result;
    std::optional<std::string> base;
    std::optional<std::string> generation;

    if (runtime().enabled()) {
      const int effective = udp_base();
      if (effective != config::sunshine.port) {
        base = std::to_string(effective);
      }
      if (const unsigned value = runtime().generation(); value > 0) {
        generation = std::to_string(value);
      }
    }

    result.emplace_back("SUNSHINE_UDP_BASE", std::move(base));
    result.emplace_back("SUNSHINE_GENERATION", std::move(generation));
    return result;
  }

  std::string restart_blocked() {
    auto &rt = runtime();
    const auto mode = rt.mode();
    if (mode == mode_e::disabled) {
      return {};
    }
    if (mode == mode_e::draining) {
      return "Restart refused: this is a draining process; it ignores restart.";
    }
    if (mode == mode_e::new_pending || mode == mode_e::new_active) {
      return "Restart refused: this process was started by a handoff and is not yet the main process.";
    }

    static const char *ways_out =
      "Wait for the drain to finish, send SIGTERM to the draining process (this ends its sessions), "
      "or run systemctl restart of the unit (this ends all sessions).";

    if (rt.handoff_in_progress()) {
      return std::string("Restart refused: a handoff is in progress. ") + ways_out;
    }

    bool drain_exists = false;
    if (auto peer = rt.peer(); peer && peer->exists()) {
      drain_exists = true;
    }
    if (!drain_exists) {
      if (auto instance = rt.instance_current()) {
        drain_exists = instance->handoff == state_e::draining || instance->handoff == state_e::handover_failed || instance->handoff == state_e::kill_pending;
      }
    }
    if (drain_exists) {
      return std::string("Restart refused: a draining process exists. ") + ways_out;
    }
    return {};
  }

  std::string quit_blocked() {
    const auto mode = runtime().mode();
    if (mode == mode_e::draining) {
      return "Quit refused: this is a draining process; it ignores quit.";
    }
    if (mode == mode_e::new_pending || mode == mode_e::new_active) {
      return "Quit refused: this process was started by a handoff and is not yet the main process.";
    }
    return {};
  }

  void on_sigterm() {
    runtime().note_sigterm();
  }

  shutdown_reason_e shutdown_reason() {
    return runtime().shutdown_reason();
  }
}  // namespace handoff::hooks
#endif
