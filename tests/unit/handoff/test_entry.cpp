/**
 * @file tests/unit/handoff/test_entry.cpp
 * @brief Blind tests for the `main()` integration points of the drain handoff (src/handoff/entry.h): the
 * `--drain-handoff` helper dispatch, `early_start`, `apply_startup_policy`, `open_instance_file`, the `READY=1` latch,
 * `post_commit_t` routing and teardown, and `handoff_process_t` (new-side thread, shutdown watcher, and the SIGTERM
 * table by stage).
 *
 * Written from `docs/drain-handoff.md` and the headers (`entry.h`, `new_ops.h`, `new_side.h`, `runtime.h`, `peer_link.h`,
 * `channel.h`, `channel_reader.h`, `control.h`, `hooks.h`) only. Nothing exits, publishes, maps a port, shows a tray or
 * touches systemd: every platform call is a recording seam, and sockets are `socketpair`s or temp-dir Unix sockets.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <cerrno>
  #include <chrono>
  #include <condition_variable>
  #include <cstdint>
  #include <cstdio>
  #include <cstdlib>
  #include <filesystem>
  #include <fstream>
  #include <functional>
  #include <future>
  #include <map>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <set>
  #include <string>
  #include <thread>
  #include <vector>

  // platform includes
  #include <fcntl.h>
  #include <poll.h>
  #include <sys/eventfd.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>

  // local includes
  #include "src/config.h"
  #include "src/handoff/entry.h"
  #include "src/handoff/hooks.h"
  #include "src/handoff/instance_file.h"
  #include "src/handoff/runtime.h"
  #include "src/upnp.h"

using namespace std::chrono_literals;

namespace {
  namespace protocol = handoff::protocol;
  using handoff::new_stage_e;
  using reply_e = handoff::drain_peer_t::reply_e;

  /**
   * @brief Thread-safe ordered record of every seam call.
   */
  class recorder_t {
  public:
    /**
     * @brief Append an entry.
     *
     * @param entry Entry text.
     */
    void add(std::string entry) {
      std::lock_guard lock {_mutex};
      _entries.push_back(std::move(entry));
    }

    /**
     * @brief Copy of all entries.
     *
     * @return Entries in call order.
     */
    std::vector<std::string> all() const {
      std::lock_guard lock {_mutex};
      return _entries;
    }

    /**
     * @brief Number of entries that start with `prefix`.
     *
     * @param prefix Prefix.
     * @return Count.
     */
    int count(const std::string &prefix) const {
      std::lock_guard lock {_mutex};
      return static_cast<int>(std::count_if(_entries.begin(), _entries.end(), [&](const std::string &e) {
        return e.rfind(prefix, 0) == 0;
      }));
    }

    /**
     * @brief Index of the first entry that starts with `prefix`.
     *
     * @param prefix Prefix.
     * @return Index, or -1.
     */
    int index_of(const std::string &prefix) const {
      std::lock_guard lock {_mutex};
      for (std::size_t i = 0; i < _entries.size(); ++i) {
        if (_entries[i].rfind(prefix, 0) == 0) {
          return static_cast<int>(i);
        }
      }
      return -1;
    }

  private:
    mutable std::mutex _mutex;  ///< Guards the entries.
    std::vector<std::string> _entries;  ///< Entries.
  };

  /**
   * @brief Poll a condition until it holds or the limit passes.
   *
   * @param pred Condition.
   * @param limit Longest wait.
   * @return `true` when the condition held.
   */
  bool wait_until(const std::function<bool()> &pred, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
      if (pred()) {
        return true;
      }
      std::this_thread::sleep_for(5ms);
    }
    return pred();
  }

  /**
   * @brief Set an environment variable for the life of the object and restore the old state afterwards.
   */
  class env_guard_t {
  public:
    /**
     * @brief Set or unset a variable.
     *
     * @param name Variable name.
     * @param value New value, or `std::nullopt` to unset it.
     */
    env_guard_t(std::string name, std::optional<std::string> value):
        _name(std::move(name)) {
      if (const char *old = ::getenv(_name.c_str())) {
        _old = std::string {old};
      }
      if (value) {
        ::setenv(_name.c_str(), value->c_str(), 1);
      } else {
        ::unsetenv(_name.c_str());
      }
    }

    /**
     * @brief Restore the variable.
     */
    ~env_guard_t() {
      if (_old) {
        ::setenv(_name.c_str(), _old->c_str(), 1);
      } else {
        ::unsetenv(_name.c_str());
      }
    }

    env_guard_t(const env_guard_t &) = delete;  ///< Not copyable.
    env_guard_t &operator=(const env_guard_t &) = delete;  ///< Not copyable.

  private:
    std::string _name;  ///< Name.
    std::optional<std::string> _old;  ///< Previous value.
  };

  /**
   * @brief Fake process environment for `runtime_t::early_init()`.
   */
  struct fake_env_t {
    std::map<std::string, std::string> vars;  ///< Variables that are set.

    /**
     * @brief Build the `early_init` callbacks over this object (it must outlive the returned callbacks).
     *
     * @return Callbacks.
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

    /**
     * @brief Fill a complete, valid handoff start.
     */
    void set_handoff_vars() {
      vars["SUNSHINE_HANDOFF_FDS"] = "https:3,http:4,webui:5,rtsp:6";
      vars["SUNSHINE_HANDOFF_CHANNEL"] = "7";
      vars["SUNSHINE_HANDOFF_UDP_BASE"] = "48100";
      vars["SUNSHINE_HANDOFF_GENERATION"] = "1";
    }
  };

  /**
   * @brief Current role of this process's instance file, as text.
   *
   * @return `pending`, `active`, `draining` or `none` (no writer).
   */
  std::string current_role() {
    const auto instance = handoff::runtime().instance_current();
    return instance ? std::string {handoff::to_string(instance->role)} : std::string {"none"};
  }

  /**
   * @brief Receive messages on a channel until one of type `T` arrives.
   *
   * @tparam T Message type.
   * @param channel Channel to read.
   * @param limit Longest total wait.
   * @return The message, or `std::nullopt`.
   */
  template<class T>
  std::optional<T> expect_message(handoff::channel_t &channel, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
      handoff::received_t received;
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
      const auto status = channel.receive(received, std::max(left, 1ms));
      if (status != handoff::recv_status_e::ok) {
        if (status == handoff::recv_status_e::timeout) {
          continue;
        }
        return std::nullopt;
      }
      if (received.fd != -1) {
        ::close(received.fd);
      }
      if (const auto *typed = std::get_if<T>(&received.message)) {
        return *typed;
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Barrier that a shutdown watcher can wait on.
   */
  class shutdown_gate_t {
  public:
    /**
     * @brief Block until `raise()` was called.
     */
    void wait() {
      std::unique_lock lock {_mutex};
      _cv.wait(lock, [this] {
        return _raised;
      });
    }

    /**
     * @brief Release every waiter.
     */
    void raise() {
      {
        std::lock_guard lock {_mutex};
        _raised = true;
      }
      _cv.notify_all();
    }

    /**
     * @brief Whether `raise()` was called.
     *
     * @return `true` after it.
     */
    bool raised() {
      std::lock_guard lock {_mutex};
      return _raised;
    }

  private:
    std::mutex _mutex;  ///< Guards the flag.
    std::condition_variable _cv;  ///< Wakes waiters.
    bool _raised {false};  ///< Raised.
  };

  /**
   * @brief UPnP guard stand-in.
   */
  class fake_guard_t: public platf::deinit_t {};
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// run_drain_handoff_cli
// ---------------------------------------------------------------------------------------------------------------------

namespace {
  /**
   * @brief Recording fake of the helper environment.
   */
  struct cli_rig_t {
    std::map<std::string, std::string> env;  ///< Variables that are set.
    std::vector<std::string> asked;  ///< Variable names that were read.
    std::vector<std::string> stderr_lines;  ///< Lines given to `write_err`.
    int run_calls {0};  ///< Calls of `run`.
    std::vector<std::string> run_args;  ///< Arguments of the last `run`.
    handoff::helper_env_t run_env;  ///< Environment of the last `run`.
    handoff::helper_exit_e run_result {handoff::helper_exit_e::switched};  ///< Result of `run`.
    std::string run_err;  ///< `err` that `run` produces.

    /**
     * @brief Build the `cli_env_t` over this object.
     *
     * @return Environment.
     */
    handoff::entry::cli_env_t make() {
      handoff::entry::cli_env_t cli;
      cli.getenv = [this](const char *name) -> const char * {
        asked.emplace_back(name);
        auto it = env.find(name);
        return it == env.end() ? nullptr : it->second.c_str();
      };
      cli.run = [this](const std::vector<std::string> &args, const handoff::helper_env_t &helper_env, std::string &err) {
        ++run_calls;
        run_args = args;
        run_env = helper_env;
        err = run_err;
        return run_result;
      };
      cli.write_err = [this](const std::string &line) {
        stderr_lines.push_back(line);
      };
      return cli;
    }
  };
}  // namespace

TEST(HandoffEntryCliTest, RunsTheHelperWithTheArgumentsAndReturnsItsCode) {
  cli_rig_t rig;
  rig.env["RUNTIME_DIRECTORY"] = "/run/sunshine";
  rig.run_result = handoff::helper_exit_e::failed;
  const auto code = handoff::entry::run_drain_handoff_cli({"1234"}, rig.make());
  EXPECT_EQ(code, 2) << "the exit code is the helper_exit_e value (failed is 2)";
  EXPECT_EQ(rig.run_calls, 1) << "run_helper must be called once";
  EXPECT_EQ(rig.run_args, std::vector<std::string> {"1234"}) << "the arguments after --drain-handoff are passed unchanged";
}

TEST(HandoffEntryCliTest, EveryHelperCodeIsReturnedAsAnInt) {
  const std::pair<handoff::helper_exit_e, int> codes[] = {
    {handoff::helper_exit_e::switched, 0},
    {handoff::helper_exit_e::refused, 1},
    {handoff::helper_exit_e::failed, 2},
    {handoff::helper_exit_e::kill_pending, 3},
    {handoff::helper_exit_e::unconfirmed, 4},
    {handoff::helper_exit_e::error, 5},
  };
  for (const auto &[result, expected] : codes) {
    cli_rig_t rig;
    rig.env["RUNTIME_DIRECTORY"] = "/run/sunshine";
    rig.run_result = result;
    EXPECT_EQ(handoff::entry::run_drain_handoff_cli({"1"}, rig.make()), expected) << "helper code " << expected;
  }
}

TEST(HandoffEntryCliTest, RuntimeDirectoryComesFromRuntimeDirectoryFirst) {
  cli_rig_t rig;
  rig.env["RUNTIME_DIRECTORY"] = "/run/sunshine";
  rig.env["XDG_RUNTIME_DIR"] = "/run/user/1000";
  handoff::entry::run_drain_handoff_cli({"1"}, rig.make());
  EXPECT_EQ(rig.run_env.runtime_dir, std::filesystem::path {"/run/sunshine"}) << "RUNTIME_DIRECTORY wins";
}

TEST(HandoffEntryCliTest, RuntimeDirectoryFallsBackToXdgSunshine) {
  cli_rig_t rig;
  rig.env["XDG_RUNTIME_DIR"] = "/run/user/1000";
  handoff::entry::run_drain_handoff_cli({"1"}, rig.make());
  EXPECT_EQ(rig.run_calls, 1) << "an XDG runtime directory is enough";
  EXPECT_EQ(rig.run_env.runtime_dir, std::filesystem::path {"/run/user/1000/sunshine"}) << "XDG_RUNTIME_DIR/sunshine";
}

TEST(HandoffEntryCliTest, NoRuntimeDirectoryReturnsFiveWithoutCallingRun) {
  cli_rig_t rig;
  const auto code = handoff::entry::run_drain_handoff_cli({"1"}, rig.make());
  EXPECT_EQ(code, 5) << "no runtime directory is exit 5";
  EXPECT_EQ(rig.run_calls, 0) << "run_helper must not be called without a runtime directory";
  ASSERT_EQ(rig.stderr_lines.size(), 1u) << "exactly one line to stderr";
  EXPECT_EQ(rig.stderr_lines[0], "no runtime directory (RUNTIME_DIRECTORY and XDG_RUNTIME_DIR are not set)") << "message";
}

TEST(HandoffEntryCliTest, RelativeRuntimeDirectoriesCountAsUnset) {
  cli_rig_t rig;
  rig.env["RUNTIME_DIRECTORY"] = "relative/dir";
  rig.env["XDG_RUNTIME_DIR"] = "also/relative";
  EXPECT_EQ(handoff::entry::run_drain_handoff_cli({"1"}, rig.make()), 5) << "relative paths are unset (runtime_dir rule)";
  EXPECT_EQ(rig.run_calls, 0) << "run_helper must not be called";
}

TEST(HandoffEntryCliTest, ErrLineGoesToStderrWhenNotEmpty) {
  cli_rig_t rig;
  rig.env["RUNTIME_DIRECTORY"] = "/run/sunshine";
  rig.run_err = "a drain is in progress";
  rig.run_result = handoff::helper_exit_e::refused;
  handoff::entry::run_drain_handoff_cli({"1"}, rig.make());
  ASSERT_EQ(rig.stderr_lines.size(), 1u) << "the outcome line must be written once";
  EXPECT_EQ(rig.stderr_lines[0], "a drain is in progress") << "the line is the helper's err text (the writer adds the newline)";
}

TEST(HandoffEntryCliTest, EmptyErrWritesNothing) {
  cli_rig_t rig;
  rig.env["RUNTIME_DIRECTORY"] = "/run/sunshine";
  handoff::entry::run_drain_handoff_cli({"1"}, rig.make());
  EXPECT_TRUE(rig.stderr_lines.empty()) << "nothing is written for an empty err";
}

TEST(HandoffEntryCliTest, ReplyTimeoutIsReadInSeconds) {
  cli_rig_t rig;
  rig.env["RUNTIME_DIRECTORY"] = "/run/sunshine";
  rig.env["SUNSHINE_DRAIN_HANDOFF_REPLY_TIMEOUT"] = "30";
  handoff::entry::run_drain_handoff_cli({"1"}, rig.make());
  EXPECT_EQ(rig.run_env.reply_timeout, 30000ms) << "30 seconds";
  EXPECT_TRUE(rig.stderr_lines.empty()) << "a valid value writes no warning";
}

TEST(HandoffEntryCliTest, ReplyTimeoutBoundsAreAccepted) {
  for (const auto &[text, expected] : std::vector<std::pair<std::string, std::chrono::milliseconds>> {{"1", 1000ms}, {"86400", 86400000ms}}) {
    cli_rig_t rig;
    rig.env["RUNTIME_DIRECTORY"] = "/run/sunshine";
    rig.env["SUNSHINE_DRAIN_HANDOFF_REPLY_TIMEOUT"] = text;
    handoff::entry::run_drain_handoff_cli({"1"}, rig.make());
    EXPECT_EQ(rig.run_env.reply_timeout, expected) << "value " << text << " is within 1 to 86400";
    EXPECT_TRUE(rig.stderr_lines.empty()) << "value " << text << " writes no warning";
  }
}

TEST(HandoffEntryCliTest, InvalidReplyTimeoutIsIgnoredWithOneWarning) {
  for (const char *text : {"0", "-5", "abc", "86401", "1.5", "5s", "99999999999999999999"}) {
    cli_rig_t rig;
    rig.env["RUNTIME_DIRECTORY"] = "/run/sunshine";
    rig.env["SUNSHINE_DRAIN_HANDOFF_REPLY_TIMEOUT"] = text;
    const handoff::helper_env_t defaults;
    handoff::entry::run_drain_handoff_cli({"1"}, rig.make());
    EXPECT_EQ(rig.run_calls, 1) << "value '" << text << "': the helper still runs";
    EXPECT_EQ(rig.run_env.reply_timeout, defaults.reply_timeout) << "value '" << text << "': the default reply timeout stays";
    ASSERT_EQ(rig.stderr_lines.size(), 1u) << "value '" << text << "': exactly one warning line";
    EXPECT_EQ(rig.stderr_lines[0], "ignoring invalid SUNSHINE_DRAIN_HANDOFF_REPLY_TIMEOUT") << "value '" << text << "'";
  }
}

TEST(HandoffEntryCliTest, UnsetReplyTimeoutKeepsTheOtherDefaults) {
  cli_rig_t rig;
  rig.env["RUNTIME_DIRECTORY"] = "/run/sunshine";
  const handoff::helper_env_t defaults;
  handoff::entry::run_drain_handoff_cli({"1"}, rig.make());
  EXPECT_EQ(rig.run_env.reply_timeout, defaults.reply_timeout) << "reply_timeout default";
  EXPECT_EQ(rig.run_env.connect_timeout, defaults.connect_timeout) << "connect_timeout default";
  EXPECT_EQ(rig.run_env.active_timeout, defaults.active_timeout) << "active_timeout default";
}

TEST(HandoffEntryCliTest, ReadsNoOtherVariableAndLeavesNotifySocketAlone) {
  cli_rig_t rig;
  rig.env["RUNTIME_DIRECTORY"] = "/run/sunshine";
  rig.env["NOTIFY_SOCKET"] = "/run/systemd/notify";
  handoff::entry::run_drain_handoff_cli({"1"}, rig.make());
  const std::set<std::string> allowed {"RUNTIME_DIRECTORY", "XDG_RUNTIME_DIR", "SUNSHINE_DRAIN_HANDOFF_REPLY_TIMEOUT"};
  for (const auto &name : rig.asked) {
    EXPECT_TRUE(allowed.count(name) > 0) << "the helper must not read " << name;
  }
  EXPECT_EQ(rig.env.count("NOTIFY_SOCKET"), 1u) << "NOTIFY_SOCKET is left alone";
}

// ---------------------------------------------------------------------------------------------------------------------
// maybe_run_helper
// ---------------------------------------------------------------------------------------------------------------------

namespace {
  /**
   * @brief Call `maybe_run_helper` with a list of arguments.
   *
   * @param args Full argv, program name first.
   * @return The dispatch result.
   */
  std::optional<int> dispatch(std::vector<std::string> args) {
    std::vector<char *> argv;
    for (auto &arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    return handoff::entry::maybe_run_helper(static_cast<int>(args.size()), argv.data());
  }
}  // namespace

TEST(HandoffEntryHelperDispatchTest, NoArgumentsMeansNoDispatch) {
  EXPECT_FALSE(dispatch({"sunshine"}).has_value()) << "argc == 1 must not dispatch";
}

TEST(HandoffEntryHelperDispatchTest, OtherFirstArgumentsMeanNoDispatch) {
  for (const char *arg : {"--help", "--version", "--drain-handoff-x", "--drain-handoffs", "--Drain-Handoff", "drain-handoff", "-drain-handoff", ""}) {
    EXPECT_FALSE(dispatch({"sunshine", arg}).has_value()) << "argv[1] '" << arg << "' is not exactly --drain-handoff";
  }
}

TEST(HandoffEntryHelperDispatchTest, TheFlagInASecondPositionMeansNoDispatch) {
  EXPECT_FALSE(dispatch({"sunshine", "config.conf", "--drain-handoff"}).has_value()) << "only argv[1] is checked";
}

TEST(HandoffEntryHelperDispatchTest, NoDispatchLeavesTheRuntimeUntouched) {
  handoff::runtime().reset_for_test();
  EXPECT_FALSE(dispatch({"sunshine", "--help"}).has_value());
  EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::disabled) << "no dispatch, no side effect";
  EXPECT_FALSE(handoff::runtime().started_by_handoff()) << "no dispatch, no side effect";
}

TEST(HandoffEntryHelperDispatchTest, TheFlagWithoutAPidDispatchesAndReturnsUsageError) {
  // Positive control: the exact flag dispatches. With no pid the helper answers exit 5 (usage), and with no runtime
  // directory it answers exit 5 as well, so the result does not depend on the machine's environment.
  const auto code = dispatch({"sunshine", "--drain-handoff"});
  ASSERT_TRUE(code.has_value()) << "argv[1] == --drain-handoff must dispatch";
  EXPECT_EQ(*code, 5) << "no pid is a usage error (5)";
}

TEST(HandoffEntryHelperDispatchTest, TheFlagWithABadPidReturnsUsageError) {
  const auto code = dispatch({"sunshine", "--drain-handoff", "not-a-pid"});
  ASSERT_TRUE(code.has_value()) << "argv[1] == --drain-handoff must dispatch";
  EXPECT_EQ(*code, 5) << "a pid that is not a positive decimal number is exit 5";
}

// ---------------------------------------------------------------------------------------------------------------------
// early_start, apply_startup_policy, open_instance_file
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Fixture for the startup helpers: resets the runtime and the configuration globals they touch.
 */
class HandoffEntryStartupTest: public ::testing::Test {
protected:
  /**
   * @brief Reset the runtime and remember the configuration globals.
   */
  void SetUp() override {
    handoff::runtime().reset_for_test();
    _saved_settings = config::drain_handoff;
    _saved_capture = config::video.capture;
    _saved_port = config::sunshine.port;
    _saved_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    config::sunshine.port = 47989;
    config::sunshine.flags[config::flag::FRESH_STATE] = false;
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() / ("sunshine-t18b-entry-" + std::to_string(::getpid()) + "-" + info->name());
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
    std::filesystem::create_directories(_dir, ec);
  }

  /**
   * @brief Restore everything and remove the temporary directory.
   */
  void TearDown() override {
    handoff::runtime().instance_remove();
    handoff::runtime().reset_for_test();
    config::drain_handoff = _saved_settings;
    config::video.capture = _saved_capture;
    config::sunshine.port = _saved_port;
    config::sunshine.flags[config::flag::FRESH_STATE] = _saved_fresh_state;
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
  }

  /**
   * @brief Configure a feature that every host check accepts (capture wlr, runtime directory set).
   */
  void configure_enabled_feature() {
    config::drain_handoff.enabled = true;
    config::video.capture = "wlr";
  }

  std::filesystem::path _dir;  ///< Temporary runtime directory.

private:
  handoff::settings_t _saved_settings;  ///< Saved `config::drain_handoff`.
  std::string _saved_capture;  ///< Saved `config::video.capture`.
  std::uint16_t _saved_port {0};  ///< Saved `config::sunshine.port`.
  bool _saved_fresh_state {false};  ///< Saved flag.
};

TEST_F(HandoffEntryStartupTest, EarlyStartOnANormalStartIsEmptyAndLeavesTheModeDisabled) {
  fake_env_t env;
  const auto reason = handoff::entry::early_start(false, env.make());
  EXPECT_EQ(reason, "") << "no handoff variables is a normal start";
  EXPECT_FALSE(handoff::runtime().started_by_handoff()) << "normal start";
  EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::disabled) << "mode stays disabled until configure()";
}

TEST_F(HandoffEntryStartupTest, EarlyStartOnAHandoffStartSetsNewPending) {
  fake_env_t env;
  env.set_handoff_vars();
  EXPECT_EQ(handoff::entry::early_start(false, env.make()), "") << "a valid handoff start without privileged execution succeeds";
  EXPECT_TRUE(handoff::runtime().started_by_handoff()) << "early_start must call runtime().early_init";
  EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::new_pending) << "handoff start mode";
}

TEST_F(HandoffEntryStartupTest, EarlyStartRefusesAHandoffStartWithPrivilegedExecution) {
  fake_env_t env;
  env.set_handoff_vars();
  EXPECT_EQ(handoff::entry::early_start(true, env.make()), "drain handoff start is refused with privileged execution") << "exact refusal text";
}

TEST_F(HandoffEntryStartupTest, EarlyStartAllowsAPrivilegedNormalStart) {
  fake_env_t env;
  EXPECT_EQ(handoff::entry::early_start(true, env.make()), "") << "privileged execution only matters for a handoff start";
}

TEST_F(HandoffEntryStartupTest, EarlyStartReturnsTheEarlyInitErrorForBrokenVariables) {
  fake_env_t env;
  env.vars["SUNSHINE_HANDOFF_FDS"] = "https:3,http:4,webui:5,rtsp:6";
  const auto reason = handoff::entry::early_start(false, env.make());
  EXPECT_NE(reason, "") << "exactly one of the two handoff variables set is an early_init error, returned as is";
  EXPECT_FALSE(handoff::runtime().started_by_handoff()) << "nothing is stored on an error";
}

TEST_F(HandoffEntryStartupTest, EarlyStartReadsAnInPlaceRestartBase) {
  fake_env_t env;
  env.vars["SUNSHINE_UDP_BASE"] = "48100";
  env.vars["SUNSHINE_GENERATION"] = "3";
  EXPECT_EQ(handoff::entry::early_start(false, env.make()), "");
  EXPECT_EQ(handoff::runtime().udp_base(), std::optional<int> {48100}) << "early_start goes through early_init, which reads SUNSHINE_UDP_BASE";
  EXPECT_EQ(handoff::runtime().generation(), 3u) << "and SUNSHINE_GENERATION";
}

TEST_F(HandoffEntryStartupTest, PolicyForADisabledFeatureReturnsEmptyAndStaysDisabled) {
  config::drain_handoff.enabled = false;
  const env_guard_t runtime_directory {"RUNTIME_DIRECTORY", _dir.string()};
  EXPECT_EQ(handoff::entry::apply_startup_policy(false), "") << "a disabled feature gives no refusal";
  EXPECT_FALSE(handoff::runtime().enabled()) << "a disabled feature leaves the runtime disabled";
}

TEST_F(HandoffEntryStartupTest, PolicyForAnAcceptableHostEnablesTheRuntime) {
  configure_enabled_feature();
  const env_guard_t runtime_directory {"RUNTIME_DIRECTORY", _dir.string()};
  // SPEC-QUESTION: the test assumes display_device::has_settings_manager() is false in the unit-test binary (it is false
  // before display_device::init, per display_device.h).
  EXPECT_EQ(handoff::entry::apply_startup_policy(false), "") << "capture = wlr, no FRESH_STATE, runtime directory present: no refusal";
  EXPECT_TRUE(handoff::runtime().enabled()) << "runtime().configure(config::drain_handoff) must run";
  EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::normal) << "enabled normal start";
  EXPECT_TRUE(config::drain_handoff.enabled) << "the option stays on when nothing refuses";
}

TEST_F(HandoffEntryStartupTest, PolicyRefusesPrivilegedExecutionAndDisablesTheFeature) {
  configure_enabled_feature();
  const env_guard_t runtime_directory {"RUNTIME_DIRECTORY", _dir.string()};
  const auto refusal = handoff::entry::apply_startup_policy(true);
  EXPECT_NE(refusal.find("privileged execution"), std::string::npos) << "refusal was: " << refusal;
  EXPECT_FALSE(config::drain_handoff.enabled) << "a refusal sets config::drain_handoff.enabled to false";
  EXPECT_FALSE(handoff::runtime().enabled()) << "a refusal disables the runtime";
  EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::disabled) << "mode after a refusal in a normal start";
}

TEST_F(HandoffEntryStartupTest, PolicyRefusesAnotherCaptureBackend) {
  configure_enabled_feature();
  config::video.capture = "kms";
  const env_guard_t runtime_directory {"RUNTIME_DIRECTORY", _dir.string()};
  const auto refusal = handoff::entry::apply_startup_policy(false);
  EXPECT_NE(refusal.find("capture = wlr"), std::string::npos) << "the capture refusal names the supported backend; was: " << refusal;
  EXPECT_FALSE(config::drain_handoff.enabled) << "enabled must be cleared";
  EXPECT_FALSE(handoff::runtime().enabled()) << "runtime disabled";
}

TEST_F(HandoffEntryStartupTest, PolicyRefusesFreshState) {
  configure_enabled_feature();
  config::sunshine.flags[config::flag::FRESH_STATE] = true;
  const env_guard_t runtime_directory {"RUNTIME_DIRECTORY", _dir.string()};
  EXPECT_NE(handoff::entry::apply_startup_policy(false), "") << "FRESH_STATE refuses";
  EXPECT_FALSE(config::drain_handoff.enabled) << "enabled must be cleared";
}

TEST_F(HandoffEntryStartupTest, PolicyRefusesWithoutARuntimeDirectory) {
  configure_enabled_feature();
  const env_guard_t runtime_directory {"RUNTIME_DIRECTORY", std::nullopt};
  const env_guard_t xdg {"XDG_RUNTIME_DIR", std::nullopt};
  EXPECT_NE(handoff::entry::apply_startup_policy(false), "") << "no runtime directory refuses";
  EXPECT_FALSE(config::drain_handoff.enabled) << "enabled must be cleared";
  EXPECT_FALSE(handoff::runtime().enabled()) << "runtime disabled";
}

TEST_F(HandoffEntryStartupTest, PolicyInAHandoffStartedProcessKeepsTheModeAndReturnsTheRefusal) {
  fake_env_t env;
  env.set_handoff_vars();
  ASSERT_EQ(handoff::entry::early_start(false, env.make()), "");
  configure_enabled_feature();
  const env_guard_t runtime_directory {"RUNTIME_DIRECTORY", _dir.string()};
  const auto refusal = handoff::entry::apply_startup_policy(true);
  EXPECT_NE(refusal, "") << "the refusal is returned so the new side can report it";
  EXPECT_FALSE(config::drain_handoff.enabled) << "enabled must be cleared";
  EXPECT_EQ(handoff::runtime().mode(), handoff::mode_e::new_pending) << "disable() does not change a handoff-started process's mode";
}

TEST_F(HandoffEntryStartupTest, OpenInstanceFileDoesNothingWhenTheRuntimeIsDisabled) {
  const env_guard_t runtime_directory {"RUNTIME_DIRECTORY", _dir.string()};
  EXPECT_FALSE(handoff::entry::open_instance_file()) << "a disabled runtime writes no file";
  EXPECT_FALSE(handoff::runtime().instance_current().has_value()) << "no writer is installed";
  EXPECT_FALSE(std::filesystem::exists(_dir / "instances" / (std::to_string(::getpid()) + ".json"))) << "no file";
}

TEST_F(HandoffEntryStartupTest, OpenInstanceFileWritesRoleActiveInANormalStart) {
  configure_enabled_feature();
  const env_guard_t runtime_directory {"RUNTIME_DIRECTORY", _dir.string()};
  ASSERT_EQ(handoff::entry::apply_startup_policy(false), "");
  ASSERT_TRUE(handoff::entry::open_instance_file()) << "the first write must succeed";
  const auto instance = handoff::runtime().instance_current();
  ASSERT_TRUE(instance.has_value()) << "a writer must be installed";
  EXPECT_EQ(instance->role, handoff::role_e::active) << "a normal start writes role active";
  EXPECT_EQ(instance->pid, ::getpid()) << "own pid";
  EXPECT_EQ(instance->sessions, 0) << "no sessions at start";
  EXPECT_EQ(instance->handoff, handoff::state_e::none) << "no handoff state at start";
  EXPECT_EQ(instance->udp_base, 47989) << "the configured port is the UDP base of a normal start";
  EXPECT_EQ(std::optional<std::uint64_t> {instance->start_time}, handoff::read_start_time(::getpid())) << "start time of this process";
  EXPECT_TRUE(std::filesystem::exists(_dir / "instances" / (std::to_string(::getpid()) + ".json"))) << "the file lives in <runtime dir>/instances/<pid>.json";
}

TEST_F(HandoffEntryStartupTest, OpenInstanceFileWritesRolePendingInAHandoffStartedProcess) {
  fake_env_t env;
  env.set_handoff_vars();
  ASSERT_EQ(handoff::entry::early_start(false, env.make()), "");
  configure_enabled_feature();
  const env_guard_t runtime_directory {"RUNTIME_DIRECTORY", _dir.string()};
  ASSERT_EQ(handoff::entry::apply_startup_policy(false), "");
  ASSERT_TRUE(handoff::entry::open_instance_file());
  const auto instance = handoff::runtime().instance_current();
  ASSERT_TRUE(instance.has_value());
  EXPECT_EQ(instance->role, handoff::role_e::pending) << "a handoff-started process writes role pending";
  EXPECT_EQ(instance->udp_base, 48100) << "the inherited UDP base";
}

TEST_F(HandoffEntryStartupTest, OpenInstanceFileReportsAFailedWrite) {
  configure_enabled_feature();
  const auto blocker = _dir / "blocker";
  {
    std::ofstream out {blocker};
    out << "x";
  }
  const env_guard_t runtime_directory {"RUNTIME_DIRECTORY", blocker.string()};
  const env_guard_t xdg {"XDG_RUNTIME_DIR", std::nullopt};
  ASSERT_EQ(handoff::entry::apply_startup_policy(false), "");
  EXPECT_FALSE(handoff::entry::open_instance_file()) << "a runtime directory that is a regular file cannot hold the instances directory";
  EXPECT_TRUE(handoff::runtime().enabled()) << "the process runs on";
}

// ---------------------------------------------------------------------------------------------------------------------
// notify_ready_if_systemd
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffEntryNotifyTest, WithoutNotifySocketNothingIsSent) {
  const env_guard_t notify {"NOTIFY_SOCKET", std::nullopt};
  EXPECT_FALSE(handoff::entry::notify_ready_if_systemd()) << "NOTIFY_SOCKET unset: nothing to send";
}

TEST(HandoffEntryNotifyTest, SendsReadyToTheSocketReadAtCallTime) {
  const auto dir = std::filesystem::temp_directory_path() / ("sunshine-t18b-notify-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const auto path = (dir / "notify").string();
  const int server = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(server, 0);
  sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
  ASSERT_EQ(::bind(server, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);

  {
    const env_guard_t notify {"NOTIFY_SOCKET", path};
    EXPECT_TRUE(handoff::entry::notify_ready_if_systemd()) << "a datagram must be sent when NOTIFY_SOCKET is set";
  }
  pollfd pfd {server, POLLIN, 0};
  ASSERT_GT(::poll(&pfd, 1, 500), 0) << "the datagram must arrive";
  char buffer[64] = {};
  const auto length = ::recv(server, buffer, sizeof(buffer) - 1, 0);
  ASSERT_GT(length, 0);
  EXPECT_EQ(std::string(buffer, static_cast<std::size_t>(length)), "READY=1") << "the datagram is READY=1";
  ::close(server);
  std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------------------------------------------------
// listening_latch_t
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffEntryLatchTest, CompletesOnTheFourthArrivalOnly) {
  int completed = 0;
  handoff::entry::listening_latch_t latch {4, [&] {
                                             ++completed;
                                           }};
  for (int i = 1; i <= 3; ++i) {
    latch.arrive();
    EXPECT_EQ(completed, 0) << "after " << i << " of 4 arrivals nothing may complete";
    EXPECT_EQ(latch.arrived(), i) << "arrived() after " << i << " arrivals";
  }
  latch.arrive();
  EXPECT_EQ(completed, 1) << "the fourth arrival completes the latch";
  EXPECT_EQ(latch.arrived(), 4);
}

TEST(HandoffEntryLatchTest, ArrivalsBeyondTheCountDoNothing) {
  int completed = 0;
  handoff::entry::listening_latch_t latch {2, [&] {
                                             ++completed;
                                           }};
  for (int i = 0; i < 6; ++i) {
    latch.arrive();
  }
  EXPECT_EQ(completed, 1) << "on_complete is called once";
  EXPECT_EQ(latch.arrived(), 2) << "arrived() is capped at the construction count";
}

TEST(HandoffEntryLatchTest, CompletionRunsOnTheThreadOfTheLastArrival) {
  std::thread::id completed_on;
  handoff::entry::listening_latch_t latch {2, [&] {
                                             completed_on = std::this_thread::get_id();
                                           }};
  latch.arrive();
  std::thread::id last_arrival;
  std::thread last([&] {
    last_arrival = std::this_thread::get_id();
    latch.arrive();
  });
  last.join();
  EXPECT_EQ(completed_on, last_arrival) << "on_complete runs on the calling thread of the arrival that completed the latch";
  EXPECT_NE(completed_on, std::this_thread::get_id()) << "not on the first arrival's thread";
}

TEST(HandoffEntryLatchTest, ConcurrentArrivalsCompleteExactlyOnce) {
  for (int round = 0; round < 20; ++round) {
    std::atomic<int> completed {0};
    handoff::entry::listening_latch_t latch {4, [&] {
                                               ++completed;
                                             }};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
      threads.emplace_back([&] {
        latch.arrive();
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }
    EXPECT_EQ(completed.load(), 1) << "round " << round << ": exactly one completion with 8 arrivals for a count of 4";
    EXPECT_EQ(latch.arrived(), 4) << "round " << round;
  }
}

TEST(HandoffEntryLatchTest, CompletionMayReenterTheLatch) {
  // The completion runs outside any lock of the latch, so it may call back into it.
  std::atomic<int> completed {0};
  handoff::entry::listening_latch_t *self = nullptr;
  handoff::entry::listening_latch_t latch {1, [&] {
                                             ++completed;
                                             self->arrive();
                                             (void) self->arrived();
                                           }};
  self = &latch;
  std::promise<void> done;
  auto future = done.get_future();
  std::thread worker([&] {
    latch.arrive();
    done.set_value();
  });
  EXPECT_EQ(future.wait_for(1500ms), std::future_status::ready) << "re-entering the latch from on_complete must not deadlock";
  worker.join();
  EXPECT_EQ(completed.load(), 1) << "the re-entrant arrival must not complete the latch again";
}

TEST(HandoffEntryLatchTest, CountOfOneCompletesAtOnce) {
  int completed = 0;
  handoff::entry::listening_latch_t latch {1, [&] {
                                             ++completed;
                                           }};
  latch.arrive();
  EXPECT_EQ(completed, 1) << "a latch of one completes on the first arrival";
}

// ---------------------------------------------------------------------------------------------------------------------
// Process fixture: ops, channel, scripted old process
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Fixture with a handoff-started runtime (mode `new_pending`, role `pending`), a channel pair, recording seams,
 * and helpers to play the old process.
 */
class HandoffEntryProcessTest: public ::testing::Test {
protected:
  /**
   * @brief Build the runtime, the channel and the seams.
   */
  void SetUp() override {
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() / ("sunshine-t18b-proc-" + std::to_string(::getpid()) + "-" + info->name());
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
    std::filesystem::create_directories(_dir, ec);

    config.port = 47989;
    config.feature_enabled = true;
    config.tray_enabled = true;
    config.servers_listening_timeout = 2000ms;
    config.resume_timeout = 500ms;

    seams.set_unmap_scope = [this](upnp::unmap_scope_e scope) {
      rec.add(std::string {"set_unmap_scope("} + (scope == upnp::unmap_scope_e::all ? "all" : scope == upnp::unmap_scope_e::udp_only ? "udp_only" :
                                                                                                                                       "none") +
              ")");
    };
    seams.own_identity = [] {
      return protocol::probe_identity_t {"build-1", "/dev/dri/renderD128", 0x1002, 0x73bf, "6.1"};
    };
    seams.set_probe_override = [this](const protocol::probe_results_t &) {
      rec.add("set_probe_override");
    };
    seams.set_cooldown_deadline_ns = [this](std::int64_t) {
      rec.add("set_cooldown_deadline_ns");
    };
    seams.validate_listener = [this](int, int, bool) {
      note_thread();
      rec.add("validate_listener");
      return std::string {};
    };
    seams.reload_all = [this] {
      note_thread();
      rec.add("reload_all");
      while (hold_reload.load()) {
        std::this_thread::sleep_for(2ms);
      }
      return std::string {};
    };
    seams.prepare_adoption = [this](const protocol::go_t &) {
      note_thread();
      rec.add("prepare_adoption");
      return std::string {};
    };
    seams.install_adoption = [this] {
      note_thread();
      rec.add("install_adoption");
    };
    seams.resume_servers = [this](std::chrono::milliseconds) {
      note_thread();
      rec.add("resume_servers");
      while (hold_resume.load()) {
        std::this_thread::sleep_for(2ms);
      }
      return resume_result;
    };
    seams.publish_mdns = [this] {
      note_thread();
      rec.add("publish_mdns");
      return true;
    };
    seams.start_upnp = [this]() -> std::unique_ptr<platf::deinit_t> {
      note_thread();
      rec.add("start_upnp");
      return std::make_unique<fake_guard_t>();
    };
    seams.request_tray_start = [this] {
      note_thread();
      rec.add("request_tray_start");
    };
    seams.enable_tray_entries = [this] {
      rec.add("enable_tray_entries");
    };
    seams.notify_ready = [this] {
      rec.add("notify_ready");
    };
    seams.start_control_server = [this] {
      rec.add("start_control_server");
    };
    seams.exit_process = [this](int code) {
      rec.add("exit_process(" + std::to_string(code) + ")");
    };
    seams.shutdown_channel = [this](handoff::channel_t &channel_ref) {
      rec.add("shutdown_channel");
      if (channel_ref.fd() != -1) {
        ::shutdown(channel_ref.fd(), SHUT_RDWR);
      }
    };

    reset_runtime();
  }

  /**
   * @brief Release holds, tear the process down and reset the process-wide state.
   */
  void TearDown() override {
    hold_reload = false;
    hold_resume = false;
    gate.raise();
    proc.reset();
    ops.reset();
    peer.reset();
    channel.reset();
    handoff::runtime().instance_remove();
    handoff::runtime().reset_for_test();
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
  }

  /**
   * @brief Put the runtime in the handoff-started state with role `pending` and make a fresh channel pair.
   */
  void reset_runtime() {
    handoff::runtime().instance_remove();
    handoff::runtime().reset_for_test();
    fake_env_t env;
    env.set_handoff_vars();
    auto callbacks = env.make();
    ASSERT_EQ(handoff::runtime().early_init(callbacks), "") << "setup: early_init";
    handoff::instance_t initial;
    initial.pid = ::getpid();
    initial.start_time = 1;
    initial.role = handoff::role_e::pending;
    initial.udp_base = 48100;
    ASSERT_TRUE(handoff::runtime().open_instance(_dir / "instances", initial)) << "setup: instance file";
    const auto pair = handoff::make_channel_pair();
    ASSERT_TRUE(pair.has_value()) << "setup: socketpair";
    channel = std::make_shared<handoff::channel_t>(pair->first);
    peer = std::make_unique<handoff::channel_t>(pair->second);
  }

  /**
   * @brief Remember the calling thread.
   */
  void note_thread() {
    std::lock_guard lock {_thread_mutex};
    threads.insert(std::this_thread::get_id());
  }

  /**
   * @brief Create the ops directly (for `post_commit_t` tests).
   */
  void build_ops() {
    ops = std::make_unique<handoff::sunshine_new_ops_t>(config, channel, upnp_slot, seams);
  }

  /**
   * @brief An old process `HELLO`.
   *
   * @return The message.
   */
  static protocol::hello_t old_hello() {
    protocol::hello_t hello;
    hello.probe = protocol::probe_results_t {};
    hello.identity = protocol::probe_identity_t {"build-1", "/dev/dri/renderD128", 0x1002, 0x73bf, "6.1"};
    return hello;
  }

  /**
   * @brief Bring `ops` to the `committed` stage by calling the six `COMMIT` effects, with `pidfd` as the old process.
   *
   * @param pidfd Fake pidfd given to `accept_old_hello`.
   */
  void commit_ops(int pidfd) {
    ops->accept_old_hello(old_hello(), pidfd);
    ops->install_adoption();
    ops->resume_accept();
    ops->publish_mdns();
    ops->map_upnp();
    ops->start_tray();
    ops->become_active();
  }

  /**
   * @brief Make a pollable fd that stands for a pidfd (readable after `kill_fake_pidfd`).
   *
   * @return The fd.
   */
  static int make_fake_pidfd() {
    return ::eventfd(0, EFD_CLOEXEC);
  }

  /**
   * @brief Make a fake pidfd readable, as if the process had died.
   *
   * @param fd Fake pidfd.
   */
  static void kill_fake_pidfd(int fd) {
    const std::uint64_t one = 1;
    [[maybe_unused]] const auto written = ::write(fd, &one, sizeof(one));
  }

  /**
   * @brief Start the new side of `proc` over the channel with the real `run_new_side`.
   *
   * @param process_seams Process seams.
   */
  void start_process(const handoff::entry::process_seams_t &process_seams = {}) {
    proc = std::make_unique<handoff::entry::handoff_process_t>(
      [this] {
        gate.wait();
      },
      [this] {
        rec.add("raise_shutdown");
        gate.raise();
      },
      process_seams
    );
    handoff::new_side_timeouts_t timeouts;
    timeouts.hello = 4000ms;
    timeouts.go = 4000ms;
    timeouts.commit = 4000ms;
    ASSERT_TRUE(proc->start_new_side_for_test(config, channel, upnp_slot, timeouts, seams)) << "setup: start_new_side_for_test";
    ASSERT_NE(proc->new_ops(), nullptr) << "setup: new_ops after start";
  }

  /**
   * @brief Process seams whose `make_post_commit` records its call and builds a real `post_commit_t`.
   *
   * @return The seams.
   */
  handoff::entry::process_seams_t recording_post_commit_seams() {
    handoff::entry::process_seams_t process_seams;
    process_seams.make_post_commit = [this](handoff::sunshine_new_ops_t &o, std::shared_ptr<handoff::channel_t> ch) {
      rec.add("make_post_commit");
      return std::make_unique<handoff::entry::post_commit_t>(o, ch, handoff::entry::post_commit_seams_t {});
    };
    return process_seams;
  }

  /**
   * @brief Play the old process up to and including `READY`: `HELLO` with a pidfd, then the four listening callbacks.
   *
   * @return `true` when the new process answered with `HELLO` and a successful `READY`.
   */
  bool peer_drive_to_ready() {
    const int fake_pidfd = make_fake_pidfd();
    protocol::hello_t hello = old_hello();
    const bool sent = peer->send(hello, fake_pidfd);
    ::close(fake_pidfd);
    if (!sent) {
      return false;
    }
    if (!expect_message<protocol::hello_t>(*peer, 2000ms)) {
      return false;
    }
    if (!wait_until([this] {
          return rec.count("validate_listener") == 4;
        },
                    2000ms)) {
      return false;
    }
    for (int i = 0; i < 4; ++i) {
      proc->new_ops()->server_listening();
    }
    const auto ready = expect_message<protocol::ready_t>(*peer, 2000ms);
    return ready && ready->ok;
  }

  /**
   * @brief Play on from `READY` to the `GO_ACK`.
   *
   * @return `true` when the new process acknowledged `GO`.
   */
  bool peer_drive_go() {
    if (!peer->send(protocol::go_t {})) {
      return false;
    }
    const auto ack = expect_message<protocol::go_ack_t>(*peer, 2000ms);
    return ack && ack->ok;
  }

  handoff::new_ops_config_t config;  ///< Ops configuration.
  handoff::new_ops_seams_t seams;  ///< Ops seams (all recording).
  recorder_t rec;  ///< Record of seam calls.
  shutdown_gate_t gate;  ///< The shutdown event of the fake process.
  std::atomic<bool> hold_reload {false};  ///< While true the `reload_all` seam blocks.
  std::atomic<bool> hold_resume {false};  ///< While true the `resume_servers` seam blocks.
  std::vector<std::string> resume_result;  ///< Result of the `resume_servers` seam.
  std::set<std::thread::id> threads;  ///< Threads that ran an op seam.
  std::unique_ptr<platf::deinit_t> upnp_slot;  ///< Guard slot.
  std::shared_ptr<handoff::channel_t> channel;  ///< The new side's channel end.
  std::unique_ptr<handoff::channel_t> peer;  ///< The old process's end.
  std::unique_ptr<handoff::sunshine_new_ops_t> ops;  ///< Ops for the direct tests.
  std::unique_ptr<handoff::entry::handoff_process_t> proc;  ///< The process under test.

private:
  std::filesystem::path _dir;  ///< Temporary directory.
  std::mutex _thread_mutex;  ///< Guards `threads`.
};

// ---------------------------------------------------------------------------------------------------------------------
// post_commit_t
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffEntryProcessTest, PostCommitRoutesMainToOnMain) {
  build_ops();
  commit_ops(make_fake_pidfd());
  ASSERT_EQ(ops->stage(), new_stage_e::committed);
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [](std::shared_ptr<handoff::peer_link_t>) {
  };
  pcs.old_pidfd = [] {
    return make_fake_pidfd();
  };
  handoff::entry::post_commit_t pc {*ops, channel, pcs};
  EXPECT_EQ(rec.count("notify_ready"), 0) << "control: no MAIN yet, so no READY=1";
  ASSERT_TRUE(peer->send(protocol::main_t {}));
  EXPECT_TRUE(wait_until([this] {
    return rec.count("notify_ready") == 1;
  },
                         1500ms))
    << "MAIN must reach sunshine_new_ops_t::on_main (READY=1 sent once)";
  EXPECT_EQ(ops->stage(), new_stage_e::main) << "stage after MAIN";
}

TEST_F(HandoffEntryProcessTest, PostCommitDoesNotCallOnMainForOtherMessages) {
  build_ops();
  commit_ops(make_fake_pidfd());
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [](std::shared_ptr<handoff::peer_link_t>) {
  };
  pcs.old_pidfd = [] {
    return make_fake_pidfd();
  };
  handoff::entry::post_commit_t pc {*ops, channel, pcs};
  protocol::handoff_state_t state;
  state.state = "kill_pending";
  ASSERT_TRUE(peer->send(state));
  EXPECT_TRUE(wait_until([&] {
    return pc.link().state() == handoff::state_e::kill_pending;
  },
                         1500ms))
    << "HANDOFF_STATE must be delivered to the link";
  EXPECT_EQ(rec.count("notify_ready"), 0) << "only MAIN may call on_main";
  EXPECT_EQ(ops->stage(), new_stage_e::committed) << "stage must stay committed";
}

TEST_F(HandoffEntryProcessTest, PostCommitDeliversEndedToAWaitingEndAll) {
  build_ops();
  commit_ops(make_fake_pidfd());
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [](std::shared_ptr<handoff::peer_link_t>) {
  };
  pcs.old_pidfd = [] {
    return make_fake_pidfd();
  };
  handoff::entry::post_commit_t pc {*ops, channel, pcs};
  std::atomic<int> result {-1};
  std::thread caller([&] {
    result = static_cast<int>(pc.link().end_all(3000ms));
  });
  const auto request = expect_message<protocol::end_all_t>(*peer, 2000ms);
  if (!request) {
    peer->close();
    caller.join();
    FAIL() << "END_ALL must reach the old process through the channel";
  }
  protocol::ended_t reply;
  reply.request_id = request->request_id;
  ASSERT_TRUE(peer->send(reply));
  caller.join();
  EXPECT_EQ(result.load(), static_cast<int>(reply_e::ended)) << "ENDED must be routed to deliver, which completes the waiting end_all";
}

TEST_F(HandoffEntryProcessTest, PostCommitDeliversEndRejected) {
  build_ops();
  commit_ops(make_fake_pidfd());
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [](std::shared_ptr<handoff::peer_link_t>) {
  };
  pcs.old_pidfd = [] {
    return make_fake_pidfd();
  };
  handoff::entry::post_commit_t pc {*ops, channel, pcs};
  std::atomic<int> result {-1};
  std::thread caller([&] {
    result = static_cast<int>(pc.link().end_all(3000ms));
  });
  const auto request = expect_message<protocol::end_all_t>(*peer, 2000ms);
  if (!request) {
    peer->close();
    caller.join();
    FAIL() << "END_ALL must reach the old process";
  }
  protocol::end_rejected_t reply;
  reply.request_id = request->request_id;
  ASSERT_TRUE(peer->send(reply));
  caller.join();
  EXPECT_EQ(result.load(), static_cast<int>(reply_e::rejected)) << "END_REJECTED must be routed to deliver";
}

TEST_F(HandoffEntryProcessTest, PostCommitDeliversASessionsReply) {
  build_ops();
  commit_ops(make_fake_pidfd());
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [](std::shared_ptr<handoff::peer_link_t>) {
  };
  pcs.old_pidfd = [] {
    return make_fake_pidfd();
  };
  handoff::entry::post_commit_t pc {*ops, channel, pcs};
  std::optional<std::vector<protocol::session_entry_t>> sessions;
  std::thread caller([&] {
    sessions = pc.link().list(3000ms);
  });
  const auto request = expect_message<protocol::sessions_request_t>(*peer, 2000ms);
  if (!request) {
    peer->close();
    caller.join();
    FAIL() << "SESSIONS? must reach the old process";
  }
  protocol::sessions_reply_t reply;
  reply.request_id = request->request_id;
  protocol::session_entry_t entry;
  entry.id = 77;
  entry.transport = "udp/ipv4";
  entry.client_category = "lan";
  reply.sessions.push_back(entry);
  ASSERT_TRUE(peer->send(reply));
  caller.join();
  ASSERT_TRUE(sessions.has_value()) << "SESSIONS must be routed to deliver";
  ASSERT_EQ(sessions->size(), 1u);
  EXPECT_EQ((*sessions)[0].id, 77u) << "the session id from the reply";
}

TEST_F(HandoffEntryProcessTest, PostCommitAnswersPingWithPong) {
  build_ops();
  commit_ops(make_fake_pidfd());
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [](std::shared_ptr<handoff::peer_link_t>) {
  };
  pcs.old_pidfd = [] {
    return make_fake_pidfd();
  };
  handoff::entry::post_commit_t pc {*ops, channel, pcs};
  protocol::ping_t ping;
  ping.seq = 42;
  ASSERT_TRUE(peer->send(ping));
  const auto pong = expect_message<protocol::pong_t>(*peer, 1500ms);
  ASSERT_TRUE(pong.has_value()) << "the reader must answer PING with PONG";
  EXPECT_EQ(pong->seq, 42u) << "PONG echoes the sequence number";
}

TEST_F(HandoffEntryProcessTest, ChannelEofFailsAWaitingRequestPromptly) {
  build_ops();
  commit_ops(make_fake_pidfd());
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [](std::shared_ptr<handoff::peer_link_t>) {
  };
  pcs.old_pidfd = [] {
    return make_fake_pidfd();
  };
  handoff::entry::post_commit_t pc {*ops, channel, pcs};
  std::atomic<int> result {-1};
  std::chrono::steady_clock::duration elapsed {};
  std::thread caller([&] {
    const auto start = std::chrono::steady_clock::now();
    result = static_cast<int>(pc.link().end_all(3000ms));
    elapsed = std::chrono::steady_clock::now() - start;
  });
  if (!expect_message<protocol::end_all_t>(*peer, 2000ms).has_value()) {
    peer->close();
    caller.join();
    FAIL() << "the request must be sent";
  }
  peer->close();
  caller.join();
  EXPECT_EQ(result.load(), static_cast<int>(reply_e::no_reply)) << "a lost channel gives no_reply";
  EXPECT_LT(elapsed, 1500ms) << "on_channel_closed must be called on EOF so that the waiter fails promptly instead of running to its 3 s timeout";
}

TEST_F(HandoffEntryProcessTest, WithoutEofAWaitingRequestRunsToItsTimeout) {
  // Control for the two promptness tests: with a live channel and a live pidfd the same request runs to its timeout.
  build_ops();
  commit_ops(make_fake_pidfd());
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [](std::shared_ptr<handoff::peer_link_t>) {
  };
  pcs.old_pidfd = [] {
    return make_fake_pidfd();
  };
  handoff::entry::post_commit_t pc {*ops, channel, pcs};
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(pc.link().end_all(300ms), reply_e::no_reply) << "no answer";
  EXPECT_GE(std::chrono::steady_clock::now() - start, 250ms) << "the request waits for its timeout when nothing closes";
}

TEST_F(HandoffEntryProcessTest, ChannelEofDoesNotExitOrStartAShutdown) {
  build_ops();
  commit_ops(make_fake_pidfd());
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [](std::shared_ptr<handoff::peer_link_t>) {
  };
  pcs.old_pidfd = [] {
    return make_fake_pidfd();
  };
  handoff::entry::post_commit_t pc {*ops, channel, pcs};
  peer->close();
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(rec.count("exit_process"), 0) << "a lost channel starts no exit";
  EXPECT_FALSE(handoff::runtime().shutdown_requested()) << "a lost channel starts no shutdown";
  EXPECT_EQ(rec.count("notify_ready"), 0) << "a lost channel is not a MAIN";
}

TEST_F(HandoffEntryProcessTest, PidfdReadinessFailsAWaitingRequestPromptly) {
  build_ops();
  const int fake_pidfd = make_fake_pidfd();
  commit_ops(make_fake_pidfd());
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [](std::shared_ptr<handoff::peer_link_t>) {
  };
  pcs.old_pidfd = [fake_pidfd] {
    return fake_pidfd;
  };
  handoff::entry::post_commit_t pc {*ops, channel, pcs};
  std::atomic<int> result {-1};
  std::chrono::steady_clock::duration elapsed {};
  std::thread caller([&] {
    const auto start = std::chrono::steady_clock::now();
    result = static_cast<int>(pc.link().end_all(3000ms));
    elapsed = std::chrono::steady_clock::now() - start;
  });
  if (!expect_message<protocol::end_all_t>(*peer, 2000ms).has_value()) {
    kill_fake_pidfd(fake_pidfd);
    caller.join();
    FAIL() << "the request must be sent; the channel stays open";
  }
  kill_fake_pidfd(fake_pidfd);
  caller.join();
  EXPECT_EQ(result.load(), static_cast<int>(reply_e::no_reply)) << "the draining process died";
  EXPECT_LT(elapsed, 1500ms) << "on_channel_closed must be called when the pidfd becomes readable, with the channel still open";
  EXPECT_TRUE(pc.link().gone()) << "the link sees the old process as gone";
}

TEST_F(HandoffEntryProcessTest, PostCommitTakesThePidfdFromTheOpsByDefault) {
  build_ops();
  const int fake_pidfd = make_fake_pidfd();
  ops->accept_old_hello(old_hello(), fake_pidfd);
  ops->install_adoption();
  ops->resume_accept();
  ops->publish_mdns();
  ops->map_upnp();
  ops->start_tray();
  ops->become_active();
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [](std::shared_ptr<handoff::peer_link_t>) {
  };
  handoff::entry::post_commit_t pc {*ops, channel, pcs};
  EXPECT_EQ(ops->take_old_pidfd(), -1) << "post_commit_t must have taken the pidfd from the ops";
  EXPECT_TRUE(pc.link().exists()) << "control: the old process exists while the pidfd is not readable";
  kill_fake_pidfd(fake_pidfd);
  EXPECT_TRUE(wait_until([&] {
    return !pc.link().exists();
  },
                         1000ms))
    << "the link must watch the pidfd that the ops accepted";
}

TEST_F(HandoffEntryProcessTest, StopClearsThePeerThroughTheSeamAndIsIdempotent) {
  build_ops();
  commit_ops(make_fake_pidfd());
  std::vector<std::shared_ptr<handoff::peer_link_t>> calls;
  std::mutex calls_mutex;
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [&](std::shared_ptr<handoff::peer_link_t> link) {
    std::lock_guard lock {calls_mutex};
    calls.push_back(std::move(link));
  };
  pcs.old_pidfd = [] {
    return make_fake_pidfd();
  };
  {
    handoff::entry::post_commit_t pc {*ops, channel, pcs};
    {
      std::lock_guard lock {calls_mutex};
      ASSERT_EQ(calls.size(), 1u) << "set_peer is called once with the link at start";
      EXPECT_NE(calls[0], nullptr) << "the first set_peer call carries the link";
      EXPECT_EQ(calls[0].get(), &pc.link()) << "and it is the link of this object";
    }
    pc.stop();
    {
      std::lock_guard lock {calls_mutex};
      ASSERT_EQ(calls.size(), 2u) << "stop() must call set_peer once more";
      EXPECT_EQ(calls[1], nullptr) << "stop() must call set_peer(nullptr)";
    }
    pc.stop();
    {
      std::lock_guard lock {calls_mutex};
      EXPECT_EQ(calls.size(), 2u) << "a second stop() must do nothing";
    }
  }
  std::lock_guard lock {calls_mutex};
  EXPECT_EQ(calls.size(), 2u) << "the destructor after stop() must not call set_peer again";
}

TEST_F(HandoffEntryProcessTest, DestructorCallsStop) {
  build_ops();
  commit_ops(make_fake_pidfd());
  int null_calls = 0;
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [&](std::shared_ptr<handoff::peer_link_t> link) {
    if (!link) {
      ++null_calls;
    }
  };
  pcs.old_pidfd = [] {
    return make_fake_pidfd();
  };
  {
    handoff::entry::post_commit_t pc {*ops, channel, pcs};
  }
  EXPECT_EQ(null_calls, 1) << "the destructor must call stop(), which clears the peer";
}

TEST_F(HandoffEntryProcessTest, StopKeepsTheChannelOpenAndStopsTheReader) {
  build_ops();
  commit_ops(make_fake_pidfd());
  handoff::entry::post_commit_seams_t pcs;
  pcs.set_peer = [](std::shared_ptr<handoff::peer_link_t>) {
  };
  pcs.old_pidfd = [] {
    return make_fake_pidfd();
  };
  handoff::entry::post_commit_t pc {*ops, channel, pcs};
  const auto start = std::chrono::steady_clock::now();
  pc.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms) << "stop() joins its threads promptly";
  EXPECT_NE(channel->fd(), -1) << "stop() does not close the channel";
  protocol::ping_t ping;
  ping.seq = 5;
  ASSERT_TRUE(peer->send(ping));
  handoff::received_t received;
  ASSERT_EQ(channel->receive(received, 500ms), handoff::recv_status_e::ok) << "the reader is stopped, so the message is still in the channel";
  EXPECT_TRUE(std::holds_alternative<protocol::ping_t>(received.message)) << "the PING was not answered or consumed by a stopped reader";
}

TEST_F(HandoffEntryProcessTest, ProductionSetPeerStoresAndClearsTheRuntimePeer) {
  build_ops();
  commit_ops(make_fake_pidfd());
  handoff::entry::post_commit_seams_t pcs;
  pcs.old_pidfd = [] {
    return make_fake_pidfd();
  };
  handoff::entry::post_commit_t pc {*ops, channel, pcs};
  const auto stored = std::dynamic_pointer_cast<handoff::peer_link_t>(handoff::runtime().peer());
  ASSERT_NE(stored, nullptr) << "with no set_peer seam the link must be stored with runtime().set_peer";
  EXPECT_EQ(stored.get(), &pc.link()) << "the stored peer is the link";
  pc.stop();
  EXPECT_EQ(handoff::runtime().peer(), nullptr) << "stop() must clear the runtime peer";
}

// ---------------------------------------------------------------------------------------------------------------------
// handoff_process_t
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffEntryProcessTest, ProcessWithoutANewSideHasNoOps) {
  handoff::entry::handoff_process_t process {[this] {
                                               gate.wait();
                                             },
                                             [this] {
                                               gate.raise();
                                             }};
  EXPECT_EQ(process.new_ops(), nullptr) << "no new side, no ops";
  process.finish_new_side();
  process.finish_new_side();
}

TEST_F(HandoffEntryProcessTest, StartNewSideRefusesAProcessNotStartedByHandoff) {
  handoff::runtime().instance_remove();
  handoff::runtime().reset_for_test();
  handoff::entry::handoff_process_t process {[this] {
                                               gate.wait();
                                             },
                                             [this] {
                                               gate.raise();
                                             }};
  handoff::new_side_timeouts_t timeouts;
  std::unique_ptr<platf::deinit_t> slot;
  EXPECT_FALSE(process.start_new_side(config, slot, timeouts)) << "a normal start has no new side";
  EXPECT_EQ(process.new_ops(), nullptr) << "and no ops";
}

TEST_F(HandoffEntryProcessTest, StartNewSideForTestCannotBeCalledTwice) {
  start_process();
  handoff::new_side_timeouts_t timeouts;
  EXPECT_FALSE(proc->start_new_side_for_test(config, channel, upnp_slot, timeouts, seams)) << "the new side starts once";
}

TEST_F(HandoffEntryProcessTest, ShutdownWatcherRequestsShutdownWhenTheEventIsRaised) {
  handoff::entry::handoff_process_t process {[this] {
                                               gate.wait();
                                             },
                                             [this] {
                                               gate.raise();
                                             }};
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(handoff::runtime().shutdown_requested()) << "control: no shutdown event, no request";
  gate.raise();
  EXPECT_TRUE(wait_until([] {
    return handoff::runtime().shutdown_requested();
  },
                         1500ms))
    << "the watcher must call runtime().request_shutdown() when wait_shutdown returns";
}

TEST_F(HandoffEntryProcessTest, DestructorRaisesShutdownAndDoesNotHang) {
  std::atomic<int> raised {0};
  auto future = std::async(std::launch::async, [&] {
    handoff::entry::handoff_process_t process {[this] {
                                                 gate.wait();
                                               },
                                               [this, &raised] {
                                                 ++raised;
                                                 gate.raise();
                                               }};
  });
  EXPECT_EQ(future.wait_for(2s), std::future_status::ready) << "destroying a handoff_process_t whose event was never raised must not hang";
  EXPECT_GE(raised.load(), 1) << "the destructor calls raise_shutdown";
}

TEST_F(HandoffEntryProcessTest, AllOpsRunOnOneNewSideThreadThatIsNotTheCaller) {
  start_process(recording_post_commit_seams());
  ASSERT_TRUE(peer_drive_to_ready());
  ASSERT_TRUE(peer_drive_go());
  ASSERT_TRUE(peer->send(protocol::commit_t {}));
  ASSERT_TRUE(wait_until([this] {
    return rec.count("make_post_commit") == 1;
  },
                         2000ms))
    << "the full protocol must reach committed and start the post-COMMIT wiring";
  proc->finish_new_side();
  std::set<std::thread::id> seen;
  {
    seen = threads;
  }
  EXPECT_EQ(seen.size(), 1u) << "every op must run on the single new-side thread";
  EXPECT_EQ(seen.count(std::this_thread::get_id()), 0u) << "run_new_side must not run on the caller (main) thread";
}

TEST_F(HandoffEntryProcessTest, ADeferredEffectOnlyHappensAfterCommitInTheFullFlow) {
  start_process(recording_post_commit_seams());
  ASSERT_TRUE(peer_drive_to_ready());
  ASSERT_TRUE(peer_drive_go());
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(rec.count("publish_mdns"), 0) << "no mDNS after GO_ACK and before COMMIT";
  EXPECT_EQ(rec.count("start_upnp"), 0) << "no UPnP after GO_ACK and before COMMIT";
  EXPECT_EQ(rec.count("request_tray_start"), 0) << "no tray after GO_ACK and before COMMIT";
  EXPECT_EQ(current_role(), "pending") << "role pending after GO_ACK and before COMMIT";
  ASSERT_TRUE(peer->send(protocol::commit_t {}));
  EXPECT_TRUE(wait_until([this] {
    return current_role() == "active";
  },
                         2000ms))
    << "control: after COMMIT the role becomes active";
  EXPECT_EQ(rec.count("publish_mdns"), 1) << "mDNS after COMMIT";
  EXPECT_EQ(rec.count("start_upnp"), 1) << "UPnP after COMMIT";
  EXPECT_EQ(rec.count("request_tray_start"), 1) << "tray after COMMIT";
  EXPECT_LT(rec.index_of("set_unmap_scope(udp_only)"), rec.index_of("start_upnp")) << "udp_only scope before start_upnp";
}

TEST_F(HandoffEntryProcessTest, MainMessageAfterCommitMakesTheProcessMain) {
  start_process(recording_post_commit_seams());
  ASSERT_TRUE(peer_drive_to_ready());
  ASSERT_TRUE(peer_drive_go());
  ASSERT_TRUE(peer->send(protocol::commit_t {}));
  ASSERT_TRUE(wait_until([this] {
    return rec.count("make_post_commit") == 1;
  },
                         2000ms));
  ASSERT_TRUE(peer->send(protocol::main_t {}));
  EXPECT_TRUE(wait_until([this] {
    return rec.count("notify_ready") == 1;
  },
                         2000ms))
    << "MAIN after COMMIT must reach on_main through the post-COMMIT reader";
  EXPECT_EQ(rec.count("set_unmap_scope(all)"), 1) << "the scope all at MAIN";
  EXPECT_EQ(rec.count("exit_process"), 0) << "a successful handoff never exits";
}

// --- Seam-driven thread body -----------------------------------------------------------------------------------------

TEST_F(HandoffEntryProcessTest, ExitFailureOutcomeExitsWithOneAndReleasesWaiters) {
  handoff::entry::process_seams_t process_seams;
  process_seams.run_new_side = [](handoff::new_side_ops_t &, handoff::channel_t &, const handoff::new_side_timeouts_t &) {
    return handoff::new_outcome_e::exit_failure;
  };
  process_seams.make_post_commit = [this](handoff::sunshine_new_ops_t &, std::shared_ptr<handoff::channel_t>) -> std::unique_ptr<handoff::entry::post_commit_t> {
    rec.add("make_post_commit");
    return nullptr;
  };
  start_process(process_seams);
  EXPECT_TRUE(wait_until([this] {
    return rec.count("exit_process(1)") == 1;
  },
                         1500ms))
    << "an exit_failure outcome must exit with status 1 through the seam";
  EXPECT_TRUE(proc->new_ops()->wait_probe_decision(500ms)) << "the thread body calls mark_finished after run_new_side";
  EXPECT_EQ(rec.count("make_post_commit"), 0) << "no post-COMMIT wiring after exit_failure";
  EXPECT_EQ(rec.count("exit_process"), 1) << "exactly one exit";
}

TEST_F(HandoffEntryProcessTest, CommittedOutcomeStartsThePostCommitWiringWithoutExiting) {
  handoff::entry::process_seams_t process_seams = recording_post_commit_seams();
  process_seams.run_new_side = [](handoff::new_side_ops_t &ops_ref, handoff::channel_t &, const handoff::new_side_timeouts_t &) {
    ops_ref.accept_old_hello(old_hello(), make_fake_pidfd());
    ops_ref.install_adoption();
    ops_ref.resume_accept();
    ops_ref.publish_mdns();
    ops_ref.map_upnp();
    ops_ref.start_tray();
    ops_ref.become_active();
    return handoff::new_outcome_e::committed;
  };
  start_process(process_seams);
  EXPECT_TRUE(wait_until([this] {
    return rec.count("make_post_commit") == 1;
  },
                         1500ms))
    << "a committed outcome must build post_commit_t";
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(rec.count("exit_process"), 0) << "a committed outcome with no failed effect does not exit";
  EXPECT_TRUE(proc->new_ops()->wait_probe_decision(500ms)) << "mark_finished ran";
}

TEST_F(HandoffEntryProcessTest, CommitFailedExitsWithOneAfterThePostCommitWiring) {
  resume_result = {"https"};
  handoff::entry::process_seams_t process_seams = recording_post_commit_seams();
  process_seams.run_new_side = [](handoff::new_side_ops_t &ops_ref, handoff::channel_t &, const handoff::new_side_timeouts_t &) {
    ops_ref.accept_old_hello(old_hello(), make_fake_pidfd());
    ops_ref.install_adoption();
    ops_ref.resume_accept();
    ops_ref.publish_mdns();
    ops_ref.map_upnp();
    ops_ref.start_tray();
    ops_ref.become_active();
    return handoff::new_outcome_e::committed;
  };
  start_process(process_seams);
  ASSERT_TRUE(wait_until([this] {
    return rec.count("exit_process(1)") == 1;
  },
                         1500ms))
    << "a failed COMMIT effect must end in exit status 1";
  EXPECT_EQ(rec.count("make_post_commit"), 1) << "the post-COMMIT wiring is completed first";
  EXPECT_LT(rec.index_of("make_post_commit"), rec.index_of("exit_process(1)")) << "post_commit_t is built before the exit, so the shutdown can still send END_ALL";
  EXPECT_EQ(rec.count("publish_mdns"), 0) << "nothing is announced after a failed resume";
  EXPECT_EQ(rec.count("start_upnp"), 0) << "no UPnP after a failed resume";
  EXPECT_EQ(rec.count("request_tray_start"), 0) << "no tray after a failed resume";
}

// --- SIGTERM by stage ------------------------------------------------------------------------------------------------

TEST_F(HandoffEntryProcessTest, SigtermAtStageStartingExitsWithOneAndPublishesNothing) {
  start_process();
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(rec.count("exit_process"), 0) << "control: without a shutdown the new side waits for the old HELLO";
  EXPECT_EQ(proc->new_ops()->stage(), new_stage_e::starting) << "stage is starting";
  gate.raise();
  EXPECT_TRUE(wait_until([this] {
    return rec.count("exit_process(1)") == 1;
  },
                         2000ms))
    << "SIGTERM at stage starting: abort(), run_new_side returns exit_failure, exit status 1";
  EXPECT_TRUE(handoff::runtime().shutdown_requested()) << "the watcher requests the shutdown";
  EXPECT_EQ(rec.count("shutdown_channel"), 1) << "the channel is half-closed";
  EXPECT_EQ(rec.count("publish_mdns") + rec.count("start_upnp") + rec.count("request_tray_start") + rec.count("set_unmap_scope"), 0) << "nothing is published, mapped or shown";
  EXPECT_EQ(rec.count("install_adoption"), 0) << "nothing is installed";
  EXPECT_EQ(current_role(), "pending") << "no active file is left behind";
}

TEST_F(HandoffEntryProcessTest, SigtermAtStageReadyExitsWithOneAndTheOldSideSeesTheChannelEnd) {
  start_process();
  ASSERT_TRUE(peer_drive_to_ready());
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(rec.count("exit_process"), 0) << "control: ready and waiting for GO, no exit yet";
  gate.raise();
  EXPECT_TRUE(wait_until([this] {
    return rec.count("exit_process(1)") == 1;
  },
                         2000ms))
    << "SIGTERM while waiting for GO exits with status 1";
  handoff::received_t received;
  handoff::recv_status_e status = handoff::recv_status_e::timeout;
  const auto end = std::chrono::steady_clock::now() + 1s;
  while (std::chrono::steady_clock::now() < end && (status == handoff::recv_status_e::timeout || status == handoff::recv_status_e::ok)) {
    status = peer->receive(received, 50ms);
    if (received.fd != -1) {
      ::close(received.fd);
      received.fd = -1;
    }
  }
  EXPECT_EQ(status, handoff::recv_status_e::eof) << "the old process must see the channel end (it then rolls back, keeping its sessions)";
  EXPECT_EQ(rec.count("install_adoption") + rec.count("publish_mdns") + rec.count("start_upnp") + rec.count("request_tray_start"), 0) << "nothing installed, published, mapped or shown";
}

TEST_F(HandoffEntryProcessTest, SigtermAtStageGoLetsTheRunningOpFinishThenExitsWithOne) {
  hold_reload = true;
  start_process();
  ASSERT_TRUE(peer_drive_to_ready());
  ASSERT_TRUE(peer->send(protocol::go_t {}));
  ASSERT_TRUE(wait_until([this] {
    return rec.count("reload_all") == 1;
  },
                         2000ms))
    << "the GO handler must start";
  EXPECT_EQ(proc->new_ops()->stage(), new_stage_e::go) << "stage go while reload_all runs";
  gate.raise();
  ASSERT_TRUE(wait_until([this] {
    return rec.count("shutdown_channel") == 1;
  },
                         2000ms))
    << "SIGTERM at stage go half-closes the channel";
  EXPECT_EQ(rec.count("exit_process"), 0) << "the running op finishes first; nothing exits while it runs";
  hold_reload = false;
  EXPECT_TRUE(wait_until([this] {
    return rec.count("exit_process(1)") == 1;
  },
                         2000ms))
    << "after the op the next channel wait fails: exit status 1";
  EXPECT_EQ(rec.count("install_adoption"), 0) << "nothing is installed";
  EXPECT_EQ(rec.count("publish_mdns") + rec.count("start_upnp") + rec.count("request_tray_start"), 0) << "nothing published, mapped or shown";
  EXPECT_EQ(current_role(), "pending") << "the role stays pending";
}

TEST_F(HandoffEntryProcessTest, SigtermAtStageCommittingKeepsTheChannelAndEndsNormally) {
  hold_resume = true;
  start_process(recording_post_commit_seams());
  ASSERT_TRUE(peer_drive_to_ready());
  ASSERT_TRUE(peer_drive_go());
  ASSERT_TRUE(peer->send(protocol::commit_t {}));
  ASSERT_TRUE(wait_until([this] {
    return rec.count("resume_servers") == 1;
  },
                         2000ms))
    << "the COMMIT effects must start";
  EXPECT_EQ(proc->new_ops()->stage(), new_stage_e::committing) << "stage committing while resume runs";
  gate.raise();
  ASSERT_TRUE(wait_until([] {
    return handoff::runtime().shutdown_requested();
  },
                         2000ms));
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(rec.count("shutdown_channel"), 0) << "from committing on, abort() is a no-op: the channel is needed for END_ALL";
  hold_resume = false;
  EXPECT_TRUE(wait_until([this] {
    return rec.count("make_post_commit") == 1;
  },
                         2000ms))
    << "the COMMIT effects finish and the post-COMMIT wiring starts";
  EXPECT_EQ(rec.count("exit_process"), 0) << "a normal shutdown: no exit(1) from the new-side thread";
  protocol::ping_t ping;
  ping.seq = 11;
  ASSERT_TRUE(peer->send(ping));
  EXPECT_TRUE(expect_message<protocol::pong_t>(*peer, 1500ms).has_value()) << "the channel is open and the reader is serving it, so the shutdown guard can still send END_ALL";
}

TEST_F(HandoffEntryProcessTest, SigtermAtStageCommittedExitsNormally) {
  start_process(recording_post_commit_seams());
  ASSERT_TRUE(peer_drive_to_ready());
  ASSERT_TRUE(peer_drive_go());
  ASSERT_TRUE(peer->send(protocol::commit_t {}));
  ASSERT_TRUE(wait_until([this] {
    return rec.count("make_post_commit") == 1;
  },
                         2000ms));
  EXPECT_EQ(proc->new_ops()->stage(), new_stage_e::committed) << "waiting for MAIN";
  gate.raise();
  ASSERT_TRUE(wait_until([] {
    return handoff::runtime().shutdown_requested();
  },
                         2000ms));
  proc->finish_new_side();
  EXPECT_EQ(rec.count("exit_process"), 0) << "a normal shutdown: exit status comes from main(), not from an exit_failure";
  EXPECT_EQ(rec.count("shutdown_channel"), 0) << "the channel stays open at stage committed";
}

TEST_F(HandoffEntryProcessTest, SigtermAtStageMainExitsNormally) {
  start_process(recording_post_commit_seams());
  ASSERT_TRUE(peer_drive_to_ready());
  ASSERT_TRUE(peer_drive_go());
  ASSERT_TRUE(peer->send(protocol::commit_t {}));
  ASSERT_TRUE(wait_until([this] {
    return rec.count("make_post_commit") == 1;
  },
                         2000ms));
  ASSERT_TRUE(peer->send(protocol::main_t {}));
  ASSERT_TRUE(wait_until([this] {
    return rec.count("notify_ready") == 1;
  },
                         2000ms));
  gate.raise();
  ASSERT_TRUE(wait_until([] {
    return handoff::runtime().shutdown_requested();
  },
                         2000ms));
  proc->finish_new_side();
  EXPECT_EQ(rec.count("exit_process"), 0) << "a normal shutdown of the active process never calls exit_failure";
  EXPECT_EQ(rec.count("shutdown_channel"), 0) << "the channel stays open";
}

TEST_F(HandoffEntryProcessTest, FinishNewSideAtStageStartingAbortsAndJoins) {
  start_process();
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(rec.count("exit_process"), 0) << "control: nothing has exited yet";
  proc->finish_new_side();
  EXPECT_EQ(rec.count("exit_process(1)"), 1) << "finish_new_side aborts a new side that has not committed, and joins it, so the exit has happened";
  proc->finish_new_side();
  EXPECT_EQ(rec.count("exit_process"), 1) << "finish_new_side is idempotent";
}

#endif  // __linux__
