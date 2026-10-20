/**
 * @file tests/unit/test_process_adoption.cpp
 * @brief Test the drain handoff additions of src/process.*: app ownership, release, adoption and the digest helpers.
 *
 * Written from the doc comments of `src/process.h`, `src/handoff/app_digest.h`, `src/handoff/protocol.h` and
 * `docs/drain-handoff.md` ("App ownership and prep commands").
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
  #include <cerrno>
  #include <csignal>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

// local includes
#include <src/process.h>

namespace fs = std::filesystem;

namespace {
  using env_list_t = std::vector<std::pair<std::string, std::string>>;

  boost::process::v1::environment make_env(const env_list_t &entries) {
    boost::process::v1::environment env;
    for (const auto &[name, value] : entries) {
      env[name] = value;
    }
    return env;
  }
}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Pure helpers: portable
// ---------------------------------------------------------------------------------------------------------------

TEST(ProcessLaunchEnvNameTest, LaunchPrefixesMatch) {
  EXPECT_TRUE(proc::is_launch_env_name("SUNSHINE_APP_ID")) << "SUNSHINE_APP_ prefix must be a launch name";
  EXPECT_TRUE(proc::is_launch_env_name("SUNSHINE_APP_NAME")) << "SUNSHINE_APP_ prefix must be a launch name";
  EXPECT_TRUE(proc::is_launch_env_name("SUNSHINE_CLIENT_WIDTH")) << "SUNSHINE_CLIENT_ prefix must be a launch name";
  EXPECT_TRUE(proc::is_launch_env_name("SUNSHINE_CLIENT_FPS")) << "SUNSHINE_CLIENT_ prefix must be a launch name";
}

TEST(ProcessLaunchEnvNameTest, PrefixAloneMatches) {
  EXPECT_TRUE(proc::is_launch_env_name("SUNSHINE_APP_")) << "a name that is exactly the prefix starts with it";
  EXPECT_TRUE(proc::is_launch_env_name("SUNSHINE_CLIENT_")) << "a name that is exactly the prefix starts with it";
}

TEST(ProcessLaunchEnvNameTest, OtherNamesDoNotMatch) {
  EXPECT_FALSE(proc::is_launch_env_name("")) << "empty name is not a launch name";
  EXPECT_FALSE(proc::is_launch_env_name("SUNSHINE_APP")) << "no trailing underscore: not the prefix";
  EXPECT_FALSE(proc::is_launch_env_name("SUNSHINE_APPLE")) << "SUNSHINE_APPLE does not start with SUNSHINE_APP_";
  EXPECT_FALSE(proc::is_launch_env_name("SUNSHINE_CLIENT")) << "no trailing underscore: not the prefix";
  EXPECT_FALSE(proc::is_launch_env_name("SUNSHINE_CLIENTS")) << "SUNSHINE_CLIENTS does not start with SUNSHINE_CLIENT_";
  EXPECT_FALSE(proc::is_launch_env_name("SUNSHINE_UDP_BASE")) << "restart variable is not a launch name";
  EXPECT_FALSE(proc::is_launch_env_name("SUNSHINE_GENERATION")) << "restart variable is not a launch name";
  EXPECT_FALSE(proc::is_launch_env_name("SUNSHINE_HANDOFF_FDS")) << "handoff variable is not a launch name";
  EXPECT_FALSE(proc::is_launch_env_name("PATH")) << "ordinary variable is not a launch name";
  EXPECT_FALSE(proc::is_launch_env_name("XSUNSHINE_APP_ID")) << "prefix must be at the start";
  EXPECT_FALSE(proc::is_launch_env_name("sunshine_app_id")) << "match is case sensitive";
  EXPECT_FALSE(proc::is_launch_env_name(" SUNSHINE_APP_ID")) << "leading blank: not a launch name";
}

TEST(ProcessBaseEnvTest, EmptyEnvironmentGivesEmptyList) {
  const auto env = make_env({});
  EXPECT_TRUE(proc::base_env_for_digest(env).empty()) << "an empty environment has no base entries";
}

TEST(ProcessBaseEnvTest, LaunchVariablesAreStripped) {
  const auto env = make_env({
    {"KEEP_ME", "1"},
    {"SUNSHINE_APP_ID", "3"},
    {"SUNSHINE_APP_NAME", "x"},
    {"SUNSHINE_CLIENT_WIDTH", "1920"},
    {"SUNSHINE_CLIENT_HEIGHT", "1080"},
  });
  const auto base = proc::base_env_for_digest(env);
  ASSERT_EQ(base.size(), 1u) << "only KEEP_ME may stay; SUNSHINE_APP_* and SUNSHINE_CLIENT_* are launch variables";
  EXPECT_EQ(base[0].first, "KEEP_ME");
  EXPECT_EQ(base[0].second, "1");
}

TEST(ProcessBaseEnvTest, RestartAndHandoffVariablesAreStripped) {
  const auto env = make_env({
    {"KEEP_ME", "1"},
    {"SUNSHINE_UDP_BASE", "47998"},
    {"SUNSHINE_GENERATION", "4"},
    {"SUNSHINE_HANDOFF_FDS", "1:2"},
    {"SUNSHINE_HANDOFF_ANYTHING", "x"},
  });
  const auto base = proc::base_env_for_digest(env);
  ASSERT_EQ(base.size(), 1u) << "SUNSHINE_UDP_BASE, SUNSHINE_GENERATION and SUNSHINE_HANDOFF_* must be stripped";
  EXPECT_EQ(base[0].first, "KEEP_ME");
}

TEST(ProcessBaseEnvTest, OtherSunshineNamesAreKept) {
  const auto env = make_env({
    {"SUNSHINE_APP", "a"},
    {"SUNSHINE_APPLE", "b"},
    {"SUNSHINE_CLIENTS", "c"},
    {"SUNSHINE_OTHER", "d"},
  });
  EXPECT_EQ(proc::base_env_for_digest(env).size(), 4u) << "names that are not launch, restart or handoff names must stay";
}

TEST(ProcessBaseEnvTest, ResultIsSortedBytewiseByName) {
  const auto env = make_env({
    {"zeta", "1"},
    {"ALPHA", "2"},
    {"Mid", "3"},
    {"_under", "4"},
    {"ALPHA2", "5"},
  });
  const auto base = proc::base_env_for_digest(env);
  ASSERT_EQ(base.size(), 5u);
  std::vector<std::string> names;
  for (const auto &entry : base) {
    names.push_back(entry.first);
  }
  EXPECT_EQ(names, (std::vector<std::string> {"ALPHA", "ALPHA2", "Mid", "_under", "zeta"})) << "entries must be sorted byte-wise by name (uppercase before lowercase)";
}

TEST(ProcessBaseEnvTest, ValuesAreKept) {
  const auto env = make_env({{"A", "value with spaces"}, {"B", ""}});
  const auto base = proc::base_env_for_digest(env);
  ASSERT_EQ(base.size(), 2u);
  EXPECT_EQ(base[0], (std::pair<std::string, std::string> {"A", "value with spaces"}));
  EXPECT_EQ(base[1].first, "B");
  EXPECT_EQ(base[1].second, "");
}

TEST(ProcessBaseEnvTest, DigestBaseEnvIgnoresProcessEnvironment) {
  proc::proc_t proc(make_env({{"B", "2"}, {"A", "1"}, {"SUNSHINE_APP_ID", "9"}, {"SUNSHINE_HANDOFF_X", "y"}}), {});
  EXPECT_TRUE(proc.digest_base_env().empty()) << "the process environment is not part of the digest; only the apps.json env section is";
}

// ---------------------------------------------------------------------------------------------------------------
// proc_t: portable atomics-only behaviour
// ---------------------------------------------------------------------------------------------------------------

namespace {
  proc::ctx_t make_app(const std::string &id, const std::string &name, std::vector<proc::cmd_t> prep = {}) {
    proc::ctx_t app {};
    app.id = id;
    app.name = name;
    app.prep_cmds = std::move(prep);
    app.elevated = false;
    app.auto_detach = false;
    app.wait_all = false;
    app.exit_timeout = std::chrono::seconds {5};
    return app;
  }

  proc::cmd_t make_cmd(const std::string &do_cmd, const std::string &undo_cmd) {
    return proc::cmd_t(std::string(do_cmd), std::string(undo_cmd), false);
  }
}  // namespace

TEST(ProcessAtomicsTest, MoveConstructCopiesValues) {
  proc::app_atomics_t a;
  a.app_id = 7;
  a.placebo = true;
  a.pgid = 1234;
  a.applied = 3;
  a.released = true;
  proc::app_atomics_t b(std::move(a));
  EXPECT_EQ(b.app_id.load(), 7);
  EXPECT_TRUE(b.placebo.load());
  EXPECT_EQ(b.pgid.load(), 1234);
  EXPECT_EQ(b.applied.load(), 3u);
  EXPECT_TRUE(b.released.load());
}

TEST(ProcessAtomicsTest, MoveAssignCopiesValues) {
  proc::app_atomics_t a;
  a.app_id = -1;
  a.placebo = true;
  a.pgid = 55;
  a.applied = 9;
  proc::app_atomics_t b;
  b.app_id = 4;
  b.released = true;
  b = std::move(a);
  EXPECT_EQ(b.app_id.load(), -1);
  EXPECT_TRUE(b.placebo.load());
  EXPECT_EQ(b.pgid.load(), 55);
  EXPECT_EQ(b.applied.load(), 9u);
  EXPECT_FALSE(b.released.load()) << "move assignment must overwrite every field, including released";
}

TEST(ProcessAtomicsTest, DefaultIsCleared) {
  proc::app_atomics_t a;
  EXPECT_EQ(a.app_id.load(), 0);
  EXPECT_FALSE(a.placebo.load());
  EXPECT_EQ(a.pgid.load(), 0);
  EXPECT_EQ(a.applied.load(), 0u);
  EXPECT_FALSE(a.released.load());
  EXPECT_FALSE(std::is_copy_constructible_v<proc::app_atomics_t>);
  EXPECT_FALSE(std::is_copy_assignable_v<proc::app_atomics_t>);
}

TEST(ProcessAdoptableTest, NoAppIsAdoptableAndLeavesWhyUntouched) {
  proc::proc_t proc;
  std::string why = "untouched";
  EXPECT_TRUE(proc.adoptable(&why)) << "no app recorded: nothing to hand over, adoptable";
  EXPECT_EQ(why, "untouched") << "why must not be touched when adoptable";
}

TEST(ProcessAdoptableTest, NullWhyIsAccepted) {
  proc::proc_t proc;
  EXPECT_TRUE(proc.adoptable(nullptr)) << "why may be null";
  proc.set_running_for_test(make_app("1", "a"), 1, false, 0, 0);
  EXPECT_FALSE(proc.adoptable(nullptr)) << "non-placebo app must be refused even with a null why";
}

TEST(ProcessAdoptableTest, NoAppDoesNotCallProbe) {
  proc::proc_t proc;
  bool called = false;
  EXPECT_TRUE(proc.adoptable(nullptr, [&](pid_t) {
    called = true;
    return true;
  }));
  EXPECT_FALSE(called) << "with no app the group probe must not be asked";
}

TEST(ProcessAdoptableTest, NonPlaceboAppIsRefused) {
  proc::proc_t proc;
  proc.set_running_for_test(make_app("1", "a"), 1, false, 0, 0);
  std::string why;
  EXPECT_FALSE(proc.adoptable(&why, [](pid_t) {
    return false;
  }))
    << "a non-placebo app is never adoptable";
  EXPECT_EQ(why, "the running app is not in placebo mode");
}

TEST(ProcessAdoptableTest, NonPlaceboAppWithGroupIsRefusedAsNotPlacebo) {
  proc::proc_t proc;
  proc.set_running_for_test(make_app("1", "a"), 1, false, 4242, 0);
  std::string why;
  EXPECT_FALSE(proc.adoptable(&why, [](pid_t) {
    return true;
  }));
  EXPECT_EQ(why, "the running app is not in placebo mode") << "placebo check comes before the group check";
}

TEST(ProcessAdoptableTest, PlaceboWithoutGroupIsAdoptable) {
  proc::proc_t proc;
  proc.set_running_for_test(make_app("1", "a"), 1, true, 0, 0);
  std::string why = "untouched";
  bool called = false;
  EXPECT_TRUE(proc.adoptable(&why, [&](pid_t) {
    called = true;
    return true;
  }))
    << "placebo app with group id 0 has no group to ask about";
  EXPECT_FALSE(called) << "pgid 0 must not reach the probe";
  EXPECT_EQ(why, "untouched");
}

TEST(ProcessAdoptableTest, PgidOneOrBelowCountsAsNoGroup) {
  for (const pid_t pgid : {pid_t {1}, pid_t {0}}) {
    proc::proc_t proc;
    proc.set_running_for_test(make_app("1", "a"), 1, true, pgid, 0);
    bool called = false;
    EXPECT_TRUE(proc.adoptable(nullptr, [&](pid_t) {
      called = true;
      return true;
    }))
      << "pgid " << pgid << " must count as no group";
    EXPECT_FALSE(called) << "pgid <= 1 must not reach the probe";
  }
}

TEST(ProcessAdoptableTest, PlaceboWithLiveGroupIsRefused) {
  proc::proc_t proc;
  proc.set_running_for_test(make_app("1", "a"), 1, true, 4242, 0);
  pid_t asked = 0;
  std::string why;
  EXPECT_FALSE(proc.adoptable(&why, [&](pid_t pgid) {
    asked = pgid;
    return true;
  }))
    << "a placebo app whose group still has a process is not adoptable";
  EXPECT_EQ(asked, 4242) << "the probe must be asked about the recorded group id";
  EXPECT_EQ(why, "the running app still has processes in its process group");
}

TEST(ProcessAdoptableTest, PlaceboWithEmptyGroupIsAdoptable) {
  proc::proc_t proc;
  proc.set_running_for_test(make_app("1", "a"), 1, true, 4242, 0);
  std::string why = "untouched";
  EXPECT_TRUE(proc.adoptable(&why, [](pid_t) {
    return false;
  }))
    << "probe says no process left: adoptable";
  EXPECT_EQ(why, "untouched");
}

TEST(ProcessAdoptableTest, AdoptableChangesNoState) {
  proc::proc_t proc;
  proc.set_running_for_test(make_app("1", "a"), 1, true, 4242, 0);
  (void) proc.adoptable(nullptr, [](pid_t) {
    return true;
  });
  EXPECT_FALSE(proc.released()) << "adoptable() must not change the released flag";
  EXPECT_EQ(proc.running(), 1) << "adoptable() must not change the running app";
}

TEST(ProcessAdoptableTest, AfterDropOfReleasedAppIsAdoptable) {
  proc::proc_t proc;
  proc.set_running_for_test(make_app("1", "a"), 1, true, 0, 0);
  proc.set_released(true);
  ASSERT_TRUE(proc.drop_released_app());
  EXPECT_TRUE(proc.adoptable(nullptr)) << "after a drop the app id is -1 (no app recorded): adoptable";
}

TEST(ProcessReleaseTest, FlagDefaultsToFalseAndToggles) {
  proc::proc_t proc;
  EXPECT_FALSE(proc.released());
  proc.set_released(true);
  EXPECT_TRUE(proc.released());
  proc.set_released(false);
  EXPECT_FALSE(proc.released()) << "set_released(false) takes the app back";
}

TEST(ProcessReleaseTest, RunningIgnoresReleasedFlag) {
  proc::proc_t proc;
  proc.set_running_for_test(make_app("3", "a"), 3, true, 0, 0);
  EXPECT_EQ(proc.running(), 3);
  proc.set_released(true);
  EXPECT_EQ(proc.running(), 3) << "running() must ignore the released flag so the control loop keeps the sessions";
}

TEST(ProcessReleaseTest, RunningInPlaceboReturnsIdOrZero) {
  proc::proc_t proc;
  EXPECT_EQ(proc.running(), 0) << "fresh object: no app";
  proc.set_running_for_test(make_app("12", "a"), 12, true, 0, 0);
  EXPECT_EQ(proc.running(), 12) << "placebo app with a recorded id: running() returns the id";
}

TEST(ProcessDropReleasedTest, NotReleasedDoesNothing) {
  proc::proc_t proc;
  proc.set_running_for_test(make_app("2", "a"), 2, true, 0, 0);
  EXPECT_FALSE(proc.drop_released_app()) << "an app that is not released must not be dropped";
  EXPECT_EQ(proc.running(), 2) << "the app must stay";
  EXPECT_FALSE(proc.released());
}

TEST(ProcessDropReleasedTest, NoAppDoesNothing) {
  proc::proc_t proc;
  EXPECT_FALSE(proc.drop_released_app());
}

TEST(ProcessDropReleasedTest, ReleasedAppIsDropped) {
  proc::proc_t proc;
  proc.set_running_for_test(make_app("2", "a"), 2, true, 0, 0);
  proc.set_released(true);
  EXPECT_TRUE(proc.drop_released_app()) << "a released app must be dropped";
  EXPECT_FALSE(proc.released()) << "the released flag must be cleared";
  EXPECT_FALSE(proc.drop_released_app()) << "a second drop finds nothing released";
}

TEST(ProcessDropReleasedTest, DroppedProcessCanAdoptAgain) {
  proc::proc_t proc;
  proc.set_running_for_test(make_app("2", "a"), 2, true, 0, 0);
  proc.set_released(true);
  ASSERT_TRUE(proc.drop_released_app());
  std::string why;
  EXPECT_TRUE(proc.adoptable(&why)) << why;
  proc.set_running_for_test(make_app("2", "a"), 2, true, 0, 0);
  EXPECT_EQ(proc.running(), 2);
}

TEST(ProcessSwapAppListTest, SwapKeepsRunningApp) {
  proc::proc_t proc(make_env({}), {make_app("1", "one")});
  proc.set_running_for_test(make_app("1", "one"), 1, true, 0, 0);
  proc.swap_app_list({make_app("5", "five"), make_app("6", "six")});
  EXPECT_EQ(proc.running(), 1) << "swap_app_list must not touch the running app";
  EXPECT_EQ(proc.get_last_run_app_name(), "one") << "the running app keeps its own copy";
}

#ifdef __linux__

namespace {
  /**
   * @brief Temp dir with helper scripts that append markers to a log file.
   */
  class marker_dir_t {
  public:
    marker_dir_t() {
      std::string tmpl = (fs::temp_directory_path() / "sunshine_adopt_XXXXXX").string();
      std::vector<char> buf(tmpl.begin(), tmpl.end());
      buf.push_back('\0');
      const char *made = mkdtemp(buf.data());
      if (made != nullptr) {
        _dir = made;
      }
      // $1 is the marker; the second field is the launch variable, to show which environment the command got
      write_script("m.sh", "#!/bin/sh\necho \"$1 ${SUNSHINE_CLIENT_WIDTH}\" >> \"" + log_path().string() + "\"\n");
    }

    ~marker_dir_t() {
      std::error_code ec;
      fs::remove_all(_dir, ec);
    }

    marker_dir_t(const marker_dir_t &) = delete;
    marker_dir_t &operator=(const marker_dir_t &) = delete;

    const fs::path &dir() const {
      return _dir;
    }

    fs::path log_path() const {
      return _dir / "log.txt";
    }

    /// A command string (no quoting needed) that appends the marker.
    std::string cmd(const std::string &marker) const {
      return (_dir / "m.sh").string() + " " + marker;
    }

    std::vector<std::string> lines() const {
      std::vector<std::string> out;
      std::ifstream in(log_path());
      std::string line;
      while (std::getline(in, line)) {
        out.push_back(line);
      }
      return out;
    }

    /// Wait (bounded) until at least n lines exist.
    bool wait_lines(std::size_t n) const {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
      while (std::chrono::steady_clock::now() < deadline) {
        if (lines().size() >= n) {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return lines().size() >= n;
    }

  private:
    void write_script(const std::string &name, const std::string &body) const {
      const auto path = _dir / name;
      {
        std::ofstream out(path);
        out << body;
      }
      fs::permissions(path, fs::perms::owner_all);
    }

    fs::path _dir;
  };

  class ProcessAdoptionTest: public BaseTest {
  protected:
    void TearDown() override {
      for (const pid_t pid : children) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
      }
      children.clear();
      BaseTest::TearDown();
    }

    /// Start /bin/sleep as the leader of its own process group; returns its pid (= pgid).
    pid_t spawn_group_leader() {
      const pid_t pid = fork();
      if (pid == 0) {
        setpgid(0, 0);
        execl("/bin/sleep", "sleep", "30", static_cast<char *>(nullptr));
        _exit(127);
      }
      if (pid > 0) {
        setpgid(pid, pid);
        children.push_back(pid);
      }
      return pid;
    }

    /// An app with three prep commands; undo markers U1..U3.
    proc::ctx_t make_prep_app(const marker_dir_t &md, const std::string &id = "1", const std::string &name = "Game") const {
      auto app = make_app(
        id,
        name,
        {make_cmd(md.cmd("D1"), md.cmd("U1")), make_cmd(md.cmd("D2"), md.cmd("U2")), make_cmd(md.cmd("D3"), md.cmd("U3"))}
      );
      app.working_dir = md.dir().string();
      return app;
    }

    std::vector<pid_t> children;
  };
}  // namespace

TEST_F(ProcessAdoptionTest, RealProbeSeesLiveGroup) {
  const pid_t pgid = spawn_group_leader();
  ASSERT_GT(pgid, 1);
  proc::proc_t proc;
  proc.set_running_for_test(make_app("1", "a"), 1, true, pgid, 0);
  std::string why;
  EXPECT_FALSE(proc.adoptable(&why)) << "the production probe must report a live process group";
  EXPECT_EQ(why, "the running app still has processes in its process group");
}

TEST_F(ProcessAdoptionTest, RealProbeCountsZombieAsPresent) {
  const pid_t pgid = spawn_group_leader();
  ASSERT_GT(pgid, 1);
  proc::proc_t proc;
  proc.set_running_for_test(make_app("1", "a"), 1, true, pgid, 0);
  kill(pgid, SIGKILL);
  // wait until the child is a zombie (state change) without reaping it
  siginfo_t info {};
  ASSERT_EQ(waitid(P_PID, static_cast<id_t>(pgid), &info, WEXITED | WNOWAIT), 0);
  EXPECT_FALSE(proc.adoptable(nullptr)) << "a zombie still counts as present (kill(-pgid, 0) succeeds)";
}

TEST_F(ProcessAdoptionTest, RealProbeSeesGroupGoneAfterReap) {
  const pid_t pgid = spawn_group_leader();
  ASSERT_GT(pgid, 1);
  proc::proc_t proc;
  proc.set_running_for_test(make_app("1", "a"), 1, true, pgid, 0);
  ASSERT_FALSE(proc.adoptable(nullptr));
  kill(-pgid, SIGKILL);
  int status = 0;
  ASSERT_EQ(waitpid(pgid, &status, 0), pgid);
  children.clear();
  std::string why = "untouched";
  EXPECT_TRUE(proc.adoptable(&why)) << "ESRCH means no process: adoptable";
  EXPECT_EQ(why, "untouched");
}

TEST_F(ProcessAdoptionTest, RealProbeCountsEpermAsPresent) {
  // Look for a process group we may not signal (EPERM). Only group ids are probed, nothing is read about processes.
  pid_t foreign = 0;
  for (pid_t candidate = 2; candidate < 200000; ++candidate) {
    errno = 0;
    if (kill(-candidate, 0) == -1 && errno == EPERM) {
      foreign = candidate;
      break;
    }
  }
  if (foreign == 0) {
    GTEST_SKIP() << "no process group owned by another user is visible here (running as root or in a container)";
  }
  proc::proc_t proc;
  proc.set_running_for_test(make_app("1", "a"), 1, true, foreign, 0);
  EXPECT_FALSE(proc.adoptable(nullptr)) << "EPERM must count as a present process (refuse safely)";
}

TEST_F(ProcessAdoptionTest, ReleasedTerminateRunsNoUndoAndKeepsGroup) {
  marker_dir_t md;
  const pid_t pgid = spawn_group_leader();
  ASSERT_GT(pgid, 1);
  proc::proc_t proc;
  proc.set_running_for_test(make_prep_app(md), 1, true, pgid, 3);
  proc.set_released(true);
  ASSERT_TRUE(proc.released());
  proc.terminate();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(md.lines().empty()) << "a released app must run no undo command at terminate";
  EXPECT_EQ(kill(-pgid, 0), 0) << "a released app's process group must not be killed by terminate()";
  EXPECT_FALSE(proc.released()) << "terminate() of a released app must clear the released flag";
  EXPECT_FALSE(proc.snapshot().has_value()) << "terminate() must clear the app state (no app recorded afterwards)";
}

TEST_F(ProcessAdoptionTest, ReleasedTerminateTwiceRunsNothing) {
  marker_dir_t md;
  proc::proc_t proc;
  proc.set_running_for_test(make_prep_app(md), 1, true, 0, 2);
  proc.set_released(true);
  proc.terminate();
  proc.terminate();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_TRUE(md.lines().empty()) << "no undo may run for a released app, also on a second terminate";
}

TEST_F(ProcessAdoptionTest, UnreleasedTerminateRunsAppliedUndoInReverse) {
  marker_dir_t md;
  proc::proc_t proc;
  proc.set_running_for_test(make_prep_app(md), 1, true, 0, 2);
  proc.terminate();
  ASSERT_TRUE(md.wait_lines(2)) << "terminate() of an app that is not released must run the applied undo commands";
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_EQ(md.lines(), (std::vector<std::string> {"U2 ", "U1 "})) << "only applied commands undo, in reverse order";
}

TEST_F(ProcessAdoptionTest, DropReleasedRunsNoUndo) {
  marker_dir_t md;
  proc::proc_t proc;
  proc.set_running_for_test(make_prep_app(md), 1, true, 0, 3);
  proc.set_released(true);
  ASSERT_TRUE(proc.drop_released_app());
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_TRUE(md.lines().empty()) << "drop_released_app() must run no undo command";
  EXPECT_FALSE(proc.snapshot().has_value()) << "the app state must be cleared by the drop";
}

TEST_F(ProcessAdoptionTest, DropReleasedKeepsProcessGroupAlive) {
  const pid_t pgid = spawn_group_leader();
  ASSERT_GT(pgid, 1);
  proc::proc_t proc;
  proc.set_running_for_test(make_app("1", "a"), 1, true, pgid, 0);
  proc.set_released(true);
  ASSERT_TRUE(proc.drop_released_app());
  EXPECT_EQ(kill(-pgid, 0), 0) << "drop_released_app() must not terminate any process group";
}

TEST_F(ProcessAdoptionTest, DropNotReleasedKeepsSnapshot) {
  marker_dir_t md;
  proc::proc_t proc;
  proc.set_running_for_test(make_prep_app(md), 1, true, 0, 2);
  EXPECT_FALSE(proc.drop_released_app());
  EXPECT_TRUE(proc.snapshot().has_value()) << "a failed drop must leave the app in place";
}

TEST_F(ProcessAdoptionTest, SwapAppListDoesNotChangeUndoOfRunningApp) {
  marker_dir_t md;
  proc::proc_t proc(make_env({}), {make_prep_app(md)});
  proc.set_running_for_test(make_prep_app(md), 1, true, 0, 1);
  proc.swap_app_list({});
  proc.terminate();
  ASSERT_TRUE(md.wait_lines(1)) << "a running app keeps its own prep list when the app list is swapped";
  EXPECT_EQ(md.lines(), (std::vector<std::string> {"U1 "}));
}

TEST_F(ProcessAdoptionTest, SwapAppListChangesWhatAdoptionChecksAgainst) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t old_proc(make_env({{"K", "v"}}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  const auto snap = old_proc.snapshot();
  ASSERT_TRUE(snap.has_value());

  auto changed = app;
  changed.prep_cmds.pop_back();
  proc::proc_t new_proc(make_env({{"K", "v"}}), {changed});
  handoff::protocol::go_t go;
  go.app = snap;
  std::string reason;
  EXPECT_FALSE(new_proc.prepare_adoption(go, reason).has_value()) << "list with a changed prep list must not adopt";
  new_proc.swap_app_list({app});
  EXPECT_TRUE(new_proc.prepare_adoption(go, reason).has_value()) << "after swap_app_list the new list is used: " << reason;
}

TEST_F(ProcessAdoptionTest, DigestInputMapsAppFields) {
  marker_dir_t md;
  auto app = make_prep_app(md);
  app.output = "null";
  app.exit_timeout = std::chrono::seconds {7};
  const env_list_t apps_env {{"A", "1"}, {"B", "2"}, {"SUNSHINE_APP_ID", "1"}};
  const auto in = proc::digest_input(app, apps_env);
  ASSERT_EQ(in.prep.size(), 3u);
  EXPECT_EQ(in.prep[0].do_cmd, md.cmd("D1"));
  EXPECT_EQ(in.prep[0].undo_cmd, md.cmd("U1"));
  EXPECT_FALSE(in.prep[0].elevated);
  EXPECT_EQ(in.prep[2].undo_cmd, md.cmd("U3"));
  EXPECT_EQ(in.working_dir, app.working_dir);
  EXPECT_EQ(in.output, "null");
  EXPECT_EQ(in.exit_timeout_s, 7);
  const env_list_t expected_base {{"A", "1"}, {"B", "2"}};
  EXPECT_EQ(in.base_env, expected_base) << "base_env is the apps.json env section minus the launch and handoff names";
}

TEST_F(ProcessAdoptionTest, DigestInputKeepsElevatedFlag) {
  auto app = make_app("1", "a");
  app.prep_cmds.emplace_back(std::string("d"), std::string("u"), true);
  const auto in = proc::digest_input(app, {});
  ASSERT_EQ(in.prep.size(), 1u);
  EXPECT_TRUE(in.prep[0].elevated);
}

TEST_F(ProcessAdoptionTest, SnapshotIsEmptyWithoutApp) {
  proc::proc_t proc;
  EXPECT_FALSE(proc.snapshot().has_value()) << "no app recorded: snapshot must be nullopt";
}

TEST_F(ProcessAdoptionTest, SnapshotDescribesRunningApp) {
  marker_dir_t md;
  const auto app = make_prep_app(md, "4", "Big Game");
  proc::proc_t proc(make_env({{"KEEP", "1"}, {"SUNSHINE_UDP_BASE", "9"}}), {app});
  proc.set_running_for_test(app, 4, true, 0, 2);
  proc.set_env_for_test("SUNSHINE_CLIENT_WIDTH", "1920");
  proc.set_env_for_test("SUNSHINE_APP_ID", "4");
  proc.set_env_for_test("AAA_NOT_LAUNCH", "x");
  const auto snap = proc.snapshot();
  ASSERT_TRUE(snap.has_value());
  EXPECT_EQ(snap->app_id, 4);
  EXPECT_EQ(snap->name, "Big Game");
  EXPECT_EQ(snap->applied_prep, 2u);
  EXPECT_EQ(snap->digest.size(), 64u);
  EXPECT_TRUE(std::all_of(snap->digest.begin(), snap->digest.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  }))
    << "digest must be lowercase hex";
  const env_list_t expected_launch {{"SUNSHINE_APP_ID", "4"}, {"SUNSHINE_CLIENT_WIDTH", "1920"}};
  EXPECT_EQ(snap->launch_env, expected_launch) << "launch_env: only launch names, sorted by name";
  EXPECT_EQ(snap->digest, handoff::app_digest(proc::digest_input(app, proc.apps_env_section())))
    << "digest is app_digest(digest_input(app, the apps.json env section)); the process environment is not part of it";
}

TEST_F(ProcessAdoptionTest, SnapshotDigestIgnoresLaunchVariables) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t a(make_env({{"KEEP", "1"}}), {app});
  proc::proc_t b(make_env({{"KEEP", "1"}, {"SUNSHINE_CLIENT_FPS", "60"}, {"SUNSHINE_HANDOFF_FDS", "x"}}), {app});
  a.set_running_for_test(app, 1, true, 0, 1);
  b.set_running_for_test(app, 1, true, 0, 1);
  EXPECT_EQ(a.snapshot()->digest, b.snapshot()->digest) << "launch and handoff variables must not change the digest";
}

TEST_F(ProcessAdoptionTest, SnapshotDigestIgnoresProcessEnvironment) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t a(make_env({{"KEEP", "1"}}), {app});
  proc::proc_t b(make_env({{"KEEP", "2"}}), {app});
  a.set_running_for_test(app, 1, true, 0, 1);
  b.set_running_for_test(app, 1, true, 0, 1);
  EXPECT_EQ(a.snapshot()->digest, b.snapshot()->digest) << "a differing process environment value must not change the digest (#47)";
}

TEST_F(ProcessAdoptionTest, RoundTripAdoptsAndTerminateRunsRemainingUndoOnce) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t old_proc(make_env({{"KEEP", "1"}}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 2);
  old_proc.set_env_for_test("SUNSHINE_CLIENT_WIDTH", "1920");
  ASSERT_TRUE(old_proc.adoptable(nullptr));
  const auto snap = old_proc.snapshot();
  ASSERT_TRUE(snap.has_value());

  proc::proc_t new_proc(make_env({{"KEEP", "1"}}), {app});
  handoff::protocol::go_t go;
  go.app = snap;
  std::string reason = "stale";
  auto plan = new_proc.prepare_adoption(go, reason);
  ASSERT_TRUE(plan.has_value()) << "matching id, name and digest must adopt: " << reason;
  EXPECT_TRUE(reason.empty()) << "reason is cleared on success";
  EXPECT_TRUE(plan->has_app);
  EXPECT_EQ(plan->app_id, 1);
  EXPECT_EQ(plan->applied_prep, 2u);
  EXPECT_EQ(plan->launch_env, snap->launch_env);
  EXPECT_EQ(plan->app.name, app.name);
  EXPECT_FALSE(new_proc.snapshot().has_value()) << "prepare_adoption() must change nothing";
  EXPECT_EQ(new_proc.running(), 0) << "prepare_adoption() must install nothing";

  new_proc.install_adoption(std::move(*plan));
  EXPECT_EQ(new_proc.running(), 1) << "after install the adopted app is running";
  EXPECT_EQ(new_proc.get_last_run_app_name(), app.name);
  EXPECT_FALSE(new_proc.released());
  EXPECT_TRUE(new_proc.adoptable(nullptr)) << "an adopted app is a placebo app with no process group";
  ASSERT_TRUE(new_proc.snapshot().has_value());
  EXPECT_EQ(*new_proc.snapshot(), *snap) << "the adopted object must describe the same app as the old one (id, name, applied count, digest, launch env)";
  EXPECT_TRUE(md.lines().empty()) << "adoption must run no command";

  new_proc.terminate();
  ASSERT_TRUE(md.wait_lines(2)) << "terminate after adoption must run the applied undo commands";
  new_proc.terminate();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(md.lines(), (std::vector<std::string> {"U2 1920", "U1 1920"}))
    << "each applied undo runs once, in reverse, with the received launch environment";
}

TEST_F(ProcessAdoptionTest, AppliedZeroAdoptsAndRunsNoUndo) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 0);
  proc::proc_t new_proc(make_env({}), {app});
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  std::string reason;
  auto plan = new_proc.prepare_adoption(go, reason);
  ASSERT_TRUE(plan.has_value()) << reason;
  new_proc.install_adoption(std::move(*plan));
  new_proc.terminate();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(md.lines().empty()) << "no applied command: no undo";
}

TEST_F(ProcessAdoptionTest, AppliedAllRunsAllUndo) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 3);
  proc::proc_t new_proc(make_env({}), {app});
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  std::string reason;
  auto plan = new_proc.prepare_adoption(go, reason);
  ASSERT_TRUE(plan.has_value()) << reason;
  new_proc.install_adoption(std::move(*plan));
  new_proc.terminate();
  ASSERT_TRUE(md.wait_lines(3));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(md.lines(), (std::vector<std::string> {"U3 ", "U2 ", "U1 "}));
}

TEST_F(ProcessAdoptionTest, NoAppInGoGivesEmptyPlanAndInstallDoesNothing) {
  marker_dir_t md;
  proc::proc_t proc(make_env({}), {make_prep_app(md)});
  handoff::protocol::go_t go;
  std::string reason = "stale";
  auto plan = proc.prepare_adoption(go, reason);
  ASSERT_TRUE(plan.has_value()) << "GO without an app is a success";
  EXPECT_FALSE(plan->has_app);
  EXPECT_TRUE(reason.empty());
  proc.install_adoption(std::move(*plan));
  EXPECT_EQ(proc.running(), 0) << "installing an empty plan changes nothing";
  EXPECT_FALSE(proc.snapshot().has_value());
}

TEST_F(ProcessAdoptionTest, NoAppInGoSucceedsEvenWhenAnAppRuns) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t proc(make_env({}), {app});
  proc.set_running_for_test(app, 1, true, 0, 1);
  handoff::protocol::go_t go;
  std::string reason;
  auto plan = proc.prepare_adoption(go, reason);
  ASSERT_TRUE(plan.has_value()) << "an empty go.app is checked first and always succeeds";
  EXPECT_FALSE(plan->has_app);
  proc.install_adoption(std::move(*plan));
  EXPECT_EQ(proc.running(), 1) << "an empty plan must not disturb the running app";
}

TEST_F(ProcessAdoptionTest, AlreadyRunningAppIsRefused) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  proc::proc_t new_proc(make_env({}), {app});
  new_proc.set_running_for_test(app, 1, true, 0, 0);
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  std::string reason;
  EXPECT_FALSE(new_proc.prepare_adoption(go, reason).has_value());
  EXPECT_EQ(reason, "this process already runs an app");
}

TEST_F(ProcessAdoptionTest, AlreadyRunningWinsOverInvalidSnapshot) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t new_proc(make_env({}), {app});
  new_proc.set_running_for_test(app, 1, true, 0, 0);
  handoff::protocol::go_t go;
  go.app = handoff::protocol::app_snapshot_t {1, app.name, 0, "", {}};
  std::string reason;
  EXPECT_FALSE(new_proc.prepare_adoption(go, reason).has_value());
  EXPECT_EQ(reason, "this process already runs an app") << "first failing rule wins";
}

TEST_F(ProcessAdoptionTest, EmptyDigestIsInvalid) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t proc(make_env({}), {app});
  handoff::protocol::go_t go;
  go.app = handoff::protocol::app_snapshot_t {1, app.name, 0, "", {}};
  std::string reason;
  EXPECT_FALSE(proc.prepare_adoption(go, reason).has_value());
  EXPECT_EQ(reason, "invalid app snapshot");
}

TEST_F(ProcessAdoptionTest, EmptyDigestWinsOverMissingApp) {
  proc::proc_t proc(make_env({}), {});
  handoff::protocol::go_t go;
  go.app = handoff::protocol::app_snapshot_t {9, "nobody", 0, "", {}};
  std::string reason;
  EXPECT_FALSE(proc.prepare_adoption(go, reason).has_value());
  EXPECT_EQ(reason, "invalid app snapshot") << "invalid snapshot is checked before the app lookup";
}

namespace {
  constexpr const char *changed_reason = "app or prep commands changed while an app runs; cancel the app first";
}

TEST_F(ProcessAdoptionTest, MissingAppIsRefused) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  proc::proc_t new_proc(make_env({}), {});
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  std::string reason;
  EXPECT_FALSE(new_proc.prepare_adoption(go, reason).has_value());
  EXPECT_EQ(reason, changed_reason);
}

TEST_F(ProcessAdoptionTest, OtherIdIsRefused) {
  marker_dir_t md;
  const auto app = make_prep_app(md, "1");
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  proc::proc_t new_proc(make_env({}), {make_prep_app(md, "2")});
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  std::string reason;
  EXPECT_FALSE(new_proc.prepare_adoption(go, reason).has_value());
  EXPECT_EQ(reason, changed_reason) << "the id is compared as std::to_string(app_id)";
}

TEST_F(ProcessAdoptionTest, OtherNameIsRefused) {
  marker_dir_t md;
  const auto app = make_prep_app(md, "1", "Game");
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  proc::proc_t new_proc(make_env({}), {make_prep_app(md, "1", "Renamed")});
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  std::string reason;
  EXPECT_FALSE(new_proc.prepare_adoption(go, reason).has_value());
  EXPECT_EQ(reason, changed_reason);
}

TEST_F(ProcessAdoptionTest, ChangedPrepCommandIsRefused) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  auto other = app;
  other.prep_cmds[1] = make_cmd(md.cmd("D2"), md.cmd("X2"));
  proc::proc_t new_proc(make_env({}), {other});
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  std::string reason;
  EXPECT_FALSE(new_proc.prepare_adoption(go, reason).has_value()) << "a changed undo command changes the digest";
  EXPECT_EQ(reason, changed_reason);
}

TEST_F(ProcessAdoptionTest, ChangedWorkingDirOutputTimeoutAreRefused) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();

  auto wd = app;
  wd.working_dir += "/sub";
  auto out = app;
  out.output = "somefile";
  auto to = app;
  to.exit_timeout = std::chrono::seconds {99};
  for (const auto &changed : {wd, out, to}) {
    proc::proc_t new_proc(make_env({}), {changed});
    std::string reason;
    EXPECT_FALSE(new_proc.prepare_adoption(go, reason).has_value()) << "working-dir, output and exit-timeout are part of the digest";
    EXPECT_EQ(reason, changed_reason);
  }
}

TEST_F(ProcessAdoptionTest, ChangedEnvironmentIsRefused) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t old_proc(make_env({{"KEEP", "1"}}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  proc::proc_t new_proc(make_env({{"KEEP", "other"}}), {app});
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  std::string reason;
  EXPECT_FALSE(new_proc.prepare_adoption(go, reason).has_value());
  EXPECT_EQ(reason, changed_reason);
}

TEST_F(ProcessAdoptionTest, WrongDigestIsRefused) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  proc::proc_t new_proc(make_env({}), {app});
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  go.app->digest = std::string(64, '0');
  std::string reason;
  EXPECT_FALSE(new_proc.prepare_adoption(go, reason).has_value());
  EXPECT_EQ(reason, changed_reason);
}

TEST_F(ProcessAdoptionTest, AppliedCountLargerThanPrepListIsRefused) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  proc::proc_t new_proc(make_env({}), {app});
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  go.app->applied_prep = 4;
  std::string reason;
  EXPECT_FALSE(new_proc.prepare_adoption(go, reason).has_value()) << "applied count 4 > 3 prep commands";
  EXPECT_EQ(reason, changed_reason);
  go.app->applied_prep = 3;
  EXPECT_TRUE(new_proc.prepare_adoption(go, reason).has_value()) << "applied count equal to the list size is valid: " << reason;
}

TEST_F(ProcessAdoptionTest, FailedPrepareChangesNothing) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t new_proc(make_env({}), {});
  handoff::protocol::go_t go;
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  go.app = old_proc.snapshot();
  std::string reason;
  EXPECT_FALSE(new_proc.prepare_adoption(go, reason).has_value());
  EXPECT_EQ(new_proc.running(), 0);
  EXPECT_FALSE(new_proc.snapshot().has_value());
}

TEST_F(ProcessAdoptionTest, InstallCreatesOutputFileForAppending) {
  marker_dir_t md;
  auto app = make_prep_app(md);
  app.output = (md.dir() / "app_output.log").string();
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  proc::proc_t new_proc(make_env({}), {app});
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  std::string reason;
  auto plan = new_proc.prepare_adoption(go, reason);
  ASSERT_TRUE(plan.has_value()) << reason;
  EXPECT_FALSE(fs::exists(app.output)) << "prepare_adoption() must not open the output file";
  new_proc.install_adoption(std::move(*plan));
  EXPECT_TRUE(fs::exists(app.output)) << "install_adoption() must reopen the app's output file for appending";
}

TEST_F(ProcessAdoptionTest, InstallKeepsExistingOutputContent) {
  marker_dir_t md;
  auto app = make_prep_app(md);
  app.output = (md.dir() / "app_output.log").string();
  {
    std::ofstream out(app.output);
    out << "old content\n";
  }
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  proc::proc_t new_proc(make_env({}), {app});
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  std::string reason;
  auto plan = new_proc.prepare_adoption(go, reason);
  ASSERT_TRUE(plan.has_value()) << reason;
  new_proc.install_adoption(std::move(*plan));
  std::ifstream in(app.output);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "old content\n") << "the output file is opened for appending, not truncated";
}

TEST_F(ProcessAdoptionTest, InstallWithUnopenableOutputContinues) {
  marker_dir_t md;
  auto app = make_prep_app(md);
  app.output = (md.dir() / "no_such_dir" / "app_output.log").string();
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 1);
  proc::proc_t new_proc(make_env({}), {app});
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  std::string reason;
  auto plan = new_proc.prepare_adoption(go, reason);
  ASSERT_TRUE(plan.has_value()) << reason;
  new_proc.install_adoption(std::move(*plan));
  EXPECT_EQ(new_proc.running(), 1) << "an output file that cannot be opened logs a Warning and the adoption continues";
}

TEST_F(ProcessAdoptionTest, ReleasedAppAfterAdoptionRunsNoUndo) {
  marker_dir_t md;
  const auto app = make_prep_app(md);
  proc::proc_t old_proc(make_env({}), {app});
  old_proc.set_running_for_test(app, 1, true, 0, 3);
  proc::proc_t new_proc(make_env({}), {app});
  handoff::protocol::go_t go;
  go.app = old_proc.snapshot();
  std::string reason;
  auto plan = new_proc.prepare_adoption(go, reason);
  ASSERT_TRUE(plan.has_value()) << reason;
  new_proc.install_adoption(std::move(*plan));
  new_proc.set_released(true);
  new_proc.terminate();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(md.lines().empty()) << "a re-released adopted app runs no undo at terminate";
}

TEST_F(ProcessAdoptionTest, AfterDropRunningIsZeroAndLaterTerminateRunsNoUndo) {
  marker_dir_t md;
  proc::proc_t proc;
  proc.set_running_for_test(make_prep_app(md), 1, true, 0, 3);
  proc.set_released(true);
  ASSERT_TRUE(proc.drop_released_app());
  EXPECT_EQ(proc.running(), 0) << "after drop_released_app() running() must return 0, never -1";
  EXPECT_EQ(proc.running(), 0) << "a second running() must still return 0";
  EXPECT_FALSE(proc.released()) << "drop_released_app() must clear the released flag";
  EXPECT_FALSE(proc.snapshot().has_value()) << "no app may be recorded after the drop (app id is -1)";
  proc.terminate();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(md.lines().empty()) << "a terminate() after drop_released_app() must run no undo command";
  EXPECT_EQ(proc.running(), 0);
}

TEST_F(ProcessAdoptionTest, AfterReleasedTerminateRunningIsZeroAndNothingRunsAgain) {
  marker_dir_t md;
  proc::proc_t proc;
  proc.set_running_for_test(make_prep_app(md), 1, true, 0, 3);
  proc.set_released(true);
  proc.terminate();
  EXPECT_FALSE(proc.released()) << "released terminate() must clear the released flag";
  EXPECT_FALSE(proc.snapshot().has_value()) << "no app may be recorded after the released terminate";
  EXPECT_EQ(proc.running(), 0) << "after the released terminate() running() must return 0, never -1";
  EXPECT_EQ(proc.running(), 0) << "a second running() must still return 0";
  proc.terminate();
  EXPECT_EQ(proc.running(), 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(md.lines().empty()) << "no undo command may run for the released terminate, running() or a second terminate()";
}

// ---------------------------------------------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------------------------------------------

TEST_F(ProcessAdoptionTest, ReadersSurviveMoveAssignment) {
  proc::proc_t a;
  proc::proc_t b;
  a.set_running_for_test(make_app("1", "one"), 1, true, 0, 0);
  b.set_running_for_test(make_app("2", "two"), 2, true, 0, 0);

  std::atomic<bool> stop {false};
  std::atomic<bool> bad {false};
  std::atomic<long> reads {0};
  std::thread reader([&] {
    while (!stop.load()) {
      const int id = a.running();
      if (id != 1 && id != 2) {
        bad = true;
      }
      if (!a.adoptable(nullptr, [](pid_t) {
            return false;
          })) {
        bad = true;
      }
      ++reads;
    }
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  for (int i = 0; i < 2000 && std::chrono::steady_clock::now() < deadline; ++i) {
    a = std::move(b);
    b.set_running_for_test(make_app(i % 2 ? "1" : "2", "x"), i % 2 ? 1 : 2, true, 0, 0);
    std::this_thread::yield();
  }
  stop = true;
  reader.join();
  EXPECT_FALSE(bad.load()) << "readers of running()/adoptable() saw a torn or impossible value while the object was move-assigned";
  EXPECT_GT(reads.load(), 0);
  const int final_id = a.running();
  EXPECT_TRUE(final_id == 1 || final_id == 2);
}

TEST_F(ProcessAdoptionTest, MoveConstructedObjectKeepsAtomics) {
  proc::proc_t a;
  a.set_running_for_test(make_app("3", "three"), 3, true, 0, 0);
  a.set_released(true);
  proc::proc_t b(std::move(a));
  EXPECT_EQ(b.running(), 3) << "move construction must copy the app id";
  EXPECT_TRUE(b.released()) << "move construction must copy the released flag";
  EXPECT_TRUE(b.adoptable(nullptr));
}

// Contract: running() during the drop returns the old id or 0, never -1 (the app id itself is not observable).
TEST_F(ProcessAdoptionTest, RunningDuringDropSeesOldIdOrZero) {
  std::atomic<bool> bad {false};
  for (int i = 0; i < 300; ++i) {
    proc::proc_t proc;
    proc.set_running_for_test(make_app("1", "a"), 1, true, 0, 0);
    proc.set_released(true);
    std::atomic<bool> go {false};
    std::atomic<bool> done {false};
    std::thread reader([&] {
      while (!go.load()) {
        std::this_thread::yield();
      }
      while (!done.load()) {
        const int id = proc.running();
        if (id != 0 && id != 1) {
          bad = true;
        }
      }
    });
    go = true;
    EXPECT_TRUE(proc.drop_released_app());
    done = true;
    reader.join();
  }
  EXPECT_FALSE(bad.load()) << "running() during drop_released_app() must return the old id or 0, never -1 or another value";
}

#endif  // __linux__
