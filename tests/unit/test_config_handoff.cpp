/**
 * @file tests/unit/test_config_handoff.cpp
 * @brief Blind tests for the drain-handoff options in `config` (`drain_handoff*` keys), the config and enable refusals
 *        in `handoff::hooks`, and `display_device::has_settings_manager`.
 *
 * Written from `docs/drain-handoff.md` ("Configuration", "Ports"), `src/config.h` (`config::drain_handoff`),
 * `src/handoff/settings.h`, `src/handoff/hooks.h` and `src/display_device.h`. No implementation file was read.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <climits>
#include <cstdint>
#include <string>
#include <string_view>

// local includes
#include <src/config.h>
#include <src/display_device.h>
#include <src/handoff/hooks.h>
#include <src/handoff/settings.h>

using namespace std::literals;

namespace {
  /**
   * @brief One integer `drain_handoff*` key and the `settings_t` field it fills.
   */
  struct IntegerOption {
    std::string_view key;  ///< Configuration key name.
    int handoff::settings_t::*field;  ///< Field that stores the parsed value.
  };

  /**
   * @brief Every integer option, in the order of `docs/drain-handoff.md`.
   */
  constexpr std::array<IntegerOption, 5> INTEGER_OPTIONS {{
    {"drain_handoff_udp_offset"sv, &handoff::settings_t::udp_offset},
    {"drain_handoff_deadline"sv, &handoff::settings_t::deadline_s},
    {"drain_handoff_ready_timeout"sv, &handoff::settings_t::ready_timeout_s},
    {"drain_handoff_rollback_window"sv, &handoff::settings_t::rollback_window_s},
    {"drain_handoff_cooldown"sv, &handoff::settings_t::cooldown_s},
  }};

  /**
   * @brief Text that is not a valid integer for a `drain_handoff*` key.
   */
  constexpr std::array<std::string_view, 8> INVALID_INTEGER_TEXT {{
    "0x10"sv,  // hex
    "12abc"sv,  // trailing text
    "ten"sv,  // words
    "1.5"sv,  // not an integer
    "+5"sv,  // only an optional leading '-' is allowed
    "\"3600\""sv,  // quoted number: "other characters"
    "99999999999"sv,  // above the int range
    "-2147483649"sv,  // below the int range
  }};

  /**
   * @brief One configuration text that the drain-handoff validation must refuse.
   */
  struct RefusalCase {
    std::string_view name;  ///< Test name suffix; a plain identifier.
    std::string_view lines;  ///< Extra configuration lines, after `port`.
  };

  /**
   * @brief Configuration texts with the feature on that violate one validation rule each.
   */
  constexpr std::array<RefusalCase, 14> REFUSAL_CASES {{
    {"OffsetZero"sv, "drain_handoff = enabled\ndrain_handoff_udp_offset = 0\n"sv},
    {"OffsetBelowPortRange"sv, "drain_handoff = enabled\ndrain_handoff_udp_offset = -46989\n"sv},  // base 1000
    {"OffsetAboveUdpRange"sv, "drain_handoff = enabled\ndrain_handoff_udp_offset = 20000\n"sv},  // ports above 65535
    {"OffsetIntMax"sv, "drain_handoff = enabled\ndrain_handoff_udp_offset = 2147483647\n"sv},  // no overflow, refused
    {"OffsetCollidesWithTcp"sv, "drain_handoff = enabled\ndrain_handoff_udp_offset = 12\n"sv},  // video = RTSP port
    {"OffsetBasesOverlap"sv, "drain_handoff = enabled\ndrain_handoff_udp_offset = 1\n"sv},  // the two bases share ports
    {"DeadlineZero"sv, "drain_handoff = enabled\ndrain_handoff_deadline = 0\n"sv},
    {"DeadlineAboveOneDay"sv, "drain_handoff = enabled\ndrain_handoff_deadline = 86401\n"sv},
    {"DeadlineShorterThanRollback"sv, "drain_handoff = enabled\ndrain_handoff_rollback_window = 600\ndrain_handoff_cooldown = 600\ndrain_handoff_deadline = 300\n"sv},
    {"CooldownShorterThanRollback"sv, "drain_handoff = enabled\ndrain_handoff_rollback_window = 120\ndrain_handoff_cooldown = 60\n"sv},
    {"ReadyTimeoutZero"sv, "drain_handoff = enabled\ndrain_handoff_ready_timeout = 0\n"sv},
    {"RollbackWindowZero"sv, "drain_handoff = enabled\ndrain_handoff_rollback_window = 0\n"sv},
    {"ReadyTimeoutIntMin"sv, "drain_handoff = enabled\ndrain_handoff_ready_timeout = -2147483648\n"sv},
    {"CooldownZero"sv, "drain_handoff = enabled\ndrain_handoff_cooldown = 0\n"sv},
  }};

  /**
   * @brief Prefix a configuration text with a fixed `port`, so validation runs against a known port.
   *
   * @param lines Extra configuration lines.
   * @return Configuration text.
   */
  std::string with_port(std::string_view lines) {
    std::string text {"port = 47989\n"};
    text += lines;
    return text;
  }
}  // namespace

/**
 * @brief Fixture that resets `config::drain_handoff` and `config::sunshine.port` around each test.
 */
class HandoffConfigTest: public testing::Test {
protected:
  /**
   * @brief Save the port and the drain-handoff options, then reset the options to their struct defaults.
   */
  void SetUp() override {
    original_port_ = config::sunshine.port;
    original_options_ = config::drain_handoff;
    config::drain_handoff = handoff::settings_t {};
  }

  /**
   * @brief Restore the port and the drain-handoff options.
   */
  void TearDown() override {
    config::sunshine.port = original_port_;
    config::drain_handoff = original_options_;
  }

  /**
   * @brief Apply a configuration text made of the fixed port plus the given lines.
   *
   * @param lines Extra configuration lines.
   */
  static void apply(std::string_view lines) {
    config::apply_config_for_test(with_port(lines));
  }

  std::uint16_t original_port_ {};  ///< Port restored after each test.
  handoff::settings_t original_options_;  ///< Options restored after each test.
};

/**
 * @brief Parameterized fixture for the refusal and acceptance tables.
 */
class HandoffConfigRefusalTest: public HandoffConfigTest, public testing::WithParamInterface<RefusalCase> {};

/**
 * @brief Parameterized fixture for the accepted boundary values (Linux only; see the tests).
 */
class HandoffConfigAcceptedTest: public HandoffConfigTest, public testing::WithParamInterface<RefusalCase> {};

// ---------------------------------------------------------------------------------------------------------------------
// Defaults and parsing (run on every platform)
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Without any `drain_handoff*` key, every option keeps its documented default and the feature stays off.
 */
TEST_F(HandoffConfigTest, OmittedKeysLeaveDocumentedDefaults) {
  apply("");

  EXPECT_FALSE(config::drain_handoff.enabled) << "A config without drain_handoff must leave the feature off.";
  EXPECT_EQ(config::drain_handoff.udp_offset, 100) << "drain_handoff_udp_offset default must be 100.";
  EXPECT_EQ(config::drain_handoff.deadline_s, 3600) << "drain_handoff_deadline default must be 3600 seconds.";
  EXPECT_EQ(config::drain_handoff.ready_timeout_s, 30) << "drain_handoff_ready_timeout default must be 30 seconds.";
  EXPECT_EQ(config::drain_handoff.rollback_window_s, 60) << "drain_handoff_rollback_window default must be 60 seconds.";
  EXPECT_EQ(config::drain_handoff.cooldown_s, 120) << "drain_handoff_cooldown default must be 120 seconds.";
  EXPECT_TRUE(config::drain_handoff.binary.empty()) << "drain_handoff_binary default must be empty (the start path).";
}

/**
 * @brief The `settings_t` struct itself carries the documented defaults, so a fresh struct is a valid "off" config.
 */
TEST_F(HandoffConfigTest, SettingsStructDefaultsMatchDocumentation) {
  const handoff::settings_t defaults {};

  EXPECT_FALSE(defaults.enabled) << "settings_t::enabled must default to false.";
  EXPECT_EQ(defaults.udp_offset, 100) << "settings_t::udp_offset must default to 100.";
  EXPECT_EQ(defaults.deadline_s, 3600) << "settings_t::deadline_s must default to 3600.";
  EXPECT_EQ(defaults.ready_timeout_s, 30) << "settings_t::ready_timeout_s must default to 30.";
  EXPECT_EQ(defaults.rollback_window_s, 60) << "settings_t::rollback_window_s must default to 60.";
  EXPECT_EQ(defaults.cooldown_s, 120) << "settings_t::cooldown_s must default to 120.";
  EXPECT_TRUE(defaults.binary.empty()) << "settings_t::binary must default to empty.";
}

/**
 * @brief An explicit `disabled` keeps the feature off, and every other option keeps its default.
 */
TEST_F(HandoffConfigTest, ExplicitDisabledKeepsFeatureOff) {
  apply("drain_handoff = disabled\n");

  EXPECT_FALSE(config::drain_handoff.enabled) << "drain_handoff = disabled must leave the feature off.";
  EXPECT_EQ(config::drain_handoff.udp_offset, 100) << "A disabled feature must not change the offset default.";
}

/**
 * @brief Each integer key stores a valid decimal value as parsed, when the feature is off.
 */
TEST_F(HandoffConfigTest, ValidIntegersAreStoredAsParsed) {
  apply(
    "drain_handoff_udp_offset = 200\n"
    "drain_handoff_deadline = 7200\n"
    "drain_handoff_ready_timeout = 45\n"
    "drain_handoff_rollback_window = 90\n"
    "drain_handoff_cooldown = 300\n"
  );

  EXPECT_EQ(config::drain_handoff.udp_offset, 200) << "drain_handoff_udp_offset = 200 must be stored as 200.";
  EXPECT_EQ(config::drain_handoff.deadline_s, 7200) << "drain_handoff_deadline = 7200 must be stored as 7200.";
  EXPECT_EQ(config::drain_handoff.ready_timeout_s, 45) << "drain_handoff_ready_timeout = 45 must be stored as 45.";
  EXPECT_EQ(config::drain_handoff.rollback_window_s, 90) << "drain_handoff_rollback_window = 90 must be stored as 90.";
  EXPECT_EQ(config::drain_handoff.cooldown_s, 300) << "drain_handoff_cooldown = 300 must be stored as 300.";
}

/**
 * @brief `drain_handoff_binary` is stored verbatim; any text is accepted here and checked only at spawn time.
 */
TEST_F(HandoffConfigTest, BinaryIsStoredVerbatim) {
  apply("drain_handoff_binary = /opt/sunshine/bin/sunshine-next\n");
  EXPECT_EQ(config::drain_handoff.binary, "/opt/sunshine/bin/sunshine-next") << "A path must be stored as given.";

  apply("drain_handoff_binary = not-a-real-path-xyz\n");
  EXPECT_EQ(config::drain_handoff.binary, "not-a-real-path-xyz") << "Text that is not a file must still be stored as given.";
}

/**
 * @brief An empty `drain_handoff_binary` is stored as empty, which means the path Sunshine was started from.
 */
TEST_F(HandoffConfigTest, EmptyBinaryMeansStartPath) {
  apply("drain_handoff_binary =\n");

  EXPECT_TRUE(config::drain_handoff.binary.empty()) << "An empty drain_handoff_binary must stay empty.";
}

/**
 * @brief The `drain_handoff` flag is case-sensitive and anything other than `enabled` or `disabled` keeps the feature off.
 */
TEST_F(HandoffConfigTest, EnabledFlagIsCaseSensitiveAndStrict) {
  for (const auto *value : {"Enabled", "ENABLED", "Disabled", "true", "yes", "on", "1", ""}) {
    SCOPED_TRACE(std::string {"drain_handoff = "} + value);
    config::drain_handoff = handoff::settings_t {};
    apply(std::string {"drain_handoff = "} + value + "\n");

    EXPECT_FALSE(config::drain_handoff.enabled) << "drain_handoff must accept only the exact text enabled; got a true flag.";
  }
}

/**
 * @brief Every integer key rejects hex, non-integer, quoted, out-of-int-range and signed-with-plus text; the feature
 *        goes off and that field keeps its default. Empty text is tested separately (`EmptyIntegerValueKeepsDefault`).
 */
TEST_F(HandoffConfigTest, InvalidIntegerTextTurnsFeatureOffAndKeepsDefault) {
  for (const auto &option : INTEGER_OPTIONS) {
    const int default_value = handoff::settings_t {}.*option.field;
    for (const auto text : INVALID_INTEGER_TEXT) {
      SCOPED_TRACE(std::string {option.key} + " = \"" + std::string {text} + "\"");
      config::drain_handoff = handoff::settings_t {};
      apply(std::string {"drain_handoff = enabled\n"} + std::string {option.key} + " = " + std::string {text} + "\n");

      EXPECT_FALSE(config::drain_handoff.enabled) << "An invalid integer must turn the feature off.";
      EXPECT_EQ(config::drain_handoff.*option.field, default_value)
        << "An invalid integer must leave its field at the default, not partially parsed.";
    }
  }
}

/**
 * @brief An empty integer value through the seam keeps the default and does not turn an enabled feature off.
 *
 * The config-file parser drops a `key =` line with an empty value, so the default stays in place.
 */
TEST_F(HandoffConfigTest, EmptyIntegerValueKeepsDefault) {
  apply("drain_handoff_deadline =\n");

  EXPECT_EQ(config::drain_handoff.deadline_s, 3600) << "An empty drain_handoff_deadline must keep the default 3600.";
  EXPECT_FALSE(config::drain_handoff.enabled) << "An empty value must not turn on a feature that was not enabled.";
#ifdef __linux__
  config::drain_handoff = handoff::settings_t {};
  apply("drain_handoff = enabled\ndrain_handoff_deadline =\n");
  EXPECT_TRUE(config::drain_handoff.enabled) << "An empty integer value must not turn an enabled feature off.";
  EXPECT_EQ(config::drain_handoff.deadline_s, 3600) << "An empty drain_handoff_deadline must keep the default with the feature on.";
#endif
}

/**
 * @brief Values at the int limits are parsed (not clamped); validation then refuses them, and the feature goes off.
 */
TEST_F(HandoffConfigTest, IntMaxIsParsedAndRefusedNotClamped) {
  apply("drain_handoff = enabled\ndrain_handoff_udp_offset = 2147483647\n");

  EXPECT_EQ(config::drain_handoff.udp_offset, INT_MAX) << "An out-of-range offset must be stored as parsed, never clamped.";
  EXPECT_FALSE(config::drain_handoff.enabled) << "An offset of INT_MAX must refuse the feature.";
}

/**
 * @brief A negative int limit is parsed and stored as given; the feature goes off because validation refuses it.
 */
TEST_F(HandoffConfigTest, IntMinIsParsedAndNotClamped) {
  apply("drain_handoff = enabled\ndrain_handoff_ready_timeout = -2147483648\n");

  EXPECT_EQ(config::drain_handoff.ready_timeout_s, INT_MIN) << "INT_MIN must be parsed and stored, never clamped.";
  EXPECT_FALSE(config::drain_handoff.enabled) << "A ready timeout below 1 must refuse the feature.";
}

/**
 * @brief When validation refuses a value, the other fields keep the values that were parsed.
 */
TEST_F(HandoffConfigTest, RefusalKeepsOtherFieldsAsParsed) {
  apply("drain_handoff = enabled\ndrain_handoff_cooldown = 10\ndrain_handoff_deadline = 7200\n");

  EXPECT_FALSE(config::drain_handoff.enabled) << "A cooldown shorter than the rollback window must refuse the feature.";
  EXPECT_EQ(config::drain_handoff.cooldown_s, 10) << "The refused field must keep the value that was parsed.";
  EXPECT_EQ(config::drain_handoff.deadline_s, 7200) << "The other fields must keep the values that were parsed.";
}

/**
 * @brief With the feature disabled nothing is checked, so an invalid-but-parseable offset is stored as given.
 */
TEST_F(HandoffConfigTest, DisabledFeatureDoesNotValidate) {
  apply("drain_handoff = disabled\ndrain_handoff_udp_offset = 0\ndrain_handoff_deadline = 0\n");

  EXPECT_FALSE(config::drain_handoff.enabled) << "A disabled feature must stay off.";
  EXPECT_EQ(config::drain_handoff.udp_offset, 0) << "With the feature off, the offset is stored as parsed, not checked.";
  EXPECT_EQ(config::drain_handoff.deadline_s, 0) << "With the feature off, the deadline is stored as parsed, not checked.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Validation through apply_config (refusal turns the feature off on every platform)
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Each rule of `handoff::validate` that the spec names refuses an enabled feature.
 */
TEST_P(HandoffConfigRefusalTest, ViolatingValueTurnsFeatureOff) {
  const auto &refusal = GetParam();
  apply(refusal.lines);

  EXPECT_FALSE(config::drain_handoff.enabled) << "Case " << refusal.name << ": a value that breaks a validation rule must turn the feature off.";
}

INSTANTIATE_TEST_SUITE_P(
  HandoffConfigRefusal,
  HandoffConfigRefusalTest,
  testing::ValuesIn(REFUSAL_CASES),
  [](const testing::TestParamInfo<RefusalCase> &info) {
    return std::string {info.param.name};
  }
);

#ifdef __linux__
/**
 * @brief Boundary values that the spec allows keep the feature on with the defaults for every other key.
 */
TEST_P(HandoffConfigAcceptedTest, BoundaryValueKeepsFeatureOn) {
  const auto &accepted = GetParam();
  apply(accepted.lines);

  EXPECT_TRUE(config::drain_handoff.enabled) << "Case " << accepted.name << ": a value the spec allows must keep the feature on.";
}

INSTANTIATE_TEST_SUITE_P(
  HandoffConfigAccepted,
  HandoffConfigAcceptedTest,
  testing::Values(
    RefusalCase {"DefaultsWithFeatureOn"sv, "drain_handoff = enabled\n"sv},
    RefusalCase {"DeadlineOne"sv, "drain_handoff = enabled\ndrain_handoff_deadline = 1\ndrain_handoff_rollback_window = 1\ndrain_handoff_cooldown = 1\n"sv},
    RefusalCase {"DeadlineOneDay"sv, "drain_handoff = enabled\ndrain_handoff_deadline = 86400\n"sv},
    RefusalCase {"DeadlineEqualToRollback"sv, "drain_handoff = enabled\ndrain_handoff_rollback_window = 60\ndrain_handoff_deadline = 60\n"sv},
    RefusalCase {"CooldownEqualToRollback"sv, "drain_handoff = enabled\ndrain_handoff_rollback_window = 60\ndrain_handoff_cooldown = 60\n"sv},
    RefusalCase {"ReadyTimeoutOne"sv, "drain_handoff = enabled\ndrain_handoff_ready_timeout = 1\n"sv},
    RefusalCase {"RollbackWindowOne"sv, "drain_handoff = enabled\ndrain_handoff_rollback_window = 1\n"sv},
    RefusalCase {"NonDefaultOffsetInsideRange"sv, "drain_handoff = enabled\ndrain_handoff_udp_offset = 200\n"sv}
  ),
  [](const testing::TestParamInfo<RefusalCase> &info) {
    return std::string {info.param.name};
  }
);

/**
 * @brief A valid default config with `drain_handoff = enabled` keeps the feature on and the default offset.
 */
TEST_F(HandoffConfigTest, ConfigWithFeatureEnabledKeepsAllDefaultsValid) {
  apply("drain_handoff = enabled\n");

  EXPECT_TRUE(config::drain_handoff.enabled) << "A valid default config with drain_handoff = enabled must keep the feature on.";
  EXPECT_EQ(config::drain_handoff.udp_offset, 100) << "The default offset must stay 100.";
}
#endif

// ---------------------------------------------------------------------------------------------------------------------
// display_device::has_settings_manager (every platform)
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Before `display_device::init()` has been called, there is no settings manager.
 */
TEST(HandoffDisplayDeviceTest, NoSettingsManagerBeforeInit) {
  EXPECT_FALSE(display_device::has_settings_manager()) << "has_settings_manager() must be false when init() was never called.";
}

#ifdef __linux__
/**
 * @brief `hooks::config_refusal` is empty when the feature is off, whatever the other options hold.
 */
TEST(HandoffHooksConfigRefusalTest, EmptyWhenDisabled) {
  handoff::settings_t settings;
  settings.enabled = false;
  settings.udp_offset = 0;  // would be refused if enabled

  EXPECT_TRUE(handoff::hooks::config_refusal(settings, 47989).empty()) << "A disabled feature must not refuse anything.";
}

/**
 * @brief With the feature on, the refusal is the reason `handoff::validate` returns, and empty for valid options.
 */
TEST(HandoffHooksConfigRefusalTest, MatchesValidateWhenEnabled) {
  handoff::settings_t valid;
  valid.enabled = true;
  EXPECT_TRUE(handoff::hooks::config_refusal(valid, 47989).empty()) << "Default options with the feature on must not be refused.";

  handoff::settings_t invalid;
  invalid.enabled = true;
  invalid.udp_offset = 0;
  const auto reason = handoff::hooks::config_refusal(invalid, 47989);
  EXPECT_FALSE(reason.empty()) << "An offset of 0 with the feature on must be refused.";
  EXPECT_EQ(reason, handoff::validate(invalid, 47989)) << "The refusal must be the reason handoff::validate returns.";
}

/**
 * @brief An offset that makes the two UDP bases share a port is refused for the port passed in, and the refusal text
 *        is the one `handoff::validate` gives for that port.
 */
TEST(HandoffHooksConfigRefusalTest, RefusalForOverlappingBasesMatchesValidateForPort) {
  handoff::settings_t settings;
  settings.enabled = true;
  settings.udp_offset = 1;  // base 47990 shares UDP ports 47999 and 48000 with base 47989

  EXPECT_FALSE(handoff::hooks::config_refusal(settings, 47989).empty()) << "Overlapping UDP bases must be refused for port 47989.";
  EXPECT_EQ(handoff::hooks::config_refusal(settings, 47989), handoff::validate(settings, 47989))
    << "The refusal must be the validate() reason for the port passed in.";
}

/**
 * @brief `hooks::enable_refusal` is empty when the feature is off, even when privileged or the host facts are bad.
 */
TEST_F(HandoffConfigTest, EnableRefusalEmptyWhenDisabled) {
  config::drain_handoff.enabled = false;
  handoff::enable_context_t bad_context;  // capture empty, runtime dir missing: check_enable would refuse

  EXPECT_TRUE(handoff::hooks::enable_refusal(bad_context, true).empty()) << "A disabled feature must not refuse, even under privileged execution.";
  EXPECT_TRUE(handoff::hooks::enable_refusal(bad_context, false).empty()) << "A disabled feature must not run check_enable.";
}

/**
 * @brief With the feature on and privileged execution, the refusal is the fixed privileged text.
 */
TEST_F(HandoffConfigTest, EnableRefusalPrivilegedText) {
  config::drain_handoff.enabled = true;
  handoff::enable_context_t good_context;
  good_context.capture = "wlr";
  good_context.runtime_dir_available = true;

  EXPECT_EQ(
    handoff::hooks::enable_refusal(good_context, true),
    "drain handoff is not available with privileged execution (its environment sanitising drops the SUNSHINE_HANDOFF_* variables)"
  ) << "Privileged execution must give the fixed refusal text.";
}

/**
 * @brief The privileged rule comes before `check_enable`, so a privileged host gets the privileged text even when
 *        `check_enable` would refuse for another reason.
 */
TEST_F(HandoffConfigTest, PrivilegedRefusalComesBeforeCheckEnable) {
  config::drain_handoff.enabled = true;
  handoff::enable_context_t bad_context;  // check_enable would refuse: capture empty

  const auto reason = handoff::hooks::enable_refusal(bad_context, true);
  EXPECT_FALSE(reason.empty()) << "Privileged execution must refuse.";
  EXPECT_NE(reason, handoff::check_enable(bad_context)) << "The privileged text must win over the check_enable text.";
  EXPECT_NE(reason.find("privileged execution"), std::string::npos) << "The privileged refusal must name privileged execution.";
}

/**
 * @brief Without privileged execution, a valid host gives an empty refusal, the same as `check_enable`.
 */
TEST_F(HandoffConfigTest, EnableRefusalEmptyForValidHostWhenNotPrivileged) {
  config::drain_handoff.enabled = true;
  handoff::enable_context_t context;
  context.capture = "wlr";
  context.runtime_dir_available = true;

  EXPECT_TRUE(handoff::check_enable(context).empty()) << "Precondition: check_enable must accept this host.";
  EXPECT_EQ(handoff::hooks::enable_refusal(context, false), handoff::check_enable(context))
    << "Without privileged execution the refusal must be the check_enable result.";
  EXPECT_TRUE(handoff::hooks::enable_refusal(context, false).empty()) << "A valid host must not be refused.";
}

/**
 * @brief Without privileged execution, each host fact that `check_enable` refuses gives that same refusal text.
 */
TEST_F(HandoffConfigTest, EnableRefusalFallsThroughToCheckEnable) {
  config::drain_handoff.enabled = true;

  handoff::enable_context_t wrong_capture;
  wrong_capture.capture = "x11";
  wrong_capture.runtime_dir_available = true;

  handoff::enable_context_t fresh_state;
  fresh_state.capture = "wlr";
  fresh_state.fresh_state = true;
  fresh_state.runtime_dir_available = true;

  handoff::enable_context_t display_manager;
  display_manager.capture = "wlr";
  display_manager.display_settings_manager = true;
  display_manager.runtime_dir_available = true;

  handoff::enable_context_t no_runtime_dir;
  no_runtime_dir.capture = "wlr";

  for (const auto *context : {&wrong_capture, &fresh_state, &display_manager, &no_runtime_dir}) {
    const auto expected = handoff::check_enable(*context);
    ASSERT_FALSE(expected.empty()) << "Precondition: check_enable must refuse this host.";
    EXPECT_EQ(handoff::hooks::enable_refusal(*context, false), expected)
      << "Without privileged execution the refusal must be exactly the check_enable text.";
  }
}
#endif
