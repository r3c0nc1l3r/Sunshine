/**
 * @file tests/unit/test_nvhttp_state_handoff.cpp
 * @brief Blind tests for the drain-handoff state and counter additions in `nvhttp`: state reload status, checked and
 * frozen saves, pairing session count, `host_audio` and the next launch session id.
 */

#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include <src/config.h>
#include <src/httpcommon.h>
#include <src/nvhttp.h>

namespace fs = std::filesystem;

namespace {
  /**
   * @brief Read a whole text file.
   *
   * @param path File to read.
   * @return File content, empty when the file does not exist.
   */
  std::string read_text(const fs::path &path) {
    std::ifstream input {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
  }

  /**
   * @brief Write text to a file, replacing it.
   *
   * @param path File to write.
   * @param text Text to write.
   */
  void write_text(const fs::path &path, const std::string &text) {
    std::ofstream output {path, std::ios::binary | std::ios::trunc};
    output << text;
  }
}  // namespace

/**
 * @brief Isolate state-file, freeze and counter tests from the user's Sunshine state.
 */
class NvhttpHandoffStateTest: public BaseTest {
protected:
  /**
   * @brief Point the state file at a per-test temp file and reset in-memory state.
   */
  void SetUp() override {
    BaseTest::SetUp();
    const auto *info_ptr = testing::UnitTest::GetInstance()->current_test_info();
    test_directory = fs::path {SUNSHINE_TEST_BIN_DIR} / (std::string {"nvhttp_handoff_"} + info_ptr->name());
    fs::remove_all(test_directory);
    fs::create_directories(test_directory);
    state_file = test_directory / "sunshine_state.json";

    original_state_file = config::nvhttp.file_state;
    original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    original_unique_id = http::unique_id;
    original_host_audio = nvhttp::host_audio();

    config::nvhttp.file_state = state_file.string();
    config::sunshine.flags[config::flag::FRESH_STATE] = false;
    nvhttp::set_state_frozen(false);
    nvhttp::test_support::reset_client_state();
    http::unique_id = "test-host-unique-id";
  }

  /**
   * @brief Restore the caller's configuration and remove the per-test directory.
   */
  void TearDown() override {
    nvhttp::set_state_frozen(false);
    nvhttp::test_support::reset_client_state();
    nvhttp::set_host_audio(original_host_audio);

    config::nvhttp.file_state = original_state_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
    http::unique_id = original_unique_id;

    std::error_code ignored;
    fs::remove_all(test_directory, ignored);
    BaseTest::TearDown();
  }

  /**
   * @brief Add a paired and enabled client and return its certificate PEM.
   *
   * @return PEM certificate of the added client.
   */
  std::string add_paired_client() {
    const auto credentials = test_utils::certificates::generate_ca_credentials();
    const auto uuid = nvhttp::test_support::add_client("handoff-client", credentials.x509, true);
    EXPECT_FALSE(uuid.empty()) << "add_client must return a uuid for a valid certificate";
    return credentials.x509;
  }

  fs::path test_directory;  ///< Per-test directory holding the state file.
  fs::path state_file;  ///< State file the tests read and write.
  std::string original_state_file;  ///< State-file setting restored after each test.
  bool original_fresh_state {false};  ///< Fresh-state flag restored after each test.
  std::string original_unique_id;  ///< Host unique id restored after each test.
  bool original_host_audio {false};  ///< `host_audio` value restored after each test.
};

// ---- reload_state -------------------------------------------------------------------------------------------------

TEST_F(NvhttpHandoffStateTest, ReloadStateLoadedReplacesUniqueIdFromFile) {
  write_text(state_file, R"({"root":{"uniqueid":"reloaded-host-id"}})");

  const auto status = nvhttp::reload_state();

  EXPECT_EQ(status, nvhttp::load_status_e::loaded) << "a file with root.uniqueid must report loaded";
  EXPECT_EQ(http::unique_id, "reloaded-host-id") << "a loaded reload must replace the in-memory host unique id with the file's value";
}

TEST_F(NvhttpHandoffStateTest, ReloadStateLoadedRestoresPairedClients) {
  const auto cert = add_paired_client();
  http::unique_id = "client-host-id";
  ASSERT_TRUE(nvhttp::save_state_checked()) << "precondition: the state must be saved before the reload";

  nvhttp::test_support::reset_client_state();
  ASSERT_FALSE(nvhttp::test_support::authorize_client_certificate(cert)) << "precondition: the client must be gone from memory";

  EXPECT_EQ(nvhttp::reload_state(), nvhttp::load_status_e::loaded) << "the saved state file must reload as loaded";
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(cert)) << "a loaded reload must restore the paired client from the file";
}

TEST_F(NvhttpHandoffStateTest, ReloadStateNoFileChangesNothing) {
  const auto cert = add_paired_client();
  ASSERT_TRUE(nvhttp::test_support::authorize_client_certificate(cert));
  http::unique_id = "keep-this-id";
  fs::remove(state_file);

  EXPECT_EQ(nvhttp::reload_state(), nvhttp::load_status_e::no_file) << "a missing state file must report no_file";

  EXPECT_EQ(http::unique_id, "keep-this-id") << "no_file must not change the host unique id (no new id may be generated)";
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(cert)) << "no_file must not change the paired clients";
  EXPECT_FALSE(fs::exists(state_file)) << "reload_state must not create the state file";
}

TEST_F(NvhttpHandoffStateTest, ReloadStateMissingUniqueIdChangesNothing) {
  const auto cert = add_paired_client();
  ASSERT_TRUE(nvhttp::test_support::authorize_client_certificate(cert));
  http::unique_id = "keep-this-id";
  const std::string content = R"({"root":{"other_key":"value"}})";
  write_text(state_file, content);

  EXPECT_EQ(nvhttp::reload_state(), nvhttp::load_status_e::no_uniqueid) << "a parsed file without root.uniqueid must report no_uniqueid";

  EXPECT_EQ(http::unique_id, "keep-this-id") << "no_uniqueid must not generate or replace the host unique id";
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(cert)) << "no_uniqueid must not change the paired clients";
  EXPECT_EQ(read_text(state_file), content) << "reload_state must not rewrite the file";
}

TEST_F(NvhttpHandoffStateTest, ReloadStateUnparsableFileIsUnreadableAndChangesNothing) {
  const auto cert = add_paired_client();
  ASSERT_TRUE(nvhttp::test_support::authorize_client_certificate(cert));
  http::unique_id = "keep-this-id";
  write_text(state_file, "{ this is not json");

  EXPECT_EQ(nvhttp::reload_state(), nvhttp::load_status_e::unreadable) << "a file that does not parse must report unreadable";

  EXPECT_EQ(http::unique_id, "keep-this-id") << "unreadable must keep the host unique id it had";
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(cert)) << "unreadable must keep the paired clients it had";
}

TEST_F(NvhttpHandoffStateTest, ReloadStateEmptyFileIsUnreadable) {
  http::unique_id = "keep-this-id";
  write_text(state_file, "");

  EXPECT_EQ(nvhttp::reload_state(), nvhttp::load_status_e::unreadable) << "an empty file does not parse and must report unreadable";
  EXPECT_EQ(http::unique_id, "keep-this-id") << "unreadable must not change the host unique id";
}

TEST_F(NvhttpHandoffStateTest, ReloadStateMalformedClientListIsUnreadableAndChangesNothing) {
  // A non-empty scalar where the client list belongs is malformed (nvhttp.h, load_status_e::unreadable).
  http::unique_id = "keep-this-id";
  write_text(state_file, R"({"root":{"uniqueid":"new-id-from-bad-file","named_devices":"not-a-list"}})");

  EXPECT_EQ(nvhttp::reload_state(), nvhttp::load_status_e::unreadable) << "a malformed client list must report unreadable";
  EXPECT_EQ(http::unique_id, "keep-this-id") << "a file that fails to parse completely must not replace the host unique id";
}

TEST_F(NvhttpHandoffStateTest, ReloadStateEmptyNamedDevicesLoadsAsEmptyList) {
  // An empty string is the empty array in the state file format, so it is an empty client list, not malformed.
  const auto cert = add_paired_client();
  ASSERT_TRUE(nvhttp::test_support::authorize_client_certificate(cert)) << "precondition: the client must be authorized before the reload";
  write_text(state_file, R"({"root":{"uniqueid":"empty-list-host-id","named_devices":""}})");

  EXPECT_EQ(nvhttp::reload_state(), nvhttp::load_status_e::loaded) << "named_devices with an empty string must load, not be unreadable";
  EXPECT_EQ(http::unique_id, "empty-list-host-id") << "a loaded reload must take the file's host unique id";
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(cert)) << "an empty client list must replace the in-memory paired clients with none";
}

TEST_F(NvhttpHandoffStateTest, ReloadStateFrozenStillReloads) {
  write_text(state_file, R"({"root":{"uniqueid":"frozen-reload-id"}})");
  nvhttp::set_state_frozen(true);

  EXPECT_EQ(nvhttp::reload_state(), nvhttp::load_status_e::loaded) << "freezing saves must not block reading the state file";
  EXPECT_EQ(http::unique_id, "frozen-reload-id");
}

// ---- save_state_checked -------------------------------------------------------------------------------------------

TEST_F(NvhttpHandoffStateTest, SaveStateCheckedCreatesFileWithUniqueId) {
  http::unique_id = "save-checked-id";
  add_paired_client();
  fs::remove(state_file);  // add_client already wrote the file; start from no file
  ASSERT_FALSE(fs::exists(state_file));

  EXPECT_TRUE(nvhttp::save_state_checked()) << "a save with no existing file and a valid directory must return true";

  ASSERT_TRUE(fs::exists(state_file)) << "save_state_checked must create the state file";
  const auto json = nlohmann::json::parse(read_text(state_file), nullptr, false);
  ASSERT_FALSE(json.is_discarded()) << "the saved state file must be valid JSON";
  ASSERT_TRUE(json.contains("root")) << "the saved state must have a root object";
  EXPECT_EQ(json["root"].value("uniqueid", std::string {}), "save-checked-id") << "the saved file must hold the in-memory host unique id";
}

TEST_F(NvhttpHandoffStateTest, SaveStateCheckedKeepsOtherKeysInExistingFile) {
  write_text(state_file, R"({"root":{"uniqueid":"merge-host-id","custom_key":"kept-value"}})");
  ASSERT_EQ(nvhttp::reload_state(), nvhttp::load_status_e::loaded) << "precondition: the file must load";
  add_paired_client();

  ASSERT_TRUE(nvhttp::save_state_checked()) << "a save over a readable file must return true";

  const auto json = nlohmann::json::parse(read_text(state_file), nullptr, false);
  ASSERT_FALSE(json.is_discarded()) << "the rewritten state file must be valid JSON";
  EXPECT_EQ(json["root"].value("custom_key", std::string {}), "kept-value") << "keys the save does not own must be kept in the file";
  EXPECT_EQ(json["root"].value("uniqueid", std::string {}), "merge-host-id") << "the host unique id must be written back";
}

TEST_F(NvhttpHandoffStateTest, SaveStateCheckedLeavesNoTemporaryFile) {
  add_paired_client();
  ASSERT_TRUE(nvhttp::save_state_checked());
  ASSERT_TRUE(nvhttp::save_state_checked());

  int entries = 0;
  for (const auto &entry : fs::directory_iterator {test_directory}) {
    ++entries;
    EXPECT_EQ(entry.path().filename().string(), "sunshine_state.json") << "a save must not leave any temporary file next to the state file";
  }
  EXPECT_EQ(entries, 1) << "only the state file may remain in the directory after saving";
}

TEST_F(NvhttpHandoffStateTest, SaveStateCheckedFailsOnUnparsableExistingFileAndWritesNothing) {
  add_paired_client();  // add_client saves the state file itself, so write the broken file after it
  const std::string broken = "{ broken state";
  write_text(state_file, broken);

  EXPECT_FALSE(nvhttp::save_state_checked()) << "an existing file that cannot be parsed must make the save fail";
  EXPECT_EQ(read_text(state_file), broken) << "a failed save must not change the existing file";
}

TEST_F(NvhttpHandoffStateTest, SaveStateCheckedFailsWhenDirectoryIsMissing) {
  config::nvhttp.file_state = (test_directory / "no_such_dir" / "sunshine_state.json").string();
  add_paired_client();

  EXPECT_FALSE(nvhttp::save_state_checked()) << "a write into a missing directory must make the save fail";
  EXPECT_FALSE(fs::exists(test_directory / "no_such_dir")) << "the save must not create the missing directory";
}

TEST_F(NvhttpHandoffStateTest, SaveStateCheckedWhileFrozenWritesNothingAndReturnsTrue) {
  add_paired_client();
  fs::remove(state_file);  // add_client already wrote the file; remove it so the frozen save's absence check is meaningful
  nvhttp::set_state_frozen(true);
  ASSERT_TRUE(nvhttp::state_frozen());

  EXPECT_TRUE(nvhttp::save_state_checked()) << "while frozen the save must return true: nothing was lost on purpose";
  EXPECT_FALSE(fs::exists(state_file)) << "while frozen the save must not create the state file";
}

TEST_F(NvhttpHandoffStateTest, SaveStateCheckedWhileFrozenDoesNotTouchBrokenFile) {
  add_paired_client();  // add_client saves the state file itself, so write the broken file after it
  const std::string broken = "{ still broken";
  write_text(state_file, broken);
  nvhttp::set_state_frozen(true);

  EXPECT_TRUE(nvhttp::save_state_checked()) << "while frozen the save returns true even when the existing file would fail to parse";
  EXPECT_EQ(read_text(state_file), broken) << "while frozen nothing may be written";
}

TEST_F(NvhttpHandoffStateTest, SaveStateCheckedWritesAgainAfterUnfreeze) {
  add_paired_client();
  fs::remove(state_file);  // add_client already wrote the file; remove it so the absence check after the frozen save is meaningful
  nvhttp::set_state_frozen(true);
  ASSERT_TRUE(nvhttp::save_state_checked());
  ASSERT_FALSE(fs::exists(state_file));

  nvhttp::set_state_frozen(false);
  EXPECT_TRUE(nvhttp::save_state_checked()) << "after unfreezing the save must write again";
  EXPECT_TRUE(fs::exists(state_file)) << "after unfreezing the state file must be written";
}

TEST_F(NvhttpHandoffStateTest, FrozenBlocksClientEnableChangeFromWritingFile) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("frozen-enable", credentials.x509, true);
  ASSERT_FALSE(uuid.empty()) << "precondition: add_client must return a uuid";
  ASSERT_TRUE(fs::exists(state_file)) << "precondition: add_client writes the state file, so the content can be compared";
  const auto before = read_text(state_file);
  nvhttp::set_state_frozen(true);

  (void) nvhttp::set_client_enabled(uuid, false);

  EXPECT_EQ(read_text(state_file), before) << "while frozen a client enable/disable save must not rewrite the state file";
}

// ---- legacy root.devices ----------------------------------------------------------------------------------------

namespace {
  /**
   * @brief Build a state file with a legacy `root.devices` list, the pre-`named_devices` Sunshine shape.
   *
   * Each legacy entry has `uniqueid` and a `certs` list of PEM certificates; the loader imports it into
   * `root.named_devices`.
   *
   * @param pem PEM certificate of the legacy client.
   * @return The state file text.
   */
  std::string legacy_state_text(const std::string &pem) {
    nlohmann::json legacy_entry {{"uniqueid", "legacy-device-uniqueid"}, {"certs", nlohmann::json::array({pem})}};
    nlohmann::json state {
      {"root", {{"uniqueid", "legacy-host-id"}, {"custom_key", "kept-value"}, {"devices", nlohmann::json::array({legacy_entry})}}},
    };
    return state.dump();
  }
}  // namespace

TEST_F(NvhttpHandoffStateTest, LegacyDevicesAreImportedOnReload) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  write_text(state_file, legacy_state_text(credentials.x509));

  EXPECT_EQ(nvhttp::reload_state(), nvhttp::load_status_e::loaded) << "a legacy state file with root.uniqueid must load";
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509)) << "the legacy root.devices client must be imported on load";
}

TEST_F(NvhttpHandoffStateTest, SaveAfterLegacyLoadRemovesDevicesAndKeepsOtherKeys) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  write_text(state_file, legacy_state_text(credentials.x509));
  ASSERT_EQ(nvhttp::reload_state(), nvhttp::load_status_e::loaded) << "precondition: the legacy file must load";

  ASSERT_TRUE(nvhttp::save_state_checked()) << "a save over a loaded legacy file must return true";

  const auto json = nlohmann::json::parse(read_text(state_file), nullptr, false);
  ASSERT_FALSE(json.is_discarded()) << "the saved state file must be valid JSON";
  ASSERT_TRUE(json.contains("root")) << "the saved state must keep the root object";
  EXPECT_FALSE(json["root"].contains("devices")) << "the save must drop the legacy root.devices list";
  EXPECT_EQ(json["root"].value("custom_key", std::string {}), "kept-value") << "keys other than the legacy list must be kept";
  EXPECT_EQ(json["root"].value("uniqueid", std::string {}), "legacy-host-id") << "the host unique id must be kept";
}

TEST_F(NvhttpHandoffStateTest, UnpairedLegacyClientStaysUnknownAfterSaveAndReload) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  write_text(state_file, legacy_state_text(credentials.x509));
  ASSERT_EQ(nvhttp::reload_state(), nvhttp::load_status_e::loaded) << "precondition: the legacy file must load";
  ASSERT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509)) << "precondition: the legacy client must be imported";

  const auto clients = nvhttp::get_all_clients();
  ASSERT_EQ(clients.size(), 1u) << "precondition: exactly the one imported client must be paired";
  const auto uuid = clients.front().value("uuid", std::string {});
  ASSERT_FALSE(uuid.empty()) << "the imported client must have a uuid to unpair";

  ASSERT_TRUE(nvhttp::unpair_client(uuid)) << "unpairing the imported client must succeed";
  ASSERT_TRUE(nvhttp::save_state_checked()) << "the save after unpairing must succeed";
  EXPECT_EQ(nvhttp::reload_state(), nvhttp::load_status_e::loaded) << "the saved state must reload as loaded";

  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509)) << "an unpaired legacy client must not be re-imported from root.devices on a later load";
  EXPECT_EQ(nvhttp::get_all_clients().size(), 0u) << "no client may come back after the save and reload";
}

// ---- set_state_frozen / state_frozen ------------------------------------------------------------------------------

TEST_F(NvhttpHandoffStateTest, StateFrozenFollowsSetStateFrozen) {
  nvhttp::set_state_frozen(true);
  EXPECT_TRUE(nvhttp::state_frozen()) << "state_frozen must report true after set_state_frozen(true)";

  nvhttp::set_state_frozen(false);
  EXPECT_FALSE(nvhttp::state_frozen()) << "state_frozen must report false after set_state_frozen(false)";
}

TEST_F(NvhttpHandoffStateTest, SetStateFrozenIsIdempotent) {
  nvhttp::set_state_frozen(true);
  nvhttp::set_state_frozen(true);
  EXPECT_TRUE(nvhttp::state_frozen());
  nvhttp::set_state_frozen(false);
  nvhttp::set_state_frozen(false);
  EXPECT_FALSE(nvhttp::state_frozen());
}

// ---- pairing_session_count ----------------------------------------------------------------------------------------

TEST_F(NvhttpHandoffStateTest, PairingSessionCountIsUnchangedByCompletingUnknownSession) {
  const auto before = nvhttp::pairing_session_count();

  EXPECT_FALSE(nvhttp::test_support::complete_pairing("no-such-pairing-session", false)) << "precondition: an unknown session cannot be completed";

  EXPECT_EQ(nvhttp::pairing_session_count(), before) << "pairing_session_count must not change when no session is removed";
}

TEST_F(NvhttpHandoffStateTest, PairingSessionCountIsCallableRepeatedly) {
  const auto first = nvhttp::pairing_session_count();
  const auto second = nvhttp::pairing_session_count();
  EXPECT_EQ(first, second) << "with no pairing activity the count must be stable across calls";
}

// SPEC-QUESTION: a positive count needs a pairing session that is waiting for its PIN. No test seam in the header
// creates one without the HTTP pairing handler, so the positive case (a waiting session is counted) is not covered
// here.

// ---- host_audio ---------------------------------------------------------------------------------------------------

TEST_F(NvhttpHandoffStateTest, HostAudioRoundTripsTrueAndFalse) {
  nvhttp::set_host_audio(true);
  EXPECT_TRUE(nvhttp::host_audio()) << "host_audio must return the value set to true";

  nvhttp::set_host_audio(false);
  EXPECT_FALSE(nvhttp::host_audio()) << "host_audio must return the value set to false";
}

TEST_F(NvhttpHandoffStateTest, HostAudioLastSetWins) {
  nvhttp::set_host_audio(true);
  nvhttp::set_host_audio(false);
  nvhttp::set_host_audio(true);
  EXPECT_TRUE(nvhttp::host_audio()) << "the value must be the last one set";
}

// ---- next_launch_session_id ---------------------------------------------------------------------------------------

TEST_F(NvhttpHandoffStateTest, NextLaunchSessionIdReturnsValueSet) {
  nvhttp::set_next_launch_session_id(5);
  EXPECT_EQ(nvhttp::next_launch_session_id(), 5u) << "the next id must be the value passed to set_next_launch_session_id";
}

TEST_F(NvhttpHandoffStateTest, NextLaunchSessionIdIsSnapshotAndDoesNotAdvance) {
  nvhttp::set_next_launch_session_id(7);
  EXPECT_EQ(nvhttp::next_launch_session_id(), 7u);
  EXPECT_EQ(nvhttp::next_launch_session_id(), 7u) << "reading the next id must not allocate or advance it";
}

TEST_F(NvhttpHandoffStateTest, NextLaunchSessionIdAcceptsOne) {
  nvhttp::set_next_launch_session_id(1);
  EXPECT_EQ(nvhttp::next_launch_session_id(), 1u) << "ids start at 1, so a next id of 1 must be reported as 1";
}

TEST_F(NvhttpHandoffStateTest, NextLaunchSessionIdWrapsAtUint32Max) {
  nvhttp::set_next_launch_session_id(UINT32_MAX);
  EXPECT_EQ(nvhttp::next_launch_session_id(), UINT32_MAX) << "the largest 32-bit id must be stored and reported unchanged";
}

TEST_F(NvhttpHandoffStateTest, NextLaunchSessionIdZeroIsTreatedAsOne) {
  // The header says ids start at 1, so a next id of 0 is treated as 1.
  nvhttp::set_next_launch_session_id(0);
  EXPECT_EQ(nvhttp::next_launch_session_id(), 1u) << "after set_next_launch_session_id(0) the next id must be 1, since ids start at 1";
}
