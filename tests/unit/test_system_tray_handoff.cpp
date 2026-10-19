/**
 * @file tests/unit/test_system_tray_handoff.cpp
 * @brief Blind tests for the tray lifecycle of the drain handoff (`run_main_loop`, `suspend`, `resume`,
 *        `request_start`, `end_tray`), written from the contract in `src/system_tray.h`. A fake backend replaces Qt.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#if defined(__linux__) && defined(SUNSHINE_TRAY) && SUNSHINE_TRAY >= 1

  // local includes
  #include <src/system_tray.h>

namespace {
  using namespace std::chrono_literals;
  namespace st = system_tray;

  /**
   * @brief Shared state of the fake backend (the backend entry points are plain function pointers).
   */
  struct fake_state_t {
    std::mutex m;  ///< Guards the fields below.
    std::condition_variable cv;  ///< Signalled by `exit` and by `release`.
    int init_calls = 0;  ///< Calls of `init`.
    int loop_calls = 0;  ///< Calls of `loop`.
    int exit_calls = 0;  ///< Calls of `exit`.
    int poll_calls = 0;  ///< Calls of `poll`.
    int init_failures = 0;  ///< The next this-many `init` calls fail.
    int zero_loops = 0;  ///< The next this-many `loop` calls return 0 at once (one served event).
    bool in_loop = false;  ///< A `loop` call is blocked.
    bool wake = false;  ///< `exit` was called while `loop` was blocked.
    bool stuck = false;  ///< `loop` ignores `exit` until `release` (a loop that cannot be woken).
    bool released = false;  ///< Set by `release`.
    bool unstick = false;  ///< Wakes a stuck `loop` once, and ends the stuck mode.
    bool queued_mode = false;  ///< Model Qt: `exit` from a non-loop thread only queues an event.
    bool queued_exit = false;  ///< A queued exit event waits for the next `loop` call.
    bool icon_up = false;  ///< The fake icon exists (set by `init`, cleared by a delivered exit).
    std::thread::id init_thread;  ///< Thread of the last `init` call.
    std::thread::id loop_thread;  ///< Thread of the last `loop` call.
    std::function<void()> in_loop_hook;  ///< Run (unlocked) at the start of the next `loop` call, then cleared.
  };

  fake_state_t g_fake;  ///< The one fake backend state.

  int fake_init() {
    std::lock_guard lk {g_fake.m};
    ++g_fake.init_calls;
    g_fake.init_thread = std::this_thread::get_id();
    if (g_fake.init_failures > 0) {
      --g_fake.init_failures;
      return -1;
    }
    g_fake.icon_up = true;
    return 0;
  }

  int fake_loop() {
    std::function<void()> hook;
    {
      std::lock_guard lk {g_fake.m};
      ++g_fake.loop_calls;
      g_fake.loop_thread = std::this_thread::get_id();
      hook = std::move(g_fake.in_loop_hook);
      g_fake.in_loop_hook = nullptr;
    }
    if (hook) {
      hook();
    }
    std::unique_lock lk {g_fake.m};
    if (g_fake.zero_loops > 0) {
      --g_fake.zero_loops;
      return 0;
    }
    if (g_fake.queued_mode && g_fake.queued_exit) {
      // A queued exit is delivered by this call: with an icon up it tears the icon down and ends the blocking loop.
      g_fake.queued_exit = false;
      if (g_fake.icon_up) {
        g_fake.icon_up = false;
        return -1;
      }
    }
    g_fake.in_loop = true;
    g_fake.cv.wait_for(lk, 10s, [] {
      return g_fake.stuck ? (g_fake.unstick || g_fake.released) : (g_fake.wake || g_fake.released);
    });
    g_fake.in_loop = false;
    g_fake.wake = false;
    if (g_fake.stuck && g_fake.unstick) {
      g_fake.stuck = false;
      g_fake.unstick = false;
    }
    if (g_fake.queued_mode && g_fake.queued_exit) {
      g_fake.queued_exit = false;
      g_fake.icon_up = false;
    }
    return -1;
  }

  void fake_exit() {
    std::lock_guard lk {g_fake.m};
    ++g_fake.exit_calls;
    if (g_fake.queued_mode) {
      if (std::this_thread::get_id() == g_fake.loop_thread) {
        g_fake.icon_up = false;  // direct call on the loop thread: handled at once
        g_fake.queued_exit = false;
      } else {
        g_fake.queued_exit = true;
        if (g_fake.in_loop) {
          g_fake.wake = true;
          g_fake.cv.notify_all();
        }
      }
      return;
    }
    if (g_fake.in_loop) {
      g_fake.wake = true;
      g_fake.cv.notify_all();
    }
  }

  void fake_poll() {
    std::lock_guard lk {g_fake.m};
    ++g_fake.poll_calls;
    if (g_fake.queued_mode && g_fake.queued_exit) {
      // Delivers the queued exit event without blocking: the old icon is torn down now.
      g_fake.queued_exit = false;
      g_fake.icon_up = false;
    }
  }

  void release_fake() {
    std::lock_guard lk {g_fake.m};
    g_fake.released = true;
    g_fake.cv.notify_all();
  }

  void unstick_fake() {
    std::lock_guard lk {g_fake.m};
    g_fake.unstick = true;
    g_fake.cv.notify_all();
  }

  int init_calls() {
    std::lock_guard lk {g_fake.m};
    return g_fake.init_calls;
  }

  int loop_calls() {
    std::lock_guard lk {g_fake.m};
    return g_fake.loop_calls;
  }

  int exit_calls() {
    std::lock_guard lk {g_fake.m};
    return g_fake.exit_calls;
  }

  /**
   * @brief Poll until a predicate holds or the limit passes.
   */
  bool wait_until(const std::function<bool()> &pred, std::chrono::milliseconds limit = 2000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
      if (pred()) {
        return true;
      }
      std::this_thread::sleep_for(5ms);
    }
    return pred();
  }

  bool phase_becomes(st::tray_phase_e phase, std::chrono::milliseconds limit = 2000ms) {
    return wait_until(
      [phase] {
        return st::current_phase() == phase;
      },
      limit
    );
  }

  class TrayLifecycleHandoffTest: public ::testing::Test {
  protected:
    void SetUp() override {
      st::reset_lifecycle_for_testing();
      g_fake.init_failures = 0;
      g_fake.zero_loops = 0;
      g_fake.init_calls = g_fake.loop_calls = g_fake.exit_calls = g_fake.poll_calls = 0;
      g_fake.in_loop = g_fake.wake = g_fake.stuck = g_fake.released = false;
      g_fake.unstick = g_fake.queued_mode = g_fake.queued_exit = g_fake.icon_up = false;
      g_fake.in_loop_hook = nullptr;
      g_fake.init_thread = g_fake.loop_thread = {};
      shutdown_flag_ = false;
      st::set_backend_for_testing({&fake_init, &fake_loop, &fake_exit, &fake_poll});
      st::set_enabled_for_testing(true);
    }

    void TearDown() override {
      shutdown_flag_ = true;
      release_fake();
      st::end_tray();
      stop_loop();
      st::reset_lifecycle_for_testing();
    }

    /**
     * @brief Run `run_main_loop` on a new thread (that thread is the "main thread" of the test).
     */
    void start_loop() {
      loop_done_ = std::promise<void>();
      loop_done_future_ = loop_done_.get_future();
      loop_thread_ = std::thread([this] {
        loop_thread_id_ = std::this_thread::get_id();
        st::run_main_loop([this] {
          return shutdown_flag_.load();
        });
        loop_done_.set_value();
      });
    }

    bool loop_returned(std::chrono::milliseconds limit = 2000ms) {
      return loop_done_future_.valid() && loop_done_future_.wait_for(limit) == std::future_status::ready;
    }

    void stop_loop() {
      if (loop_thread_.joinable()) {
        if (loop_done_future_.wait_for(5s) == std::future_status::ready) {
          loop_thread_.join();
        } else {
          loop_thread_.detach();
          ADD_FAILURE() << "run_main_loop did not return at teardown (it must return once shutdown is requested or end_tray ran)";
        }
      }
    }

    /**
     * @brief Start the loop and a request_start, and wait for the running phase.
     */
    void get_running() {
      ASSERT_TRUE(st::request_start());
      start_loop();
      ASSERT_TRUE(phase_becomes(st::tray_phase_e::running)) << "request_start then run_main_loop must reach phase running";
      ASSERT_TRUE(wait_until([] {
        return loop_calls() >= 1;
      }))
        << "a running tray must block in the backend's loop";
    }

    std::atomic<bool> shutdown_flag_ {false};
    std::promise<void> loop_done_;
    std::future<void> loop_done_future_;
    std::thread loop_thread_;
    std::thread::id loop_thread_id_;
  };
}  // namespace

TEST_F(TrayLifecycleHandoffTest, InitialPhaseIsNotStarted) {
  EXPECT_EQ(st::current_phase(), st::tray_phase_e::not_started) << "a fresh lifecycle must be in phase not_started";
}

TEST_F(TrayLifecycleHandoffTest, NothingIsShownBeforeRequestStart) {
  start_loop();
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(init_calls(), 0) << "the loop must not call init without a start or resume command";
  EXPECT_EQ(loop_calls(), 0) << "the loop must not call the backend loop while no icon is shown";
  EXPECT_EQ(st::current_phase(), st::tray_phase_e::not_started) << "phase must stay not_started without a command";
  EXPECT_FALSE(loop_returned(50ms)) << "the loop must keep idling while shutdown is not requested";
}

TEST_F(TrayLifecycleHandoffTest, RequestStartBeforeLoopInitsOnLoopThread) {
  ASSERT_TRUE(st::request_start()) << "request_start must be accepted when the tray is enabled";
  EXPECT_EQ(init_calls(), 0) << "request_start must not call init itself (only the loop does)";
  start_loop();
  ASSERT_TRUE(phase_becomes(st::tray_phase_e::running)) << "the loop must execute the pending start and reach running";
  EXPECT_EQ(init_calls(), 1) << "exactly one init call is expected";
  EXPECT_EQ(g_fake.init_thread, loop_thread_id_) << "init must run on the thread that called run_main_loop";
}

TEST_F(TrayLifecycleHandoffTest, RequestStartWhileLoopIdleStartsTheIcon) {
  start_loop();
  std::this_thread::sleep_for(50ms);
  ASSERT_TRUE(st::request_start()) << "request_start must be accepted";
  EXPECT_TRUE(phase_becomes(st::tray_phase_e::running)) << "the idle loop must wake on request_start and init";
  EXPECT_EQ(g_fake.init_thread, loop_thread_id_) << "init must run on the loop thread";
}

TEST_F(TrayLifecycleHandoffTest, RequestStartWhenNotEnabledIsRejected) {
  st::set_enabled_for_testing(false);
  EXPECT_FALSE(st::request_start()) << "request_start must return false when the tray is not enabled";
  EXPECT_EQ(init_calls(), 0) << "a rejected request_start must not init";
}

TEST_F(TrayLifecycleHandoffTest, RequestStartWhenRunningIsAcceptedWithoutSecondInit) {
  get_running();
  EXPECT_TRUE(st::request_start()) << "request_start while running must return true";
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(init_calls(), 1) << "request_start while running must not init again";
}

TEST_F(TrayLifecycleHandoffTest, ResumeFromNotStartedIsRejected) {
  EXPECT_FALSE(st::resume()) << "resume with phase not_started must return false (use request_start)";
}

TEST_F(TrayLifecycleHandoffTest, ResumeWhenRunningIsAcceptedWithoutSecondInit) {
  get_running();
  EXPECT_TRUE(st::resume()) << "resume while running must return true";
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(init_calls(), 1) << "resume while running must not init again";
}

TEST_F(TrayLifecycleHandoffTest, SuspendWithNoIconReturnsTrueAtOnceWithoutExit) {
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(st::suspend(1000ms)) << "suspend with nothing to remove must return true";
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 500ms) << "suspend with nothing to remove must not wait";
  EXPECT_EQ(exit_calls(), 0) << "suspend with nothing to remove must not call the backend";
  EXPECT_EQ(st::current_phase(), st::tray_phase_e::not_started) << "phase must not change";
}

TEST_F(TrayLifecycleHandoffTest, SuspendRemovesIconAndKeepsLoopAlive) {
  get_running();
  EXPECT_TRUE(st::suspend(2000ms)) << "suspend of a running tray must return true once the icon is gone";
  EXPECT_EQ(st::current_phase(), st::tray_phase_e::suspended) << "phase must be suspended after suspend returned";
  EXPECT_GE(exit_calls(), 1) << "suspend must ask the backend to exit";
  EXPECT_FALSE(loop_returned(100ms)) << "run_main_loop must keep running after a suspend";
}

TEST_F(TrayLifecycleHandoffTest, SuspendTwiceIsIdempotent) {
  get_running();
  ASSERT_TRUE(st::suspend(2000ms));
  const int exits = exit_calls();
  EXPECT_TRUE(st::suspend(500ms)) << "suspend in phase suspended must return true";
  EXPECT_EQ(exit_calls(), exits) << "a suspend in phase suspended must not call the backend";
}

TEST_F(TrayLifecycleHandoffTest, ResumeAfterSuspendReinitsOnLoopThread) {
  get_running();
  ASSERT_TRUE(st::suspend(2000ms));
  ASSERT_TRUE(st::resume()) << "resume in phase suspended must be accepted";
  ASSERT_TRUE(phase_becomes(st::tray_phase_e::running)) << "the loop must re-init and reach running";
  EXPECT_EQ(init_calls(), 2) << "resume must init once more";
  EXPECT_EQ(g_fake.init_thread, loop_thread_id_) << "re-init must run on the loop thread";
  EXPECT_TRUE(wait_until([] {
    return loop_calls() >= 2;
  }))
    << "after the resume the loop must block in the backend again";
}

TEST_F(TrayLifecycleHandoffTest, RequestStartInSuspendedPhaseBehavesAsResume) {
  get_running();
  ASSERT_TRUE(st::suspend(2000ms));
  ASSERT_TRUE(st::request_start()) << "request_start in phase suspended must be accepted";
  EXPECT_TRUE(phase_becomes(st::tray_phase_e::running)) << "request_start in phase suspended must bring the icon back";
  EXPECT_EQ(init_calls(), 2) << "one more init is expected";
}

TEST_F(TrayLifecycleHandoffTest, SuspendResumeSuspendCycles) {
  get_running();
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(st::suspend(2000ms)) << "suspend failed in cycle " << i;
    ASSERT_TRUE(st::resume()) << "resume failed in cycle " << i;
    ASSERT_TRUE(phase_becomes(st::tray_phase_e::running)) << "running not reached in cycle " << i;
    ASSERT_TRUE(wait_until([i] {
      return loop_calls() >= i + 2;
    }))
      << "backend loop not re-entered in cycle " << i;
  }
  EXPECT_EQ(init_calls(), 4) << "one init per start or resume";
}

TEST_F(TrayLifecycleHandoffTest, InitFailureKeepsPhaseAndAllowsRetry) {
  g_fake.init_failures = 1;
  ASSERT_TRUE(st::request_start());
  start_loop();
  ASSERT_TRUE(wait_until([] {
    return init_calls() >= 1;
  }))
    << "the loop must try init";
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(st::current_phase(), st::tray_phase_e::not_started) << "a failed init must keep the phase";
  EXPECT_EQ(init_calls(), 1) << "a failed start command is dropped, not retried by the loop";
  EXPECT_EQ(loop_calls(), 0) << "no backend loop without an icon";
  EXPECT_FALSE(loop_returned(50ms)) << "a failed init must not end the loop";
  ASSERT_TRUE(st::request_start()) << "a new request_start must be accepted";
  EXPECT_TRUE(phase_becomes(st::tray_phase_e::running)) << "the retry must succeed";
  EXPECT_EQ(init_calls(), 2);
}

TEST_F(TrayLifecycleHandoffTest, BackendLoopServingEventsIsCalledAgain) {
  g_fake.zero_loops = 3;
  get_running();
  EXPECT_TRUE(wait_until([] {
    return loop_calls() >= 4;
  }))
    << "a loop that returns 0 (one served event) must be called again";
  EXPECT_EQ(init_calls(), 1);
  EXPECT_FALSE(loop_returned(50ms)) << "returning 0 from the backend loop must not end run_main_loop";
}

TEST_F(TrayLifecycleHandoffTest, BackendLoopEndingWithoutCommandEndsRunMainLoop) {
  // Quit chosen in the menu: the fake loop returns non-zero at once.
  g_fake.zero_loops = 0;
  g_fake.in_loop_hook = [] {
    std::thread([] {
      std::this_thread::sleep_for(50ms);
      fake_exit();
    }).detach();
  };
  ASSERT_TRUE(st::request_start());
  start_loop();
  EXPECT_TRUE(loop_returned(2000ms)) << "a backend loop that ended with no suspend pending and no end_tray must end run_main_loop";
}

TEST_F(TrayLifecycleHandoffTest, SuspendPendingBetweenIterationsCompletesWithoutLoopAndExitsAgain) {
  // SPEC-QUESTION: the spec's "suspend pending before run_main_loop is entered" cannot be built with a fake backend
  // (phase running needs init_tray). The same code path is reached when the suspend arrives while the loop is between
  // iterations: the fake loop serves one event (returns 0) after the suspend was issued.
  g_fake.zero_loops = 1;
  std::promise<bool> suspended;
  auto suspended_future = suspended.get_future();
  std::thread suspender;
  g_fake.in_loop_hook = [&suspender, &suspended] {
    suspender = std::thread([&suspended] {
      suspended.set_value(st::suspend(3000ms));
    });
    // Wait for the suspender's own exit call (if the implementation makes one) before the event "returns".
    wait_until(
      [] {
        return exit_calls() >= 1;
      },
      300ms
    );
  };
  ASSERT_TRUE(st::request_start());
  start_loop();
  ASSERT_EQ(suspended_future.wait_for(3s), std::future_status::ready) << "suspend must be acknowledged by the loop";
  suspender.join();
  EXPECT_TRUE(suspended_future.get()) << "suspend must return true";
  EXPECT_EQ(st::current_phase(), st::tray_phase_e::suspended) << "phase must be suspended";
  EXPECT_EQ(loop_calls(), 1) << "a pending suspend must be completed without calling the backend loop again";
  EXPECT_EQ(exit_calls(), 2) << "the loop must call exit once more on its own thread for a suspend issued from another thread";
  EXPECT_FALSE(loop_returned(50ms)) << "run_main_loop must keep running after the suspend";
}

TEST_F(TrayLifecycleHandoffTest, SuspendOnLoopThreadReturnsFalseWithoutWaiting) {
  std::atomic<bool> result {true};
  std::atomic<long long> elapsed_ms {-1};
  g_fake.zero_loops = 1;
  g_fake.in_loop_hook = [&] {
    const auto t0 = std::chrono::steady_clock::now();
    result = st::suspend(3000ms);
    elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  };
  ASSERT_TRUE(st::request_start());
  start_loop();
  ASSERT_TRUE(wait_until([&] {
    return elapsed_ms.load() >= 0;
  }))
    << "the hook must run";
  EXPECT_FALSE(result.load()) << "suspend called on the main-loop thread must return false";
  EXPECT_LT(elapsed_ms.load(), 1000) << "suspend on the main-loop thread must not wait for itself";
  EXPECT_TRUE(phase_becomes(st::tray_phase_e::suspended)) << "the posted command must still run";
}

TEST_F(TrayLifecycleHandoffTest, SuspendTimesOutWhenLoopCannotBeWoken) {
  g_fake.stuck = true;
  get_running();
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(st::suspend(150ms)) << "suspend must return false when the loop does not acknowledge in time";
  const auto took = std::chrono::steady_clock::now() - t0;
  EXPECT_GE(took, 120ms) << "suspend must wait for the timeout";
  EXPECT_LT(took, 1500ms) << "suspend must not wait much longer than the timeout";
  EXPECT_EQ(st::current_phase(), st::tray_phase_e::running) << "the phase changes only when the loop acknowledges";
  release_fake();
  EXPECT_TRUE(phase_becomes(st::tray_phase_e::suspended)) << "the command stays pending after a timeout and still runs";
}

TEST_F(TrayLifecycleHandoffTest, EndTrayWakesAWaitingSuspendWithTrue) {
  g_fake.stuck = true;
  get_running();
  auto pending = std::async(std::launch::async, [] {
    return st::suspend(4000ms);
  });
  std::this_thread::sleep_for(100ms);
  release_fake();
  st::end_tray();
  ASSERT_EQ(pending.wait_for(3s), std::future_status::ready) << "end_tray must wake a suspend that is waiting";
  EXPECT_TRUE(pending.get()) << "a suspend waiting when end_tray runs must return true";
}

TEST_F(TrayLifecycleHandoffTest, UpdateTrayNeverCallsBackendAfterSuspend) {
  get_running();
  ASSERT_TRUE(st::suspend(2000ms));
  const int inits = init_calls();
  const int loops = loop_calls();
  const int exits = exit_calls();
  st::update_tray_playing("App");
  st::update_tray_pausing("App");
  st::update_tray_stopped("App");
  st::update_tray_require_pin();
  EXPECT_EQ(init_calls(), inits) << "update_tray_* must not init the backend";
  EXPECT_EQ(loop_calls(), loops) << "update_tray_* must not run the backend loop";
  EXPECT_EQ(exit_calls(), exits) << "update_tray_* must not call exit";
}

TEST_F(TrayLifecycleHandoffTest, UpdateTrayDoesNothingBeforeStart) {
  st::update_tray_playing("App");
  st::update_tray_pausing("App");
  st::update_tray_stopped("App");
  st::update_tray_require_pin();
  EXPECT_EQ(init_calls() + loop_calls() + exit_calls(), 0) << "update_tray_* must not touch the backend when no icon is shown";
}

TEST_F(TrayLifecycleHandoffTest, IdlePhaseReturnsWhenShutdownIsRequested) {
  start_loop();
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(loop_returned(50ms)) << "the idle loop must wait while shutdown is not requested";
  shutdown_flag_ = true;
  EXPECT_TRUE(loop_returned(1500ms)) << "the idle loop must notice shutdown_requested within about the 500 ms poll";
}

TEST_F(TrayLifecycleHandoffTest, SuspendedPhaseReturnsWhenShutdownIsRequested) {
  get_running();
  ASSERT_TRUE(st::suspend(2000ms));
  shutdown_flag_ = true;
  EXPECT_TRUE(loop_returned(1500ms)) << "the suspended loop must notice shutdown_requested within about the 500 ms poll";
}

TEST_F(TrayLifecycleHandoffTest, EmptyShutdownFunctionIsAccepted) {
  std::promise<void> done;
  auto fut = done.get_future();
  std::thread t([&done] {
    st::run_main_loop(std::function<bool()> {});
    done.set_value();
  });
  std::this_thread::sleep_for(100ms);
  st::end_tray();
  const bool returned = fut.wait_for(2s) == std::future_status::ready;
  if (returned) {
    t.join();
  } else {
    t.detach();
  }
  EXPECT_TRUE(returned) << "with an empty shutdown function the loop must still end through end_tray";
}

TEST_F(TrayLifecycleHandoffTest, EndTrayBeforeLoopMakesLoopReturnAtOnce) {
  EXPECT_EQ(st::end_tray(), 0) << "end_tray returns 0";
  EXPECT_EQ(st::current_phase(), st::tray_phase_e::ended) << "end_tray must set the terminal phase";
  start_loop();
  EXPECT_TRUE(loop_returned(1000ms)) << "run_main_loop must return at once when end_tray already ran";
  EXPECT_EQ(init_calls(), 0) << "no icon is created after end_tray";
}

TEST_F(TrayLifecycleHandoffTest, EndTrayWakesIdleLoop) {
  start_loop();
  std::this_thread::sleep_for(50ms);
  st::end_tray();
  EXPECT_TRUE(loop_returned(1000ms)) << "end_tray must wake an idle run_main_loop, which then returns";
  EXPECT_EQ(st::current_phase(), st::tray_phase_e::ended);
}

TEST_F(TrayLifecycleHandoffTest, EndTrayWakesSuspendedLoop) {
  get_running();
  ASSERT_TRUE(st::suspend(2000ms));
  st::end_tray();
  EXPECT_TRUE(loop_returned(1000ms)) << "end_tray must end run_main_loop in phase suspended";
}

TEST_F(TrayLifecycleHandoffTest, EndTrayIsIdempotentAndCommandsDoNothingAfterIt) {
  EXPECT_EQ(st::end_tray(), 0);
  EXPECT_EQ(st::end_tray(), 0) << "end_tray must be idempotent";
  EXPECT_EQ(st::current_phase(), st::tray_phase_e::ended);
  EXPECT_FALSE(st::resume()) << "resume after end_tray must return false";
  EXPECT_FALSE(st::request_start()) << "request_start after end_tray must return false";
  EXPECT_TRUE(st::suspend(200ms)) << "suspend after end_tray must return true";
  EXPECT_EQ(st::current_phase(), st::tray_phase_e::ended) << "ended is terminal";
  EXPECT_EQ(init_calls(), 0);
}

TEST_F(TrayLifecycleHandoffTest, StaleQueuedExitDoesNotKillTheResumedIcon) {
  // SPEC-QUESTION: a suspend pending before run_main_loop is entered cannot be built with a fake backend; the suspend
  // is issued while the first loop iteration is running its event, i.e. between two iterations.
  g_fake.queued_mode = true;
  g_fake.zero_loops = 1;
  std::promise<bool> suspended;
  auto suspended_future = suspended.get_future();
  std::thread suspender;
  g_fake.in_loop_hook = [&suspender, &suspended] {
    suspender = std::thread([&suspended] {
      suspended.set_value(st::suspend(3000ms));
    });
    wait_until(
      [] {
        return exit_calls() >= 1;
      },
      300ms
    );
  };
  ASSERT_TRUE(st::request_start());
  start_loop();
  ASSERT_EQ(suspended_future.wait_for(3s), std::future_status::ready) << "suspend must be acknowledged";
  suspender.join();
  ASSERT_TRUE(suspended_future.get());
  ASSERT_TRUE(phase_becomes(st::tray_phase_e::suspended));
  {
    std::lock_guard lk {g_fake.m};
    EXPECT_GE(g_fake.poll_calls, 1) << "completing a pending suspend must call the backend's poll so the queued exit reaches the old icon";
    EXPECT_FALSE(g_fake.queued_exit) << "no exit event may stay queued once the suspend completed";
    EXPECT_FALSE(g_fake.icon_up) << "the old icon must be gone after the suspend";
  }
  ASSERT_TRUE(st::resume()) << "resume after suspend must be accepted";
  EXPECT_TRUE(phase_becomes(st::tray_phase_e::running)) << "the icon must come back";
  std::this_thread::sleep_for(250ms);
  bool icon_up = false;
  {
    std::lock_guard lk {g_fake.m};
    icon_up = g_fake.icon_up;
  }
  EXPECT_TRUE(icon_up) << "a stale queued exit from the earlier suspend must not tear down the resumed icon";
  EXPECT_EQ(st::current_phase(), st::tray_phase_e::running) << "the phase must stay running";
  EXPECT_FALSE(loop_returned(10ms)) << "run_main_loop must not return because of a stale exit";
  st::end_tray();
  release_fake();
  EXPECT_TRUE(loop_returned(2000ms)) << "the loop must end cleanly after end_tray";
}

TEST_F(TrayLifecycleHandoffTest, ResumeDuringSuspendIsDeferredAndRunsAfterIt) {
  g_fake.stuck = true;
  get_running();
  auto pending = std::async(std::launch::async, [] {
    return st::suspend(4000ms);
  });
  std::this_thread::sleep_for(100ms);
  EXPECT_TRUE(st::resume()) << "a resume that arrives during a suspend must be accepted";
  unstick_fake();
  ASSERT_EQ(pending.wait_for(3s), std::future_status::ready) << "the suspend must finish";
  EXPECT_TRUE(pending.get());
  EXPECT_TRUE(wait_until([] {
    return init_calls() >= 2;
  }))
    << "the deferred resume must re-init after the suspend finished";
  EXPECT_TRUE(phase_becomes(st::tray_phase_e::running)) << "the final phase must be running";
  EXPECT_FALSE(loop_returned(100ms)) << "run_main_loop must keep running";
}

TEST_F(TrayLifecycleHandoffTest, ResetLifecycleRestoresFreshState) {
  st::end_tray();
  st::reset_lifecycle_for_testing();
  EXPECT_EQ(st::current_phase(), st::tray_phase_e::not_started) << "reset must return the phase to not_started";
}
#endif
