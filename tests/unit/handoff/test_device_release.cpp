/**
 * @file tests/unit/handoff/test_device_release.cpp
 * @brief Blind regression tests for `handoff::entry::device_release_t` (src/handoff/entry.h, section 7) and the
 * registered release function (src/handoff/drain.h): SIGTERM during a drain exit while the device-release closure is
 * still pending.
 *
 * Written from the headers only. The registered closure is reached through the drain controller's default
 * `release_devices` seam. The fake guard counts its destructions and can block on a latch. Everything is bounded,
 * deterministic (no timing-based success) and repeat-safe; the tests are meant to be run under ASan.
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
  #include <thread>
  #include <vector>

  // local includes
  #include "src/handoff/drain.h"
  #include "src/handoff/entry.h"
  #include "src/handoff/runtime.h"

using namespace std::chrono_literals;

namespace {
  /**
   * @brief State shared by the fake guards of one test.
   */
  struct guard_state_t {
    std::atomic<int> destroyed {0};  ///< Destructor completions.
    std::atomic<int> entered {0};  ///< Destructor entries.
    std::mutex mutex;  ///< Guards `open`.
    std::condition_variable cv;  ///< Signals `open`.
    bool open {true};  ///< When false, the destructor blocks until `unblock()`.

    /**
     * @brief Make the next destructor block.
     */
    void block() {
      std::lock_guard lock(mutex);
      open = false;
    }

    /**
     * @brief Let a blocked destructor continue.
     */
    void unblock() {
      {
        std::lock_guard lock(mutex);
        open = true;
      }
      cv.notify_all();
    }
  };

  /**
   * @brief Fake `platf::deinit_t` whose destructor counts and can block on a latch.
   */
  class fake_guard_t: public platf::deinit_t {
  public:
    /**
     * @brief Build the guard.
     *
     * @param state Shared counters.
     */
    explicit fake_guard_t(guard_state_t &state):
        _state(state) {
    }

    /**
     * @brief Count, and wait for the latch when it is closed (bounded at 5 s so a bug cannot hang the run).
     */
    ~fake_guard_t() override {
      ++_state.entered;
      {
        std::unique_lock lock(_state.mutex);
        _state.cv.wait_for(lock, 5s, [this] {
          return _state.open;
        });
      }
      ++_state.destroyed;
    }

  private:
    guard_state_t &_state;  ///< Counters.
  };

  /**
   * @brief Poll a predicate until it holds or the limit passes.
   *
   * @param pred Predicate.
   * @param limit Longest wait.
   * @return `true` when the predicate held.
   */
  bool wait_until(const std::function<bool()> &pred, std::chrono::milliseconds limit = 1000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
      if (pred()) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    }
    return pred();
  }

  /**
   * @brief Fixture: a drain controller whose `release_devices` is the production default (the registered function).
   */
  class HandoffDeviceReleaseTest: public ::testing::Test {
  protected:
    void SetUp() override {
      handoff::runtime().reset_for_test();
      handoff::reset_drain_for_test();
      handoff::settings_t settings;
      settings.enabled = true;
      settings.deadline_s = 10;
      handoff::runtime().configure(settings);
    }

    void TearDown() override {
      state.unblock();
      ctrl.reset();
      handoff::register_device_release({});
      handoff::reset_drain_for_test();
      handoff::runtime().reset_for_test();
    }

    /**
     * @brief Build the controller. Its `release_devices` is left null, so it takes the registered function.
     */
    void make_controller() {
      handoff::drain_ops_t ops;
      ops.now = [] {
        return std::chrono::steady_clock::now();
      };
      ops.sessions = [] {
        return 0;
      };
      ops.shutdown_requested = [] {
        return false;
      };
      ops.end_sessions = [](stream::stop_reason_e) {
      };
      ops.ensure_unmap_scope = [] {
      };
      ops.arm_limit = [](std::chrono::milliseconds, std::function<void()>) {
      };
      ops.exit_graceful = [this] {
        exited = true;
      };
      ops.force_exit = [](int) {
      };
      ops.log_error = [](const std::string &) {
      };
      handoff::drain_timing_t timing;
      timing.tick = 1ms;
      timing.zero_grace = 5ms;
      timing.forced_shutdown = 10000ms;
      ctrl = std::make_unique<handoff::drain_controller_t>(ops, timing);
    }

    /**
     * @brief Start the drain exit: the controller thread calls the release function.
     */
    void start_exit() {
      ctrl->begin(std::chrono::steady_clock::now());
      ctrl->handed_over();
    }

    guard_state_t state;  ///< Fake guard counters.
    std::atomic<bool> exited {false};  ///< The fake `exit_graceful` ran (the exit passed the release step).
    std::unique_ptr<handoff::drain_controller_t> ctrl;  ///< Controller.
  };
}  // namespace

// Case A: the closure outlives the device_release_t.
TEST_F(HandoffDeviceReleaseTest, ClosureCalledAfterDestructionDoesNothing) {
  auto release = std::make_unique<handoff::entry::device_release_t>(std::make_unique<fake_guard_t>(state));
  make_controller();  // takes the registered function while the object is alive
  release.reset();  // main unwinds: the guard is destroyed once here
  ASSERT_EQ(state.destroyed.load(), 1) << "The device_release_t destructor must destroy the guard exactly once.";
  start_exit();  // the drain exit now calls the (copy of the) release function
  ASSERT_TRUE(wait_until([this] {
    return exited.load();
  }))
    << "The drain exit must go on to exit_graceful even when the release function has nothing left to release.";
  EXPECT_EQ(state.destroyed.load(), 1) << "A release call after the device_release_t was destroyed must not destroy "
                                          "the guard again, and must not touch destroyed state.";
  EXPECT_EQ(state.entered.load(), 1);
}

// Control for case A: the same sequence with the object alive releases the guard through the closure.
TEST_F(HandoffDeviceReleaseTest, ClosureOfLiveObjectReleasesDevices) {
  handoff::entry::device_release_t release(std::make_unique<fake_guard_t>(state));
  make_controller();
  ASSERT_EQ(state.destroyed.load(), 0) << "Constructing the device_release_t must not release the guard.";
  start_exit();
  ASSERT_TRUE(wait_until([this] {
    return exited.load();
  }));
  EXPECT_EQ(state.destroyed.load(), 1) << "The registered closure of a live device_release_t must destroy the guard "
                                          "(the drain exit releases the devices).";
  release.release();
  EXPECT_EQ(state.destroyed.load(), 1) << "release() after the closure released the guard must do nothing.";
}

// After the closure released the guard, the destructor of the object must not release it again.
TEST_F(HandoffDeviceReleaseTest, DestructorAfterClosureReleaseDoesNotDestroyTwice) {
  {
    handoff::entry::device_release_t release(std::make_unique<fake_guard_t>(state));
    make_controller();
    start_exit();
    ASSERT_TRUE(wait_until([this] {
      return exited.load();
    }));
    ASSERT_EQ(state.destroyed.load(), 1);
  }
  EXPECT_EQ(state.destroyed.load(), 1) << "main's later destruction of device_release_t must not destroy the guard "
                                          "the drain exit already released.";
}

// Case B: a release is in flight (the guard's destructor is blocked) when main unwinds.
TEST_F(HandoffDeviceReleaseTest, DestructionWaitsForReleaseInFlight) {
  auto release = std::make_unique<handoff::entry::device_release_t>(std::make_unique<fake_guard_t>(state));
  state.block();
  std::thread releaser([&release] {
    release->release();
  });
  ASSERT_TRUE(wait_until([this] {
    return state.entered.load() == 1;
  }))
    << "The release must have entered the guard's destructor.";

  std::atomic<bool> destroying {false};
  std::atomic<bool> destroyed_object {false};
  std::thread unwinder([&] {
    destroying = true;
    release.reset();  // simulates main unwinding on SIGTERM
    destroyed_object = true;
  });
  ASSERT_TRUE(wait_until([&destroying] {
    return destroying.load();
  }));
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(destroyed_object.load()) << "Destroying the device_release_t must wait for the release in flight.";
  EXPECT_EQ(state.entered.load(), 1) << "The guard must not be destroyed twice while the first destruction is "
                                        "blocked.";

  state.unblock();
  releaser.join();
  unwinder.join();
  EXPECT_TRUE(destroyed_object.load());
  EXPECT_EQ(state.destroyed.load(), 1) << "The guard must be destroyed exactly once.";
  EXPECT_EQ(state.entered.load(), 1);
}

// Case B through the drain exit: the controller thread is blocked in the release when main unwinds.
TEST_F(HandoffDeviceReleaseTest, DestructionWaitsForDrainExitReleaseInFlight) {
  auto release = std::make_unique<handoff::entry::device_release_t>(std::make_unique<fake_guard_t>(state));
  make_controller();
  state.block();
  start_exit();
  ASSERT_TRUE(wait_until([this] {
    return state.entered.load() == 1;
  }))
    << "The drain exit must call the registered release function.";
  EXPECT_FALSE(exited.load()) << "The exit must not go on while the release is pending.";

  std::atomic<bool> destroying {false};
  std::atomic<bool> destroyed_object {false};
  std::thread unwinder([&] {
    destroying = true;
    release.reset();
    destroyed_object = true;
  });
  ASSERT_TRUE(wait_until([&destroying] {
    return destroying.load();
  }));
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(destroyed_object.load()) << "Destroying the device_release_t must wait for the drain exit's release.";
  EXPECT_EQ(state.entered.load(), 1) << "No second destruction of the guard may start.";

  state.unblock();
  unwinder.join();
  EXPECT_TRUE(wait_until([this] {
    return exited.load();
  }));
  EXPECT_EQ(state.destroyed.load(), 1);
  EXPECT_EQ(state.entered.load(), 1);
}

// Case C: idempotence and a null guard.
TEST_F(HandoffDeviceReleaseTest, ReleaseTwiceDestroysOnce) {
  handoff::entry::device_release_t release(std::make_unique<fake_guard_t>(state));
  release.release();
  EXPECT_EQ(state.destroyed.load(), 1) << "release() must destroy the guard.";
  release.release();
  EXPECT_EQ(state.destroyed.load(), 1) << "A second release() must do nothing.";
}

TEST_F(HandoffDeviceReleaseTest, DestructorReleasesWhenNotReleasedBefore) {
  {
    handoff::entry::device_release_t release(std::make_unique<fake_guard_t>(state));
    EXPECT_EQ(state.destroyed.load(), 0);
  }
  EXPECT_EQ(state.destroyed.load(), 1) << "The destructor must release the devices at main's place in the order.";
}

TEST_F(HandoffDeviceReleaseTest, NullGuardIsFine) {
  handoff::entry::device_release_t release(nullptr);
  EXPECT_NO_THROW(release.release());
  EXPECT_NO_THROW(release.release());
  make_controller();
  start_exit();
  EXPECT_TRUE(wait_until([this] {
    return exited.load();
  }))
    << "A null guard must not stop the drain exit from going on.";
}

TEST_F(HandoffDeviceReleaseTest, ConcurrentReleasesDestroyOnce) {
  handoff::entry::device_release_t release(std::make_unique<fake_guard_t>(state));
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&release] {
      release.release();
    });
  }
  for (auto &t : threads) {
    t.join();
  }
  EXPECT_EQ(state.destroyed.load(), 1) << "Concurrent release() calls must destroy the guard exactly once.";
  EXPECT_EQ(state.entered.load(), 1);
}

// The destructor registers an empty release function: a controller built afterwards has nothing to release.
TEST_F(HandoffDeviceReleaseTest, DestructorClearsTheRegistration) {
  {
    handoff::entry::device_release_t release(std::make_unique<fake_guard_t>(state));
  }
  ASSERT_EQ(state.destroyed.load(), 1);
  make_controller();  // built after the object is gone: the registered function is empty
  start_exit();
  EXPECT_TRUE(wait_until([this] {
    return exited.load();
  }))
    << "With nothing registered the drain exit must still reach exit_graceful.";
  EXPECT_EQ(state.destroyed.load(), 1);
  // SPEC-QUESTION: whether the controller default resolves the registered function at construction or at call time is
  // not stated in drain.h; case A above is a strong test only when it is resolved at construction. Both readings
  // satisfy these assertions.
}

#endif  // __linux__
