/**
 * @file src/handoff/hooks.cpp
 * @brief Definitions for the Linux half of `src/handoff/hooks.h`.
 */
#ifdef __linux__
  // local includes
  #include "hooks.h"

  // local includes
  #include "mdns_holder.h"
  #include "settings.h"
  #include "src/config.h"
  #include "src/video.h"

namespace handoff::hooks {
  namespace {
    /**
     * @brief Handle returned by `mdns_start`; destroying it stops the holder's publisher.
     */
    class mdns_stop_guard_t: public platf::deinit_t {
    public:
      /**
       * @brief Stop publishing through the holder.
       */
      ~mdns_stop_guard_t() override {
        mdns_holder().stop();
      }
    };
  }  // namespace

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

  std::string config_refusal(const settings_t &settings, int port) {
    if (!settings.enabled) {
      return {};
    }
    return handoff::validate(settings, port);
  }

  std::string enable_refusal(const enable_context_t &context, bool privileged_execution) {
    if (!config::drain_handoff.enabled) {
      return {};
    }
    if (privileged_execution) {
      return "drain handoff is not available with privileged execution (its environment sanitising drops the SUNSHINE_HANDOFF_* variables)";
    }
    return handoff::check_enable(context);
  }

  std::unique_ptr<platf::deinit_t> mdns_start(bool arm_rename_checks) {
    mdns_holder().start(arm_rename_checks);
    return std::make_unique<mdns_stop_guard_t>();
  }

  bool startup_probe_skipped() {
    return started_by_handoff() && video::handoff_probe_override().has_value();
  }
}  // namespace handoff::hooks
#endif
