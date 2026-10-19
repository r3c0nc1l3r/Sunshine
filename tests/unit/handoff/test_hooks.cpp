/**
 * @file tests/unit/handoff/test_hooks.cpp
 * @brief Blind tests for the platform-neutral drain-handoff facade (src/handoff/hooks.h) on Linux: the state queries,
 * `restart_environment()`, `restart_blocked()`, `quit_blocked()`, `on_sigterm()` and `shutdown_reason()`.
 *
 * Written from `docs/drain-handoff.md` and the public headers only. Every test resets the process-wide runtime, because
 * the hooks keep no state of their own.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <cstdint>
  #include <filesystem>
  #include <map>
  #include <memory>
  #include <optional>
  #include <string>
  #include <utility>
  #include <vector>

  // lib includes
  #include <poll.h>
  #include <unistd.h>

  // local includes
  #include "src/config.h"
  #include "src/handoff/hooks.h"
  #include "src/handoff/runtime.h"

namespace {
  using handoff::mode_e;
  using handoff::shutdown_reason_e;

  /**
   * @brief Fake process environment for `runtime_t::early_init()`.
   */
  struct fake_env_t {
    std::map<std::string, std::string> vars;  ///< Variables that are set.

    /**
     * @brief Build the `early_init` callbacks over this object.
     *
     * @return Callbacks that read and change `vars`.
     */
    handoff::early_init_env_t make() {
      handoff::early_init_env_t env;
      env.getenv = [this](const char *name) -> const char * {
        auto it = vars.find(name);
        return it == vars.end() ? nullptr : it->second.c_str();
      };
      env.unsetenv = [this](const char *name) {
        vars.erase(name);
      };
      env.scrub_fds = [] {
      };
      return env;
    }
  };

  /**
   * @brief Whether a file descriptor is readable right now (zero-timeout poll).
   *
   * @param fd Descriptor to test.
   * @return `true` when `POLLIN` is reported.
   */
  bool readable(int fd) {
    pollfd pfd {fd, POLLIN, 0};
    return ::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0;
  }

  /**
   * @brief Whether a text is a single line (no newline), as the hook texts must be.
   *
   * @param text Text to check.
   * @return `true` when the text has no newline character.
   */
  bool single_line(const std::string &text) {
    return text.find('\n') == std::string::npos;
  }

  /**
   * @brief Whether a text names the ways out of a refused restart: wait for the drain, `SIGTERM`, `systemctl restart`.
   *
   * @param text Text to check.
   * @return `true` when all three ways out are named.
   */
  bool names_ways_out(const std::string &text) {
    return text.find("drain") != std::string::npos && text.find("SIGTERM") != std::string::npos &&
           text.find("systemctl restart") != std::string::npos;
  }

  /**
   * @brief Drive the process-wide runtime to `target` using only the documented steps.
   *
   * @param target Mode to reach.
   * @return `true` when the runtime is in `target`.
   */
  bool reach_mode(mode_e target) {
    auto &rt = handoff::runtime();
    if (target == mode_e::disabled) {
      return rt.mode() == mode_e::disabled;
    }
    if (target == mode_e::normal || target == mode_e::draining) {
      handoff::settings_t settings;
      settings.enabled = true;
      rt.configure(settings);
      if (target == mode_e::draining) {
        return rt.transition(mode_e::normal, mode_e::draining);
      }
      return rt.mode() == mode_e::normal;
    }
    fake_env_t env;
    env.vars["SUNSHINE_HANDOFF_FDS"] = "https:3,http:4,webui:5,rtsp:6";
    env.vars["SUNSHINE_HANDOFF_CHANNEL"] = "7";
    env.vars["SUNSHINE_HANDOFF_UDP_BASE"] = "48100";
    env.vars["SUNSHINE_HANDOFF_GENERATION"] = "1";
    auto callbacks = env.make();
    if (!rt.early_init(callbacks).empty()) {
      return false;
    }
    if (target == mode_e::new_pending) {
      return rt.mode() == mode_e::new_pending;
    }
    return rt.transition(mode_e::new_pending, mode_e::new_active) && rt.mode() == mode_e::new_active;
  }

  /**
   * @brief Fake draining peer for the `set_peer` slot.
   */
  class fake_peer_t: public handoff::drain_peer_t {
  public:
    /**
     * @brief Create a fake peer.
     *
     * @param alive Value returned by `exists()`.
     */
    explicit fake_peer_t(bool alive):
        _alive(alive) {}

    /**
     * @brief Whether the fake draining process exists.
     *
     * @return The value given at construction.
     */
    bool exists() override {
      return _alive;
    }

    /**
     * @brief Whether the fake draining process is gone.
     *
     * @return The negation of `exists()`.
     */
    bool gone() override {
      return !_alive;
    }

    /**
     * @brief Not exercised by these tests.
     *
     * @param timeout Ignored.
     * @return Always `no_reply`.
     */
    reply_e end_all(std::chrono::milliseconds timeout) override {
      (void) timeout;
      return reply_e::no_reply;
    }

  private:
    bool _alive;  ///< Value of `exists()`.
  };
}  // namespace

/**
 * @brief Fixture: resets the process-wide runtime, pins the configured TCP port, and owns a temporary runtime directory.
 */
class HandoffHooksTest: public ::testing::Test {
protected:
  /**
   * @brief Reset the runtime, pin `config::sunshine.port` and create the temporary directory.
   */
  void SetUp() override {
    handoff::runtime().reset_for_test();
    _saved_port = config::sunshine.port;
    config::sunshine.port = 47989;
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() /
           ("sunshine-handoff-hooks-" + std::to_string(::getpid()) + "-" + info->name());
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
    std::filesystem::create_directories(_dir, ec);
  }

  /**
   * @brief Restore the port, remove the temporary directory and reset the runtime.
   */
  void TearDown() override {
    handoff::runtime().reset_for_test();
    config::sunshine.port = _saved_port;
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
  }

  std::uint16_t _saved_port {0};  ///< Port value to restore after the test.
  std::filesystem::path _dir;  ///< Temporary runtime directory.
};

// ---------------------------------------------------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Before `configure()` the hooks answer as a process with the feature off.
 */
TEST_F(HandoffHooksTest, NeutralAnswersBeforeConfigure) {
  EXPECT_FALSE(handoff::hooks::enabled()) << "enabled() must be false before configure().";
  EXPECT_FALSE(handoff::hooks::draining()) << "draining() must be false when the mode is disabled.";
  EXPECT_FALSE(handoff::hooks::started_by_handoff()) << "started_by_handoff() must be false for a normal start.";
  EXPECT_EQ(handoff::hooks::generation(), 0u) << "generation() must be 0 for a process never started by a handoff.";
  EXPECT_EQ(handoff::hooks::udp_base(), 47989) << "udp_base() must be the configured port when there is no override.";
  EXPECT_FALSE(handoff::hooks::host_sink().has_value()) << "host_sink() must be empty for a normal start.";
  EXPECT_EQ(handoff::hooks::shutdown_reason(), shutdown_reason_e::terminated)
    << "shutdown_reason() must be terminated when nothing is draining and nothing was requested.";
}

/**
 * @brief `enabled()` and `draining()` follow the mode.
 */
TEST_F(HandoffHooksTest, EnabledAndDrainingFollowMode) {
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  EXPECT_TRUE(handoff::hooks::enabled()) << "enabled() must be true in mode normal.";
  EXPECT_FALSE(handoff::hooks::draining()) << "draining() must be false in mode normal.";
  ASSERT_TRUE(handoff::runtime().transition(mode_e::normal, mode_e::draining)) << "Precondition: normal to draining.";
  EXPECT_TRUE(handoff::hooks::enabled()) << "enabled() must stay true while draining.";
  EXPECT_TRUE(handoff::hooks::draining()) << "draining() must be true in mode draining.";
}

/**
 * @brief After a valid handoff start the hooks report the inherited generation, UDP base and host sink.
 */
TEST_F(HandoffHooksTest, HandoffStartReportsGenerationBaseAndHostSink) {
  fake_env_t env;
  env.vars["SUNSHINE_HANDOFF_FDS"] = "https:3,http:4,webui:5,rtsp:6";
  env.vars["SUNSHINE_HANDOFF_CHANNEL"] = "7";
  env.vars["SUNSHINE_HANDOFF_UDP_BASE"] = "48100";
  env.vars["SUNSHINE_HANDOFF_GENERATION"] = "3";
  env.vars["SUNSHINE_HANDOFF_HOST_SINK"] = "alsa_output.test-sink";
  auto callbacks = env.make();
  ASSERT_EQ(handoff::runtime().early_init(callbacks), "") << "Precondition: a valid handoff start must succeed.";
  EXPECT_TRUE(handoff::hooks::started_by_handoff()) << "started_by_handoff() must be true after a handoff start.";
  EXPECT_TRUE(handoff::hooks::enabled()) << "enabled() must be true after a handoff start (mode new_pending).";
  EXPECT_FALSE(handoff::hooks::draining()) << "draining() must be false for a new process.";
  EXPECT_EQ(handoff::hooks::generation(), 3u) << "generation() must be the inherited SUNSHINE_HANDOFF_GENERATION.";
  EXPECT_EQ(handoff::hooks::udp_base(), 48100) << "udp_base() must be the inherited UDP base, not the configured port.";
  ASSERT_TRUE(handoff::hooks::host_sink().has_value()) << "host_sink() must return the inherited host sink.";
  EXPECT_EQ(*handoff::hooks::host_sink(), "alsa_output.test-sink") << "host_sink() must return the passed sink name.";
}

/**
 * @brief `udp_base()` follows the configured port, and an override replaces it.
 */
TEST_F(HandoffHooksTest, UdpBaseFollowsConfiguredPortThenOverride) {
  config::sunshine.port = 50000;
  EXPECT_EQ(handoff::hooks::udp_base(), 50000) << "udp_base() must follow the configured port when no override is set.";
  handoff::runtime().set_udp_base_for_test(48100);
  EXPECT_EQ(handoff::hooks::udp_base(), 48100) << "udp_base() must return the override when one is set.";
}

// ---------------------------------------------------------------------------------------------------------------------
// restart_environment()
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief With the feature disabled both restart variables are unset, even if the process inherited a UDP base.
 *
 * A disabled process that inherited an override reverts to the configured port on restart.
 */
TEST_F(HandoffHooksTest, RestartEnvironmentDisabledUnsetsBothEvenWithInheritedBase) {
  fake_env_t env;
  env.vars["SUNSHINE_UDP_BASE"] = "48089";
  env.vars["SUNSHINE_GENERATION"] = "2";
  auto callbacks = env.make();
  ASSERT_EQ(handoff::runtime().early_init(callbacks), "") << "Precondition: a normal start must succeed.";
  ASSERT_FALSE(handoff::hooks::enabled()) << "Precondition: the feature must be disabled.";
  const auto restart = handoff::hooks::restart_environment();
  ASSERT_EQ(restart.size(), 2u) << "restart_environment() must always return two entries.";
  EXPECT_EQ(restart[0].first, "SUNSHINE_UDP_BASE") << "The first entry must be SUNSHINE_UDP_BASE.";
  EXPECT_FALSE(restart[0].second.has_value()) << "A disabled process must unset SUNSHINE_UDP_BASE on restart.";
  EXPECT_EQ(restart[1].first, "SUNSHINE_GENERATION") << "The second entry must be SUNSHINE_GENERATION.";
  EXPECT_FALSE(restart[1].second.has_value()) << "A disabled process must unset SUNSHINE_GENERATION on restart.";
}

/**
 * @brief An enabled process with the default base and generation 0 unsets both variables.
 */
TEST_F(HandoffHooksTest, RestartEnvironmentEnabledDefaultsUnsetBoth) {
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  const auto restart = handoff::hooks::restart_environment();
  ASSERT_EQ(restart.size(), 2u) << "restart_environment() must always return two entries when enabled.";
  EXPECT_EQ(restart[0].first, "SUNSHINE_UDP_BASE") << "The first entry must be SUNSHINE_UDP_BASE.";
  EXPECT_FALSE(restart[0].second.has_value()) << "With the default base SUNSHINE_UDP_BASE must be unset.";
  EXPECT_EQ(restart[1].first, "SUNSHINE_GENERATION") << "The second entry must be SUNSHINE_GENERATION.";
  EXPECT_FALSE(restart[1].second.has_value()) << "With generation 0 SUNSHINE_GENERATION must be unset.";
}

/**
 * @brief An overridden base that differs from the configured port is passed as a decimal value.
 */
TEST_F(HandoffHooksTest, RestartEnvironmentOverriddenBaseIsSetAsDecimal) {
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  handoff::runtime().set_udp_base_for_test(48089);
  const auto restart = handoff::hooks::restart_environment();
  ASSERT_EQ(restart.size(), 2u) << "restart_environment() must always return two entries when enabled.";
  ASSERT_TRUE(restart[0].second.has_value()) << "A base that differs from the configured port must be passed on restart.";
  EXPECT_EQ(*restart[0].second, "48089") << "SUNSHINE_UDP_BASE must be the decimal UDP base.";
  EXPECT_FALSE(restart[1].second.has_value()) << "Generation 0 must still leave SUNSHINE_GENERATION unset.";
}

/**
 * @brief An override equal to the configured port is not needed, so `SUNSHINE_UDP_BASE` is unset.
 */
TEST_F(HandoffHooksTest, RestartEnvironmentBaseEqualToPortIsUnset) {
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  handoff::runtime().set_udp_base_for_test(47989);
  const auto restart = handoff::hooks::restart_environment();
  ASSERT_EQ(restart.size(), 2u) << "restart_environment() must always return two entries when enabled.";
  EXPECT_FALSE(restart[0].second.has_value())
    << "A UDP base equal to the configured port must leave SUNSHINE_UDP_BASE unset.";
}

/**
 * @brief A generation above 0 inherited by an in-place restart is passed on as a decimal value.
 */
TEST_F(HandoffHooksTest, RestartEnvironmentGenerationAboveZeroIsSet) {
  fake_env_t env;
  env.vars["SUNSHINE_GENERATION"] = "2";
  env.vars["SUNSHINE_UDP_BASE"] = "48089";
  auto callbacks = env.make();
  ASSERT_EQ(handoff::runtime().early_init(callbacks), "") << "Precondition: a normal start must succeed.";
  handoff::runtime().configure([] {
    handoff::settings_t settings;
    settings.enabled = true;
    return settings;
  }());
  const auto restart = handoff::hooks::restart_environment();
  ASSERT_EQ(restart.size(), 2u) << "restart_environment() must always return two entries when enabled.";
  ASSERT_TRUE(restart[0].second.has_value()) << "The inherited base must be passed on restart.";
  EXPECT_EQ(*restart[0].second, "48089") << "The inherited UDP base must come back as SUNSHINE_UDP_BASE.";
  ASSERT_TRUE(restart[1].second.has_value()) << "A generation above 0 must be passed on restart.";
  EXPECT_EQ(*restart[1].second, "2") << "The inherited generation must come back as SUNSHINE_GENERATION=2.";
}

/**
 * @brief A handoff child keeps its inherited base and generation for a later in-place restart.
 */
TEST_F(HandoffHooksTest, RestartEnvironmentHandoffChildKeepsBaseAndGeneration) {
  ASSERT_TRUE(reach_mode(mode_e::new_pending)) << "Precondition: the runtime must reach mode new_pending.";
  const auto restart = handoff::hooks::restart_environment();
  ASSERT_EQ(restart.size(), 2u) << "restart_environment() must always return two entries when enabled.";
  ASSERT_TRUE(restart[0].second.has_value()) << "The handoff child's base (48100) differs from the port and must be passed on.";
  EXPECT_EQ(*restart[0].second, "48100") << "SUNSHINE_UDP_BASE must be the handoff child's base.";
  ASSERT_TRUE(restart[1].second.has_value()) << "The handoff child's generation 1 must be passed on.";
  EXPECT_EQ(*restart[1].second, "1") << "SUNSHINE_GENERATION must be the handoff child's generation.";
}

// ---------------------------------------------------------------------------------------------------------------------
// restart_blocked()
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief With the feature disabled a restart is never refused, whatever else is going on.
 */
TEST_F(HandoffHooksTest, RestartNeverRefusedWhileDisabled) {
  handoff::runtime().set_handoff_in_progress(true);
  handoff::runtime().set_peer(std::make_shared<fake_peer_t>(true));
  EXPECT_EQ(handoff::hooks::restart_blocked(), "")
    << "A disabled feature must never refuse a restart, even with a handoff in progress or a live peer.";
}

/**
 * @brief An idle active process may restart.
 */
TEST_F(HandoffHooksTest, RestartAllowedWhenIdleAndActive) {
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  EXPECT_EQ(handoff::hooks::restart_blocked(), "") << "An idle active process must be allowed to restart.";
}

/**
 * @brief A running handoff refuses the restart, and the text names the ways out.
 */
TEST_F(HandoffHooksTest, RestartRefusedWhileHandoffInProgressNamesWaysOut) {
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  handoff::runtime().set_handoff_in_progress(true);
  const auto text = handoff::hooks::restart_blocked();
  EXPECT_NE(text, "") << "A restart must be refused while a handoff is in progress.";
  EXPECT_TRUE(names_ways_out(text))
    << "The refusal must name the ways out (wait for the drain, SIGTERM to the draining pid, systemctl restart); got: "
    << text;
  EXPECT_TRUE(single_line(text)) << "The refusal text must be one line; got: " << text;
}

/**
 * @brief A live draining peer refuses the restart, and the text names the ways out.
 */
TEST_F(HandoffHooksTest, RestartRefusedWithLivePeerNamesWaysOut) {
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  handoff::runtime().set_peer(std::make_shared<fake_peer_t>(true));
  const auto text = handoff::hooks::restart_blocked();
  EXPECT_NE(text, "") << "A restart must be refused while a draining peer exists.";
  EXPECT_TRUE(names_ways_out(text))
    << "The refusal must name the ways out (wait for the drain, SIGTERM to the draining pid, systemctl restart); got: "
    << text;
}

/**
 * @brief A peer that is already gone does not block the restart.
 */
TEST_F(HandoffHooksTest, RestartAllowedWhenPeerIsGone) {
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  handoff::runtime().set_peer(std::make_shared<fake_peer_t>(false));
  EXPECT_EQ(handoff::hooks::restart_blocked(), "") << "A peer that no longer exists must not block a restart.";
}

/**
 * @brief The instance states `draining`, `handover_failed` and `kill_pending` refuse the restart.
 *
 */
TEST_F(HandoffHooksTest, RestartRefusedForDrainingInstanceStates) {
  // SPEC-QUESTION: restart_blocked() does not say which instance file its "instance state" is read from (this
  // process's own file, or a draining peer's file found by scanning the instances directory). This test sets the state
  // on this process's own instance file through the runtime, the only instance API a test can use.
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  handoff::instance_t initial;
  initial.pid = 4242;
  ASSERT_TRUE(handoff::runtime().open_instance(_dir / "instances", initial)) << "Precondition: open_instance must succeed.";
  for (const auto state : {handoff::state_e::draining, handoff::state_e::handover_failed, handoff::state_e::kill_pending}) {
    ASSERT_TRUE(handoff::runtime().instance_set_state(state)) << "Precondition: the state must be writable.";
    const auto text = handoff::hooks::restart_blocked();
    EXPECT_NE(text, "") << "A restart must be refused while the instance state is " << static_cast<int>(state)
                        << " (draining, handover_failed or kill_pending).";
    EXPECT_TRUE(names_ways_out(text)) << "The refusal for instance state " << static_cast<int>(state)
                                      << " must name the ways out; got: " << text;
  }
}

/**
 * @brief The instance state `none` does not refuse the restart.
 */
TEST_F(HandoffHooksTest, RestartAllowedForNoneInstanceState) {
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  handoff::instance_t initial;
  initial.pid = 4242;
  ASSERT_TRUE(handoff::runtime().open_instance(_dir / "instances", initial)) << "Precondition: open_instance must succeed.";
  EXPECT_EQ(handoff::hooks::restart_blocked(), "") << "An instance state of none must not refuse a restart.";
}

/**
 * @brief A draining process refuses the restart (it ignores restart).
 */
TEST_F(HandoffHooksTest, RestartRefusedWhileDraining) {
  ASSERT_TRUE(reach_mode(mode_e::draining)) << "Precondition: the runtime must reach mode draining.";
  const auto text = handoff::hooks::restart_blocked();
  EXPECT_NE(text, "") << "A draining process must refuse an in-place restart.";
  EXPECT_TRUE(single_line(text)) << "The refusal text must be one line; got: " << text;
}

/**
 * @brief A new process before COMMIT refuses the restart, since its exit would be a rollback.
 */
TEST_F(HandoffHooksTest, RestartRefusedWhileNewPending) {
  ASSERT_TRUE(reach_mode(mode_e::new_pending)) << "Precondition: the runtime must reach mode new_pending.";
  const auto text = handoff::hooks::restart_blocked();
  EXPECT_NE(text, "") << "A new_pending process must refuse an in-place restart.";
  EXPECT_TRUE(single_line(text)) << "The refusal text must be one line; got: " << text;
}

/**
 * @brief A new process after COMMIT but before MAIN refuses the restart.
 */
TEST_F(HandoffHooksTest, RestartRefusedWhileNewActive) {
  ASSERT_TRUE(reach_mode(mode_e::new_active)) << "Precondition: the runtime must reach mode new_active.";
  const auto text = handoff::hooks::restart_blocked();
  EXPECT_NE(text, "") << "A new_active process must refuse an in-place restart.";
  EXPECT_TRUE(single_line(text)) << "The refusal text must be one line; got: " << text;
}

/**
 * @brief Once the process is the main process (MAIN, new_active to normal) an idle restart is allowed again.
 */
TEST_F(HandoffHooksTest, RestartAllowedAfterMain) {
  ASSERT_TRUE(reach_mode(mode_e::new_active)) << "Precondition: the runtime must reach mode new_active.";
  ASSERT_TRUE(handoff::runtime().transition(mode_e::new_active, mode_e::normal)) << "Precondition: MAIN must move to normal.";
  EXPECT_EQ(handoff::hooks::restart_blocked(), "") << "After MAIN an idle active process must be allowed to restart.";
}

// ---------------------------------------------------------------------------------------------------------------------
// quit_blocked()
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief With the feature disabled Quit is never refused.
 */
TEST_F(HandoffHooksTest, QuitNeverRefusedWhileDisabled) {
  handoff::runtime().set_handoff_in_progress(true);
  EXPECT_EQ(handoff::hooks::quit_blocked(), "") << "A disabled feature must never refuse Quit.";
}

/**
 * @brief An idle active process may quit.
 */
TEST_F(HandoffHooksTest, QuitAllowedWhenIdleAndActive) {
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  EXPECT_EQ(handoff::hooks::quit_blocked(), "") << "An idle active process must be allowed to quit.";
}

/**
 * @brief Quit in the active role is allowed while a handoff is in progress (the SIGTERM aborts it).
 */
TEST_F(HandoffHooksTest, QuitAllowedWhileHandoffInProgress) {
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  handoff::runtime().set_handoff_in_progress(true);
  EXPECT_EQ(handoff::hooks::quit_blocked(), "") << "Quit in the active role must be allowed while a handoff is in progress.";
}

/**
 * @brief Quit in the active role is allowed while a draining process exists (systemd stops both).
 */
TEST_F(HandoffHooksTest, QuitAllowedWithLivePeer) {
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  handoff::runtime().set_peer(std::make_shared<fake_peer_t>(true));
  EXPECT_EQ(handoff::hooks::quit_blocked(), "") << "Quit in the active role must be allowed while a draining peer exists.";
}

/**
 * @brief A draining process refuses Quit, and the text is one line.
 */
TEST_F(HandoffHooksTest, QuitRefusedWhileDraining) {
  ASSERT_TRUE(reach_mode(mode_e::draining)) << "Precondition: the runtime must reach mode draining.";
  const auto text = handoff::hooks::quit_blocked();
  EXPECT_NE(text, "") << "A draining process must refuse Quit.";
  EXPECT_TRUE(single_line(text)) << "The refusal text must be one line; got: " << text;
}

/**
 * @brief A new process before COMMIT refuses Quit, since its exit would be a rollback rather than a stop.
 */
TEST_F(HandoffHooksTest, QuitRefusedWhileNewPending) {
  ASSERT_TRUE(reach_mode(mode_e::new_pending)) << "Precondition: the runtime must reach mode new_pending.";
  const auto text = handoff::hooks::quit_blocked();
  EXPECT_NE(text, "") << "A new_pending process must refuse Quit.";
  EXPECT_TRUE(single_line(text)) << "The refusal text must be one line; got: " << text;
}

/**
 * @brief A new process after COMMIT but before MAIN refuses Quit.
 */
TEST_F(HandoffHooksTest, QuitRefusedWhileNewActive) {
  ASSERT_TRUE(reach_mode(mode_e::new_active)) << "Precondition: the runtime must reach mode new_active.";
  const auto text = handoff::hooks::quit_blocked();
  EXPECT_NE(text, "") << "A new_active process must refuse Quit until it is the main process.";
  EXPECT_TRUE(single_line(text)) << "The refusal text must be one line; got: " << text;
}

/**
 * @brief After MAIN the new process accepts Quit like any active process.
 */
TEST_F(HandoffHooksTest, QuitAllowedAfterMain) {
  ASSERT_TRUE(reach_mode(mode_e::new_active)) << "Precondition: the runtime must reach mode new_active.";
  ASSERT_TRUE(handoff::runtime().transition(mode_e::new_active, mode_e::normal)) << "Precondition: MAIN must move to normal.";
  EXPECT_EQ(handoff::hooks::quit_blocked(), "") << "After MAIN Quit must be allowed.";
}

// ---------------------------------------------------------------------------------------------------------------------
// on_sigterm() and shutdown_reason()
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief `on_sigterm()` records the shutdown, latches `terminated` in mode normal, and makes the wake fd readable.
 */
TEST_F(HandoffHooksTest, OnSigtermRecordsShutdownInNormalMode) {
  ASSERT_TRUE(reach_mode(mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  handoff::hooks::on_sigterm();
  EXPECT_TRUE(handoff::runtime().shutdown_requested()) << "on_sigterm() must set shutdown_requested().";
  EXPECT_TRUE(readable(handoff::runtime().shutdown_wake_fd())) << "on_sigterm() must make the shutdown wake fd readable.";
  EXPECT_EQ(handoff::hooks::shutdown_reason(), shutdown_reason_e::terminated)
    << "A SIGTERM in mode normal must give the shutdown reason terminated.";
}

/**
 * @brief `on_sigterm()` while draining gives the shutdown reason `stop`, so the sessions end with `stop`.
 */
TEST_F(HandoffHooksTest, OnSigtermWhileDrainingGivesStopReason) {
  ASSERT_TRUE(reach_mode(mode_e::draining)) << "Precondition: the runtime must reach mode draining.";
  handoff::hooks::on_sigterm();
  EXPECT_EQ(handoff::hooks::shutdown_reason(), shutdown_reason_e::stop)
    << "A SIGTERM while draining must give the shutdown reason stop.";
}

/**
 * @brief Calling `on_sigterm()` twice changes nothing after the first call.
 */
TEST_F(HandoffHooksTest, OnSigtermIsIdempotent) {
  ASSERT_TRUE(reach_mode(mode_e::draining)) << "Precondition: the runtime must reach mode draining.";
  handoff::hooks::on_sigterm();
  handoff::hooks::on_sigterm();
  EXPECT_EQ(handoff::hooks::shutdown_reason(), shutdown_reason_e::stop)
    << "A second on_sigterm() must not change the latched stop reason.";
  EXPECT_TRUE(readable(handoff::runtime().shutdown_wake_fd())) << "The wake fd must stay readable after a second on_sigterm().";
}

#endif  // __linux__
