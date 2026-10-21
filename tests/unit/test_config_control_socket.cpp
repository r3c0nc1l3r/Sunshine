/**
 * @file tests/unit/test_config_control_socket.cpp
 * @brief Blind tests for the `control_socket` configuration key (`settings_t::control_socket`, `config.h`), written from
 *        docs/drain-handoff.md and the headers only.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#ifndef _WIN32
  #include <unistd.h>
#endif

// local includes
#include <src/config.h>
#include <src/handoff/settings.h>

using namespace std::literals;

namespace {
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
class ConfigControlSocketTest: public testing::Test {
protected:
  /**
   * @brief Save the port, the drain-handoff options and the apps file path, then point the apps file at a temp file.
   */
  void SetUp() override {
    original_port_ = config::sunshine.port;
    original_options_ = config::drain_handoff;
    original_file_apps_ = config::stream.file_apps;
    static std::atomic<unsigned> counter {0};
    long pid = 0;
#ifndef _WIN32
    pid = static_cast<long>(::getpid());
#endif
    file_apps_path_ = std::filesystem::temp_directory_path() / ("sunshine_control_socket_cfg_" + std::to_string(pid) + "_" + std::to_string(++counter) + ".json");
    std::filesystem::remove(file_apps_path_);
    { std::ofstream created {file_apps_path_}; }
    config::stream.file_apps = file_apps_path_.string();
  }

  /**
   * @brief Restore everything saved in `SetUp`.
   */
  void TearDown() override {
    config::sunshine.port = original_port_;
    config::drain_handoff = original_options_;
    config::stream.file_apps = original_file_apps_;
    std::error_code ignored;
    std::filesystem::remove(file_apps_path_, ignored);
  }

  /**
   * @brief Reset the drain-handoff options to their struct defaults, then apply a configuration text made of the fixed
   * port plus the given lines.
   *
   * @param lines Extra configuration lines.
   */
  static void apply(std::string_view lines) {
    config::drain_handoff = handoff::settings_t {};
    config::apply_config_for_test(with_port(lines));
  }

  std::uint16_t original_port_ {};  ///< Port restored after each test.
  handoff::settings_t original_options_;  ///< Options restored after each test.
  std::string original_file_apps_;  ///< Apps file path restored after each test.
  std::filesystem::path file_apps_path_;  ///< Temp apps file created for each test and removed after it.
};

/**
 * @brief `control_socket = enabled` sets `settings_t::control_socket` and leaves the drain handoff off.
 */
TEST_F(ConfigControlSocketTest, EnabledSetsTheFieldWithoutEnablingTheHandoff) {
  apply("control_socket = enabled\n");

  EXPECT_TRUE(config::drain_handoff.control_socket) << "control_socket = enabled must set control_socket to true";
  EXPECT_FALSE(config::drain_handoff.enabled) << "control_socket = enabled must not enable the drain handoff";
}

/**
 * @brief An absent key, and any value that is not exactly `enabled` (here `Enabled`, `yes` and `disabled`), leaves
 * `control_socket` false.
 */
TEST_F(ConfigControlSocketTest, AbsentOrNotExactlyEnabledLeavesItFalse) {
  apply("");
  EXPECT_FALSE(config::drain_handoff.control_socket) << "without a control_socket key the field must be false (default disabled)";

  apply("control_socket = Enabled\n");
  EXPECT_FALSE(config::drain_handoff.control_socket) << "the value is case-sensitive: \"Enabled\" must leave the field false";

  apply("control_socket = yes\n");
  EXPECT_FALSE(config::drain_handoff.control_socket) << "\"yes\" is not an accepted value and must leave the field false";

  apply("control_socket = disabled\n");
  EXPECT_FALSE(config::drain_handoff.control_socket) << "control_socket = disabled must leave the field false";
}

/**
 * @brief The key is read whatever `drain_handoff` says: with `drain_handoff = disabled` it is true, and with an invalid
 * drain-handoff setting it is still true.
 */
TEST_F(ConfigControlSocketTest, ReadIndependentlyOfDrainHandoff) {
  apply("drain_handoff = disabled\ncontrol_socket = enabled\n");
  EXPECT_TRUE(config::drain_handoff.control_socket) << "control_socket = enabled must stay true with drain_handoff = disabled";
  EXPECT_FALSE(config::drain_handoff.enabled) << "drain_handoff = disabled must keep the drain handoff off";

  apply("drain_handoff = enabled\ndrain_handoff_udp_offset = 0\ncontrol_socket = enabled\n");
  EXPECT_TRUE(config::drain_handoff.control_socket) << "control_socket = enabled must stay true even when drain_handoff validation refuses the handoff (offset 0)";
}
