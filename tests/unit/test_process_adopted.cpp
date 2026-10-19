/**
 * @file tests/unit/test_process_adopted.cpp
 * @brief Blind tests for `proc::proc_t::adopted()` and the `adopted` member of `proc::app_atomics_t` (src/process.h).
 *
 * Written from the doc comments of `src/process.h` only. `execute()` clearing the flag needs a real launch and is not
 * covered here (no documented seam starts an app without the platform).
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <string>
#include <utility>

// local includes
#include <src/process.h>

namespace {
  proc::ctx_t make_simple_app() {
    proc::ctx_t app {};
    app.id = "1";
    app.name = "Game";
    app.exit_timeout = std::chrono::seconds {5};
    return app;
  }
}  // namespace

TEST(ProcessAdoptedAtomicsTest, DefaultIsFalse) {
  proc::app_atomics_t a;
  EXPECT_FALSE(a.adopted.load()) << "A fresh app_atomics_t must have adopted == false.";
}

TEST(ProcessAdoptedAtomicsTest, MoveConstructCopiesAdopted) {
  proc::app_atomics_t a;
  a.adopted = true;
  proc::app_atomics_t b(std::move(a));
  EXPECT_TRUE(b.adopted.load()) << "The move constructor must copy the adopted flag.";
}

TEST(ProcessAdoptedAtomicsTest, MoveAssignCopiesAdoptedBothWays) {
  proc::app_atomics_t a;
  a.adopted = true;
  proc::app_atomics_t b;
  b = std::move(a);
  EXPECT_TRUE(b.adopted.load()) << "Move assignment must copy adopted = true.";

  proc::app_atomics_t c;
  c.adopted = false;
  proc::app_atomics_t d;
  d.adopted = true;
  d = std::move(c);
  EXPECT_FALSE(d.adopted.load()) << "Move assignment must overwrite adopted, also with false.";
}

TEST(ProcessAdoptedTest, FalseByDefault) {
  proc::proc_t proc;
  EXPECT_FALSE(proc.adopted()) << "With no app running adopted() is false.";
}

TEST(ProcessAdoptedTest, FalseForAPlaceboAppSetUpWithoutAdoption) {
  proc::proc_t proc;
  proc.set_running_for_test(make_simple_app(), 1, true, 0, 0);
  EXPECT_EQ(proc.running(), 1);
  EXPECT_FALSE(proc.adopted()) << "A placebo app with no process group that this process runs is not adopted.";
}

TEST(ProcessAdoptedTest, TerminateKeepsFalse) {
  proc::proc_t proc;
  proc.set_running_for_test(make_simple_app(), 1, true, 0, 0);
  proc.terminate();
  EXPECT_FALSE(proc.adopted());
}

#ifdef __linux__

namespace {
  /// Installs an adoption of the simple app into `target` (the object's own app list holds the app).
  bool adopt_into(proc::proc_t &target) {
    const auto app = make_simple_app();
    proc::proc_t source(boost::process::v1::environment {}, {app});
    source.set_running_for_test(app, 1, true, 0, 0);
    const auto snapshot = source.snapshot();
    if (!snapshot) {
      return false;
    }
    handoff::protocol::go_t go;
    go.app = snapshot;
    std::string reason;
    auto plan = target.prepare_adoption(go, reason);
    if (!plan) {
      return false;
    }
    target.install_adoption(std::move(*plan));
    return true;
  }

  proc::proc_t make_target() {
    return proc::proc_t(boost::process::v1::environment {}, {make_simple_app()});
  }
}  // namespace

TEST(ProcessAdoptedTest, PrepareAdoptionDoesNotSetTheFlag) {
  const auto app = make_simple_app();
  proc::proc_t source(boost::process::v1::environment {}, {app});
  source.set_running_for_test(app, 1, true, 0, 0);
  proc::proc_t target = make_target();
  handoff::protocol::go_t go;
  go.app = source.snapshot();
  std::string reason;
  auto plan = target.prepare_adoption(go, reason);
  ASSERT_TRUE(plan.has_value()) << reason;
  EXPECT_FALSE(target.adopted()) << "prepare_adoption installs nothing; adopted() stays false until install_adoption.";
}

TEST(ProcessAdoptedTest, InstallAdoptionSetsTheFlag) {
  proc::proc_t target = make_target();
  ASSERT_TRUE(adopt_into(target));
  EXPECT_EQ(target.running(), 1);
  EXPECT_TRUE(target.adopted()) << "install_adoption with an app must set adopted().";
}

TEST(ProcessAdoptedTest, InstallAdoptionWithoutAppLeavesFlagFalse) {
  proc::proc_t target = make_target();
  handoff::protocol::go_t go;  // no app
  std::string reason;
  auto plan = target.prepare_adoption(go, reason);
  ASSERT_TRUE(plan.has_value()) << reason;
  ASSERT_FALSE(plan->has_app);
  target.install_adoption(std::move(*plan));
  EXPECT_FALSE(target.adopted()) << "A GO without an app adopts nothing, so adopted() stays false.";
}

TEST(ProcessAdoptedTest, TerminateClearsTheFlag) {
  proc::proc_t target = make_target();
  ASSERT_TRUE(adopt_into(target));
  ASSERT_TRUE(target.adopted());
  target.terminate();
  EXPECT_FALSE(target.adopted()) << "terminate() must clear adopted().";
  EXPECT_EQ(target.running(), 0);
}

TEST(ProcessAdoptedTest, DropReleasedAppClearsTheFlag) {
  proc::proc_t target = make_target();
  ASSERT_TRUE(adopt_into(target));
  target.set_released(true);
  ASSERT_TRUE(target.drop_released_app());
  EXPECT_FALSE(target.adopted()) << "drop_released_app() must clear adopted().";
}

TEST(ProcessAdoptedTest, AdoptionAfterTerminateSetsTheFlagAgain) {
  proc::proc_t target = make_target();
  ASSERT_TRUE(adopt_into(target));
  target.terminate();
  ASSERT_FALSE(target.adopted());
  ASSERT_TRUE(adopt_into(target));
  EXPECT_TRUE(target.adopted()) << "A second adoption after terminate() must set the flag again.";
}

TEST(ProcessAdoptedTest, MoveConstructCopiesTheFlag) {
  proc::proc_t source = make_target();
  ASSERT_TRUE(adopt_into(source));
  proc::proc_t moved(std::move(source));
  EXPECT_TRUE(moved.adopted()) << "Moving a proc_t must copy the adopted flag (proc::refresh replaces the object by move).";
  EXPECT_EQ(moved.running(), 1);
}

TEST(ProcessAdoptedTest, MoveAssignCopiesTheFlagBothWays) {
  proc::proc_t adopted_proc = make_target();
  ASSERT_TRUE(adopt_into(adopted_proc));
  proc::proc_t destination;
  destination = std::move(adopted_proc);
  EXPECT_TRUE(destination.adopted()) << "Move assignment from an adopted object must give adopted() == true.";

  proc::proc_t plain;
  destination = std::move(plain);
  EXPECT_FALSE(destination.adopted()) << "Move assignment from a non-adopted object must overwrite the flag with false.";
}

#endif  // __linux__
