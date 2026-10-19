/**
 * @file tests/unit/test_config.cpp
 * @brief Tests for Sunshine configuration persistence helpers.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// local includes
#include <src/config.h>
#include <src/file_handler.h>
#include <src/platform/common.h>

using namespace std::literals;

using NvencPresetNameParam = std::pair<int, std::string_view>;

/**
 * @brief Parameterized coverage for FFmpeg's symbolic NVENC preset names.
 */
struct NvencPresetNameTest: testing::TestWithParam<NvencPresetNameParam> {};

TEST_P(NvencPresetNameTest, UsesStableSymbolicName) {
  const auto &[quality_preset, expected] = GetParam();

  EXPECT_EQ(expected, config::nv::ffmpeg_preset_from_quality(quality_preset));
}

INSTANTIATE_TEST_SUITE_P(
  NvencQualityPresets,
  NvencPresetNameTest,
  testing::Values(
    NvencPresetNameParam {1, "p1"sv},
    NvencPresetNameParam {2, "p2"sv},
    NvencPresetNameParam {3, "p3"sv},
    NvencPresetNameParam {4, "p4"sv},
    NvencPresetNameParam {5, "p5"sv},
    NvencPresetNameParam {6, "p6"sv},
    NvencPresetNameParam {7, "p7"sv}
  )
);

namespace {

  /**
   * @brief Fixture that redirects configuration persistence to a temporary file.
   */
  class ConfigPersistenceTest: public testing::Test {
  protected:
    /**
     * @brief Preserve the active config path and prepare an empty temporary path.
     */
    void SetUp() override {
      original_config_file_ = config::sunshine.config_file;
      original_gamepad_driver_ = config::input.gamepad_driver;
      config_file_ = std::filesystem::temp_directory_path() / "sunshine_test_config_persistence.conf";  // NOSONAR(cpp:S5443): safe for tests
      std::filesystem::remove(config_file_);
      config::sunshine.config_file = config_file_.string();
      config::input.gamepad_driver.clear();
    }

    /**
     * @brief Remove the temporary file and restore the active config path.
     */
    void TearDown() override {
      config::sunshine.config_file = std::move(original_config_file_);
      config::input.gamepad_driver = std::move(original_gamepad_driver_);
      std::filesystem::remove(config_file_);
    }

    /**
     * @brief Get the temporary configuration file used by the test.
     *
     * @return Temporary configuration file path.
     */
    const std::filesystem::path &config_file() const {
      return config_file_;
    }

  private:
    std::filesystem::path config_file_;  ///< Temporary configuration file used by the test.
    std::string original_config_file_;  ///< Active configuration path restored after each test.
    std::string original_gamepad_driver_;  ///< Gamepad driver preference restored after each test.
  };

}  // namespace

TEST_F(ConfigPersistenceTest, AppendsMissingOptionWithoutReplacingExistingText) {
  ASSERT_EQ(file_handler::write_file(config_file().string().c_str(), "# retained comment"), 0);

  EXPECT_TRUE(config::persist_config_option_if_missing("gamepad_driver", "all"));
  EXPECT_EQ(
    file_handler::read_file(config_file().string().c_str()),
    "# retained comment\ngamepad_driver = all\n"
  );

  EXPECT_FALSE(config::persist_config_option_if_missing("gamepad_driver", "virtualhid"));
  EXPECT_EQ(
    file_handler::read_file(config_file().string().c_str()),
    "# retained comment\ngamepad_driver = all\n"
  );
}

TEST_F(ConfigPersistenceTest, DoesNotReplaceAnExistingOption) {
  ASSERT_EQ(file_handler::write_file(config_file().string().c_str(), "gamepad_driver = vigembus\n"), 0);

  EXPECT_FALSE(config::persist_config_option_if_missing("gamepad_driver", "all"));
  EXPECT_EQ(file_handler::read_file(config_file().string().c_str()), "gamepad_driver = vigembus\n");
}

TEST_F(ConfigPersistenceTest, SelectsAllDriversOnlyForLicensedUsersWithoutAPreference) {
  EXPECT_FALSE(config::select_all_gamepad_drivers_if_licensed(false));
  EXPECT_TRUE(config::input.gamepad_driver.empty());
  EXPECT_TRUE(file_handler::read_file(config_file().string().c_str()).empty());

  EXPECT_TRUE(config::select_all_gamepad_drivers_if_licensed(true));
  EXPECT_EQ(config::input.gamepad_driver, config::GAMEPAD_DRIVER_ALL);
  EXPECT_EQ(file_handler::read_file(config_file().string().c_str()), "gamepad_driver = all\n");

  EXPECT_FALSE(config::select_all_gamepad_drivers_if_licensed(true));
  EXPECT_EQ(file_handler::read_file(config_file().string().c_str()), "gamepad_driver = all\n");
}

namespace {

  /**
   * @brief Copy the names of the gamepad options reported by the platform.
   *
   * @details The platform list is reassigned on every call, so the names are copied out before any other call.
   *
   * @return Names of every gamepad option, in the order reported by the platform.
   */
  std::vector<std::string> supported_gamepad_names() {
    std::vector<std::string> names;
    for (const auto &gamepad : platf::supported_gamepads(nullptr)) {
      names.push_back(gamepad.name);
    }
    return names;
  }

  /**
   * @brief Fixture that saves and restores the input and modified-setting state around each test.
   */
  class ConfigGamepadOptionsTest: public testing::Test {
  protected:
    /**
     * @brief Save every configuration global and clear the gamepad driver policy before each test.
     */
    void SetUp() override {
      original_video_ = config::video;
      original_audio_ = config::audio;
      original_stream_ = config::stream;
      original_nvhttp_ = config::nvhttp;
      original_input_ = config::input;
      original_sunshine_ = config::sunshine;
      original_modified_config_settings_ = config::modified_config_settings;
      config::input.gamepad_driver.clear();

      apps_file_ = std::filesystem::temp_directory_path() /
                   ("sunshine_test_gamepad_options_apps_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");  // NOSONAR(cpp:S5443): safe for tests
      ASSERT_EQ(file_handler::write_file(apps_file_.string().c_str(), "{\"apps\": []}\n"), 0);
      config::stream.file_apps = apps_file_.string();
    }

    /**
     * @brief Apply a single `gamepad` configuration line, keeping the temporary apps file configured.
     *
     * @param value Gamepad name written after `gamepad =`.
     */
    void apply_gamepad_line(std::string_view value) {
      config::apply_config_for_test(std::string("file_apps = ") + apps_file_.string() + "\ngamepad = " + std::string(value) + "\n");
    }

    /**
     * @brief Restore every configuration global after each test.
     */
    void TearDown() override {
      config::video = std::move(original_video_);
      config::audio = std::move(original_audio_);
      config::stream = std::move(original_stream_);
      config::nvhttp = std::move(original_nvhttp_);
      config::input = std::move(original_input_);
      config::sunshine = std::move(original_sunshine_);
      config::modified_config_settings = std::move(original_modified_config_settings_);
      std::error_code ec;
      std::filesystem::remove(apps_file_, ec);
    }

  private:
    std::filesystem::path apps_file_;  ///< Temporary apps file that `file_apps` points at during each test.
    config::video_t original_video_;  ///< Video configuration restored after each test.
    config::audio_t original_audio_;  ///< Audio configuration restored after each test.
    config::stream_t original_stream_;  ///< Stream configuration restored after each test.
    config::nvhttp_t original_nvhttp_;  ///< HTTP configuration restored after each test.
    config::input_t original_input_;  ///< Input configuration restored after each test.
    config::sunshine_t original_sunshine_;  ///< Core configuration restored after each test.
    decltype(config::modified_config_settings) original_modified_config_settings_;  ///< Modified settings restored after each test.
  };

}  // namespace

TEST_F(ConfigGamepadOptionsTest, AcceptsEverySupportedGamepadName) {
  const auto names = supported_gamepad_names();
  ASSERT_FALSE(names.empty()) << "The platform reported no supported gamepad options.";

  for (std::size_t i = 0; i < names.size(); ++i) {
    const auto &name = names[i];
    const auto &other = names[(i + 1) % names.size()];
    SCOPED_TRACE(name);

    ASSERT_NE(other, name) << "The test needs at least two distinct supported gamepad names.";

    // Start from a different valid value so that acceptance is observable.
    config::input.gamepad = other;
    apply_gamepad_line(name);

    EXPECT_EQ(config::input.gamepad, name) << "Supported gamepad name was not accepted: " << name;
  }
}

TEST_F(ConfigGamepadOptionsTest, RejectsUnsupportedGamepadNameAndKeepsPreviousValue) {
  const auto names = supported_gamepad_names();
  ASSERT_FALSE(names.empty()) << "The platform reported no supported gamepad options.";

  config::input.gamepad = names.front();
  apply_gamepad_line("not-a-real-gamepad");

  EXPECT_EQ(config::input.gamepad, names.front()) << "An unsupported gamepad name replaced the previous value.";
}

TEST_F(ConfigGamepadOptionsTest, StaysCorrectAcrossRepeatedLoads) {
  const auto names = supported_gamepad_names();
  ASSERT_FALSE(names.empty()) << "The platform reported no supported gamepad options.";

  // Repeated applies exercise the allowed-value list on every load.
  constexpr int load_count = 5;
  for (int load = 0; load < load_count; ++load) {
    const auto &name = names[static_cast<std::size_t>(load) % names.size()];
    SCOPED_TRACE(load);

    config::input.gamepad.clear();
    apply_gamepad_line(name);

    EXPECT_EQ(config::input.gamepad, name) << "Supported gamepad name was not accepted on load " << load;
  }

  const auto &last_accepted = names[static_cast<std::size_t>(load_count - 1) % names.size()];
  apply_gamepad_line("not-a-real-gamepad");

  EXPECT_EQ(config::input.gamepad, last_accepted) << "An unsupported gamepad name was accepted after repeated loads.";

  config::input.gamepad.clear();
  apply_gamepad_line(names.front());

  EXPECT_EQ(config::input.gamepad, names.front()) << "A supported gamepad name was rejected after repeated loads.";
}
