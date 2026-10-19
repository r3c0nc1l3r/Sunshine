/**
 * @file tests/unit/handoff/test_refusal.cpp
 * @brief Blind tests for the refusal rules and their input gatherers (src/handoff/refusal.*), written from the contract
 *        in `refusal.h` and from `docs/drain-handoff.md` section 1.
 *
 * The tests use only the documented seams: the `refusal_sources_t` fakes, the injected runner of
 * `gather_systemd_version()` and temporary instance directories. They never call the default systemd runner, so the
 * process-wide version cache is never touched.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <cstddef>
  #include <cstdint>
  #include <cstdlib>
  #include <filesystem>
  #include <fstream>
  #include <functional>
  #include <map>
  #include <optional>
  #include <string>
  #include <string_view>
  #include <sys/types.h>
  #include <system_error>
  #include <unistd.h>
  #include <vector>

  // local includes
  #include "src/handoff/instance_file.h"
  #include "src/handoff/refusal.h"

namespace fs = std::filesystem;
namespace hf = handoff;
using steady_tp = std::chrono::steady_clock::time_point;

namespace {
  constexpr pid_t kSelfPid = 4242;  ///< Pid of "this process" in the fakes.
  constexpr pid_t kOtherPid = 5555;  ///< Pid of another live process.

  /**
   * @brief Inputs in which every check passes. The systemd checks (`NOTIFY_SOCKET`, version, unit) are set explicitly.
   *
   * @return Inputs that `check_refusal()` allows.
   */
  hf::refusal_inputs_t clear_inputs() {
    hf::refusal_inputs_t in;
    in.notify_socket_set = true;
    in.systemd_version = hf::minimum_systemd_version;
    in.unit_found = true;
    return in;
  }

  /**
   * @brief Refusal conditions, in the documented precedence order of `check_refusal()`.
   */
  enum class cond_e {
    kill_pending,
    in_progress,
    drain,
    cooldown,
    no_notify,
    systemd_old,
    no_unit,
    app,
    pairing,
    pending_launch,
    count,
  };

  constexpr int kCondCount = static_cast<int>(cond_e::count);

  /**
   * @brief Set the inputs that make one condition apply.
   *
   * @param in Inputs to change.
   * @param cond Condition to make apply.
   */
  void apply(hf::refusal_inputs_t &in, cond_e cond) {
    switch (cond) {
      case cond_e::kill_pending:
        in.kill_pending = true;
        break;
      case cond_e::in_progress:
        in.in_progress = true;
        break;
      case cond_e::drain:
        in.drain_instance_exists = true;
        break;
      case cond_e::cooldown:
        in.cooldown_active = true;
        break;
      case cond_e::no_notify:
        in.notify_socket_set = false;
        break;
      case cond_e::systemd_old:
        in.systemd_version = 253;
        break;
      case cond_e::no_unit:
        in.unit_found = false;
        break;
      case cond_e::app:
        in.app_adoptable = false;
        break;
      case cond_e::pairing:
        in.pairing_count = 1;
        break;
      case cond_e::pending_launch:
        in.pending_launch = true;
        break;
      case cond_e::count:
        break;
    }
  }

  /**
   * @brief The exact reason string that a condition alone must produce (with an empty `app_reason` for `app`).
   *
   * @param cond Condition.
   * @return Expected reason.
   */
  std::string reason_for(cond_e cond) {
    switch (cond) {
      case cond_e::kill_pending:
        return std::string(hf::refusal_reason::kill_pending);
      case cond_e::in_progress:
        return std::string(hf::refusal_reason::in_progress);
      case cond_e::drain:
        return std::string(hf::refusal_reason::drain_exists);
      case cond_e::cooldown:
        return std::string(hf::refusal_reason::cooldown);
      case cond_e::no_notify:
        return std::string(hf::refusal_reason::no_notify_socket);
      case cond_e::systemd_old:
        return std::string(hf::refusal_reason::systemd_old);
      case cond_e::no_unit:
        return std::string(hf::refusal_reason::no_unit);
      case cond_e::app:
        return std::string(hf::refusal_reason::app_generic);
      case cond_e::pairing:
        return std::string(hf::refusal_reason::pairing);
      case cond_e::pending_launch:
        return std::string(hf::refusal_reason::pending_launch);
      case cond_e::count:
        break;
    }
    return {};
  }

  /**
   * @brief Values behind the fake sources, plus a count of how often each source was called.
   */
  struct world_t {
    std::string notify {"/run/systemd/notify"};  ///< `NOTIFY_SOCKET` value.
    std::optional<int> version {hf::minimum_systemd_version};  ///< Systemd major version.
    std::optional<std::string> cgroup {"0::/system.slice/sunshine.service\n"};  ///< `/proc/self/cgroup` content.
    bool in_progress {false};  ///< Handoff started by this process.
    bool peer_exists {false};  ///< Draining peer exists.
    hf::scan_result_t scan;  ///< Instance-file scan.
    bool adoptable {true};  ///< Answer of the app check.
    std::string why;  ///< Reason written by the app check when not adoptable.
    std::size_t pairing {0};  ///< Pairing session count.
    bool pending_launch {false};  ///< Raised launch not yet streaming.
    pid_t self {kSelfPid};  ///< Pid of this process.
    steady_tp now {std::chrono::seconds(1000)};  ///< Current time.
    std::map<std::string, int> calls;  ///< Calls per source name.
  };

  /**
   * @brief Build sources that read from a `world_t`. The world must outlive the sources.
   *
   * @param w World.
   * @return Sources, every member set.
   */
  hf::refusal_sources_t make_sources(world_t &w) {
    hf::refusal_sources_t s;
    s.notify_socket = [&w] {
      ++w.calls["notify_socket"];
      return w.notify;
    };
    s.systemd_version = [&w] {
      ++w.calls["systemd_version"];
      return w.version;
    };
    s.cgroup = [&w] {
      ++w.calls["cgroup"];
      return w.cgroup;
    };
    s.handoff_in_progress = [&w] {
      ++w.calls["handoff_in_progress"];
      return w.in_progress;
    };
    s.peer_exists = [&w] {
      ++w.calls["peer_exists"];
      return w.peer_exists;
    };
    s.scan = [&w] {
      ++w.calls["scan"];
      return w.scan;
    };
    s.app_adoptable = [&w](std::string &why) {
      ++w.calls["app_adoptable"];
      if (!w.adoptable) {
        why = w.why;
      }
      return w.adoptable;
    };
    s.pairing_count = [&w] {
      ++w.calls["pairing_count"];
      return w.pairing;
    };
    s.pending_launch = [&w] {
      ++w.calls["pending_launch"];
      return w.pending_launch;
    };
    s.self_pid = [&w] {
      ++w.calls["self_pid"];
      return w.self;
    };
    s.now = [&w] {
      ++w.calls["now"];
      return w.now;
    };
    return s;
  }

  /**
   * @brief Build one instance value.
   *
   * @param pid Pid.
   * @param start Start time.
   * @param role Role.
   * @param state Handoff state.
   * @return Instance.
   */
  hf::instance_t make_instance(pid_t pid, std::uint64_t start, hf::role_e role, hf::state_e state) {
    hf::instance_t inst;
    inst.pid = pid;
    inst.start_time = start;
    inst.role = role;
    inst.udp_base = 47989;
    inst.sessions = 0;
    inst.handoff = state;
    return inst;
  }

  /**
   * @brief Write one instance file under the name the writer uses (`<pid>.json`).
   *
   * @param dir Instances directory.
   * @param inst Content.
   */
  void write_instance_file(const fs::path &dir, const hf::instance_t &inst) {
    std::ofstream out(dir / (std::to_string(inst.pid) + ".json"));
    out << hf::to_json(inst);
  }

  /**
   * @brief Scan a directory with a fake process table. Pids in `alive` are live with the mapped start time.
   *
   * @param dir Instances directory.
   * @param alive Pid to start time of the live processes.
   * @return Scan result.
   */
  hf::scan_result_t scan_with(const fs::path &dir, const std::map<pid_t, std::uint64_t> &alive) {
    return hf::scan_instances(dir, [&alive](pid_t pid) -> std::optional<std::uint64_t> {
      auto it = alive.find(pid);
      if (it == alive.end()) {
        return std::nullopt;
      }
      return it->second;
    });
  }

  /**
   * @brief Inputs in which only the application check fails, with the given `app_reason`.
   *
   * @param reason Application reason.
   * @return Inputs.
   */
  hf::refusal_inputs_t app_inputs(std::string reason) {
    hf::refusal_inputs_t in = clear_inputs();
    in.app_adoptable = false;
    in.app_reason = std::move(reason);
    return in;
  }
}  // namespace

/**
 * @brief Fixture with a fresh temporary root and an empty `instances` directory for every test.
 */
class HandoffRefusalTest: public testing::Test {
protected:
  void SetUp() override {
    std::string templ = (fs::temp_directory_path() / "hf_refusal_XXXXXX").string();
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    char *made = mkdtemp(buf.data());
    ASSERT_NE(made, nullptr) << "could not create a temporary root directory";
    root = made;
    instances = root / "instances";
    std::error_code ec;
    fs::create_directories(instances, ec);
    ASSERT_FALSE(ec) << "could not create the instances directory";
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root, ec);
    root.clear();
    instances.clear();
  }

  fs::path root;  ///< Temporary root, removed in TearDown.
  fs::path instances;  ///< `<root>/instances`.
};

// ---------------------------------------------------------------------------------------------------------------------
// check_refusal: singles, clear, precedence
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffRefusalTest, ClearInputsAllowTheHandoff) {
  EXPECT_EQ(hf::check_refusal(clear_inputs()), "") << "all checks pass, so the refusal must be an empty string";
}

TEST_F(HandoffRefusalTest, DefaultsWithPassingSystemdChecksAreAllowed) {
  // The defaults pass every non-systemd check; with the systemd checks (NOTIFY_SOCKET, version, unit) passed, allowed.
  hf::refusal_inputs_t in;
  in.notify_socket_set = true;
  in.systemd_version = hf::minimum_systemd_version;
  in.unit_found = true;
  EXPECT_EQ(hf::check_refusal(in), "") << "defaults with passing systemd checks must be allowed";
}

TEST_F(HandoffRefusalTest, EachConditionAloneGivesItsExactReason) {
  for (int i = 0; i < kCondCount; ++i) {
    auto cond = static_cast<cond_e>(i);
    SCOPED_TRACE("condition index " + std::to_string(i));
    hf::refusal_inputs_t in = clear_inputs();
    apply(in, cond);
    EXPECT_EQ(hf::check_refusal(in), reason_for(cond))
      << "a single applicable input must give exactly its refusal_reason string";
  }
}

TEST_F(HandoffRefusalTest, EachReasonStringIsOneLine) {
  const std::string_view all[] {
    hf::refusal_reason::kill_pending,
    hf::refusal_reason::in_progress,
    hf::refusal_reason::drain_exists,
    hf::refusal_reason::cooldown,
    hf::refusal_reason::no_notify_socket,
    hf::refusal_reason::systemd_unknown,
    hf::refusal_reason::systemd_old,
    hf::refusal_reason::no_unit,
    hf::refusal_reason::app_generic,
    hf::refusal_reason::app_prefix,
    hf::refusal_reason::pairing,
    hf::refusal_reason::pending_launch,
  };
  for (auto text : all) {
    EXPECT_EQ(text.find('\n'), std::string_view::npos) << "reason must have no newline: " << text;
    EXPECT_EQ(text.find('\r'), std::string_view::npos) << "reason must have no carriage return: " << text;
    EXPECT_FALSE(text.empty()) << "reason must not be empty";
  }
}

TEST_F(HandoffRefusalTest, SystemdUnknownAloneGivesUnknownReason) {
  hf::refusal_inputs_t in = clear_inputs();
  in.systemd_version = std::nullopt;
  EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::systemd_unknown))
    << "a systemd version that could not be read must give the unknown reason";
}

TEST_F(HandoffRefusalTest, SystemdVersionBoundaryAt254) {
  struct row_t {
    int version;
    std::string expected;
    const char *name;
  };

  const std::vector<row_t> rows {
    {253, std::string(hf::refusal_reason::systemd_old), "253 is too old"},
    {254, "", "254 passes"},
    {255, "", "255 passes"},
    {256, "", "256 passes"},
  };
  for (const auto &row : rows) {
    SCOPED_TRACE(row.name);
    hf::refusal_inputs_t in = clear_inputs();
    in.systemd_version = row.version;
    EXPECT_EQ(hf::check_refusal(in), row.expected) << "systemd version " << row.version << " gave the wrong refusal";
  }
  EXPECT_EQ(hf::minimum_systemd_version, 254) << "the minimum systemd version is 254";
}

TEST_F(HandoffRefusalTest, PairingCountZeroPassesAndAnyCountRefuses) {
  for (std::size_t count : {std::size_t {0}, std::size_t {1}, std::size_t {2}, std::size_t {7}}) {
    SCOPED_TRACE("pairing count " + std::to_string(count));
    hf::refusal_inputs_t in = clear_inputs();
    in.pairing_count = count;
    if (count == 0) {
      EXPECT_EQ(hf::check_refusal(in), "") << "no pairing in progress must pass";
    } else {
      EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::pairing))
        << "a pairing in progress must refuse";
    }
  }
}

TEST_F(HandoffRefusalTest, AppReasonIsIgnoredWhileTheAppIsAdoptable) {
  hf::refusal_inputs_t in = clear_inputs();
  in.app_reason = "this text belongs to a refusal that does not apply";
  EXPECT_EQ(hf::check_refusal(in), "") << "app_reason only matters when app_adoptable is false";
}

TEST_F(HandoffRefusalTest, EmptyAppReasonGivesTheGenericAppReason) {
  EXPECT_EQ(hf::check_refusal(app_inputs("")), std::string(hf::refusal_reason::app_generic))
    << "a non-adoptable app with an empty reason must give the generic reason";
}

TEST_F(HandoffRefusalTest, AppReasonIsPrefixedWhenNotEmpty) {
  EXPECT_EQ(hf::check_refusal(app_inputs("auto-detach group still has processes")), std::string(hf::refusal_reason::app_prefix) + "auto-detach group still has processes")
    << "a non-empty app reason must follow the app prefix";
}

TEST_F(HandoffRefusalTest, AppReasonCarriageReturnAndLineFeedBecomeSpaces) {
  EXPECT_EQ(hf::check_refusal(app_inputs("one\r\ntwo\nthree\rfour")), std::string(hf::refusal_reason::app_prefix) + "one  two three four")
    << "every CR and LF in the app reason must become one space";
}

TEST_F(HandoffRefusalTest, AppReasonResultIsOneLine) {
  const std::string result = hf::check_refusal(app_inputs("a\nb\rc\r\nd"));
  EXPECT_EQ(result.find('\n'), std::string::npos) << "the refusal must be one line";
  EXPECT_EQ(result.find('\r'), std::string::npos) << "the refusal must be one line";
}

TEST_F(HandoffRefusalTest, AppReasonIsTruncatedToTwoHundredBytes) {
  const std::string prefix(hf::refusal_reason::app_prefix);
  EXPECT_EQ(hf::check_refusal(app_inputs(std::string(300, 'x'))), prefix + std::string(200, 'x'))
    << "a reason longer than 200 bytes must be cut to 200 bytes";
}

TEST_F(HandoffRefusalTest, AppReasonAtOrBelowTwoHundredBytesIsKeptWhole) {
  const std::string prefix(hf::refusal_reason::app_prefix);
  EXPECT_EQ(hf::check_refusal(app_inputs(std::string(199, 'y'))), prefix + std::string(199, 'y'))
    << "199 bytes must be kept whole";
  EXPECT_EQ(hf::check_refusal(app_inputs(std::string(200, 'z'))), prefix + std::string(200, 'z'))
    << "exactly 200 bytes must be kept whole";
  EXPECT_EQ(hf::check_refusal(app_inputs(std::string(201, 'w'))), prefix + std::string(200, 'w'))
    << "201 bytes must be cut to 200";
}

TEST_F(HandoffRefusalTest, EmptyAfterCleaningFallsBackToGenericReason) {
  // An empty reason is the only input that is empty after cleaning: the generic reason is used.
  EXPECT_EQ(hf::check_refusal(app_inputs("")), std::string(hf::refusal_reason::app_generic))
    << "an empty result after cleaning must give the generic reason";
}

// SPEC-QUESTION: the contract says the sanitised text is cleaned by replacing each CR and LF with a space. It does not
// say whether whitespace-only text counts as "empty after cleaning" (it would fall back to the generic reason), or is
// kept as spaces. This test follows the literal rule (kept as spaces); the lead decides which one is meant.
TEST_F(HandoffRefusalTest, WhitespaceOnlyAppReasonKeepsItsSpacesLiterally) {
  EXPECT_EQ(hf::check_refusal(app_inputs("\r\n")), std::string(hf::refusal_reason::app_prefix) + "  ")
    << "a CR LF pair must become two spaces after the prefix";
}

TEST_F(HandoffRefusalTest, KillPendingAlwaysWinsOverEveryOtherRefusal) {
  hf::refusal_inputs_t in;  // every other refusal applies, including the unknown systemd version
  in.kill_pending = true;
  in.in_progress = true;
  in.drain_instance_exists = true;
  in.cooldown_active = true;
  in.notify_socket_set = false;
  in.systemd_version = std::nullopt;
  in.unit_found = false;
  in.app_adoptable = false;
  in.app_reason = "something else is also wrong";
  in.pairing_count = 3;
  in.pending_launch = true;
  EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::kill_pending))
    << "kill_pending must be the reason shown when every other refusal also applies";
}

TEST_F(HandoffRefusalTest, FirstApplicableRowWinsWhenPairsApply) {
  for (int i = 0; i < kCondCount; ++i) {
    for (int j = i + 1; j < kCondCount; ++j) {
      SCOPED_TRACE("pair " + std::to_string(i) + " and " + std::to_string(j));
      hf::refusal_inputs_t in = clear_inputs();
      apply(in, static_cast<cond_e>(i));
      apply(in, static_cast<cond_e>(j));
      EXPECT_EQ(hf::check_refusal(in), reason_for(static_cast<cond_e>(i)))
        << "the earlier row of the precedence list must win over the later one";
    }
  }
}

TEST_F(HandoffRefusalTest, PrecedenceChainGivesTheTopApplicableRow) {
  // Starting from the top condition, remove conditions one by one: each step must report the new top one.
  for (int first = 0; first < kCondCount; ++first) {
    SCOPED_TRACE("first applicable condition index " + std::to_string(first));
    hf::refusal_inputs_t in = clear_inputs();
    for (int c = first; c < kCondCount; ++c) {
      apply(in, static_cast<cond_e>(c));
    }
    EXPECT_EQ(hf::check_refusal(in), reason_for(static_cast<cond_e>(first)))
      << "with every condition from this row down applying, the row itself must be the reason";
  }
}

TEST_F(HandoffRefusalTest, ContractPrecedenceOrderIsKillInProgressDrainCooldownNotifyVersionUnitAppPairingLaunch) {
  // Explicit list, so the precedence order is checked as documented in refusal.h.
  const std::vector<cond_e> order {
    cond_e::kill_pending,
    cond_e::in_progress,
    cond_e::drain,
    cond_e::cooldown,
    cond_e::no_notify,
    cond_e::systemd_old,
    cond_e::no_unit,
    cond_e::app,
    cond_e::pairing,
    cond_e::pending_launch,
  };
  for (std::size_t i = 0; i + 1 < order.size(); ++i) {
    SCOPED_TRACE("row " + std::to_string(i));
    hf::refusal_inputs_t in = clear_inputs();
    apply(in, order[i]);
    apply(in, order[i + 1]);
    EXPECT_EQ(hf::check_refusal(in), reason_for(order[i]))
      << "the documented precedence order must hold for adjacent rows";
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// log_refusal (the header documents only the Warning line; no log capture seam exists)
// ---------------------------------------------------------------------------------------------------------------------

// SPEC-QUESTION: refusal.h has no seam to capture the log line, so these tests only check that logging a refusal is
// safe for a known pid, for pid 0 (unknown) and for an empty reason. The message text is not verified.
TEST_F(HandoffRefusalTest, LogRefusalAcceptsAKnownPid) {
  EXPECT_NO_THROW(hf::log_refusal(hf::refusal_reason::kill_pending, kOtherPid))
    << "logging a refusal for a known peer pid must not throw";
}

TEST_F(HandoffRefusalTest, LogRefusalAcceptsUnknownPidZero) {
  EXPECT_NO_THROW(hf::log_refusal(hf::refusal_reason::cooldown, 0))
    << "pid 0 (unknown peer) must not throw";
}

TEST_F(HandoffRefusalTest, LogRefusalAcceptsAnEmptyReason) {
  EXPECT_NO_THROW(hf::log_refusal(std::string_view {}, kOtherPid))
    << "an empty reason must not throw";
}

// ---------------------------------------------------------------------------------------------------------------------
// cooldown_active boundaries
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffRefusalTest, CooldownIsActiveStrictlyBeforeItsDeadline) {
  const steady_tp until = steady_tp(std::chrono::seconds(1000));
  EXPECT_TRUE(hf::cooldown_active(until, until - steady_tp::duration(1)))
    << "one nanosecond before the deadline the cooldown is active";
  EXPECT_TRUE(hf::cooldown_active(until, until - std::chrono::seconds(1)))
    << "one second before the deadline the cooldown is active";
}

TEST_F(HandoffRefusalTest, CooldownIsNotActiveAtTheDeadline) {
  const steady_tp until = steady_tp(std::chrono::seconds(1000));
  EXPECT_FALSE(hf::cooldown_active(until, until))
    << "at the deadline (now == until) the cooldown has passed";
}

TEST_F(HandoffRefusalTest, CooldownIsNotActiveAfterTheDeadline) {
  const steady_tp until = steady_tp(std::chrono::seconds(1000));
  EXPECT_FALSE(hf::cooldown_active(until, until + steady_tp::duration(1)))
    << "one nanosecond after the deadline the cooldown has passed";
  EXPECT_FALSE(hf::cooldown_active(until, until + std::chrono::hours(1)))
    << "long after the deadline the cooldown has passed";
}

TEST_F(HandoffRefusalTest, CooldownWithMinimumDeadlineMeansNoCooldown) {
  const steady_tp now = steady_tp(std::chrono::seconds(1000));
  EXPECT_FALSE(hf::cooldown_active(steady_tp::min(), now))
    << "time_point::min() means no cooldown at all";
}

// ---------------------------------------------------------------------------------------------------------------------
// gather_systemd_version with injected runners (never the default runner, never the cache)
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffRefusalTest, SystemdVersionParsesTypicalOutputs) {
  struct row_t {
    const char *output;
    std::optional<int> expected;
  };

  const std::vector<row_t> rows {
    {"systemd 253 (253.10-1)\n", 253},
    {"systemd 254 (254.5-1)\n+PAM +AUDIT +SELINUX\n", 254},
    {"systemd 255 (255.4-1-arch)\n+PAM +AUDIT\n", 255},
    {"systemd 256 (256.5-1-arch1)\n+PAM +AUDIT +SELINUX\n", 256},
  };
  for (const auto &row : rows) {
    SCOPED_TRACE(row.output);
    int calls = 0;
    auto result = hf::gather_systemd_version([&calls, &row] {
      ++calls;
      return std::optional<std::string>(std::string(row.output));
    });
    EXPECT_EQ(result, row.expected) << "typical systemctl --version output must parse to its major version";
    EXPECT_EQ(calls, 1) << "an injected runner must be called once per call";
  }
}

TEST_F(HandoffRefusalTest, SystemdVersionGivesNulloptForGarbage) {
  const std::vector<std::string> garbage {
    "",
    "command not found\n",
    "systemd\n",
    "systemd abc (x)\n",
    "hello world\n",
  };
  for (const auto &text : garbage) {
    SCOPED_TRACE("output: " + text);
    auto result = hf::gather_systemd_version([text] {
      return std::optional<std::string>(text);
    });
    EXPECT_EQ(result, std::nullopt) << "output that does not parse must give nullopt";
  }
}

TEST_F(HandoffRefusalTest, SystemdVersionGivesNulloptWhenTheRunnerFails) {
  int calls = 0;
  auto result = hf::gather_systemd_version([&calls] {
    ++calls;
    return std::optional<std::string> {};
  });
  EXPECT_EQ(result, std::nullopt) << "a failing runner (spawn failure, timeout or non-zero exit) must give nullopt";
  EXPECT_EQ(calls, 1) << "the failing runner must be called once";
}

TEST_F(HandoffRefusalTest, InjectedRunnersDoNotShareAnyCachedAnswer) {
  // An injected runner never reads or writes the cache: each call answers from its own runner, in any order.
  auto good253 = hf::gather_systemd_version([] {
    return std::optional<std::string>(std::string("systemd 253 (253.10-1)\n"));
  });
  auto failing = hf::gather_systemd_version([] {
    return std::optional<std::string> {};
  });
  auto good254 = hf::gather_systemd_version([] {
    return std::optional<std::string>(std::string("systemd 254 (254.5-1)\n"));
  });
  EXPECT_EQ(good253, 253) << "first injected answer";
  EXPECT_EQ(failing, std::nullopt) << "a failing injected runner must not return an earlier successful answer";
  EXPECT_EQ(good254, 254) << "a later injected answer must not return an earlier one";
}

// ---------------------------------------------------------------------------------------------------------------------
// classify_instances over temporary instance directories
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffRefusalTest, OtherLiveDrainingInstanceGivesDrainExists) {
  write_instance_file(instances, make_instance(kOtherPid, 100, hf::role_e::draining, hf::state_e::draining));
  auto scan = scan_with(instances, {{kOtherPid, 100}});
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_TRUE(view.drain_instance_exists) << "another live draining instance must give drain_instance_exists";
  EXPECT_FALSE(view.handoff_pending) << "a draining instance is not a pending handoff";
  EXPECT_FALSE(view.kill_pending) << "no kill_pending state was written";
}

TEST_F(HandoffRefusalTest, OtherLivePendingRoleGivesInProgress) {
  write_instance_file(instances, make_instance(kOtherPid, 100, hf::role_e::pending, hf::state_e::none));
  auto scan = scan_with(instances, {{kOtherPid, 100}});
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_TRUE(view.handoff_pending) << "another live pending role must give handoff_pending (in progress)";
  EXPECT_FALSE(view.drain_instance_exists) << "a pending instance is not a draining one";
  EXPECT_FALSE(view.kill_pending) << "no kill_pending state was written";
}

TEST_F(HandoffRefusalTest, OtherLiveActiveInstanceChangesNothing) {
  write_instance_file(instances, make_instance(kOtherPid, 100, hf::role_e::active, hf::state_e::none));
  auto scan = scan_with(instances, {{kOtherPid, 100}});
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_FALSE(view.handoff_pending) << "an active instance is not pending";
  EXPECT_FALSE(view.drain_instance_exists) << "an active instance is not draining";
  EXPECT_FALSE(view.kill_pending) << "an active instance without kill_pending does not set it";
}

TEST_F(HandoffRefusalTest, OwnPendingAndDrainingInstancesDoNotCountAsOthers) {
  write_instance_file(instances, make_instance(kSelfPid, 200, hf::role_e::pending, hf::state_e::none));
  auto scan = scan_with(instances, {{kSelfPid, 200}});
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_FALSE(view.handoff_pending) << "a live instance with pid == self_pid must not count as another pending handoff";

  fs::remove(instances / (std::to_string(kSelfPid) + ".json"));
  write_instance_file(instances, make_instance(kSelfPid, 200, hf::role_e::draining, hf::state_e::draining));
  scan = scan_with(instances, {{kSelfPid, 200}});
  view = hf::classify_instances(scan, kSelfPid);
  EXPECT_FALSE(view.drain_instance_exists) << "a live instance with pid == self_pid must not count as a draining instance";
}

TEST_F(HandoffRefusalTest, OwnKillPendingCountsForKillOnly) {
  write_instance_file(instances, make_instance(kSelfPid, 200, hf::role_e::active, hf::state_e::kill_pending));
  auto scan = scan_with(instances, {{kSelfPid, 200}});
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_TRUE(view.kill_pending) << "a live own instance with state kill_pending must set kill_pending";
  EXPECT_FALSE(view.handoff_pending) << "kill_pending of this process does not make a handoff pending";
  EXPECT_FALSE(view.drain_instance_exists) << "kill_pending of this process does not make a drain";
}

TEST_F(HandoffRefusalTest, OtherKillPendingCountsForKillOnly) {
  write_instance_file(instances, make_instance(kOtherPid, 100, hf::role_e::active, hf::state_e::kill_pending));
  auto scan = scan_with(instances, {{kOtherPid, 100}});
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_TRUE(view.kill_pending) << "another live instance with kill_pending must set kill_pending";
}

TEST_F(HandoffRefusalTest, StaleInstanceFilesAreIgnored) {
  // The process is gone: the file must be ignored, not counted as a draining instance.
  write_instance_file(instances, make_instance(kOtherPid, 100, hf::role_e::draining, hf::state_e::kill_pending));
  auto gone = scan_with(instances, {});
  auto view = hf::classify_instances(gone, kSelfPid);
  EXPECT_FALSE(view.drain_instance_exists) << "a file whose process is gone must not give drain_instance_exists";
  EXPECT_FALSE(view.kill_pending) << "a file whose process is gone must not give kill_pending";

  // The pid is reused by another process with a different start time: also stale.
  auto reused = scan_with(instances, {{kOtherPid, 999}});
  view = hf::classify_instances(reused, kSelfPid);
  EXPECT_FALSE(view.drain_instance_exists) << "a pid reused with another start time must be treated as stale";
  EXPECT_FALSE(view.kill_pending) << "a pid reused with another start time must be treated as stale";
}

TEST_F(HandoffRefusalTest, MissingDirectoryAloneChangesNothing) {
  auto scan = scan_with(root / "missing", {});
  EXPECT_TRUE(scan.dir_missing) << "precondition: the scan reports a missing directory";
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_FALSE(view.handoff_pending) << "dir_missing alone must not give a pending handoff";
  EXPECT_FALSE(view.drain_instance_exists) << "dir_missing alone must not give a draining instance";
  EXPECT_FALSE(view.kill_pending) << "dir_missing alone must not give kill_pending";
}

TEST_F(HandoffRefusalTest, DirectoryThatIsAFileFailsClosed) {
  const fs::path not_a_dir = root / "not_a_directory";
  std::ofstream(not_a_dir) << "x";
  auto scan = scan_with(not_a_dir, {});
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_TRUE(view.drain_instance_exists) << "an instance directory that cannot be listed must fail closed (drain exists)";
}

TEST_F(HandoffRefusalTest, CorruptInstanceFileFailsClosed) {
  std::ofstream(instances / (std::to_string(kOtherPid) + ".json")) << "{ this is not json";
  auto scan = scan_with(instances, {{kOtherPid, 100}});
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_TRUE(view.drain_instance_exists) << "an unreadable instance file must fail closed (drain exists)";
}

TEST_F(HandoffRefusalTest, FileNamedForAnotherPidFailsClosed) {
  // The file name says pid 5556 but the content says kOtherPid: counts as unreadable.
  std::ofstream(instances / (std::to_string(kOtherPid + 1) + ".json"))
    << hf::to_json(make_instance(kOtherPid, 100, hf::role_e::active, hf::state_e::none));
  auto scan = scan_with(instances, {{kOtherPid, 100}, {kOtherPid + 1, 101}});
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_TRUE(view.drain_instance_exists) << "a file whose name pid differs from its content must fail closed";
}

TEST_F(HandoffRefusalTest, NonJsonNamesAreIgnored) {
  std::ofstream(instances / "notes.txt") << "not an instance";
  auto scan = scan_with(instances, {});
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_FALSE(view.drain_instance_exists) << "files that are not *.json must be ignored";
  EXPECT_FALSE(view.handoff_pending) << "files that are not *.json must be ignored";
  EXPECT_FALSE(view.kill_pending) << "files that are not *.json must be ignored";
}

TEST_F(HandoffRefusalTest, PureClassifyFailsClosedOnUnreadableFlag) {
  hf::scan_result_t scan;
  scan.any_unreadable = true;
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_TRUE(view.drain_instance_exists) << "any_unreadable must set drain_instance_exists (fail closed)";
}

TEST_F(HandoffRefusalTest, PureClassifyOfAnEmptyScanGivesNothing) {
  hf::scan_result_t scan;
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_FALSE(view.handoff_pending) << "an empty scan has no pending handoff";
  EXPECT_FALSE(view.drain_instance_exists) << "an empty scan has no draining instance";
  EXPECT_FALSE(view.kill_pending) << "an empty scan has no kill_pending";
}

TEST_F(HandoffRefusalTest, PendingAndDrainingFromOthersAreBothReported) {
  write_instance_file(instances, make_instance(kOtherPid, 100, hf::role_e::pending, hf::state_e::none));
  write_instance_file(instances, make_instance(kOtherPid + 1, 101, hf::role_e::draining, hf::state_e::draining));
  auto scan = scan_with(instances, {{kOtherPid, 100}, {kOtherPid + 1, 101}});
  auto view = hf::classify_instances(scan, kSelfPid);
  EXPECT_TRUE(view.handoff_pending) << "the pending role of another process must be reported";
  EXPECT_TRUE(view.drain_instance_exists) << "the draining role of another process must be reported";
}

// ---------------------------------------------------------------------------------------------------------------------
// gather_refusal_inputs through fake sources
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffRefusalTest, GatherFromClearSourcesGivesClearInputs) {
  world_t w;
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_TRUE(in.notify_socket_set) << "a non-empty NOTIFY_SOCKET must set notify_socket_set";
  EXPECT_EQ(in.systemd_version, hf::minimum_systemd_version) << "the systemd version must be copied from the source";
  EXPECT_TRUE(in.unit_found) << "a service cgroup must give unit_found";
  EXPECT_FALSE(in.in_progress) << "no handoff in progress";
  EXPECT_FALSE(in.drain_instance_exists) << "no drain";
  EXPECT_FALSE(in.cooldown_active) << "no cooldown";
  EXPECT_FALSE(in.kill_pending) << "no kill_pending";
  EXPECT_TRUE(in.app_adoptable) << "the app is adoptable";
  EXPECT_TRUE(in.app_reason.empty()) << "app_reason must be empty when the app is adoptable";
  EXPECT_EQ(in.pairing_count, 0u) << "no pairing sessions";
  EXPECT_FALSE(in.pending_launch) << "no pending launch";
  EXPECT_EQ(hf::check_refusal(in), "") << "the gathered inputs of a clear world must be allowed";
}

TEST_F(HandoffRefusalTest, GatherCallsEverySourceExactlyOnce) {
  world_t w;
  auto sources = make_sources(w);
  (void) hf::gather_refusal_inputs(sources, steady_tp::min());
  for (const char *name : {"notify_socket", "systemd_version", "cgroup", "handoff_in_progress", "peer_exists", "scan", "app_adoptable", "pairing_count", "pending_launch", "self_pid", "now"}) {
    EXPECT_EQ(w.calls[name], 1) << "source " << name << " must be called exactly once per gather";
  }
}

TEST_F(HandoffRefusalTest, GatherReadsAnEmptyNotifySocketAsNotSet) {
  world_t w;
  w.notify = "";
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_FALSE(in.notify_socket_set) << "an empty NOTIFY_SOCKET must not set notify_socket_set";
  EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::no_notify_socket))
    << "an unset NOTIFY_SOCKET must refuse with the no-notify reason";
}

TEST_F(HandoffRefusalTest, GatherWithUnknownSystemdVersionRefusesAsUnknown) {
  world_t w;
  w.version = std::nullopt;
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_EQ(in.systemd_version, std::nullopt) << "a failed version read must stay nullopt";
  EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::systemd_unknown));
}

TEST_F(HandoffRefusalTest, GatherWithOldSystemdRefusesAsOld) {
  world_t w;
  w.version = 253;
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::systemd_old));
}

TEST_F(HandoffRefusalTest, GatherWithUnreadableCgroupHasNoUnit) {
  world_t w;
  w.cgroup = std::nullopt;
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_FALSE(in.unit_found) << "an unreadable /proc/self/cgroup must give unit_found false";
  EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::no_unit));
}

TEST_F(HandoffRefusalTest, GatherWithOnlyUserManagerCgroupHasNoUnit) {
  world_t w;
  w.cgroup = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-x.scope\n";
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_FALSE(in.unit_found) << "the user manager's own user@<uid>.service must not count as the unit";
}

TEST_F(HandoffRefusalTest, GatherInProgressFromTheHandoffFlag) {
  world_t w;
  w.in_progress = true;
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_TRUE(in.in_progress) << "a handoff started by this process must give in_progress";
  EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::in_progress));
}

TEST_F(HandoffRefusalTest, GatherInProgressFromAnotherPendingInstance) {
  world_t w;
  w.scan.live.push_back(make_instance(kOtherPid, 100, hf::role_e::pending, hf::state_e::none));
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_TRUE(in.in_progress) << "another live pending instance must give in_progress";
}

TEST_F(HandoffRefusalTest, GatherOwnPendingInstanceIsNotInProgress) {
  world_t w;
  w.scan.live.push_back(make_instance(kSelfPid, 200, hf::role_e::pending, hf::state_e::none));
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_FALSE(in.in_progress) << "this process's own pending file must not count as in progress";
}

TEST_F(HandoffRefusalTest, GatherDrainFromThePeerView) {
  world_t w;
  w.peer_exists = true;
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_TRUE(in.drain_instance_exists) << "an existing draining peer must give drain_instance_exists";
  EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::drain_exists));
}

TEST_F(HandoffRefusalTest, GatherDrainFromAnotherDrainingInstance) {
  world_t w;
  w.scan.live.push_back(make_instance(kOtherPid, 100, hf::role_e::draining, hf::state_e::draining));
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_TRUE(in.drain_instance_exists) << "another live draining instance must give drain_instance_exists";
}

TEST_F(HandoffRefusalTest, GatherDrainFailsClosedOnAnUnreadableScan) {
  world_t w;
  w.scan.any_unreadable = true;
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_TRUE(in.drain_instance_exists) << "an incomplete scan must fail closed (drain exists)";
}

TEST_F(HandoffRefusalTest, GatherCooldownFromTheDeadline) {
  world_t w;
  auto sources = make_sources(w);
  auto active = hf::gather_refusal_inputs(sources, w.now + std::chrono::seconds(1));
  EXPECT_TRUE(active.cooldown_active) << "a deadline in the future must give cooldown_active";

  auto passed = hf::gather_refusal_inputs(sources, w.now);
  EXPECT_FALSE(passed.cooldown_active) << "a deadline equal to now has passed";

  auto none = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_FALSE(none.cooldown_active) << "time_point::min() means no cooldown";
}

TEST_F(HandoffRefusalTest, GatherKillPendingFromOwnInstance) {
  world_t w;
  w.scan.live.push_back(make_instance(kSelfPid, 200, hf::role_e::active, hf::state_e::kill_pending));
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_TRUE(in.kill_pending) << "a live own instance with kill_pending must set kill_pending";
  EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::kill_pending));
}

TEST_F(HandoffRefusalTest, GatherKillPendingFromAnotherInstance) {
  world_t w;
  w.scan.live.push_back(make_instance(kOtherPid, 100, hf::role_e::active, hf::state_e::kill_pending));
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_TRUE(in.kill_pending) << "another live instance with kill_pending must set kill_pending";
}

TEST_F(HandoffRefusalTest, GatherIgnoresStaleEntriesAndMissingDirectory) {
  world_t w;
  w.scan.dir_missing = true;
  w.scan.stale.push_back(kOtherPid);
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_EQ(hf::check_refusal(in), "") << "a missing directory and stale pids alone must not refuse";
}

TEST_F(HandoffRefusalTest, GatherAppReasonIsTakenWhenNotAdoptable) {
  world_t w;
  w.adoptable = false;
  w.why = "auto-detach group still has processes";
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_FALSE(in.app_adoptable) << "a non-adoptable app must give app_adoptable false";
  EXPECT_EQ(in.app_reason, "auto-detach group still has processes") << "app_reason must be the answer's why";
  EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::app_prefix) + "auto-detach group still has processes");
}

TEST_F(HandoffRefusalTest, GatherAppWithEmptyReasonGivesGenericReason) {
  world_t w;
  w.adoptable = false;
  w.why = "";
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_TRUE(in.app_reason.empty()) << "an empty why must give an empty app_reason";
  EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::app_generic));
}

TEST_F(HandoffRefusalTest, GatherPairingCountFromTheSource) {
  world_t w;
  w.pairing = 2;
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_EQ(in.pairing_count, 2u) << "pairing_count must be copied from the source";
  EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::pairing));
}

TEST_F(HandoffRefusalTest, GatherPendingLaunchFromTheSource) {
  world_t w;
  w.pending_launch = true;
  auto sources = make_sources(w);
  auto in = hf::gather_refusal_inputs(sources, steady_tp::min());
  EXPECT_TRUE(in.pending_launch) << "a raised launch that has not started streaming must set pending_launch";
  EXPECT_EQ(hf::check_refusal(in), std::string(hf::refusal_reason::pending_launch));
}

TEST_F(HandoffRefusalTest, ProductionSourcesSetEveryMember) {
  // Only the presence of each function is checked: none is called, so no systemctl runs and no state is touched.
  auto sources = hf::production_refusal_sources();
  EXPECT_TRUE(static_cast<bool>(sources.notify_socket)) << "notify_socket must be set";
  EXPECT_TRUE(static_cast<bool>(sources.systemd_version)) << "systemd_version must be set";
  EXPECT_TRUE(static_cast<bool>(sources.cgroup)) << "cgroup must be set";
  EXPECT_TRUE(static_cast<bool>(sources.handoff_in_progress)) << "handoff_in_progress must be set";
  EXPECT_TRUE(static_cast<bool>(sources.peer_exists)) << "peer_exists must be set";
  EXPECT_TRUE(static_cast<bool>(sources.scan)) << "scan must be set";
  EXPECT_TRUE(static_cast<bool>(sources.app_adoptable)) << "app_adoptable must be set";
  EXPECT_TRUE(static_cast<bool>(sources.pairing_count)) << "pairing_count must be set";
  EXPECT_TRUE(static_cast<bool>(sources.pending_launch)) << "pending_launch must be set";
  EXPECT_TRUE(static_cast<bool>(sources.self_pid)) << "self_pid must be set";
  EXPECT_TRUE(static_cast<bool>(sources.now)) << "now must be set";
}

#endif
