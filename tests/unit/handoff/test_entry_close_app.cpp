/**
 * @file tests/unit/handoff/test_entry_close_app.cpp
 * @brief Blind tests for the `--close-app` dispatch of the helper (src/handoff/entry.h): `run_close_app_cli()` with a
 *        fake environment, the reply timeout variable, and `maybe_run_helper()` for the `--close-app` argument. Written
 *        from docs/drain-handoff.md and the header only.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <map>
  #include <string>
  #include <vector>

  // local includes
  #include "src/handoff/control.h"
  #include "src/handoff/entry.h"

using namespace std::chrono_literals;
namespace hf = handoff;

namespace {
  /**
   * @brief Recording fake of the helper environment: variables, the two helper functions and stderr.
   */
  struct close_cli_rig_t {
    std::map<std::string, std::string> env;  ///< Variables that are set.
    int run_calls {0};  ///< Calls of the drain helper (`run`); must stay 0 for `--close-app`.
    int close_calls {0};  ///< Calls of the close-app helper (`run_close_app`).
    std::vector<std::string> close_args;  ///< Arguments of the last `run_close_app` call.
    hf::helper_env_t close_env;  ///< Environment of the last `run_close_app` call.
    hf::helper_exit_e close_result {hf::helper_exit_e::switched};  ///< Result of `run_close_app`.
    std::string close_err {"closed app"};  ///< `err` that `run_close_app` produces.
    std::vector<std::string> stderr_lines;  ///< Lines given to `write_err`.

    /**
     * @brief Build the `cli_env_t` over this object.
     *
     * @return Environment.
     */
    hf::entry::cli_env_t make() {
      hf::entry::cli_env_t cli;
      cli.getenv = [this](const char *name) -> const char * {
        auto it = env.find(name);
        return it == env.end() ? nullptr : it->second.c_str();
      };
      cli.run = [this](const std::vector<std::string> &, const hf::helper_env_t &, std::string &) {
        ++run_calls;
        return hf::helper_exit_e::switched;
      };
      cli.run_close_app = [this](const std::vector<std::string> &args, const hf::helper_env_t &helper_env, std::string &err) {
        ++close_calls;
        close_args = args;
        close_env = helper_env;
        err = close_err;
        return close_result;
      };
      cli.write_err = [this](const std::string &line) {
        stderr_lines.push_back(line);
      };
      return cli;
    }
  };
}  // namespace

/**
 * @brief Tests for the `--close-app` dispatch.
 */
class HandoffEntryCloseAppTest: public testing::Test {};

/**
 * @brief With no runtime directory the dispatch returns 5, writes the "no runtime directory" line once and calls
 * neither helper.
 */
TEST_F(HandoffEntryCloseAppTest, NoRuntimeDirectoryReturnsFiveAndCallsNoHelper) {
  close_cli_rig_t rig;
  const int code = hf::entry::run_close_app_cli({"1234"}, rig.make());
  EXPECT_EQ(code, 5) << "--close-app without a runtime directory must return 5";
  EXPECT_EQ(rig.close_calls, 0) << "run_close_app must not be called without a runtime directory";
  EXPECT_EQ(rig.run_calls, 0) << "the drain helper must never be called by the --close-app dispatch";
  ASSERT_EQ(rig.stderr_lines.size(), 1u) << "exactly one line must go to stderr, got " << rig.stderr_lines.size();
  EXPECT_EQ(rig.stderr_lines[0], "no runtime directory (RUNTIME_DIRECTORY and XDG_RUNTIME_DIR are not set)")
    << "the stderr line must be the no runtime directory message";
}

/**
 * @brief With a runtime directory the dispatch calls `run_close_app` (never `run`) with the arguments unchanged, a 60 s
 * reply timeout by default, and writes the helper's outcome line; the return value is the helper's exit code.
 */
TEST_F(HandoffEntryCloseAppTest, CallsRunCloseAppWithDefaultSixtySecondTimeout) {
  close_cli_rig_t rig;
  rig.env["RUNTIME_DIRECTORY"] = "/run/sunshine";
  rig.close_result = hf::helper_exit_e::refused;
  rig.close_err = "a drain is in progress";

  const int code = hf::entry::run_close_app_cli({"1234"}, rig.make());

  EXPECT_EQ(code, 1) << "the return value must be the exit code of run_close_app (refused is 1)";
  EXPECT_EQ(rig.close_calls, 1) << "run_close_app must be called once, got " << rig.close_calls;
  EXPECT_EQ(rig.run_calls, 0) << "run (the drain helper) must not be called by --close-app";
  EXPECT_EQ(rig.close_args, std::vector<std::string> {"1234"}) << "the arguments after --close-app must be passed unchanged";
  EXPECT_EQ(rig.close_env.reply_timeout, 60000ms) << "the default reply timeout of --close-app must be 60 s";
  EXPECT_EQ(rig.close_env.runtime_dir, std::filesystem::path {"/run/sunshine"}) << "the runtime directory must come from RUNTIME_DIRECTORY";
  ASSERT_EQ(rig.stderr_lines.size(), 1u) << "the outcome line must be written once, got " << rig.stderr_lines.size();
  EXPECT_EQ(rig.stderr_lines[0], "a drain is in progress") << "the stderr line must be the helper's err text";
}

/**
 * @brief `SUNSHINE_CLOSE_APP_REPLY_TIMEOUT` (seconds) sets the reply timeout; an invalid value is ignored, with one
 * stderr line "ignoring invalid SUNSHINE_CLOSE_APP_REPLY_TIMEOUT", and the timeout stays 60 s.
 */
TEST_F(HandoffEntryCloseAppTest, ReplyTimeoutVariableOverridesAndInvalidValueIsIgnored) {
  close_cli_rig_t valid;
  valid.env["RUNTIME_DIRECTORY"] = "/run/sunshine";
  valid.env["SUNSHINE_CLOSE_APP_REPLY_TIMEOUT"] = "5";
  hf::entry::run_close_app_cli({"1234"}, valid.make());
  EXPECT_EQ(valid.close_env.reply_timeout, 5000ms) << "SUNSHINE_CLOSE_APP_REPLY_TIMEOUT=5 must give a 5 s reply timeout";

  close_cli_rig_t invalid;
  invalid.env["RUNTIME_DIRECTORY"] = "/run/sunshine";
  invalid.env["SUNSHINE_CLOSE_APP_REPLY_TIMEOUT"] = "abc";
  hf::entry::run_close_app_cli({"1234"}, invalid.make());
  EXPECT_EQ(invalid.close_env.reply_timeout, 60000ms) << "an invalid SUNSHINE_CLOSE_APP_REPLY_TIMEOUT must leave the 60 s default";
  bool announced = false;
  for (const auto &line : invalid.stderr_lines) {
    if (line.find("ignoring invalid SUNSHINE_CLOSE_APP_REPLY_TIMEOUT") != std::string::npos) {
      announced = true;
    }
  }
  EXPECT_TRUE(announced) << "an invalid timeout must be announced with the line \"ignoring invalid SUNSHINE_CLOSE_APP_REPLY_TIMEOUT\"";
}

/**
 * @brief `maybe_run_helper()` does nothing without an argument (`std::nullopt`), and `--close-app` with no pid is a
 * usage error (exit 5) whatever the runtime directory is.
 */
TEST_F(HandoffEntryCloseAppTest, MaybeRunHelperIgnoresPlainStartAndTreatsMissingPidAsUsageError) {
  char program[] = "sunshine";
  char *plain_argv[] = {program, nullptr};
  EXPECT_FALSE(hf::entry::maybe_run_helper(1, plain_argv).has_value()) << "without an argument maybe_run_helper must return std::nullopt";

  char verb[] = "--close-app";
  char *close_argv[] = {program, verb, nullptr};
  const auto code = hf::entry::maybe_run_helper(2, close_argv);
  ASSERT_TRUE(code.has_value()) << "argv[1] equal to --close-app must run the helper and return an exit code";
  EXPECT_EQ(*code, 5) << "--close-app with no pid must be a usage error (exit 5)";
}

#endif  // __linux__
