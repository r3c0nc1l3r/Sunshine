/**
 * @file tests/unit/handoff/test_end_all.cpp
 * @brief Tests for END_ALL against rollback and the terminate rule (src/handoff/end_all.h).
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <functional>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <stdexcept>
  #include <thread>
  #include <vector>

  // local includes
  #include "src/handoff/end_all.h"

using namespace std::chrono_literals;

namespace {
  using clock_type = std::chrono::steady_clock;
  using handoff::end_all_answer_e;
  using handoff::end_state_e;
  using handoff::rollback_app_e;

  /**
   * @brief Run a function on a detached thread; false when it did not finish in time. Everything it touches must be
   * owned by shared pointers, so a hang cannot leave dangling references.
   */
  bool finishes_within(std::function<void()> fn, std::chrono::milliseconds limit) {
    struct state_t {
      std::mutex m;
      std::condition_variable cv;
      bool done {false};
    };

    auto st = std::make_shared<state_t>();
    std::thread([st, fn = std::move(fn)] {
      fn();
      {
        std::lock_guard lock(st->m);
        st->done = true;
      }
      st->cv.notify_all();
    }).detach();
    std::unique_lock lock(st->m);
    return st->cv.wait_for(lock, limit, [&] {
      return st->done;
    });
  }

  /**
   * @brief An `end_and_join` that blocks until the test releases it.
   */
  struct blocker_t {
    std::mutex m;  ///< Guards the flags.
    std::condition_variable cv;  ///< Signals changes.
    bool started {false};  ///< The function was entered.
    bool released {false};  ///< The test released it.
    std::atomic<int> runs {0};  ///< Number of calls.

    void operator()() {
      ++runs;
      std::unique_lock lock(m);
      started = true;
      cv.notify_all();
      cv.wait_for(lock, 5s, [&] {
        return released;
      });
    }

    bool wait_started(std::chrono::milliseconds t) {
      std::unique_lock lock(m);
      return cv.wait_for(lock, t, [&] {
        return started;
      });
    }

    void release() {
      {
        std::lock_guard lock(m);
        released = true;
      }
      cv.notify_all();
    }
  };

  /**
   * @brief Wait for a state, bounded.
   */
  bool wait_state(handoff::end_all_machine_t &m, end_state_e s, std::chrono::milliseconds t) {
    auto deadline = clock_type::now() + t;
    while (clock_type::now() < deadline) {
      if (m.state() == s) {
        return true;
      }
      std::this_thread::sleep_for(2ms);
    }
    return m.state() == s;
  }
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// end_all_machine_t
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffEndAllTest, StartsIdle) {
  handoff::end_all_machine_t m;
  EXPECT_EQ(end_state_e::idle, m.state());
}

TEST(HandoffEndAllTest, EndAllInIdleRunsEndAndJoinOnceAndAnswersEnded) {
  handoff::end_all_machine_t m;
  int runs = 0;
  auto answer = m.handle_end_all([&] {
    ++runs;
  });
  EXPECT_EQ(end_all_answer_e::ended, answer);
  EXPECT_EQ(1, runs);
  EXPECT_EQ(end_state_e::ended, m.state());
}

TEST(HandoffEndAllTest, SecondEndAllInEndedAnswersEndedWithoutRunning) {
  handoff::end_all_machine_t m;
  int runs = 0;
  auto fn = [&] {
    ++runs;
  };
  ASSERT_EQ(end_all_answer_e::ended, m.handle_end_all(fn));
  EXPECT_EQ(end_all_answer_e::ended, m.handle_end_all(fn));
  EXPECT_EQ(1, runs) << "END_ALL in ended must be answered at once without running end_and_join again";
  EXPECT_EQ(end_state_e::ended, m.state());
}

TEST(HandoffEndAllTest, StateIsEndingWhileEndAndJoinRuns) {
  auto m = std::make_shared<handoff::end_all_machine_t>();
  auto b = std::make_shared<blocker_t>();
  std::thread t([m, b] {
    m->handle_end_all(std::ref(*b));
  });
  ASSERT_TRUE(b->wait_started(1000ms));
  EXPECT_EQ(end_state_e::ending, m->state());
  b->release();
  t.join();
  EXPECT_EQ(end_state_e::ended, m->state());
}

TEST(HandoffEndAllTest, EndAllWhileEndingWaitsAndGetsTheSameAnswer) {
  auto m = std::make_shared<handoff::end_all_machine_t>();
  auto b = std::make_shared<blocker_t>();
  auto second_runs = std::make_shared<std::atomic<int>>(0);
  std::atomic<int> first_answer {-1};
  std::atomic<int> second_answer {-1};
  std::atomic<bool> second_done {false};

  std::thread first([&, m, b] {
    first_answer = static_cast<int>(m->handle_end_all(std::ref(*b)));
  });
  ASSERT_TRUE(b->wait_started(1000ms));
  std::thread second([&, m, second_runs] {
    second_answer = static_cast<int>(m->handle_end_all([second_runs] {
      ++*second_runs;
    }));
    second_done = true;
  });
  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(second_done.load()) << "END_ALL while ending must wait for the running one";
  b->release();
  first.join();
  second.join();
  EXPECT_EQ(static_cast<int>(end_all_answer_e::ended), first_answer.load());
  EXPECT_EQ(static_cast<int>(end_all_answer_e::ended), second_answer.load()) << "the waiting END_ALL gets the same answer";
  EXPECT_EQ(1, b->runs.load());
  EXPECT_EQ(0, second_runs->load()) << "the waiting END_ALL must not run end_and_join itself";
  EXPECT_EQ(end_state_e::ended, m->state());
}

TEST(HandoffEndAllTest, RollbackInIdleTakesBackAndLaterEndAllIsRejected) {
  handoff::end_all_machine_t m;
  EXPECT_EQ(rollback_app_e::take_back, m.decide_rollback());
  EXPECT_EQ(end_state_e::rolling_back, m.state());
  int runs = 0;
  auto answer = m.handle_end_all([&] {
    ++runs;
  });
  EXPECT_EQ(end_all_answer_e::rejected, answer) << "END_ALL after a rollback decision is answered END_REJECTED";
  EXPECT_EQ(0, runs) << "END_ALL in rolling_back must not end any session";
  EXPECT_EQ(end_state_e::rolling_back, m.state());
}

TEST(HandoffEndAllTest, SecondRollbackAfterTakeBackTakesBackAgain) {
  handoff::end_all_machine_t m;
  ASSERT_EQ(rollback_app_e::take_back, m.decide_rollback());
  EXPECT_EQ(rollback_app_e::take_back, m.decide_rollback());
  EXPECT_EQ(end_state_e::rolling_back, m.state());
}

TEST(HandoffEndAllTest, RollbackInEndedDrops) {
  handoff::end_all_machine_t m;
  ASSERT_EQ(end_all_answer_e::ended, m.handle_end_all([] {
  }));
  EXPECT_EQ(rollback_app_e::drop, m.decide_rollback()) << "a rollback decided after ENDED drops the app";
  EXPECT_EQ(end_state_e::rolling_back, m.state());
}

TEST(HandoffEndAllTest, RollbackAgainAfterDropDrops) {
  handoff::end_all_machine_t m;
  ASSERT_EQ(end_all_answer_e::ended, m.handle_end_all([] {
  }));
  ASSERT_EQ(rollback_app_e::drop, m.decide_rollback());
  EXPECT_EQ(rollback_app_e::drop, m.decide_rollback());
}

TEST(HandoffEndAllTest, EndAllAfterRollbackFromEndedIsRejected) {
  handoff::end_all_machine_t m;
  ASSERT_EQ(end_all_answer_e::ended, m.handle_end_all([] {
  }));
  ASSERT_EQ(rollback_app_e::drop, m.decide_rollback());
  int runs = 0;
  EXPECT_EQ(end_all_answer_e::rejected, m.handle_end_all([&] {
    ++runs;
  }));
  EXPECT_EQ(0, runs);
}

TEST(HandoffEndAllTest, RollbackDuringEndingDropsAndRunningEndAllIsRejected) {
  auto m = std::make_shared<handoff::end_all_machine_t>();
  auto b = std::make_shared<blocker_t>();
  std::atomic<int> answer {-1};
  std::thread t([&, m, b] {
    answer = static_cast<int>(m->handle_end_all(std::ref(*b)));
  });
  ASSERT_TRUE(b->wait_started(1000ms));

  rollback_app_e decision = rollback_app_e::take_back;
  EXPECT_TRUE(finishes_within(
    [&decision, m] {
      decision = m->decide_rollback();
    },
    1000ms
  )) << "decide_rollback() blocked while end_and_join runs (the mutex must not be held)";
  EXPECT_EQ(rollback_app_e::drop, decision) << "a rollback decided in ending drops the app";
  EXPECT_EQ(end_state_e::rolling_back, m->state());

  b->release();
  t.join();
  EXPECT_EQ(static_cast<int>(end_all_answer_e::rejected), answer.load())
    << "the END_ALL that was running when the rollback was decided answers END_REJECTED";
  EXPECT_EQ(end_state_e::rolling_back, m->state()) << "the state stays rolling_back after the running END_ALL finishes";
}

TEST(HandoffEndAllTest, WaitingEndAllIsRejectedToo) {
  auto m = std::make_shared<handoff::end_all_machine_t>();
  auto b = std::make_shared<blocker_t>();
  std::atomic<int> first_answer {-1};
  std::atomic<int> second_answer {-1};
  std::thread first([&, m, b] {
    first_answer = static_cast<int>(m->handle_end_all(std::ref(*b)));
  });
  ASSERT_TRUE(b->wait_started(1000ms));
  std::thread second([&, m] {
    second_answer = static_cast<int>(m->handle_end_all([] {
    }));
  });
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(rollback_app_e::drop, m->decide_rollback());
  b->release();
  first.join();
  second.join();
  EXPECT_EQ(static_cast<int>(end_all_answer_e::rejected), first_answer.load());
  EXPECT_EQ(static_cast<int>(end_all_answer_e::rejected), second_answer.load())
    << "an END_ALL that waited for a round that ended in a rollback gets the same answer, END_REJECTED";
}

TEST(HandoffEndAllTest, WaitingEndAllKeepsTheRunnersAnswerEvenIfRollbackFollowsAtOnce) {
  for (int i = 0; i < 25; ++i) {
    auto m = std::make_shared<handoff::end_all_machine_t>();
    auto b = std::make_shared<blocker_t>();
    std::atomic<int> first_answer {-1};
    std::atomic<int> second_answer {-1};
    std::thread first([&, m, b] {
      first_answer = static_cast<int>(m->handle_end_all(std::ref(*b)));
    });
    ASSERT_TRUE(b->wait_started(1000ms));
    std::thread second([&, m] {
      second_answer = static_cast<int>(m->handle_end_all([] {
      }));
    });
    std::this_thread::sleep_for(20ms);
    b->release();
    first.join();
    m->decide_rollback();
    second.join();
    ASSERT_EQ(static_cast<int>(end_all_answer_e::ended), first_answer.load());
    ASSERT_EQ(static_cast<int>(end_all_answer_e::ended), second_answer.load())
      << "iteration " << i << ": a waiting END_ALL must get the runner's answer even if decide_rollback() follows at once";
  }
}

TEST(HandoffEndAllTest, ThrowingEndAndJoinReturnsToIdleAndNextEndAllRuns) {
  handoff::end_all_machine_t m;
  try {
    m.handle_end_all([] {
      throw std::runtime_error("boom");
    });
  } catch (...) {
    // Whether the exception propagates is not specified.
  }
  EXPECT_EQ(end_state_e::idle, m.state()) << "a throwing end_and_join must leave the state idle, not stuck in ending";
  int runs = 0;
  auto answer = m.handle_end_all([&] {
    ++runs;
  });
  EXPECT_EQ(1, runs) << "the next END_ALL must run end_and_join again";
  EXPECT_EQ(end_all_answer_e::ended, answer);
  EXPECT_EQ(end_state_e::ended, m.state());
}

TEST(HandoffEndAllTest, StateAndRollbackCanBeCalledInsideEndAndJoin) {
  auto m = std::make_shared<handoff::end_all_machine_t>();
  auto inner_state = std::make_shared<std::atomic<int>>(-1);
  auto inner_decision = std::make_shared<std::atomic<int>>(-1);
  auto answer = std::make_shared<std::atomic<int>>(-1);
  bool done = finishes_within(
    [m, inner_state, inner_decision, answer] {
      *answer = static_cast<int>(m->handle_end_all([&] {
        *inner_state = static_cast<int>(m->state());
        *inner_decision = static_cast<int>(m->decide_rollback());
      }));
    },
    2000ms
  );
  ASSERT_TRUE(done) << "state()/decide_rollback() inside end_and_join deadlocked: the mutex must not be held";
  EXPECT_EQ(static_cast<int>(end_state_e::ending), inner_state->load());
  EXPECT_EQ(static_cast<int>(rollback_app_e::drop), inner_decision->load());
  EXPECT_EQ(static_cast<int>(end_all_answer_e::rejected), answer->load());
  EXPECT_EQ(end_state_e::rolling_back, m->state());
}

TEST(HandoffEndAllTest, ResetReturnsToIdleFromEnded) {
  handoff::end_all_machine_t m;
  int runs = 0;
  auto fn = [&] {
    ++runs;
  };
  ASSERT_EQ(end_all_answer_e::ended, m.handle_end_all(fn));
  m.reset();
  EXPECT_EQ(end_state_e::idle, m.state());
  EXPECT_EQ(end_all_answer_e::ended, m.handle_end_all(fn));
  EXPECT_EQ(2, runs) << "after reset() a new END_ALL runs end_and_join again";
}

TEST(HandoffEndAllTest, ResetReturnsToIdleFromRollingBack) {
  handoff::end_all_machine_t m;
  ASSERT_EQ(rollback_app_e::take_back, m.decide_rollback());
  m.reset();
  EXPECT_EQ(end_state_e::idle, m.state());
  int runs = 0;
  EXPECT_EQ(end_all_answer_e::ended, m.handle_end_all([&] {
    ++runs;
  }));
  EXPECT_EQ(1, runs);
  m.reset();
  EXPECT_EQ(rollback_app_e::take_back, m.decide_rollback()) << "after reset() a rollback in idle takes the app back again";
}

TEST(HandoffEndAllTest, ResetClearsEarlierDropDecision) {
  handoff::end_all_machine_t m;
  ASSERT_EQ(end_all_answer_e::ended, m.handle_end_all([] {
  }));
  ASSERT_EQ(rollback_app_e::drop, m.decide_rollback());
  m.reset();
  EXPECT_EQ(rollback_app_e::take_back, m.decide_rollback()) << "a new handoff starts with a fresh decision";
}

// ---------------------------------------------------------------------------------------------------------------------
// allows_terminate and await_terminate_permission
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffEndAllTest, AllowsTerminate) {
  using handoff::terminate_permission_e;
  EXPECT_TRUE(handoff::allows_terminate(terminate_permission_e::no_drain));
  EXPECT_TRUE(handoff::allows_terminate(terminate_permission_e::ended));
  EXPECT_TRUE(handoff::allows_terminate(terminate_permission_e::peer_gone));
  EXPECT_FALSE(handoff::allows_terminate(terminate_permission_e::refused));
}

namespace {
  /**
   * @brief Fake draining peer.
   */
  class fake_peer_t: public handoff::drain_peer_t {
  public:
    bool exists_value {true};  ///< Result of `exists()`.
    bool gone_value {false};  ///< Result of `gone()`.
    bool gone_after_end_all {false};  ///< `gone()` turns true when `end_all()` returns.
    reply_e reply {reply_e::ended};  ///< Result of `end_all()`.
    bool block_for_timeout {false};  ///< `end_all()` waits for its whole timeout (as a real, silent peer does).
    int end_all_calls {0};  ///< Calls of `end_all()`.
    std::chrono::milliseconds last_timeout {0};  ///< Timeout of the last `end_all()`.
    std::optional<clock_type::time_point> gone_at;  ///< `gone()` turns true at this time.

    bool exists() override {
      return exists_value;
    }

    bool gone() override {
      return gone_value || (gone_at && clock_type::now() >= *gone_at);
    }

    reply_e end_all(std::chrono::milliseconds timeout) override {
      ++end_all_calls;
      last_timeout = timeout;
      if (block_for_timeout) {
        std::this_thread::sleep_for(timeout);
      }
      if (gone_after_end_all) {
        gone_value = true;
      }
      return reply;
    }
  };
}  // namespace

TEST(HandoffEndAllTest, NoDrainMeansNoDrainAndNoEndAll) {
  fake_peer_t peer;
  peer.exists_value = false;
  EXPECT_EQ(handoff::terminate_permission_e::no_drain, handoff::await_terminate_permission(peer, 200ms));
  EXPECT_EQ(0, peer.end_all_calls) << "no END_ALL may be sent when no draining process exists";
}

TEST(HandoffEndAllTest, PeerAlreadyGoneMeansPeerGoneWithoutEndAll) {
  fake_peer_t peer;
  peer.gone_value = true;
  EXPECT_EQ(handoff::terminate_permission_e::peer_gone, handoff::await_terminate_permission(peer, 200ms));
  EXPECT_EQ(0, peer.end_all_calls) << "no END_ALL is needed when the pidfd already shows the peer gone";
}

TEST(HandoffEndAllTest, EndedMeansEnded) {
  fake_peer_t peer;
  peer.reply = handoff::drain_peer_t::reply_e::ended;
  EXPECT_EQ(handoff::terminate_permission_e::ended, handoff::await_terminate_permission(peer, 200ms));
  EXPECT_EQ(1, peer.end_all_calls);
}

TEST(HandoffEndAllTest, EndAllIsSentWithTheWholeBudget) {
  fake_peer_t peer;
  peer.reply = handoff::drain_peer_t::reply_e::ended;
  handoff::await_terminate_permission(peer, 5000ms);
  EXPECT_GE(peer.last_timeout, 4900ms) << "END_ALL must be sent with the whole budget";
  EXPECT_LE(peer.last_timeout, 5000ms);
}

TEST(HandoffEndAllTest, RejectedWhileAliveIsRefusedAtTheEndOfTheBudget) {
  fake_peer_t peer;
  peer.reply = handoff::drain_peer_t::reply_e::rejected;
  auto start = clock_type::now();
  EXPECT_EQ(handoff::terminate_permission_e::refused, handoff::await_terminate_permission(peer, 200ms));
  auto elapsed = clock_type::now() - start;
  EXPECT_GE(elapsed, 160ms) << "END_REJECTED counts as a timeout: the peer is polled until the budget ends";
  EXPECT_LT(elapsed, 1500ms);
  EXPECT_EQ(1, peer.end_all_calls);
}

TEST(HandoffEndAllTest, RejectedThenPeerDiesDuringBudgetIsPeerGone) {
  fake_peer_t peer;
  peer.reply = handoff::drain_peer_t::reply_e::rejected;
  peer.gone_at = clock_type::now() + 60ms;
  auto start = clock_type::now();
  EXPECT_EQ(handoff::terminate_permission_e::peer_gone, handoff::await_terminate_permission(peer, 1500ms));
  EXPECT_LT(clock_type::now() - start, 1000ms) << "peer_gone is returned as soon as the peer is gone, not at the end of the budget";
}

TEST(HandoffEndAllTest, NoReplyThenPeerDiesDuringBudgetIsPeerGone) {
  fake_peer_t peer;
  peer.reply = handoff::drain_peer_t::reply_e::no_reply;
  peer.gone_at = clock_type::now() + 60ms;
  EXPECT_EQ(handoff::terminate_permission_e::peer_gone, handoff::await_terminate_permission(peer, 1500ms));
}

TEST(HandoffEndAllTest, RejectedButGoneByThenIsPeerGone) {
  fake_peer_t peer;
  peer.reply = handoff::drain_peer_t::reply_e::rejected;
  peer.gone_after_end_all = true;
  EXPECT_EQ(handoff::terminate_permission_e::peer_gone, handoff::await_terminate_permission(peer, 200ms));
}

TEST(HandoffEndAllTest, NoReplyWhileAliveIsRefusedAfterTheBudget) {
  fake_peer_t peer;
  peer.reply = handoff::drain_peer_t::reply_e::no_reply;
  peer.block_for_timeout = true;
  auto start = clock_type::now();
  EXPECT_EQ(handoff::terminate_permission_e::refused, handoff::await_terminate_permission(peer, 150ms));
  auto elapsed = clock_type::now() - start;
  EXPECT_GE(elapsed, 120ms) << "without ENDED and with the peer alive, the budget is used up first";
  EXPECT_LT(elapsed, 1500ms);
  EXPECT_EQ(1, peer.end_all_calls);
}

TEST(HandoffEndAllTest, NoReplyButGoneByThenIsPeerGone) {
  fake_peer_t peer;
  peer.reply = handoff::drain_peer_t::reply_e::no_reply;
  peer.gone_after_end_all = true;
  EXPECT_EQ(handoff::terminate_permission_e::peer_gone, handoff::await_terminate_permission(peer, 200ms));
  EXPECT_EQ(1, peer.end_all_calls);
}

#endif  // __linux__
