/**
 * @file tests/unit/test_process_digest_env.cpp
 * @brief Blind tests for fork #47: the app digest covers the `apps.json` top-level `env` section as written, not the
 * process environment.
 *
 * Written from the doc comments of `src/process.h`, `src/handoff/app_digest.h` and `src/handoff/sd_notify.h`, and from
 * the #47 contract. Only public interfaces are used: `proc::parse`, `proc_t::apps_env_section`, `proc::digest_input`,
 * `handoff::app_digest` and `handoff::sd::notify`.
 */
#ifdef __linux__

  // test includes
  #include "../tests_common.h"

  // standard includes
  #include <atomic>
  #include <cstdio>
  #include <cstdlib>
  #include <filesystem>
  #include <fstream>
  #include <optional>
  #include <string>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>
  #include <utility>
  #include <vector>

  // local includes
  #include <src/handoff/app_digest.h>
  #include <src/handoff/sd_notify.h>
  #include <src/process.h>

namespace fs = std::filesystem;

namespace {
  using env_list_t = std::vector<std::pair<std::string, std::string>>;

  /**
   * @brief Process-unique number for temp names (pid plus a counter).
   * @return Name fragment that differs between calls and between test processes.
   */
  std::string unique_tag() {
    static std::atomic<unsigned> counter {0};
    return std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(++counter);
  }

  /**
   * @brief Build the text of an `apps.json` with one placebo app that has one prep command.
   *
   * @param env_entries Top-level `env` section entries, written as given (raw values).
   * @param prep_do The `do` command of the app's prep entry.
   * @return JSON text.
   */
  std::string apps_json(const env_list_t &env_entries, const std::string &prep_do) {
    std::string env_text;
    for (const auto &[name, value] : env_entries) {
      if (!env_text.empty()) {
        env_text += ", ";
      }
      env_text += "\"" + name + "\": \"" + value + "\"";
    }
    return "{\"env\": {" + env_text +
           "}, \"apps\": [{\"name\": \"Placebo\", \"cmd\": \"\", "
           "\"prep-cmd\": [{\"do\": \"" +
           prep_do + "\", \"undo\": \"echo undo\"}]}]}";
  }
}  // namespace

/**
 * @brief Fixture: temp directory for `apps.json` files and the environment variables these tests change.
 *
 * SetUp saves `SUNSHINE_TEST_UNRELATED` and `NOTIFY_SOCKET`; TearDown restores them (or unsets them when they were
 * not set), closes the test socket and removes the temp directory.
 */
class ProcessDigestEnvTest: public BaseTest {
protected:
  /**
   * @brief Create the temp directory and save the environment variables that the tests change.
   */
  void SetUp() override {
    BaseTest::SetUp();
    _dir = fs::temp_directory_path() / ("sunshine_digest_env_" + unique_tag());
    fs::create_directories(_dir);
    _saved_unrelated = save("SUNSHINE_TEST_UNRELATED");
    _saved_notify = save("NOTIFY_SOCKET");
  }

  /**
   * @brief Close the socket, restore the environment variables and remove the temp directory.
   */
  void TearDown() override {
    if (_sock_fd >= 0) {
      ::close(_sock_fd);
      _sock_fd = -1;
    }
    restore("SUNSHINE_TEST_UNRELATED", _saved_unrelated);
    restore("NOTIFY_SOCKET", _saved_notify);
    std::error_code ignored;
    fs::remove_all(_dir, ignored);
    BaseTest::TearDown();
  }

  /**
   * @brief Write an `apps.json` into the temp directory.
   *
   * @param name File name.
   * @param text JSON text.
   * @return Path of the file.
   */
  fs::path write_file(const std::string &name, const std::string &text) const {
    const auto path = _dir / name;
    std::ofstream out(path);
    out << text;
    return path;
  }

  /**
   * @brief Parse an `apps.json` and compute the digest of its first app, as `GO` would.
   *
   * @param path File to parse.
   * @return Lowercase hex digest; empty when the file did not parse or has no app.
   */
  static std::string digest_of(const fs::path &path) {
    auto parsed = proc::parse(path.string());
    if (!parsed.has_value() || parsed->get_apps().empty()) {
      return {};
    }
    const auto &app = parsed->get_apps().front();
    return handoff::app_digest(proc::digest_input(app, parsed->apps_env_section()));
  }

  /**
   * @brief Create a bound AF_UNIX datagram socket in the temp directory and remember its fd for TearDown.
   *
   * @return Socket path (short enough for `sockaddr_un`).
   */
  std::string bind_notify_socket() {
    const std::string path = (_dir / "n.sock").string();
    _sock_fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (_sock_fd < 0 || ::bind(_sock_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      ADD_FAILURE() << "could not bind the test notify socket";
    }
    return path;
  }

  fs::path _dir;  ///< Temp directory, removed in TearDown.
  int _sock_fd {-1};  ///< Test notify socket, closed in TearDown.

private:
  /**
   * @brief Read an environment variable.
   *
   * @param name Variable name.
   * @return Value, or `std::nullopt` when unset.
   */
  static std::optional<std::string> save(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr) {
      return std::nullopt;
    }
    return std::string(value);
  }

  /**
   * @brief Restore an environment variable saved by `save`.
   *
   * @param name Variable name.
   * @param saved Saved value, or `std::nullopt` when it was unset.
   */
  static void restore(const char *name, const std::optional<std::string> &saved) {
    if (saved) {
      ::setenv(name, saved->c_str(), 1);
    } else {
      ::unsetenv(name);
    }
  }

  std::optional<std::string> _saved_unrelated;  ///< Value of `SUNSHINE_TEST_UNRELATED` before the test.
  std::optional<std::string> _saved_notify;  ///< Value of `NOTIFY_SOCKET` before the test.
};

/**
 * @brief The parsed `env` section is the raw text of each value, sorted by name.
 */
TEST_F(ProcessDigestEnvTest, ParseKeepsEnvSectionRawAndSorted) {
  const auto path = write_file("apps.json", apps_json({{"PATH", "$(PATH):/opt/x"}, {"FOO", "bar"}}, "echo d"));
  auto parsed = proc::parse(path.string());
  ASSERT_TRUE(parsed.has_value()) << "a valid apps.json must parse";
  const env_list_t expected {{"FOO", "bar"}, {"PATH", "$(PATH):/opt/x"}};
  EXPECT_EQ(parsed->apps_env_section(), expected) << "the env section keeps names and raw values (no expansion), sorted by name";
}

/**
 * @brief Parent and child equivalence (#47): an unrelated process environment variable does not change the digest.
 */
TEST_F(ProcessDigestEnvTest, DigestIgnoresUnrelatedProcessEnvironment) {
  const auto path = write_file("apps.json", apps_json({{"FOO", "bar"}}, "echo d"));

  ::setenv("SUNSHINE_TEST_UNRELATED", "parent", 1);
  const auto before = digest_of(path);
  ::setenv("SUNSHINE_TEST_UNRELATED", "child", 1);
  const auto after = digest_of(path);

  ASSERT_FALSE(before.empty()) << "the test file must give a digest";
  EXPECT_EQ(before, after) << "the process environment must not be part of the digest (#47)";
}

/**
 * @brief A changed value in the `apps.json` `env` section changes the digest.
 */
TEST_F(ProcessDigestEnvTest, EnvSectionValueChangesDigest) {
  const auto first = write_file("first.json", apps_json({{"FOO", "bar"}}, "echo d"));
  const auto second = write_file("second.json", apps_json({{"FOO", "baz"}}, "echo d"));
  EXPECT_NE(digest_of(first), digest_of(second)) << "a changed env section value must change the digest";
}

/**
 * @brief A changed prep `do` command changes the digest.
 */
TEST_F(ProcessDigestEnvTest, PrepDoCommandChangesDigest) {
  const auto first = write_file("first.json", apps_json({{"FOO", "bar"}}, "echo d"));
  const auto second = write_file("second.json", apps_json({{"FOO", "bar"}}, "echo changed"));
  EXPECT_NE(digest_of(first), digest_of(second)) << "a changed prep do command must change the digest";
}

/**
 * @brief Names that the digest excludes (launch, restart and handoff names) do not change it when they appear in the
 * `apps.json` `env` section.
 */
TEST_F(ProcessDigestEnvTest, ExcludedNamesInEnvSectionDoNotChangeDigest) {
  const auto plain = write_file("plain.json", apps_json({{"FOO", "bar"}}, "echo d"));
  const auto excluded = write_file(
    "excluded.json",
    apps_json(
      {
        {"FOO", "bar"},
        {"SUNSHINE_HANDOFF_FDS", "3"},
        {"SUNSHINE_APP_ID", "9"},
        {"SUNSHINE_UDP_BASE", "47990"},
        {"SUNSHINE_GENERATION", "2"},
      },
      "echo d"
    )
  );
  EXPECT_EQ(digest_of(plain), digest_of(excluded)) << "SUNSHINE_HANDOFF_*, launch and restart names are excluded from the digest";
}

/**
 * @brief The digest computed before `READY` equals the one computed after it in the same process, with
 * `NOTIFY_SOCKET` set (the environment changes that a systemd start brings).
 */
TEST_F(ProcessDigestEnvTest, DigestUnchangedAcrossReadyNotification) {
  const auto path = write_file("apps.json", apps_json({{"FOO", "bar"}}, "echo d"));
  const auto socket_path = bind_notify_socket();
  ASSERT_GE(_sock_fd, 0) << "the test notify socket must exist";
  ::setenv("NOTIFY_SOCKET", socket_path.c_str(), 1);

  const auto before = digest_of(path);
  EXPECT_TRUE(handoff::sd::notify(socket_path, "READY=1")) << "READY=1 must be sent to the test socket";
  const auto after = digest_of(path);

  ASSERT_FALSE(before.empty()) << "the test file must give a digest";
  EXPECT_EQ(before, after) << "sending READY must not change the digest (#47)";
}
#endif
