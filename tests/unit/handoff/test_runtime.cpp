/**
 * @file tests/unit/handoff/test_runtime.cpp
 * @brief Blind tests for the drain-handoff process state (src/handoff/runtime.h): modes and transitions, configuration,
 * the `early_init` environment rules, shutdown latching, the shutdown wake fd, instance-file write-through, the
 * channel and peer slots, and `reset_for_test()`.
 *
 * Written from `docs/drain-handoff.md` and the public headers only.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <chrono>
  #include <cstdint>
  #include <filesystem>
  #include <fstream>
  #include <map>
  #include <memory>
  #include <optional>
  #include <sstream>
  #include <string>
  #include <utility>
  #include <vector>

  // lib includes
  #include <poll.h>
  #include <unistd.h>

  // local includes
  #include "src/handoff/runtime.h"

namespace {
  using handoff::mode_e;
  using handoff::shutdown_reason_e;

  /**
   * @brief Fake process environment for `runtime_t::early_init()`.
   *
   * Values live in `vars`; `unsetenv` erases them and records the name, `scrub_fds` counts its calls.
   */
  struct fake_env_t {
    std::map<std::string, std::string> vars;  ///< Variables that are set.
    std::vector<std::string> unset_calls;  ///< Names given to `unsetenv`, in call order.
    int scrub_calls {0};  ///< Number of `scrub_fds` calls.

    /**
     * @brief Build the `early_init` callbacks over this object. The object must outlive the returned callbacks.
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
        unset_calls.emplace_back(name);
        vars.erase(name);
      };
      env.scrub_fds = [this] {
        ++scrub_calls;
      };
      return env;
    }
  };

  /**
   * @brief Fill a fake environment with a complete, valid handoff start (`SUNSHINE_HANDOFF_*`).
   *
   * @param env Environment to fill.
   */
  void set_valid_handoff_vars(fake_env_t &env) {
    env.vars["SUNSHINE_HANDOFF_FDS"] = "https:3,http:4,webui:5,rtsp:6";
    env.vars["SUNSHINE_HANDOFF_CHANNEL"] = "7";
    env.vars["SUNSHINE_HANDOFF_UDP_BASE"] = "48100";
    env.vars["SUNSHINE_HANDOFF_GENERATION"] = "1";
  }

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
   * @brief Drive a fresh runtime to `target` using only the documented steps (`configure`, `transition`, `early_init`).
   *
   * @param rt Runtime, expected to be freshly constructed (mode `disabled`).
   * @param target Mode to reach.
   * @return `true` when the runtime is in `target`.
   */
  bool reach_mode(handoff::runtime_t &rt, mode_e target) {
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
    set_valid_handoff_vars(env);
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

  /**
   * @brief Read an instance file and parse it.
   *
   * @param path File to read.
   * @return The parsed content, or `std::nullopt` when the file is missing or invalid.
   */
  std::optional<handoff::instance_t> read_instance_file(const std::filesystem::path &path) {
    std::ifstream in(path);
    if (!in) {
      return std::nullopt;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return handoff::parse_instance(buffer.str());
  }
}  // namespace

/**
 * @brief Fixture for the process-wide runtime: resets it before and after every test.
 */
class HandoffRuntimeTest: public ::testing::Test {
protected:
  /**
   * @brief Reset the process-wide runtime to its fresh state.
   */
  void SetUp() override {
    handoff::runtime().reset_for_test();
  }

  /**
   * @brief Reset the process-wide runtime again so later tests start clean.
   */
  void TearDown() override {
    handoff::runtime().reset_for_test();
  }
};

/**
 * @brief Fixture that also owns a temporary runtime directory for instance files.
 */
class HandoffRuntimeInstanceTest: public HandoffRuntimeTest {
protected:
  /**
   * @brief Create an empty temporary directory named after the running test.
   */
  void SetUp() override {
    HandoffRuntimeTest::SetUp();
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() /
           ("sunshine-handoff-runtime-" + std::to_string(::getpid()) + "-" + info->name());
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
    std::filesystem::create_directories(_dir, ec);
  }

  /**
   * @brief Remove the temporary directory and reset the runtime.
   */
  void TearDown() override {
    handoff::runtime().reset_for_test();
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
    HandoffRuntimeTest::TearDown();
  }

  std::filesystem::path _dir;  ///< Temporary runtime directory (created in `SetUp`).
};

// ---------------------------------------------------------------------------------------------------------------------
// Construction state and initial mode
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief A freshly constructed runtime is disabled, and `enabled()` is false until `configure()`.
 */
TEST_F(HandoffRuntimeTest, InitialModeIsDisabledAndNotEnabled) {
  handoff::runtime_t rt;
  EXPECT_EQ(rt.mode(), mode_e::disabled) << "A fresh runtime must start in mode disabled.";
  EXPECT_FALSE(rt.enabled()) << "enabled() must be false before configure() or a handoff start.";
  EXPECT_EQ(handoff::to_string(rt.mode()), "disabled") << "to_string of the initial mode must be 'disabled'.";
}

/**
 * @brief A freshly constructed runtime holds the documented defaults for every other member.
 */
TEST_F(HandoffRuntimeTest, FreshRuntimeHoldsDocumentedDefaults) {
  handoff::runtime_t rt;
  EXPECT_FALSE(rt.started_by_handoff()) << "A fresh runtime was not started by a handoff.";
  EXPECT_EQ(rt.generation(), 0u) << "A fresh runtime must have generation 0.";
  EXPECT_FALSE(rt.udp_base().has_value()) << "A fresh runtime must have no UDP base override.";
  EXPECT_TRUE(rt.is_main()) << "A fresh runtime is the main process.";
  EXPECT_FALSE(rt.frozen()) << "A fresh runtime is not frozen.";
  EXPECT_FALSE(rt.handoff_in_progress()) << "A fresh runtime has no handoff in progress.";
  EXPECT_FALSE(rt.shutdown_requested()) << "A fresh runtime has no shutdown requested.";
  EXPECT_FALSE(rt.inherited().has_value()) << "A fresh runtime has no inherited fds.";
  EXPECT_FALSE(rt.host_sink().has_value()) << "A fresh runtime has no host sink.";
  EXPECT_EQ(rt.instance_current(), std::nullopt) << "A fresh runtime has no instance writer.";
  EXPECT_EQ(rt.channel(), nullptr) << "A fresh runtime has no channel.";
  EXPECT_EQ(rt.peer(), nullptr) << "A fresh runtime has no draining peer.";
  EXPECT_EQ(rt.process_tag(), "") << "A non-draining process has an empty log tag.";
}

/**
 * @brief The default settings match the documented option defaults.
 */
TEST_F(HandoffRuntimeTest, DefaultSettingsMatchDocumentedDefaults) {
  handoff::runtime_t rt;
  const auto settings = rt.settings();
  EXPECT_FALSE(settings.enabled) << "drain_handoff must default to disabled.";
  EXPECT_EQ(settings.udp_offset, 100) << "drain_handoff_udp_offset must default to 100.";
  EXPECT_EQ(settings.deadline_s, 3600) << "drain_handoff_deadline must default to 3600 seconds.";
  EXPECT_EQ(settings.ready_timeout_s, 30) << "drain_handoff_ready_timeout must default to 30 seconds.";
  EXPECT_EQ(settings.rollback_window_s, 60) << "drain_handoff_rollback_window must default to 60 seconds.";
  EXPECT_EQ(settings.cooldown_s, 120) << "drain_handoff_cooldown must default to 120 seconds.";
  EXPECT_TRUE(settings.binary.empty()) << "drain_handoff_binary must default to empty (the start path).";
}

/**
 * @brief The shutdown eventfd is created by the constructor, so `shutdown_wake_fd()` is a valid descriptor.
 */
TEST_F(HandoffRuntimeTest, ConstructorCreatesShutdownWakeFd) {
  handoff::runtime_t rt;
  EXPECT_GE(rt.shutdown_wake_fd(), 0) << "The shutdown eventfd must be created in the constructor on Linux.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Mode names
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Each mode has its documented wire name.
 */
TEST_F(HandoffRuntimeTest, ToStringReturnsWireNames) {
  EXPECT_EQ(handoff::to_string(mode_e::disabled), "disabled") << "mode_e::disabled must print as 'disabled'.";
  EXPECT_EQ(handoff::to_string(mode_e::normal), "normal") << "mode_e::normal must print as 'normal'.";
  EXPECT_EQ(handoff::to_string(mode_e::new_pending), "new_pending") << "mode_e::new_pending must print as 'new_pending'.";
  EXPECT_EQ(handoff::to_string(mode_e::new_active), "new_active") << "mode_e::new_active must print as 'new_active'.";
  EXPECT_EQ(handoff::to_string(mode_e::draining), "draining") << "mode_e::draining must print as 'draining'.";
}

// ---------------------------------------------------------------------------------------------------------------------
// configure() and disable()
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief `configure()` with the feature enabled on a normal start moves the mode to `normal`.
 */
TEST_F(HandoffRuntimeTest, ConfigureEnabledMovesDisabledToNormal) {
  handoff::runtime_t rt;
  handoff::settings_t settings;
  settings.enabled = true;
  rt.configure(settings);
  EXPECT_EQ(rt.mode(), mode_e::normal) << "configure() with enabled=true must set mode normal.";
  EXPECT_TRUE(rt.enabled()) << "enabled() must be true in mode normal.";
}

/**
 * @brief `configure()` with the feature disabled keeps the mode `disabled`.
 */
TEST_F(HandoffRuntimeTest, ConfigureDisabledKeepsDisabled) {
  handoff::runtime_t rt;
  handoff::settings_t settings;
  settings.enabled = false;
  rt.configure(settings);
  EXPECT_EQ(rt.mode(), mode_e::disabled) << "configure() with enabled=false must leave the mode disabled.";
  EXPECT_FALSE(rt.enabled()) << "enabled() must be false when the feature is not configured on.";
}

/**
 * @brief `configure()` stores every field of the settings it is given.
 */
TEST_F(HandoffRuntimeTest, ConfigureStoresEveryField) {
  handoff::runtime_t rt;
  handoff::settings_t settings;
  settings.enabled = true;
  settings.udp_offset = 200;
  settings.deadline_s = 90;
  settings.ready_timeout_s = 12;
  settings.rollback_window_s = 15;
  settings.cooldown_s = 45;
  settings.binary = "/opt/sunshine/sunshine";
  rt.configure(settings);
  const auto stored = rt.settings();
  EXPECT_TRUE(stored.enabled) << "settings().enabled must hold the configured value.";
  EXPECT_EQ(stored.udp_offset, 200) << "settings().udp_offset must hold the configured value.";
  EXPECT_EQ(stored.deadline_s, 90) << "settings().deadline_s must hold the configured value.";
  EXPECT_EQ(stored.ready_timeout_s, 12) << "settings().ready_timeout_s must hold the configured value.";
  EXPECT_EQ(stored.rollback_window_s, 15) << "settings().rollback_window_s must hold the configured value.";
  EXPECT_EQ(stored.cooldown_s, 45) << "settings().cooldown_s must hold the configured value.";
  EXPECT_EQ(stored.binary, "/opt/sunshine/sunshine") << "settings().binary must hold the configured value.";
}

/**
 * @brief `configure()` from mode `normal` with the feature disabled returns the mode to `disabled`.
 */
TEST_F(HandoffRuntimeTest, ConfigureDisabledAfterNormalReturnsToDisabled) {
  handoff::runtime_t rt;
  handoff::settings_t on;
  on.enabled = true;
  rt.configure(on);
  ASSERT_EQ(rt.mode(), mode_e::normal) << "Precondition: configure(enabled) must reach normal.";
  handoff::settings_t off;
  off.enabled = false;
  rt.configure(off);
  EXPECT_EQ(rt.mode(), mode_e::disabled) << "configure(enabled=false) from normal must return the mode to disabled.";
  EXPECT_FALSE(rt.settings().enabled) << "The stored enabled flag must follow the latest configure().";
}

/**
 * @brief `configure()` may be called again while the mode is still `disabled`, and enabling later works.
 */
TEST_F(HandoffRuntimeTest, ConfigureCanBeCalledAgainBeforeEnabling) {
  handoff::runtime_t rt;
  handoff::settings_t off;
  off.enabled = false;
  rt.configure(off);
  handoff::settings_t on;
  on.enabled = true;
  rt.configure(on);
  EXPECT_EQ(rt.mode(), mode_e::normal) << "A later configure(enabled) after a disabled one must reach mode normal.";
}

/**
 * @brief During a handoff (mode `draining`), `configure()` only stores the settings and leaves the mode alone.
 */
TEST_F(HandoffRuntimeTest, ConfigureWhileDrainingOnlyStoresSettings) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::draining)) << "Precondition: the runtime must reach mode draining.";
  handoff::settings_t settings;
  settings.enabled = false;
  settings.deadline_s = 77;
  rt.configure(settings);
  EXPECT_EQ(rt.mode(), mode_e::draining) << "configure() during a draining handoff must not change the mode.";
  EXPECT_EQ(rt.settings().deadline_s, 77) << "configure() during a draining handoff must still store the settings.";
}

/**
 * @brief After a handoff start (mode `new_pending`), `configure()` leaves the mode unchanged.
 */
TEST_F(HandoffRuntimeTest, ConfigureAfterHandoffStartKeepsNewPending) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::new_pending)) << "Precondition: the runtime must reach mode new_pending.";
  handoff::settings_t settings;
  settings.enabled = false;
  rt.configure(settings);
  EXPECT_EQ(rt.mode(), mode_e::new_pending) << "configure() after a handoff start must leave new_pending unchanged.";
  EXPECT_TRUE(rt.enabled()) << "A handoff start keeps enabled() true, whatever configure() stores.";
}

/**
 * @brief `disable()` from mode `normal` turns the feature off and clears the stored enabled flag.
 */
TEST_F(HandoffRuntimeTest, DisableFromNormalTurnsFeatureOff) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  EXPECT_TRUE(rt.disable()) << "disable() in mode normal must succeed.";
  EXPECT_EQ(rt.mode(), mode_e::disabled) << "disable() must set the mode to disabled.";
  EXPECT_FALSE(rt.enabled()) << "enabled() must be false after disable().";
  EXPECT_FALSE(rt.settings().enabled) << "disable() must set settings().enabled to false.";
}

/**
 * @brief `disable()` in mode `disabled` succeeds and changes nothing.
 */
TEST_F(HandoffRuntimeTest, DisableFromDisabledSucceeds) {
  handoff::runtime_t rt;
  EXPECT_TRUE(rt.disable()) << "disable() in mode disabled must report success.";
  EXPECT_EQ(rt.mode(), mode_e::disabled) << "disable() in mode disabled must leave the mode disabled.";
}

/**
 * @brief `disable()` is refused while a handoff is running (mode `draining`), and the mode is unchanged.
 */
TEST_F(HandoffRuntimeTest, DisableRefusedWhileDraining) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::draining)) << "Precondition: the runtime must reach mode draining.";
  EXPECT_FALSE(rt.disable()) << "disable() must be refused while the mode is draining.";
  EXPECT_EQ(rt.mode(), mode_e::draining) << "A refused disable() must leave the mode draining.";
}

/**
 * @brief `disable()` is refused after a handoff start (mode `new_pending`), and the mode is unchanged.
 */
TEST_F(HandoffRuntimeTest, DisableRefusedWhileNewPending) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::new_pending)) << "Precondition: the runtime must reach mode new_pending.";
  EXPECT_FALSE(rt.disable()) << "disable() must be refused while the mode is new_pending.";
  EXPECT_EQ(rt.mode(), mode_e::new_pending) << "A refused disable() must leave the mode new_pending.";
}

// ---------------------------------------------------------------------------------------------------------------------
// transition()
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief The edge `normal` to `draining` is legal and changes the mode.
 */
TEST_F(HandoffRuntimeTest, TransitionNormalToDrainingIsLegal) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  EXPECT_TRUE(rt.transition(mode_e::normal, mode_e::draining)) << "normal to draining must be a legal transition.";
  EXPECT_EQ(rt.mode(), mode_e::draining) << "A legal transition must change the mode to the target.";
}

/**
 * @brief The rollback edge `draining` to `normal` is legal.
 */
TEST_F(HandoffRuntimeTest, TransitionDrainingBackToNormalIsLegal) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::draining)) << "Precondition: the runtime must reach mode draining.";
  EXPECT_TRUE(rt.transition(mode_e::draining, mode_e::normal)) << "draining to normal (rollback) must be legal.";
  EXPECT_EQ(rt.mode(), mode_e::normal) << "After the rollback the mode must be normal.";
}

/**
 * @brief The edge `new_pending` to `new_active` (COMMIT) is legal.
 */
TEST_F(HandoffRuntimeTest, TransitionNewPendingToNewActiveIsLegal) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::new_pending)) << "Precondition: the runtime must reach mode new_pending.";
  EXPECT_TRUE(rt.transition(mode_e::new_pending, mode_e::new_active)) << "new_pending to new_active must be legal.";
  EXPECT_EQ(rt.mode(), mode_e::new_active) << "After COMMIT the mode must be new_active.";
}

/**
 * @brief The edge `new_active` to `normal` (MAIN) is legal.
 */
TEST_F(HandoffRuntimeTest, TransitionNewActiveToNormalIsLegal) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::new_active)) << "Precondition: the runtime must reach mode new_active.";
  EXPECT_TRUE(rt.transition(mode_e::new_active, mode_e::normal)) << "new_active to normal (MAIN) must be legal.";
  EXPECT_EQ(rt.mode(), mode_e::normal) << "After MAIN the mode must be normal.";
}

/**
 * @brief Every edge that is not documented as legal is refused, and the mode stays where it was.
 *
 * The test drives the runtime to each source mode with documented steps, then tries each illegal target.
 */
TEST_F(HandoffRuntimeTest, TransitionRefusesIllegalEdgesAndKeepsMode) {
  const std::vector<std::pair<mode_e, mode_e>> illegal = {
    {mode_e::disabled, mode_e::normal},
    {mode_e::disabled, mode_e::draining},
    {mode_e::disabled, mode_e::new_pending},
    {mode_e::disabled, mode_e::new_active},
    {mode_e::disabled, mode_e::disabled},
    {mode_e::normal, mode_e::disabled},
    {mode_e::normal, mode_e::normal},
    {mode_e::normal, mode_e::new_pending},
    {mode_e::normal, mode_e::new_active},
    {mode_e::draining, mode_e::disabled},
    {mode_e::draining, mode_e::draining},
    {mode_e::draining, mode_e::new_pending},
    {mode_e::draining, mode_e::new_active},
    {mode_e::new_pending, mode_e::disabled},
    {mode_e::new_pending, mode_e::normal},
    {mode_e::new_pending, mode_e::draining},
    {mode_e::new_pending, mode_e::new_pending},
    {mode_e::new_active, mode_e::disabled},
    {mode_e::new_active, mode_e::draining},
    {mode_e::new_active, mode_e::new_pending},
    {mode_e::new_active, mode_e::new_active},
  };
  for (const auto &[from, to] : illegal) {
    handoff::runtime_t rt;
    ASSERT_TRUE(reach_mode(rt, from)) << "Precondition: the runtime must reach mode " << handoff::to_string(from);
    EXPECT_FALSE(rt.transition(from, to))
      << "transition from " << handoff::to_string(from) << " to " << handoff::to_string(to) << " must be refused.";
    EXPECT_EQ(rt.mode(), from)
      << "A refused transition from " << handoff::to_string(from) << " to " << handoff::to_string(to)
      << " must leave the mode unchanged.";
  }
}

/**
 * @brief A transition whose expected source mode does not match the current mode is refused.
 */
TEST_F(HandoffRuntimeTest, TransitionRefusedWhenExpectedFromDoesNotMatch) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  EXPECT_FALSE(rt.transition(mode_e::draining, mode_e::normal))
    << "transition(draining, normal) must be refused when the current mode is normal.";
  EXPECT_FALSE(rt.transition(mode_e::new_pending, mode_e::new_active))
    << "transition(new_pending, new_active) must be refused when the current mode is normal.";
  EXPECT_EQ(rt.mode(), mode_e::normal) << "Refused transitions with a wrong expected mode must leave normal in place.";
}

/**
 * @brief `disabled` and `normal` change only through `configure()` and `disable()`, never through `transition()`.
 */
TEST_F(HandoffRuntimeTest, TransitionCannotEnterOrLeaveDisabledViaTransition) {
  handoff::runtime_t rt;
  EXPECT_FALSE(rt.transition(mode_e::disabled, mode_e::normal))
    << "transition(disabled, normal) must be refused; only configure() may enable the feature.";
  EXPECT_EQ(rt.mode(), mode_e::disabled) << "The refused transition must leave the mode disabled.";
}

// ---------------------------------------------------------------------------------------------------------------------
// early_init(): the environment rules
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Both handoff variables unset is a normal start: no error, no handoff, mode still `disabled`.
 */
TEST_F(HandoffRuntimeTest, EarlyInitWithNoVariablesIsNormalStart) {
  handoff::runtime_t rt;
  fake_env_t env;
  auto callbacks = env.make();
  const auto error = rt.early_init(callbacks);
  EXPECT_EQ(error, "") << "A normal start (no handoff variables) must not report an error.";
  EXPECT_FALSE(rt.started_by_handoff()) << "A normal start must not be marked as started by a handoff.";
  EXPECT_EQ(rt.mode(), mode_e::disabled) << "A normal start must leave the mode disabled until configure().";
  EXPECT_TRUE(rt.is_main()) << "A normal start is the main process.";
  EXPECT_FALSE(rt.inherited().has_value()) << "A normal start must hold no inherited fds.";
  EXPECT_EQ(env.scrub_calls, 1) << "A normal start must call scrub_fds exactly once.";
}

/**
 * @brief A normal start reads `SUNSHINE_UDP_BASE` and `SUNSHINE_GENERATION` and unsets both afterwards.
 */
TEST_F(HandoffRuntimeTest, EarlyInitNormalStartReadsAndUnsetsUdpBaseAndGeneration) {
  handoff::runtime_t rt;
  fake_env_t env;
  env.vars["SUNSHINE_UDP_BASE"] = "48089";
  env.vars["SUNSHINE_GENERATION"] = "2";
  auto callbacks = env.make();
  EXPECT_EQ(rt.early_init(callbacks), "") << "A normal start with valid restart variables must not report an error.";
  ASSERT_TRUE(rt.udp_base().has_value()) << "SUNSHINE_UDP_BASE must be read on a normal start.";
  EXPECT_EQ(*rt.udp_base(), 48089) << "The UDP base must be the decimal value of SUNSHINE_UDP_BASE.";
  EXPECT_EQ(rt.generation(), 2u) << "The generation must be the decimal value of SUNSHINE_GENERATION.";
  EXPECT_FALSE(env.vars.contains("SUNSHINE_UDP_BASE")) << "SUNSHINE_UDP_BASE must be removed after early_init.";
  EXPECT_FALSE(env.vars.contains("SUNSHINE_GENERATION")) << "SUNSHINE_GENERATION must be removed after early_init.";
  EXPECT_EQ(env.unset_calls.size(), 2u) << "early_init must remove the two restart variables with unsetenv.";
  EXPECT_EQ(rt.mode(), mode_e::disabled) << "A normal start with restart variables must still leave the mode disabled.";
  EXPECT_FALSE(rt.started_by_handoff()) << "Restart variables alone must not mark the process as a handoff start.";
}

/**
 * @brief Generation 0 is accepted for a normal start, and the generation is then 0.
 */
TEST_F(HandoffRuntimeTest, EarlyInitNormalStartAcceptsGenerationZero) {
  handoff::runtime_t rt;
  fake_env_t env;
  env.vars["SUNSHINE_GENERATION"] = "0";
  auto callbacks = env.make();
  EXPECT_EQ(rt.early_init(callbacks), "") << "SUNSHINE_GENERATION=0 must be accepted on a normal start.";
  EXPECT_EQ(rt.generation(), 0u) << "SUNSHINE_GENERATION=0 must leave the generation at 0.";
}

/**
 * @brief The inclusive boundaries of the normal-start variables are accepted: UDP base 1 and 65535, generation 65535.
 */
TEST_F(HandoffRuntimeTest, EarlyInitNormalStartAcceptsBoundaryValues) {
  {
    handoff::runtime_t rt;
    fake_env_t env;
    env.vars["SUNSHINE_UDP_BASE"] = "1";
    auto callbacks = env.make();
    rt.early_init(callbacks);
    ASSERT_TRUE(rt.udp_base().has_value()) << "SUNSHINE_UDP_BASE=1 is in range (1 to 65524) and must be accepted.";
    EXPECT_EQ(*rt.udp_base(), 1) << "SUNSHINE_UDP_BASE=1 must become UDP base 1.";
  }
  {
    handoff::runtime_t rt;
    fake_env_t env;
    env.vars["SUNSHINE_UDP_BASE"] = "65524";
    env.vars["SUNSHINE_GENERATION"] = "65535";
    auto callbacks = env.make();
    rt.early_init(callbacks);
    ASSERT_TRUE(rt.udp_base().has_value()) << "SUNSHINE_UDP_BASE=65524 is the upper boundary (1 to 65524) and must be accepted.";
    EXPECT_EQ(*rt.udp_base(), 65524) << "SUNSHINE_UDP_BASE=65524 must become UDP base 65524.";
    EXPECT_EQ(rt.generation(), 65535u) << "SUNSHINE_GENERATION=65535 must become generation 65535.";
  }
  for (const char *value : {"65525", "65535"}) {
    handoff::runtime_t rt;
    fake_env_t env;
    env.vars["SUNSHINE_UDP_BASE"] = value;
    auto callbacks = env.make();
    EXPECT_EQ(rt.early_init(callbacks), "") << "SUNSHINE_UDP_BASE=" << value << " is out of range on a normal start and must be ignored, not an error.";
    EXPECT_FALSE(rt.udp_base().has_value()) << "SUNSHINE_UDP_BASE=" << value << " is out of range (1 to 65524) and must not become an override.";
    EXPECT_FALSE(env.vars.contains("SUNSHINE_UDP_BASE")) << "SUNSHINE_UDP_BASE=" << value << " must still be removed.";
  }
}

/**
 * @brief Invalid normal-start values for `SUNSHINE_UDP_BASE` are ignored (no override), still removed, and not an error.
 */
TEST_F(HandoffRuntimeTest, EarlyInitNormalStartIgnoresInvalidUdpBase) {
  const std::vector<std::string> bad = {"", "abc", "0", "65536", "-5", "4810x"};
  for (const auto &value : bad) {
    handoff::runtime_t rt;
    fake_env_t env;
    env.vars["SUNSHINE_UDP_BASE"] = value;
    auto callbacks = env.make();
    EXPECT_EQ(rt.early_init(callbacks), "")
      << "An invalid SUNSHINE_UDP_BASE ('" << value << "') on a normal start must be ignored, not reported as an error.";
    EXPECT_FALSE(rt.udp_base().has_value())
      << "An invalid SUNSHINE_UDP_BASE ('" << value << "') must not become a UDP base override.";
    EXPECT_FALSE(env.vars.contains("SUNSHINE_UDP_BASE"))
      << "SUNSHINE_UDP_BASE ('" << value << "') must still be removed after early_init.";
    EXPECT_EQ(rt.mode(), mode_e::disabled) << "An ignored SUNSHINE_UDP_BASE must not change the mode.";
  }
}

/**
 * @brief Invalid normal-start values for `SUNSHINE_GENERATION` are ignored (generation stays 0), and not an error.
 */
TEST_F(HandoffRuntimeTest, EarlyInitNormalStartIgnoresInvalidGeneration) {
  const std::vector<std::string> bad = {"", "abc", "65536", "-1"};
  for (const auto &value : bad) {
    handoff::runtime_t rt;
    fake_env_t env;
    env.vars["SUNSHINE_GENERATION"] = value;
    auto callbacks = env.make();
    EXPECT_EQ(rt.early_init(callbacks), "")
      << "An invalid SUNSHINE_GENERATION ('" << value << "') on a normal start must be ignored, not an error.";
    EXPECT_EQ(rt.generation(), 0u)
      << "An invalid SUNSHINE_GENERATION ('" << value << "') must leave the generation at 0.";
    EXPECT_FALSE(env.vars.contains("SUNSHINE_GENERATION"))
      << "SUNSHINE_GENERATION ('" << value << "') must still be removed after early_init.";
  }
}

/**
 * @brief Only `SUNSHINE_HANDOFF_FDS` set (no channel) is an error: nothing is stored and `scrub_fds` does not run.
 */
TEST_F(HandoffRuntimeTest, EarlyInitOnlyFdsSetIsError) {
  handoff::runtime_t rt;
  fake_env_t env;
  env.vars["SUNSHINE_HANDOFF_FDS"] = "https:3,http:4,webui:5,rtsp:6";
  auto callbacks = env.make();
  const auto error = rt.early_init(callbacks);
  EXPECT_NE(error, "") << "Only SUNSHINE_HANDOFF_FDS set must be reported as an error.";
  EXPECT_FALSE(rt.started_by_handoff()) << "A failed early_init must not mark the process as a handoff start.";
  EXPECT_EQ(rt.mode(), mode_e::disabled) << "A failed early_init must leave the mode disabled.";
  EXPECT_FALSE(rt.inherited().has_value()) << "A failed early_init must store no inherited fds.";
  EXPECT_EQ(env.scrub_calls, 0) << "A failed early_init must not call scrub_fds.";
}

/**
 * @brief Only `SUNSHINE_HANDOFF_CHANNEL` set (no fds) is an error.
 */
TEST_F(HandoffRuntimeTest, EarlyInitOnlyChannelSetIsError) {
  handoff::runtime_t rt;
  fake_env_t env;
  env.vars["SUNSHINE_HANDOFF_CHANNEL"] = "7";
  auto callbacks = env.make();
  EXPECT_NE(rt.early_init(callbacks), "") << "Only SUNSHINE_HANDOFF_CHANNEL set must be reported as an error.";
  EXPECT_EQ(rt.mode(), mode_e::disabled) << "A failed early_init must leave the mode disabled.";
  EXPECT_EQ(env.scrub_calls, 0) << "A failed early_init must not call scrub_fds.";
}

/**
 * @brief Both handoff fd variables set but not accepted by the parser is an error.
 */
TEST_F(HandoffRuntimeTest, EarlyInitInvalidFdsIsError) {
  const std::vector<std::pair<std::string, std::string>> bad = {
    {"https:3,http:4,webui:5", "7"},  // rtsp missing
    {"https:3,http:4,webui:5,rtsp:6", "2"},  // channel below 3
    {"https:3,http:3,webui:5,rtsp:6", "7"},  // repeated fd
    {"https:3,http:4,webui:5,bogus:6", "7"},  // unknown name
  };
  for (const auto &[fds, channel] : bad) {
    handoff::runtime_t rt;
    fake_env_t env;
    env.vars["SUNSHINE_HANDOFF_FDS"] = fds;
    env.vars["SUNSHINE_HANDOFF_CHANNEL"] = channel;
    env.vars["SUNSHINE_HANDOFF_UDP_BASE"] = "48100";
    env.vars["SUNSHINE_HANDOFF_GENERATION"] = "1";
    auto callbacks = env.make();
    EXPECT_NE(rt.early_init(callbacks), "")
      << "Fds '" << fds << "' with channel '" << channel << "' must be reported as an error.";
    EXPECT_EQ(rt.mode(), mode_e::disabled) << "A rejected fd set must leave the mode disabled.";
    EXPECT_FALSE(rt.inherited().has_value()) << "A rejected fd set must store no inherited fds.";
    EXPECT_EQ(env.scrub_calls, 0) << "A rejected fd set must not call scrub_fds.";
  }
}

/**
 * @brief A handoff start without `SUNSHINE_HANDOFF_UDP_BASE` is an error.
 */
TEST_F(HandoffRuntimeTest, EarlyInitMissingHandoffUdpBaseIsError) {
  handoff::runtime_t rt;
  fake_env_t env;
  set_valid_handoff_vars(env);
  env.vars.erase("SUNSHINE_HANDOFF_UDP_BASE");
  auto callbacks = env.make();
  EXPECT_NE(rt.early_init(callbacks), "") << "A handoff start without SUNSHINE_HANDOFF_UDP_BASE must be an error.";
  EXPECT_EQ(rt.mode(), mode_e::disabled) << "A failed handoff start must leave the mode disabled.";
  EXPECT_FALSE(rt.started_by_handoff()) << "A failed handoff start must not be marked as a handoff start.";
  EXPECT_EQ(env.scrub_calls, 0) << "A failed handoff start must not call scrub_fds.";
}

/**
 * @brief A handoff start with an out-of-range or non-decimal `SUNSHINE_HANDOFF_UDP_BASE` is an error.
 */
TEST_F(HandoffRuntimeTest, EarlyInitInvalidHandoffUdpBaseIsError) {
  for (const std::string value : {"0", "65536", "abc", ""}) {
    handoff::runtime_t rt;
    fake_env_t env;
    set_valid_handoff_vars(env);
    env.vars["SUNSHINE_HANDOFF_UDP_BASE"] = value;
    auto callbacks = env.make();
    EXPECT_NE(rt.early_init(callbacks), "")
      << "SUNSHINE_HANDOFF_UDP_BASE='" << value << "' must be reported as an error (valid range is 1 to 65535).";
    EXPECT_EQ(rt.mode(), mode_e::disabled) << "A rejected UDP base must leave the mode disabled.";
    EXPECT_FALSE(rt.udp_base().has_value()) << "A rejected UDP base must not be stored.";
  }
}

/**
 * @brief A handoff start without `SUNSHINE_HANDOFF_GENERATION` is an error.
 */
TEST_F(HandoffRuntimeTest, EarlyInitMissingHandoffGenerationIsError) {
  handoff::runtime_t rt;
  fake_env_t env;
  set_valid_handoff_vars(env);
  env.vars.erase("SUNSHINE_HANDOFF_GENERATION");
  auto callbacks = env.make();
  EXPECT_NE(rt.early_init(callbacks), "") << "A handoff start without SUNSHINE_HANDOFF_GENERATION must be an error.";
  EXPECT_EQ(rt.mode(), mode_e::disabled) << "A failed handoff start must leave the mode disabled.";
  EXPECT_EQ(env.scrub_calls, 0) << "A failed handoff start must not call scrub_fds.";
}

/**
 * @brief A handoff start with an invalid `SUNSHINE_HANDOFF_GENERATION` is an error (a handoff child needs 1 to 65535).
 */
TEST_F(HandoffRuntimeTest, EarlyInitInvalidHandoffGenerationIsError) {
  for (const std::string value : {"0", "65536", "x"}) {
    handoff::runtime_t rt;
    fake_env_t env;
    set_valid_handoff_vars(env);
    env.vars["SUNSHINE_HANDOFF_GENERATION"] = value;
    auto callbacks = env.make();
    EXPECT_NE(rt.early_init(callbacks), "")
      << "SUNSHINE_HANDOFF_GENERATION='" << value << "' must be reported as an error (valid range is 1 to 65535).";
    EXPECT_EQ(rt.mode(), mode_e::disabled) << "A rejected generation must leave the mode disabled.";
    EXPECT_EQ(rt.generation(), 0u) << "A rejected generation must not be stored.";
  }
}

/**
 * @brief A complete, valid handoff start: a `new_pending` process with the inherited fds, UDP base and generation.
 */
TEST_F(HandoffRuntimeTest, EarlyInitValidHandoffStartSetsNewPendingAndInheritedState) {
  handoff::runtime_t rt;
  fake_env_t env;
  set_valid_handoff_vars(env);
  auto callbacks = env.make();
  EXPECT_EQ(rt.early_init(callbacks), "") << "A complete, valid handoff start must not report an error.";
  EXPECT_TRUE(rt.started_by_handoff()) << "A valid handoff start must be marked as started by a handoff.";
  EXPECT_EQ(rt.mode(), mode_e::new_pending) << "A valid handoff start must set the mode to new_pending.";
  EXPECT_TRUE(rt.enabled()) << "A valid handoff start must make enabled() true.";
  EXPECT_FALSE(rt.is_main()) << "A handoff child is not the main process until MAIN.";
  EXPECT_EQ(rt.generation(), 1u) << "SUNSHINE_HANDOFF_GENERATION=1 must become generation 1.";
  ASSERT_TRUE(rt.udp_base().has_value()) << "SUNSHINE_HANDOFF_UDP_BASE must become the UDP base override.";
  EXPECT_EQ(*rt.udp_base(), 48100) << "SUNSHINE_HANDOFF_UDP_BASE=48100 must become UDP base 48100.";
  ASSERT_TRUE(rt.inherited().has_value()) << "A valid handoff start must hold the inherited fds.";
  EXPECT_EQ(rt.inherited()->https, 3) << "The https fd must be 3 as passed.";
  EXPECT_EQ(rt.inherited()->http, 4) << "The http fd must be 4 as passed.";
  EXPECT_EQ(rt.inherited()->webui, 5) << "The webui fd must be 5 as passed.";
  EXPECT_EQ(rt.inherited()->rtsp, 6) << "The rtsp fd must be 6 as passed.";
  EXPECT_EQ(rt.inherited()->channel, 7) << "The channel fd must be 7 as passed.";
  EXPECT_EQ(env.scrub_calls, 1) << "A valid handoff start must call scrub_fds exactly once.";
}

/**
 * @brief A valid handoff start copies every value before the variables are removed, and removes the restart variables.
 */
TEST_F(HandoffRuntimeTest, EarlyInitHandoffStartCopiesValuesAndUnsetsRestartVariables) {
  handoff::runtime_t rt;
  fake_env_t env;
  set_valid_handoff_vars(env);
  auto callbacks = env.make();
  ASSERT_EQ(rt.early_init(callbacks), "") << "Precondition: a valid handoff start must succeed.";
  // The fake unsetenv erases map entries, so any value read after unsetenv would be gone. Values must still be right.
  EXPECT_EQ(rt.generation(), 1u) << "The generation copied from the environment must survive the variable removal.";
  ASSERT_TRUE(rt.udp_base().has_value()) << "The UDP base copied from the environment must survive the removal.";
  EXPECT_EQ(*rt.udp_base(), 48100) << "The copied UDP base must be unchanged after the environment was cleaned.";
  EXPECT_NE(std::find(env.unset_calls.begin(), env.unset_calls.end(), "SUNSHINE_UDP_BASE"), env.unset_calls.end())
    << "early_init must call unsetenv for SUNSHINE_UDP_BASE on a non-error path.";
  EXPECT_NE(std::find(env.unset_calls.begin(), env.unset_calls.end(), "SUNSHINE_GENERATION"), env.unset_calls.end())
    << "early_init must call unsetenv for SUNSHINE_GENERATION on a non-error path.";
}

/**
 * @brief The optional host sink is stored from `SUNSHINE_HANDOFF_HOST_SINK`.
 */
TEST_F(HandoffRuntimeTest, EarlyInitHandoffStartStoresHostSink) {
  handoff::runtime_t rt;
  fake_env_t env;
  set_valid_handoff_vars(env);
  env.vars["SUNSHINE_HANDOFF_HOST_SINK"] = "alsa_output.test-sink";
  auto callbacks = env.make();
  ASSERT_EQ(rt.early_init(callbacks), "") << "Precondition: a valid handoff start must succeed.";
  ASSERT_TRUE(rt.host_sink().has_value()) << "A set SUNSHINE_HANDOFF_HOST_SINK must be kept for host_sink().";
  EXPECT_EQ(*rt.host_sink(), "alsa_output.test-sink") << "host_sink() must return the passed sink name.";
}

/**
 * @brief An empty or absent `SUNSHINE_HANDOFF_HOST_SINK` leaves `host_sink()` empty, and the handoff start still works.
 */
TEST_F(HandoffRuntimeTest, EarlyInitHandoffStartWithEmptyOrMissingHostSinkHasNoSink) {
  {
    handoff::runtime_t rt;
    fake_env_t env;
    set_valid_handoff_vars(env);
    env.vars["SUNSHINE_HANDOFF_HOST_SINK"] = "";
    auto callbacks = env.make();
    EXPECT_EQ(rt.early_init(callbacks), "") << "An empty SUNSHINE_HANDOFF_HOST_SINK is optional and must not be an error.";
    EXPECT_FALSE(rt.host_sink().has_value()) << "An empty SUNSHINE_HANDOFF_HOST_SINK must give no host sink.";
    EXPECT_EQ(rt.mode(), mode_e::new_pending) << "A handoff start with an empty host sink must still reach new_pending.";
  }
  {
    handoff::runtime_t rt;
    fake_env_t env;
    set_valid_handoff_vars(env);
    auto callbacks = env.make();
    EXPECT_EQ(rt.early_init(callbacks), "") << "A missing SUNSHINE_HANDOFF_HOST_SINK is optional and must not be an error.";
    EXPECT_FALSE(rt.host_sink().has_value()) << "A missing SUNSHINE_HANDOFF_HOST_SINK must give no host sink.";
  }
}

/**
 * @brief A normal start never reports a host sink, even if the variable is present.
 */
TEST_F(HandoffRuntimeTest, EarlyInitNormalStartHasNoHostSink) {
  handoff::runtime_t rt;
  fake_env_t env;
  auto callbacks = env.make();
  rt.early_init(callbacks);
  EXPECT_FALSE(rt.host_sink().has_value()) << "A normal start must have no host sink.";
}

/**
 * @brief After `early_init` in a normal start, `configure()` enables the feature as usual.
 */
TEST_F(HandoffRuntimeTest, EarlyInitNormalStartThenConfigureEnables) {
  handoff::runtime_t rt;
  fake_env_t env;
  auto callbacks = env.make();
  ASSERT_EQ(rt.early_init(callbacks), "") << "Precondition: a normal start must succeed.";
  handoff::settings_t settings;
  settings.enabled = true;
  rt.configure(settings);
  EXPECT_EQ(rt.mode(), mode_e::normal) << "configure(enabled) after a normal start must reach mode normal.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Simple state accessors
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief The UDP base is the override when set, and the configured port otherwise.
 */
TEST_F(HandoffRuntimeTest, EffectiveUdpBaseUsesOverrideOrConfiguredPort) {
  handoff::runtime_t rt;
  EXPECT_EQ(rt.effective_udp_base(47989), 47989) << "Without an override effective_udp_base must return the configured port.";
  rt.set_udp_base_for_test(48100);
  EXPECT_EQ(rt.effective_udp_base(47989), 48100) << "With an override effective_udp_base must return the override.";
  ASSERT_TRUE(rt.udp_base().has_value()) << "udp_base() must report the override set by set_udp_base_for_test.";
  EXPECT_EQ(*rt.udp_base(), 48100) << "udp_base() must return the override set by set_udp_base_for_test.";
}

/**
 * @brief The main-process flag is true by default and follows `set_main()`.
 */
TEST_F(HandoffRuntimeTest, MainFlagFollowsSetMain) {
  handoff::runtime_t rt;
  EXPECT_TRUE(rt.is_main()) << "is_main() must be true for a fresh runtime.";
  rt.set_main(false);
  EXPECT_FALSE(rt.is_main()) << "set_main(false) must make is_main() false.";
  rt.set_main(true);
  EXPECT_TRUE(rt.is_main()) << "set_main(true) must make is_main() true again.";
}

/**
 * @brief The frozen flag is false by default and follows `set_frozen()`.
 */
TEST_F(HandoffRuntimeTest, FrozenFlagFollowsSetFrozen) {
  handoff::runtime_t rt;
  EXPECT_FALSE(rt.frozen()) << "frozen() must be false for a fresh runtime.";
  rt.set_frozen(true);
  EXPECT_TRUE(rt.frozen()) << "set_frozen(true) must make frozen() true.";
  rt.set_frozen(false);
  EXPECT_FALSE(rt.frozen()) << "set_frozen(false) must make frozen() false.";
}

/**
 * @brief The handoff-in-progress flag is false by default and follows `set_handoff_in_progress()`.
 */
TEST_F(HandoffRuntimeTest, HandoffInProgressFlagFollowsSetter) {
  handoff::runtime_t rt;
  EXPECT_FALSE(rt.handoff_in_progress()) << "handoff_in_progress() must be false for a fresh runtime.";
  rt.set_handoff_in_progress(true);
  EXPECT_TRUE(rt.handoff_in_progress()) << "set_handoff_in_progress(true) must make handoff_in_progress() true.";
  rt.set_handoff_in_progress(false);
  EXPECT_FALSE(rt.handoff_in_progress()) << "set_handoff_in_progress(false) must make handoff_in_progress() false.";
}

/**
 * @brief The log tag is `drain` only while the mode is `draining`.
 */
TEST_F(HandoffRuntimeTest, ProcessTagIsDrainOnlyWhileDraining) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  EXPECT_EQ(rt.process_tag(), "") << "The process tag must be empty in mode normal.";
  ASSERT_TRUE(rt.transition(mode_e::normal, mode_e::draining)) << "Precondition: normal must move to draining.";
  EXPECT_EQ(rt.process_tag(), "drain") << "The process tag must be 'drain' while the mode is draining.";
  ASSERT_TRUE(rt.transition(mode_e::draining, mode_e::normal)) << "Precondition: draining must roll back to normal.";
  EXPECT_EQ(rt.process_tag(), "") << "The process tag must be empty again once the mode is no longer draining.";
}

/**
 * @brief `set_channel` stores the channel, and passing null drops it.
 */
TEST_F(HandoffRuntimeTest, SetChannelStoresAndDropsChannel) {
  handoff::runtime_t rt;
  auto channel = std::make_shared<handoff::channel_t>(-1);
  rt.set_channel(channel);
  EXPECT_EQ(rt.channel(), channel) << "channel() must return the channel given to set_channel().";
  rt.set_channel(nullptr);
  EXPECT_EQ(rt.channel(), nullptr) << "set_channel(nullptr) must drop the channel.";
}

/**
 * @brief `set_peer` stores the draining-peer view, and passing null drops it.
 */
TEST_F(HandoffRuntimeTest, SetPeerStoresAndDropsPeer) {
  handoff::runtime_t rt;
  auto peer = std::make_shared<fake_peer_t>(true);
  rt.set_peer(peer);
  EXPECT_EQ(rt.peer(), peer) << "peer() must return the peer given to set_peer().";
  rt.set_peer(nullptr);
  EXPECT_EQ(rt.peer(), nullptr) << "set_peer(nullptr) must drop the peer.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Shutdown: note_sigterm(), request_shutdown(), the reason latch and the wake fd
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Before any shutdown request there is no shutdown, the wake fd is quiet, and the reason follows the mode.
 */
TEST_F(HandoffRuntimeTest, NothingRequestedBeforeAnyShutdownCall) {
  handoff::runtime_t rt;
  EXPECT_FALSE(rt.shutdown_requested()) << "shutdown_requested() must be false before any shutdown call.";
  EXPECT_FALSE(readable(rt.shutdown_wake_fd())) << "The shutdown wake fd must not be readable before any shutdown call.";
  EXPECT_EQ(rt.shutdown_reason(), shutdown_reason_e::terminated)
    << "With mode disabled and no latch the shutdown reason must be terminated.";
}

/**
 * @brief Before any signal, a draining process reports the reason `stop`.
 */
TEST_F(HandoffRuntimeTest, UnlatchedReasonIsStopWhileDraining) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::draining)) << "Precondition: the runtime must reach mode draining.";
  EXPECT_EQ(rt.shutdown_reason(), shutdown_reason_e::stop)
    << "With no latch and mode draining the shutdown reason must be stop.";
}

/**
 * @brief `note_sigterm()` sets the requested flag and makes the wake fd readable.
 */
TEST_F(HandoffRuntimeTest, NoteSigtermSetsFlagAndWakesFd) {
  handoff::runtime_t rt;
  rt.note_sigterm();
  EXPECT_TRUE(rt.shutdown_requested()) << "note_sigterm() must set shutdown_requested().";
  EXPECT_TRUE(readable(rt.shutdown_wake_fd())) << "After note_sigterm() the shutdown wake fd must be readable.";
}

/**
 * @brief `request_shutdown()` sets the requested flag and makes the wake fd readable, like `note_sigterm()`.
 */
TEST_F(HandoffRuntimeTest, RequestShutdownSetsFlagAndWakesFd) {
  handoff::runtime_t rt;
  rt.request_shutdown();
  EXPECT_TRUE(rt.shutdown_requested()) << "request_shutdown() must set shutdown_requested().";
  EXPECT_TRUE(readable(rt.shutdown_wake_fd())) << "After request_shutdown() the shutdown wake fd must be readable.";
}

/**
 * @brief Calling `note_sigterm()` twice changes nothing after the first call and keeps the fd readable.
 */
TEST_F(HandoffRuntimeTest, NoteSigtermIsIdempotent) {
  handoff::runtime_t rt;
  rt.note_sigterm();
  const auto first = rt.shutdown_reason();
  rt.note_sigterm();
  EXPECT_TRUE(rt.shutdown_requested()) << "A second note_sigterm() must keep shutdown_requested() true.";
  EXPECT_EQ(rt.shutdown_reason(), first) << "A second note_sigterm() must not change the latched reason.";
  EXPECT_TRUE(readable(rt.shutdown_wake_fd())) << "The wake fd must stay readable after a second note_sigterm().";
}

/**
 * @brief Calling `request_shutdown()` twice changes nothing after the first call.
 */
TEST_F(HandoffRuntimeTest, RequestShutdownIsIdempotent) {
  handoff::runtime_t rt;
  rt.request_shutdown();
  const auto first = rt.shutdown_reason();
  rt.request_shutdown();
  EXPECT_TRUE(rt.shutdown_requested()) << "A second request_shutdown() must keep shutdown_requested() true.";
  EXPECT_EQ(rt.shutdown_reason(), first) << "A second request_shutdown() must not change the latched reason.";
  EXPECT_TRUE(readable(rt.shutdown_wake_fd())) << "The wake fd must stay readable after a second request_shutdown().";
}

/**
 * @brief A shutdown in mode `normal` latches the reason `terminated`, and a later move to `draining` does not change it.
 */
TEST_F(HandoffRuntimeTest, NoteSigtermLatchesTerminatedInNormalMode) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  rt.note_sigterm();
  EXPECT_EQ(rt.shutdown_reason(), shutdown_reason_e::terminated)
    << "A SIGTERM in mode normal must latch the reason terminated.";
  ASSERT_TRUE(rt.transition(mode_e::normal, mode_e::draining)) << "Precondition: normal must move to draining.";
  EXPECT_EQ(rt.shutdown_reason(), shutdown_reason_e::terminated)
    << "The latched reason must not change when the mode later becomes draining.";
}

/**
 * @brief A SIGTERM while draining latches the reason `stop`, and a later rollback does not change it.
 */
TEST_F(HandoffRuntimeTest, NoteSigtermLatchesStopWhileDraining) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::draining)) << "Precondition: the runtime must reach mode draining.";
  rt.note_sigterm();
  EXPECT_EQ(rt.shutdown_reason(), shutdown_reason_e::stop) << "A SIGTERM while draining must latch the reason stop.";
  ASSERT_TRUE(rt.transition(mode_e::draining, mode_e::normal)) << "Precondition: draining must roll back to normal.";
  EXPECT_EQ(rt.shutdown_reason(), shutdown_reason_e::stop)
    << "The latched stop reason must survive a later change of mode.";
}

/**
 * @brief `request_shutdown()` latches `stop` when draining and `terminated` otherwise.
 */
TEST_F(HandoffRuntimeTest, RequestShutdownLatchesReasonByMode) {
  {
    handoff::runtime_t rt;
    ASSERT_TRUE(reach_mode(rt, mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
    rt.request_shutdown();
    EXPECT_EQ(rt.shutdown_reason(), shutdown_reason_e::terminated)
      << "request_shutdown() in mode normal must latch the reason terminated.";
  }
  {
    handoff::runtime_t rt;
    ASSERT_TRUE(reach_mode(rt, mode_e::draining)) << "Precondition: the runtime must reach mode draining.";
    rt.request_shutdown();
    EXPECT_EQ(rt.shutdown_reason(), shutdown_reason_e::stop)
      << "request_shutdown() while draining must latch the reason stop.";
  }
}

/**
 * @brief The first shutdown call latches the reason: a later signal does not overwrite a `request_shutdown()` latch.
 */
TEST_F(HandoffRuntimeTest, FirstLatchedReasonWinsOverLaterSignal) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::normal)) << "Precondition: the runtime must reach mode normal.";
  rt.request_shutdown();
  ASSERT_TRUE(rt.transition(mode_e::normal, mode_e::draining)) << "Precondition: normal must move to draining.";
  rt.note_sigterm();
  EXPECT_TRUE(rt.shutdown_requested()) << "A later note_sigterm() must keep shutdown_requested() true.";
  EXPECT_EQ(rt.shutdown_reason(), shutdown_reason_e::terminated)
    << "A later SIGTERM must not overwrite the reason latched by request_shutdown().";
}

/**
 * @brief The first latch wins in the other direction too: a later request does not overwrite a `note_sigterm()` latch.
 */
TEST_F(HandoffRuntimeTest, FirstLatchedReasonWinsOverLaterRequest) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::draining)) << "Precondition: the runtime must reach mode draining.";
  rt.note_sigterm();
  ASSERT_TRUE(rt.transition(mode_e::draining, mode_e::normal)) << "Precondition: draining must roll back to normal.";
  rt.request_shutdown();
  EXPECT_EQ(rt.shutdown_reason(), shutdown_reason_e::stop)
    << "A later request_shutdown() must not overwrite the stop reason latched by note_sigterm().";
}

/**
 * @brief `reset_for_test()` clears the shutdown flags and the latch, and drains the wake fd.
 */
TEST_F(HandoffRuntimeTest, ResetForTestClearsShutdownStateAndDrainsWakeFd) {
  handoff::runtime_t rt;
  rt.note_sigterm();
  ASSERT_TRUE(readable(rt.shutdown_wake_fd())) << "Precondition: the wake fd must be readable after a signal.";
  rt.reset_for_test();
  EXPECT_FALSE(rt.shutdown_requested()) << "reset_for_test() must clear shutdown_requested().";
  EXPECT_FALSE(readable(rt.shutdown_wake_fd())) << "reset_for_test() must drain the shutdown wake fd.";
  EXPECT_EQ(rt.shutdown_reason(), shutdown_reason_e::terminated)
    << "reset_for_test() must clear the latched reason (mode is disabled, so terminated).";
}

// ---------------------------------------------------------------------------------------------------------------------
// instance_* write-through to a temporary runtime directory
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Without an instance writer every `instance_*` call does nothing and reports so.
 */
TEST_F(HandoffRuntimeInstanceTest, InstanceCallsWithoutWriterDoNothing) {
  handoff::runtime_t rt;
  EXPECT_FALSE(rt.instance_set_role(handoff::role_e::active)) << "instance_set_role() without a writer must return false.";
  EXPECT_FALSE(rt.instance_set_state(handoff::state_e::draining)) << "instance_set_state() without a writer must return false.";
  EXPECT_FALSE(rt.instance_set_sessions(2)) << "instance_set_sessions() without a writer must return false.";
  EXPECT_EQ(rt.instance_current(), std::nullopt) << "instance_current() without a writer must return nullopt.";
  EXPECT_NO_FATAL_FAILURE(rt.instance_remove()) << "instance_remove() without a writer must be safe.";
}

/**
 * @brief `open_instance()` writes `<dir>/<pid>.json` with the initial content and reports success.
 */
TEST_F(HandoffRuntimeInstanceTest, OpenInstanceWritesInitialFile) {
  handoff::runtime_t rt;
  handoff::instance_t initial;
  initial.pid = 4242;
  initial.start_time = 99;
  initial.role = handoff::role_e::pending;
  initial.udp_base = 47989;
  initial.sessions = 0;
  initial.handoff = handoff::state_e::none;
  const auto instances = _dir / "instances";
  EXPECT_TRUE(rt.open_instance(instances, initial)) << "open_instance() must report success when the first write works.";
  const auto file = instances / "4242.json";
  ASSERT_TRUE(std::filesystem::exists(file)) << "open_instance() must write <dir>/<pid>.json.";
  const auto parsed = read_instance_file(file);
  ASSERT_TRUE(parsed.has_value()) << "The written instance file must be valid instance JSON.";
  EXPECT_EQ(*parsed, initial) << "The first write must contain exactly the initial content.";
  EXPECT_EQ(rt.instance_current(), initial) << "instance_current() must return the initial content.";
}

/**
 * @brief `instance_set_role()` writes the new role through to the file.
 */
TEST_F(HandoffRuntimeInstanceTest, InstanceSetRoleWritesThrough) {
  handoff::runtime_t rt;
  handoff::instance_t initial;
  initial.pid = 4242;
  initial.role = handoff::role_e::pending;
  const auto instances = _dir / "instances";
  ASSERT_TRUE(rt.open_instance(instances, initial)) << "Precondition: open_instance must succeed.";
  EXPECT_TRUE(rt.instance_set_role(handoff::role_e::active)) << "instance_set_role() must report success with a writer.";
  const auto parsed = read_instance_file(instances / "4242.json");
  ASSERT_TRUE(parsed.has_value()) << "The instance file must still be valid after a role update.";
  EXPECT_EQ(parsed->role, handoff::role_e::active) << "The file must show the role set by instance_set_role().";
  ASSERT_TRUE(rt.instance_current().has_value()) << "instance_current() must exist while a writer is installed.";
  EXPECT_EQ(rt.instance_current()->role, handoff::role_e::active) << "instance_current() must show the updated role.";
}

/**
 * @brief `instance_set_state()` writes the handoff state through to the file.
 */
TEST_F(HandoffRuntimeInstanceTest, InstanceSetStateWritesThrough) {
  handoff::runtime_t rt;
  handoff::instance_t initial;
  initial.pid = 4242;
  const auto instances = _dir / "instances";
  ASSERT_TRUE(rt.open_instance(instances, initial)) << "Precondition: open_instance must succeed.";
  EXPECT_TRUE(rt.instance_set_state(handoff::state_e::kill_pending)) << "instance_set_state() must report success.";
  const auto parsed = read_instance_file(instances / "4242.json");
  ASSERT_TRUE(parsed.has_value()) << "The instance file must be valid after a state update.";
  EXPECT_EQ(parsed->handoff, handoff::state_e::kill_pending) << "The file must show the state set by instance_set_state().";
}

/**
 * @brief `instance_set_sessions()` writes the session count through to the file.
 */
TEST_F(HandoffRuntimeInstanceTest, InstanceSetSessionsWritesThrough) {
  handoff::runtime_t rt;
  handoff::instance_t initial;
  initial.pid = 4242;
  const auto instances = _dir / "instances";
  ASSERT_TRUE(rt.open_instance(instances, initial)) << "Precondition: open_instance must succeed.";
  EXPECT_TRUE(rt.instance_set_sessions(3)) << "instance_set_sessions() must report success.";
  const auto parsed = read_instance_file(instances / "4242.json");
  ASSERT_TRUE(parsed.has_value()) << "The instance file must be valid after a session update.";
  EXPECT_EQ(parsed->sessions, 3) << "The file must show the session count set by instance_set_sessions().";
  ASSERT_TRUE(rt.instance_current().has_value()) << "instance_current() must exist while a writer is installed.";
  EXPECT_EQ(rt.instance_current()->sessions, 3) << "instance_current() must show the updated session count.";
}

/**
 * @brief `instance_remove()` deletes the file and drops the writer, so later calls do nothing.
 */
TEST_F(HandoffRuntimeInstanceTest, InstanceRemoveDeletesFileAndDropsWriter) {
  handoff::runtime_t rt;
  handoff::instance_t initial;
  initial.pid = 4242;
  const auto instances = _dir / "instances";
  ASSERT_TRUE(rt.open_instance(instances, initial)) << "Precondition: open_instance must succeed.";
  rt.instance_remove();
  EXPECT_FALSE(std::filesystem::exists(instances / "4242.json")) << "instance_remove() must delete the instance file.";
  EXPECT_EQ(rt.instance_current(), std::nullopt) << "instance_remove() must drop the writer.";
  EXPECT_FALSE(rt.instance_set_role(handoff::role_e::draining))
    << "After instance_remove() instance_set_role() must do nothing and return false.";
  EXPECT_FALSE(std::filesystem::exists(instances / "4242.json"))
    << "A call after instance_remove() must not recreate the instance file.";
}

/**
 * @brief A second `open_instance()` replaces the writer but does not remove the first process's file.
 */
TEST_F(HandoffRuntimeInstanceTest, OpenInstanceReplacesWriterWithoutRemovingOldFile) {
  handoff::runtime_t rt;
  handoff::instance_t first;
  first.pid = 4242;
  handoff::instance_t second;
  second.pid = 4343;
  const auto instances = _dir / "instances";
  ASSERT_TRUE(rt.open_instance(instances, first)) << "Precondition: the first open_instance must succeed.";
  EXPECT_TRUE(rt.open_instance(instances, second)) << "The second open_instance must report success.";
  EXPECT_TRUE(std::filesystem::exists(instances / "4242.json"))
    << "Replacing the writer must not remove the earlier writer's file.";
  ASSERT_TRUE(std::filesystem::exists(instances / "4343.json")) << "The new writer must write its own file.";
  ASSERT_TRUE(rt.instance_current().has_value()) << "instance_current() must exist after the replacement.";
  EXPECT_EQ(rt.instance_current()->pid, 4343) << "instance_current() must show the replacement writer's content.";
}

/**
 * @brief When the first write fails the writer stays installed and `open_instance()` reports false.
 *
 * The instances "directory" is placed under a regular file, so it cannot be created.
 */
TEST_F(HandoffRuntimeInstanceTest, OpenInstanceFailureKeepsWriterInstalled) {
  handoff::runtime_t rt;
  const auto blocker = _dir / "not_a_dir";
  {
    std::ofstream out(blocker);
    out << "x";
  }
  handoff::instance_t initial;
  initial.pid = 4242;
  initial.sessions = 5;
  EXPECT_FALSE(rt.open_instance(blocker / "instances", initial))
    << "open_instance() must return false when the first write cannot be done.";
  ASSERT_TRUE(rt.instance_current().has_value())
    << "The writer must stay installed even when the first write failed.";
  EXPECT_EQ(rt.instance_current()->sessions, 5) << "instance_current() must hold the content given to open_instance().";
}

/**
 * @brief `reset_for_test()` drops the writer but leaves the instance file on disk.
 */
TEST_F(HandoffRuntimeInstanceTest, ResetForTestDropsWriterButKeepsFile) {
  handoff::runtime_t rt;
  handoff::instance_t initial;
  initial.pid = 4242;
  const auto instances = _dir / "instances";
  ASSERT_TRUE(rt.open_instance(instances, initial)) << "Precondition: open_instance must succeed.";
  rt.reset_for_test();
  EXPECT_EQ(rt.instance_current(), std::nullopt) << "reset_for_test() must drop the instance writer.";
  EXPECT_TRUE(std::filesystem::exists(instances / "4242.json"))
    << "reset_for_test() must not remove the instance file.";
}

// ---------------------------------------------------------------------------------------------------------------------
// reset_for_test()
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief `reset_for_test()` returns every piece of state to the freshly constructed values.
 */
TEST_F(HandoffRuntimeTest, ResetForTestRestoresFreshState) {
  handoff::runtime_t rt;
  fake_env_t env;
  set_valid_handoff_vars(env);
  env.vars["SUNSHINE_HANDOFF_HOST_SINK"] = "sink";
  auto callbacks = env.make();
  ASSERT_EQ(rt.early_init(callbacks), "") << "Precondition: a valid handoff start must succeed.";
  rt.set_frozen(true);
  rt.set_main(true);
  rt.set_handoff_in_progress(true);
  rt.set_channel(std::make_shared<handoff::channel_t>(-1));
  rt.set_peer(std::make_shared<fake_peer_t>(true));
  rt.request_shutdown();
  rt.set_udp_base_for_test(48200);

  rt.reset_for_test();
  EXPECT_EQ(rt.mode(), mode_e::disabled) << "reset_for_test() must return the mode to disabled.";
  EXPECT_FALSE(rt.enabled()) << "reset_for_test() must make enabled() false.";
  EXPECT_FALSE(rt.started_by_handoff()) << "reset_for_test() must clear the handoff-start marker.";
  EXPECT_EQ(rt.generation(), 0u) << "reset_for_test() must reset the generation to 0.";
  EXPECT_FALSE(rt.udp_base().has_value()) << "reset_for_test() must remove the UDP base override.";
  EXPECT_TRUE(rt.is_main()) << "reset_for_test() must make the process the main process.";
  EXPECT_FALSE(rt.frozen()) << "reset_for_test() must clear the frozen flag.";
  EXPECT_FALSE(rt.handoff_in_progress()) << "reset_for_test() must clear the handoff-in-progress flag.";
  EXPECT_FALSE(rt.shutdown_requested()) << "reset_for_test() must clear the shutdown-requested flag.";
  EXPECT_FALSE(rt.inherited().has_value()) << "reset_for_test() must clear the inherited fds.";
  EXPECT_FALSE(rt.host_sink().has_value()) << "reset_for_test() must clear the host sink.";
  EXPECT_EQ(rt.channel(), nullptr) << "reset_for_test() must drop the channel.";
  EXPECT_EQ(rt.peer(), nullptr) << "reset_for_test() must drop the draining peer.";
  EXPECT_FALSE(rt.settings().enabled) << "reset_for_test() must restore the default settings.";
  EXPECT_EQ(rt.shutdown_reason(), shutdown_reason_e::terminated) << "reset_for_test() must clear the latched reason.";
}

/**
 * @brief After `reset_for_test()` the runtime accepts a fresh handoff start, just like a new process.
 */
TEST_F(HandoffRuntimeTest, ResetForTestAllowsNewHandoffStart) {
  handoff::runtime_t rt;
  ASSERT_TRUE(reach_mode(rt, mode_e::draining)) << "Precondition: the runtime must reach mode draining.";
  rt.reset_for_test();
  ASSERT_TRUE(reach_mode(rt, mode_e::new_pending)) << "After reset a handoff start must reach new_pending again.";
  EXPECT_EQ(rt.generation(), 1u) << "The generation from the new handoff start must be stored after the reset.";
}

#endif  // __linux__
