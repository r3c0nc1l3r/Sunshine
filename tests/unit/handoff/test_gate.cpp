/**
 * @file tests/unit/handoff/test_gate.cpp
 * @brief Tests for the 503 gate and in-flight counter (src/handoff/gate.h).
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <cstddef>
  #include <memory>
  #include <thread>
  #include <utility>
  #include <vector>

  // local includes
  #include "src/handoff/gate.h"

using namespace std::chrono_literals;

namespace {
  using clock_type = std::chrono::steady_clock;
}  // namespace

TEST(HandoffGateTest, StartsLoweredAndIdle) {
  handoff::gate_t gate;
  EXPECT_FALSE(gate.raised());
  EXPECT_EQ(0u, gate.in_flight());
}

TEST(HandoffGateTest, EnterWhenLoweredAdmitsAndCounts) {
  handoff::gate_t gate;
  auto g = gate.enter();
  EXPECT_TRUE(g.admitted());
  EXPECT_EQ(1u, gate.in_flight());
  auto g2 = gate.enter();
  EXPECT_TRUE(g2.admitted());
  EXPECT_EQ(2u, gate.in_flight());
}

TEST(HandoffGateTest, EnterWhenRaisedRefusesAndDoesNotCount) {
  handoff::gate_t gate;
  gate.raise();
  EXPECT_TRUE(gate.raised());
  {
    auto g = gate.enter();
    EXPECT_FALSE(g.admitted()) << "a raised gate must refuse";
    EXPECT_EQ(0u, gate.in_flight()) << "a refused entry must not stay counted";
  }
  EXPECT_EQ(0u, gate.in_flight()) << "the destructor of a refused guard must not decrement (counter underflow)";
}

TEST(HandoffGateTest, GuardDestructorDecrementsOnce) {
  handoff::gate_t gate;
  {
    auto g = gate.enter();
    EXPECT_EQ(1u, gate.in_flight());
  }
  EXPECT_EQ(0u, gate.in_flight());
}

TEST(HandoffGateTest, DefaultGuardIsNotAdmittedAndDoesNotDecrement) {
  handoff::gate_t gate;
  auto held = gate.enter();
  {
    handoff::gate_t::guard_t empty;
    EXPECT_FALSE(empty.admitted());
  }
  EXPECT_EQ(1u, gate.in_flight()) << "an empty guard must not decrement";
}

TEST(HandoffGateTest, MovedGuardDecrementsOnlyOnce) {
  handoff::gate_t gate;
  {
    auto g1 = gate.enter();
    {
      handoff::gate_t::guard_t g2(std::move(g1));
      EXPECT_TRUE(g2.admitted());
      EXPECT_EQ(1u, gate.in_flight());
    }
    EXPECT_EQ(0u, gate.in_flight()) << "the destination of a move releases the entry";
  }
  EXPECT_EQ(0u, gate.in_flight()) << "the moved-from guard must not decrement again";
}

TEST(HandoffGateTest, MoveAssignReleasesTheCurrentEntryFirst) {
  handoff::gate_t gate;
  auto g1 = gate.enter();
  auto g2 = gate.enter();
  ASSERT_EQ(2u, gate.in_flight());
  g1 = std::move(g2);
  EXPECT_EQ(1u, gate.in_flight()) << "move assignment must release the entry the target held";
  EXPECT_TRUE(g1.admitted());
  g1 = handoff::gate_t::guard_t {};
  EXPECT_EQ(0u, gate.in_flight()) << "assigning an empty guard releases the entry";
  EXPECT_FALSE(g1.admitted());
}

TEST(HandoffGateTest, RaiseDoesNotAffectAdmittedGuards) {
  handoff::gate_t gate;
  auto g = gate.enter();
  gate.raise();
  EXPECT_TRUE(g.admitted()) << "a guard admitted before the raise stays admitted";
  EXPECT_EQ(1u, gate.in_flight());
}

TEST(HandoffGateTest, LowerAdmitsAgain) {
  handoff::gate_t gate;
  gate.raise();
  EXPECT_FALSE(gate.enter().admitted());
  gate.lower();
  EXPECT_FALSE(gate.raised());
  auto g = gate.enter();
  EXPECT_TRUE(g.admitted());
}

TEST(HandoffGateTest, RaiseAndLowerAreIdempotent) {
  handoff::gate_t gate;
  gate.raise();
  gate.raise();
  EXPECT_TRUE(gate.raised());
  gate.lower();
  gate.lower();
  EXPECT_FALSE(gate.raised());
}

TEST(HandoffGateTest, WaitIdleTrueAtZero) {
  handoff::gate_t gate;
  auto start = clock_type::now();
  EXPECT_TRUE(gate.wait_idle(500ms));
  EXPECT_LT(clock_type::now() - start, 400ms) << "wait_idle with nothing in flight must return at once";
  EXPECT_TRUE(gate.wait_idle(0ms));
}

TEST(HandoffGateTest, WaitIdleFalseOnTimeout) {
  handoff::gate_t gate;
  auto g = gate.enter();
  auto start = clock_type::now();
  EXPECT_FALSE(gate.wait_idle(60ms));
  auto elapsed = clock_type::now() - start;
  EXPECT_GE(elapsed, 45ms) << "wait_idle returned before its timeout";
  EXPECT_LT(elapsed, 1000ms);
}

TEST(HandoffGateTest, WaitIdleTrueWhenLastGuardEndsDuringWait) {
  handoff::gate_t gate;
  auto g1 = std::make_unique<handoff::gate_t::guard_t>(gate.enter());
  auto g2 = std::make_unique<handoff::gate_t::guard_t>(gate.enter());
  std::thread releaser([&] {
    std::this_thread::sleep_for(30ms);
    g1.reset();
    std::this_thread::sleep_for(30ms);
    g2.reset();
  });
  auto start = clock_type::now();
  bool idle = gate.wait_idle(2000ms);
  auto elapsed = clock_type::now() - start;
  releaser.join();
  EXPECT_TRUE(idle) << "wait_idle must see the counter reach zero";
  EXPECT_LT(elapsed, 1500ms) << "wait_idle must wake when the last guard ends, not at its timeout";
  EXPECT_EQ(0u, gate.in_flight());
}

TEST(HandoffGateTest, WaitIdleWithNegativeTimeoutWaitsForZero) {
  handoff::gate_t gate;
  auto g = std::make_unique<handoff::gate_t::guard_t>(gate.enter());
  std::thread releaser([&] {
    std::this_thread::sleep_for(50ms);
    g.reset();
  });
  bool idle = gate.wait_idle(-1ms);
  releaser.join();
  EXPECT_TRUE(idle) << "a negative timeout waits without limit";
}

TEST(HandoffGateTest, WaitIdleDoesNotWaitForRefusedEntries) {
  handoff::gate_t gate;
  gate.raise();
  auto refused = gate.enter();
  EXPECT_TRUE(gate.wait_idle(100ms)) << "refused entries are not in flight";
}

TEST(HandoffGateTest, RouteGateReturnsTheSameInstance) {
  EXPECT_EQ(&handoff::route_gate(), &handoff::route_gate());
}

TEST(HandoffGateTest, InFlightIsZeroAfterWaitIdleWhenNobodyEnters) {
  handoff::gate_t gate;
  auto g = std::make_unique<handoff::gate_t::guard_t>(gate.enter());
  gate.raise();
  std::thread releaser([&] {
    std::this_thread::sleep_for(30ms);
    g.reset();
  });
  bool idle = gate.wait_idle(2000ms);
  releaser.join();
  ASSERT_TRUE(idle);
  EXPECT_EQ(0u, gate.in_flight()) << "with no thread calling enter(), in_flight() is 0 after wait_idle() returned true";
}

TEST(HandoffGateTest, StressRaisedGateAdmitsNobodyAfterIdle) {
  handoff::gate_t gate;
  constexpr int thread_count = 8;
  std::atomic<bool> stop {false};
  std::atomic<long> admitted_total {0};  // Bumped while a guard is held.
  std::atomic<int> active {0};  // Guards alive, decremented before the guard dies.
  std::atomic<long> refused_total {0};

  std::vector<std::thread> threads;
  for (int i = 0; i < thread_count; ++i) {
    threads.emplace_back([&] {
      while (!stop.load()) {
        auto g = gate.enter();
        if (g.admitted()) {
          ++active;
          ++admitted_total;
          std::this_thread::yield();
          --active;
        } else {
          ++refused_total;
          std::this_thread::yield();
        }
      }
    });
  }

  // Let admitted traffic start.
  auto deadline = clock_type::now() + 1s;
  while (admitted_total.load() < 100 && clock_type::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_GE(admitted_total.load(), 100) << "the threads were never admitted by a lowered gate";

  int failures = 0;
  for (int round = 0; round < 15; ++round) {
    gate.raise();
    bool idle = gate.wait_idle(2000ms);
    if (!idle) {
      ++failures;
      ADD_FAILURE() << "wait_idle timed out in round " << round << " (in flight " << gate.in_flight() << ")";
      gate.lower();
      continue;
    }
    // The gate is raised and nothing is counted: nobody may hold a guard, and nobody may be admitted until lower().
    long before = admitted_total.load();
    EXPECT_EQ(0, active.load()) << "round " << round << ": an admitted guard is still alive after wait_idle() returned true";
    std::this_thread::sleep_for(15ms);
    EXPECT_EQ(before, admitted_total.load()) << "round " << round << ": a request was admitted while the gate was raised and idle";
    EXPECT_EQ(0, active.load());
    gate.lower();
    auto resume_deadline = clock_type::now() + 1s;
    while (admitted_total.load() == before && clock_type::now() < resume_deadline) {
      std::this_thread::sleep_for(1ms);
    }
    EXPECT_GT(admitted_total.load(), before) << "round " << round << ": lower() did not let requests in again";
  }
  stop = true;
  for (auto &t : threads) {
    t.join();
  }
  EXPECT_GT(refused_total.load(), 0) << "no request was ever refused while the gate was raised";
  EXPECT_EQ(0u, gate.in_flight());
  EXPECT_EQ(0, failures);
}

#endif  // __linux__
