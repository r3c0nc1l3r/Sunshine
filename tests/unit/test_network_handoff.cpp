/**
 * @file tests/unit/test_network_handoff.cpp
 * @brief Blind tests for `net::map_udp_port` (the UDP base of this process) and the UDP base environment rules of
 *        `handoff::runtime_t::early_init` and `handoff::hooks::restart_environment`.
 *
 * Written from `docs/drain-handoff.md` ("Ports") and the headers `src/network.h`, `src/handoff/runtime.h` and
 * `src/handoff/hooks.h`. No implementation file was read.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// local includes
#include <src/config.h>
#include <src/handoff/hooks.h>
#include <src/network.h>

#ifdef __linux__
  #include <src/handoff/runtime.h>
#endif

namespace {
  /**
   * @brief Fake process environment for `early_init`: the variables that are set, the names removed, and the number
   *        of times the fd scrub ran.
   */
  struct FakeEnv {
    std::map<std::string, std::string> vars;  ///< Variables that are set.
    std::vector<std::string> unset_names;  ///< Names passed to `unsetenv`, in call order.
    int scrub_calls {0};  ///< Times `scrub_fds` was called.
  };

  /**
   * @brief Build an `early_init` environment that reads from and records into a fake.
   *
   * @param fake Fake to use; must outlive the returned environment.
   * @return The environment.
   */
  handoff::early_init_env_t make_env(FakeEnv &fake) {
    handoff::early_init_env_t env;
    env.getenv = [&fake](const char *name) -> const char * {
      const auto found = fake.vars.find(name);
      return found == fake.vars.end() ? nullptr : found->second.c_str();
    };
    env.unsetenv = [&fake](const char *name) {
      fake.unset_names.emplace_back(name);
      fake.vars.erase(name);
    };
    env.scrub_fds = [&fake] {
      ++fake.scrub_calls;
    };
    return env;
  }

  /**
   * @brief Whether a list of removed names contains a given name.
   *
   * @param names Removed names.
   * @param name Name to look for.
   * @return `true` when present.
   */
  bool was_unset(const std::vector<std::string> &names, const std::string &name) {
    for (const auto &entry : names) {
      if (entry == name) {
        return true;
      }
    }
    return false;
  }
}  // namespace

/**
 * @brief Fixture that saves the configured port and resets the handoff runtime around each test.
 */
class HandoffNetworkTest: public testing::Test {
protected:
  /**
   * @brief Save the port, pin it to 47989 and reset the handoff runtime to its fresh state.
   */
  void SetUp() override {
    original_port_ = config::sunshine.port;
    config::sunshine.port = 47989;
#ifdef __linux__
    handoff::runtime().reset_for_test();
#endif
  }

  /**
   * @brief Reset the handoff runtime and restore the port.
   */
  void TearDown() override {
#ifdef __linux__
    handoff::runtime().reset_for_test();
#endif
    config::sunshine.port = original_port_;
  }

  std::uint16_t original_port_ {};  ///< Port restored after each test.
};

// ---------------------------------------------------------------------------------------------------------------------
// Default behaviour (every platform)
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief With no UDP base set, `map_udp_port` returns exactly what `map_port` returns for the same offset.
 */
TEST_F(HandoffNetworkTest, DefaultMapUdpPortEqualsMapPort) {
  for (const int offset : {0, 9, 10, 11, 21}) {
    EXPECT_EQ(net::map_udp_port(offset), net::map_port(offset))
      << "With no UDP base set, map_udp_port(" << offset << ") must equal map_port(" << offset << ").";
  }
}

/**
 * @brief With no UDP base set, `map_udp_port` adds the offset to the configured `port`.
 */
TEST_F(HandoffNetworkTest, DefaultMapUdpPortUsesConfiguredPort) {
  config::sunshine.port = 48000;

  EXPECT_EQ(net::map_udp_port(9), 48009) << "map_udp_port(9) must be the configured port plus 9 when no base is set.";
  EXPECT_EQ(net::map_udp_port(9), net::map_port(9)) << "The default must match map_port for the same offset.";
}

/**
 * @brief The TCP mapping keeps using the configured port with `map_port`.
 */
TEST_F(HandoffNetworkTest, MapPortStaysOnConfiguredPort) {
  config::sunshine.port = 48000;

  EXPECT_EQ(net::map_port(0), 48000) << "map_port(0) must be the configured port.";
  EXPECT_EQ(net::map_port(21), 48021) << "map_port(21) must be the configured port plus 21 (RTSP).";
}

#ifdef __linux__
// ---------------------------------------------------------------------------------------------------------------------
// UDP base set through the runtime seam (Linux)
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief A UDP base set through the test seam moves the video, control and audio ports to that base.
 */
TEST_F(HandoffNetworkTest, UdpBaseOverrideMovesUdpPorts) {
  handoff::runtime().set_udp_base_for_test(47100);

  EXPECT_EQ(handoff::hooks::udp_base(), 47100) << "hooks::udp_base() must report the override.";
  EXPECT_EQ(net::map_udp_port(9), 47109) << "Video must be base + 9.";
  EXPECT_EQ(net::map_udp_port(10), 47110) << "Control must be base + 10.";
  EXPECT_EQ(net::map_udp_port(11), 47111) << "Audio must be base + 11.";
}

/**
 * @brief Once a base is set, UDP ports no longer follow the configured port.
 */
TEST_F(HandoffNetworkTest, UdpBaseOverrideWinsOverConfiguredPort) {
  handoff::runtime().set_udp_base_for_test(47100);

  EXPECT_NE(net::map_udp_port(9), net::map_port(9)) << "With a base override, UDP must not use the configured port.";
  EXPECT_EQ(net::map_udp_port(9), 47109) << "The override base must be used for UDP.";
}

/**
 * @brief The UDP base override does not move the TCP mapping, which keeps using the configured port.
 */
TEST_F(HandoffNetworkTest, UdpBaseOverrideLeavesTcpPortsAlone) {
  handoff::runtime().set_udp_base_for_test(47100);

  EXPECT_EQ(net::map_port(0), 47989) << "TCP ports must keep the configured port with an override set.";
  EXPECT_EQ(net::map_port(21), 48010) << "RTSP must keep the configured port plus 21 with an override set.";
}

/**
 * @brief The highest allowed base (65524) maps audio to 65535 and does not overflow.
 */
TEST_F(HandoffNetworkTest, HighestBaseMapsAudioToTopOfRange) {
  handoff::runtime().set_udp_base_for_test(65524);

  EXPECT_EQ(net::map_udp_port(11), 65535) << "Base 65524 plus 11 must be 65535.";
}

/**
 * @brief A reset returns the process to the default mapping.
 */
TEST_F(HandoffNetworkTest, ResetRestoresDefaultMapping) {
  handoff::runtime().set_udp_base_for_test(47100);
  handoff::runtime().reset_for_test();

  EXPECT_FALSE(handoff::runtime().udp_base().has_value()) << "After reset there must be no UDP base override.";
  EXPECT_EQ(net::map_udp_port(9), net::map_port(9)) << "After reset map_udp_port must equal map_port again.";
}

// ---------------------------------------------------------------------------------------------------------------------
// SUNSHINE_UDP_BASE through early_init (Linux)
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief A valid `SUNSHINE_UDP_BASE` in a normal start sets the base, and the variable is removed from the environment.
 */
TEST_F(HandoffNetworkTest, EnvUdpBaseIsReadAndRemoved) {
  FakeEnv fake;
  fake.vars["SUNSHINE_UDP_BASE"] = "47100";

  const auto error = handoff::runtime().early_init(make_env(fake));

  EXPECT_TRUE(error.empty()) << "A valid SUNSHINE_UDP_BASE in a normal start must not be an error; got: " << error;
  EXPECT_EQ(handoff::hooks::udp_base(), 47100) << "SUNSHINE_UDP_BASE=47100 must set the process UDP base.";
  EXPECT_EQ(net::map_udp_port(9), 47109) << "The mapped video port must follow the base read from the environment.";
  EXPECT_TRUE(was_unset(fake.unset_names, "SUNSHINE_UDP_BASE")) << "SUNSHINE_UDP_BASE must be removed from the environment.";
  EXPECT_EQ(fake.scrub_calls, 1) << "The fd scrub must run once on a normal start that succeeded.";
  EXPECT_FALSE(handoff::runtime().started_by_handoff()) << "A normal start must not be marked as a handoff start.";
}

/**
 * @brief Boundary values 1 and 65524 are accepted for `SUNSHINE_UDP_BASE`.
 */
TEST_F(HandoffNetworkTest, EnvUdpBaseAcceptsRangeEnds) {
  for (const std::string value : {"1", "65524"}) {
    SCOPED_TRACE("SUNSHINE_UDP_BASE=" + value);
    handoff::runtime().reset_for_test();
    FakeEnv fake;
    fake.vars["SUNSHINE_UDP_BASE"] = value;

    EXPECT_TRUE(handoff::runtime().early_init(make_env(fake)).empty()) << "The range end must be accepted in a normal start.";
    EXPECT_EQ(handoff::hooks::udp_base(), std::stoi(value)) << "The range end must become the UDP base.";
  }
}

/**
 * @brief Values outside 1 to 65524, or that do not parse, are ignored in a normal start: no base override, no error,
 *        the mapping stays on the configured port, and the variable is still removed.
 */
TEST_F(HandoffNetworkTest, EnvUdpBaseOutOfRangeOrInvalidIsIgnored) {
  for (const std::string value : {"0", "65525", "70000", "-1", "abc", "", "47100x", "1.5", "0x7000"}) {
    SCOPED_TRACE("SUNSHINE_UDP_BASE=\"" + value + "\"");
    handoff::runtime().reset_for_test();
    FakeEnv fake;
    fake.vars["SUNSHINE_UDP_BASE"] = value;

    const auto error = handoff::runtime().early_init(make_env(fake));

    EXPECT_TRUE(error.empty()) << "An ignored SUNSHINE_UDP_BASE in a normal start must not be an error; got: " << error;
    EXPECT_FALSE(handoff::runtime().udp_base().has_value()) << "An out-of-range or unparsable base must not become an override.";
    EXPECT_EQ(net::map_udp_port(9), net::map_port(9)) << "With the base ignored, UDP must use the configured port.";
    EXPECT_TRUE(was_unset(fake.unset_names, "SUNSHINE_UDP_BASE")) << "The variable must still be removed.";
  }
}

/**
 * @brief `SUNSHINE_GENERATION` 0 and 65535 are accepted; 65536 is ignored (generation stays 0).
 */
TEST_F(HandoffNetworkTest, EnvGenerationRangeIsEnforced) {
  {
    handoff::runtime().reset_for_test();
    FakeEnv fake;
    fake.vars["SUNSHINE_GENERATION"] = "65535";
    EXPECT_TRUE(handoff::runtime().early_init(make_env(fake)).empty()) << "Generation 65535 must be accepted in a normal start.";
    EXPECT_EQ(handoff::runtime().generation(), 65535u) << "SUNSHINE_GENERATION=65535 must set the generation.";
  }
  {
    handoff::runtime().reset_for_test();
    FakeEnv fake;
    fake.vars["SUNSHINE_GENERATION"] = "65536";
    EXPECT_TRUE(handoff::runtime().early_init(make_env(fake)).empty()) << "An out-of-range generation in a normal start must be ignored, not an error.";
    EXPECT_EQ(handoff::runtime().generation(), 0u) << "SUNSHINE_GENERATION=65536 must be ignored, leaving generation 0.";
    EXPECT_TRUE(was_unset(fake.unset_names, "SUNSHINE_GENERATION")) << "SUNSHINE_GENERATION must still be removed.";
  }
}

/**
 * @brief An invalid `SUNSHINE_HANDOFF_UDP_BASE` in a handoff start is an error: nothing is stored, the fd scrub does
 *        not run, the mode stays disabled, and the UDP base stays the configured port.
 */
TEST_F(HandoffNetworkTest, HandoffStartWithInvalidUdpBaseIsRefused) {
  for (const std::string value : {"0", "65525", "70000", "abc", ""}) {
    SCOPED_TRACE("SUNSHINE_HANDOFF_UDP_BASE=\"" + value + "\"");
    handoff::runtime().reset_for_test();
    FakeEnv fake;
    fake.vars["SUNSHINE_HANDOFF_FDS"] = "3";
    fake.vars["SUNSHINE_HANDOFF_CHANNEL"] = "7";
    fake.vars["SUNSHINE_HANDOFF_GENERATION"] = "1";
    fake.vars["SUNSHINE_HANDOFF_UDP_BASE"] = value;

    const auto error = handoff::runtime().early_init(make_env(fake));

    EXPECT_FALSE(error.empty()) << "An invalid SUNSHINE_HANDOFF_UDP_BASE must be an error.";
    EXPECT_EQ(fake.scrub_calls, 0) << "On an error the fd scrub must not run.";
    EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::disabled) << "On an error the mode must stay disabled.";
    EXPECT_FALSE(handoff::runtime().udp_base().has_value()) << "On an error no UDP base may be stored.";
  }
}

/**
 * @brief A handoff start that lacks `SUNSHINE_HANDOFF_UDP_BASE` is an error.
 */
TEST_F(HandoffNetworkTest, HandoffStartWithoutUdpBaseIsRefused) {
  FakeEnv fake;
  fake.vars["SUNSHINE_HANDOFF_FDS"] = "3";
  fake.vars["SUNSHINE_HANDOFF_CHANNEL"] = "7";
  fake.vars["SUNSHINE_HANDOFF_GENERATION"] = "1";

  const auto error = handoff::runtime().early_init(make_env(fake));

  EXPECT_FALSE(error.empty()) << "A handoff start without SUNSHINE_HANDOFF_UDP_BASE must be an error.";
  EXPECT_EQ(fake.scrub_calls, 0) << "On an error the fd scrub must not run.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Restart environment (Linux)
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief With the feature off, a restart unsets both variables, even when a base override is set.
 */
TEST_F(HandoffNetworkTest, RestartEnvironmentUnsetsEverythingWhenDisabled) {
  handoff::runtime().set_udp_base_for_test(47100);

  const auto entries = handoff::hooks::restart_environment();

  ASSERT_EQ(entries.size(), 2u) << "restart_environment must return two entries (base, generation).";
  EXPECT_EQ(entries[0].first, "SUNSHINE_UDP_BASE") << "The first entry must be SUNSHINE_UDP_BASE.";
  EXPECT_FALSE(entries[0].second.has_value()) << "With the feature off, SUNSHINE_UDP_BASE must be unset.";
  EXPECT_EQ(entries[1].first, "SUNSHINE_GENERATION") << "The second entry must be SUNSHINE_GENERATION.";
  EXPECT_FALSE(entries[1].second.has_value()) << "With the feature off, SUNSHINE_GENERATION must be unset.";
}

/**
 * @brief With the feature on and no override, a restart unsets both variables.
 */
TEST_F(HandoffNetworkTest, RestartEnvironmentUnsetsWhenNoOverride) {
  handoff::settings_t settings;
  settings.enabled = true;
  handoff::runtime().configure(settings);

  const auto entries = handoff::hooks::restart_environment();

  ASSERT_EQ(entries.size(), 2u) << "With the feature on, restart_environment must return two entries.";
  EXPECT_FALSE(entries[0].second.has_value()) << "No override means SUNSHINE_UDP_BASE is unset.";
  EXPECT_FALSE(entries[1].second.has_value()) << "Generation 0 means SUNSHINE_GENERATION is unset.";
}

/**
 * @brief With the feature on, an overridden base is passed on as decimal in `SUNSHINE_UDP_BASE`.
 */
TEST_F(HandoffNetworkTest, RestartEnvironmentKeepsOverriddenBase) {
  handoff::settings_t settings;
  settings.enabled = true;
  handoff::runtime().configure(settings);
  handoff::runtime().set_udp_base_for_test(47100);

  const auto entries = handoff::hooks::restart_environment();

  ASSERT_EQ(entries.size(), 2u) << "restart_environment must return two entries.";
  ASSERT_TRUE(entries[0].second.has_value()) << "An override that differs from the port must be passed on.";
  EXPECT_EQ(*entries[0].second, "47100") << "SUNSHINE_UDP_BASE must be the decimal base.";
}

/**
 * @brief A base equal to the configured port is not passed on; the variable is unset.
 */
TEST_F(HandoffNetworkTest, RestartEnvironmentDropsBaseEqualToPort) {
  handoff::settings_t settings;
  settings.enabled = true;
  handoff::runtime().configure(settings);
  handoff::runtime().set_udp_base_for_test(47989);

  const auto entries = handoff::hooks::restart_environment();

  ASSERT_EQ(entries.size(), 2u) << "restart_environment must return two entries.";
  EXPECT_FALSE(entries[0].second.has_value()) << "A base equal to the configured port must leave SUNSHINE_UDP_BASE unset.";
}

/**
 * @brief A non-zero generation read from the environment is passed on in `SUNSHINE_GENERATION`.
 */
TEST_F(HandoffNetworkTest, RestartEnvironmentKeepsGeneration) {
  FakeEnv fake;
  fake.vars["SUNSHINE_GENERATION"] = "3";
  ASSERT_TRUE(handoff::runtime().early_init(make_env(fake)).empty()) << "Precondition: early_init must accept generation 3.";

  handoff::settings_t settings;
  settings.enabled = true;
  handoff::runtime().configure(settings);

  const auto entries = handoff::hooks::restart_environment();

  ASSERT_EQ(entries.size(), 2u) << "restart_environment must return two entries.";
  ASSERT_TRUE(entries[1].second.has_value()) << "A generation above 0 must be passed on.";
  EXPECT_EQ(*entries[1].second, "3") << "SUNSHINE_GENERATION must be the decimal generation.";
}
#endif
