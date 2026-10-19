/**
 * @file tests/unit/handoff/test_drain.cpp
 * @brief Blind tests for the drain (src/handoff/drain.h): the session notifier, the drain controller (deadline timer,
 * zero-session grace, bounded exit), the forced-shutdown limit and the shutdown-stage name.
 *
 * Written from `docs/drain-handoff.md` ("8. Drain", "Stopping, restarting and quitting") and the public headers only.
 * Every clock, session count, exit and limit is a seam: nothing waits for a real 10 s, nothing really exits, and no
 * systemd, GPU, display, network or root is used. Tests are bounded well under 2 s and repeat-safe.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <filesystem>
  #include <functional>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <thread>
  #include <unistd.h>
  #include <vector>

  // local includes
  #include "src/config.h"
  #include "src/handoff/drain.h"
  #include "src/handoff/hooks.h"
  #include "src/handoff/peer_link.h"
  #include "src/handoff/runtime.h"
  #include "src/stream.h"
  #include "src/upnp.h"

using namespace std::chrono_literals;

namespace {
  using clock_type = std::chrono::steady_clock;
  using handoff::drain_phase_e;
  using handoff::mode_e;
  using handoff::state_e;

  std::atomic<bool> g_global_notifier_started {false};  ///< The process-wide notifier was started by a test here.
  std::atomic<bool> g_global_notifier_may_have_ended {false};  ///< A shutdown request ran while it was running.

  /**
   * @brief Poll a predicate until it holds or the limit passes.
   *
   * @param pred Predicate.
   * @param limit Longest wait.
   * @return `true` when the predicate held.
   */
  bool wait_until(const std::function<bool()> &pred, std::chrono::milliseconds limit = 1000ms) {
    const auto end = clock_type::now() + limit;
    while (clock_type::now() < end) {
      if (pred()) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    }
    return pred();
  }

  // ---------------------------------------------------------------------------------------------------------------
  // The drain controller
  // ---------------------------------------------------------------------------------------------------------------

  /**
   * @brief Recorder for every effect of the controller, plus a fake clock and fake session count.
   *
   * The controller must be destroyed before this object (declare it after).
   */
  struct rig_t {
    clock_type::time_point epoch {clock_type::now()};  ///< Time zero of the fake clock.
    std::atomic<long long> clock_ms {0};  ///< Fake clock, milliseconds after `epoch`.
    std::atomic<int> sessions {0};  ///< Fake running-session count.
    std::atomic<bool> shutdown {false};  ///< Fake "shutdown requested".
    std::atomic<bool> hold_end_sessions {false};  ///< While true, `end_sessions` blocks (a slow session join).
    std::atomic<bool> in_end_sessions {false};  ///< `end_sessions` is running.

    std::mutex mutex;  ///< Guards the members below.
    std::vector<std::string> events;  ///< Effects in call order.
    std::vector<std::string> errors;  ///< Lines given to `log_error`.
    std::function<void()> expiry;  ///< The `on_expiry` handed to `arm_limit`.
    std::chrono::milliseconds armed_limit {0};  ///< The limit handed to `arm_limit`.
    int force_exit_code {-1};  ///< Code given to `force_exit`.

    /**
     * @brief Fake clock reading as a time point.
     *
     * @param ms Milliseconds after `epoch`.
     * @return The time point.
     */
    clock_type::time_point at(long long ms) const {
      return epoch + std::chrono::milliseconds(ms);
    }

    /**
     * @brief Record an event.
     *
     * @param name Event name.
     */
    void record(const std::string &name) {
      std::lock_guard lock(mutex);
      events.push_back(name);
    }

    /**
     * @brief Copy of the recorded events.
     *
     * @return Events.
     */
    std::vector<std::string> snapshot() {
      std::lock_guard lock(mutex);
      return events;
    }

    /**
     * @brief Forget every recorded effect.
     */
    void reset() {
      std::lock_guard lock(mutex);
      events.clear();
      errors.clear();
      expiry = nullptr;
      force_exit_code = -1;
      in_end_sessions = false;
    }

    /**
     * @brief Number of events that start with a prefix.
     *
     * @param prefix Prefix.
     * @return Count.
     */
    int count(const std::string &prefix) {
      std::lock_guard lock(mutex);
      int n = 0;
      for (const auto &e : events) {
        if (e.rfind(prefix, 0) == 0) {
          ++n;
        }
      }
      return n;
    }

    /**
     * @brief Controller effects wired to this rig.
     *
     * @return The ops.
     */
    handoff::drain_ops_t ops() {
      handoff::drain_ops_t o;
      o.now = [this] {
        return at(clock_ms.load());
      };
      o.sessions = [this] {
        return sessions.load();
      };
      o.shutdown_requested = [this] {
        return shutdown.load();
      };
      o.end_sessions = [this](stream::stop_reason_e reason) {
        in_end_sessions = true;
        record("end_sessions:" + std::string(stream::to_string(reason)));
        while (hold_end_sessions.load()) {
          std::this_thread::sleep_for(1ms);
        }
      };
      o.release_devices = [this] {
        record("release_devices");
      };
      o.ensure_unmap_scope = [this] {
        record("ensure_unmap_scope");
      };
      o.arm_limit = [this](std::chrono::milliseconds limit, std::function<void()> on_expiry) {
        {
          std::lock_guard lock(mutex);
          armed_limit = limit;
          expiry = std::move(on_expiry);
        }
        record("arm_limit");
      };
      o.exit_graceful = [this] {
        record("exit_graceful");
      };
      o.force_exit = [this](int code) {
        {
          std::lock_guard lock(mutex);
          force_exit_code = code;
        }
        record("force_exit");
      };
      o.log_error = [this](const std::string &line) {
        {
          std::lock_guard lock(mutex);
          errors.push_back(line);
        }
        record("log_error");
      };
      return o;
    }
  };

  /**
   * @brief Fixture: resets the runtime, enables the feature with a 10 s drain deadline and builds a controller on a
   * fake clock. The controller ticks every millisecond; the grace is the documented 1 s and the limit the documented
   * 10 s, both in fake time, so no test waits for them.
   */
  class HandoffDrainControllerTest: public ::testing::Test {
  protected:
    void SetUp() override {
      handoff::runtime().reset_for_test();
      handoff::reset_drain_for_test();
      handoff::settings_t settings;
      settings.enabled = true;
      settings.deadline_s = 10;
      handoff::runtime().configure(settings);
      make();
    }

    void TearDown() override {
      rig.hold_end_sessions = false;
      ctrl.reset();
      handoff::reset_drain_for_test();
      handoff::runtime().reset_for_test();
    }

    /**
     * @brief (Re)build the controller with the rig's ops.
     */
    void make() {
      ctrl.reset();
      handoff::drain_timing_t timing;
      timing.tick = 1ms;
      timing.zero_grace = 1000ms;
      timing.forced_shutdown = 10000ms;
      ctrl = std::make_unique<handoff::drain_controller_t>(rig.ops(), timing);
    }

    /**
     * @brief Move the fake clock forward in small steps, giving the controller thread time to read every step.
     *
     * @param total_ms Total fake time to add.
     * @param step_ms Size of each step.
     */
    void advance(long long total_ms, long long step_ms = 100) {
      for (long long done = 0; done < total_ms; done += step_ms) {
        rig.clock_ms += std::min(step_ms, total_ms - done);
        std::this_thread::sleep_for(4ms);
      }
    }

    /**
     * @brief Begin a cycle at the current fake time.
     */
    void begin_now() {
      ctrl->begin(rig.at(rig.clock_ms.load()));
    }

    /**
     * @brief Wait until an event with this prefix was recorded.
     *
     * @param prefix Event prefix.
     * @return `true` when it was recorded within a second.
     */
    bool saw(const std::string &prefix) {
      return wait_until([&] {
        return rig.count(prefix) > 0;
      });
    }

    /**
     * @brief Give the controller time to (wrongly) do something, then report whether it did anything.
     *
     * @return `true` when no effect was recorded at all.
     */
    bool stays_silent(std::chrono::milliseconds window = 80ms) {
      std::this_thread::sleep_for(window);
      return rig.snapshot().empty();
    }

    rig_t rig;  ///< Effects, declared before the controller.
    std::unique_ptr<handoff::drain_controller_t> ctrl;  ///< Controller under test.
  };

  /**
   * @brief Index of an event in a list, or -1.
   *
   * @param events Events.
   * @param name Event name.
   * @return Index.
   */
  int index_of(const std::vector<std::string> &events, const std::string &name) {
    for (size_t i = 0; i < events.size(); ++i) {
      if (events[i] == name) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// No exit before handed_over()
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffDrainControllerTest, NoExitBeforeHandoverWithZeroSessionsPastGrace) {
  rig.sessions = 0;
  begin_now();
  advance(3000);
  EXPECT_TRUE(stays_silent()) << "With zero sessions for longer than the grace, the controller must still do nothing "
                                 "until handed_over() was called (the exit never precedes the main-PID handover).";
  EXPECT_EQ(ctrl->phase(), drain_phase_e::draining) << "Before the handover the phase must stay draining.";

  // Positive control: the same state exits once handed_over() was called.
  ctrl->handed_over();
  EXPECT_TRUE(saw("exit_graceful")) << "After handed_over() with zero sessions past the grace, the exit must happen.";
  EXPECT_TRUE(wait_until([&] {
    return ctrl->phase() == drain_phase_e::done;
  }))
    << "After the exit sequence handed the process to exit_graceful the phase must be done.";
}

TEST_F(HandoffDrainControllerTest, NoExitBeforeHandoverWhenDeadlinePassed) {
  rig.sessions = 2;
  begin_now();
  advance(11000, 1000);
  EXPECT_TRUE(stays_silent()) << "Past the drain deadline but before handed_over() nothing may be ended or exited.";

  // Positive control.
  ctrl->handed_over();
  EXPECT_TRUE(saw("exit_graceful")) << "After handed_over() past the deadline the exit must happen.";
}

TEST_F(HandoffDrainControllerTest, DeadlineIsLatchedUntilHandover) {
  rig.sessions = 2;
  begin_now();
  advance(11000, 1000);
  ASSERT_TRUE(stays_silent()) << "Precondition: no exit before the handover.";

  // The deadline passed earlier; the exit starts at the first tick after the handover, with the deadline reason.
  ctrl->handed_over();
  ASSERT_TRUE(saw("exit_graceful")) << "The latched deadline must start the exit right after handed_over().";
  EXPECT_EQ(rig.count("end_sessions:drain-deadline"), 1)
    << "Sessions ended because of the deadline must get the reason drain-deadline.";
}

TEST_F(HandoffDrainControllerTest, HandedOverBeforeBeginTakesEffectAtBegin) {
  rig.sessions = 0;
  ctrl->handed_over();
  EXPECT_EQ(ctrl->phase(), drain_phase_e::idle) << "handed_over() before begin() must not start anything.";
  EXPECT_TRUE(stays_silent(30ms)) << "handed_over() before begin() must have no effect before begin().";

  begin_now();
  advance(1500);
  EXPECT_TRUE(saw("exit_graceful")) << "handed_over() before begin() is remembered: after begin() and the grace the "
                                       "exit must happen.";
}

TEST_F(HandoffDrainControllerTest, WithoutDeadlineOrGraceNoExitEvenAfterHandover) {
  rig.sessions = 2;
  begin_now();
  ctrl->handed_over();
  advance(5000, 500);
  EXPECT_TRUE(stays_silent()) << "With running sessions and the deadline not reached, the exit must not start.";
  EXPECT_EQ(ctrl->phase(), drain_phase_e::draining);

  // Positive control: the sessions end, the grace passes, the exit happens with reason stop.
  rig.sessions = 0;
  advance(1500);
  EXPECT_TRUE(saw("exit_graceful")) << "Once the sessions reached zero for the grace, the exit must happen.";
  EXPECT_EQ(rig.count("end_sessions:stop"), 1) << "A zero-session exit must end sessions with the reason stop.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Zero-session grace
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffDrainControllerTest, ZeroGraceHoldsExitUntilItElapsed) {
  // SPEC-QUESTION: the spec does not say which clock measures zero_grace; these tests use the injected `now` clock
  // (the only clock the header names).
  rig.sessions = 0;
  begin_now();
  ctrl->handed_over();
  advance(800);
  EXPECT_TRUE(stays_silent(30ms)) << "The exit must not start before the session count read 0 for the whole grace.";

  advance(500);
  EXPECT_TRUE(saw("exit_graceful")) << "After the count read 0 for the whole grace the exit must start.";
}

TEST_F(HandoffDrainControllerTest, NonZeroReadingRestartsTheGrace) {
  rig.sessions = 0;
  begin_now();
  ctrl->handed_over();
  advance(700);
  rig.sessions = 1;
  advance(200);
  rig.sessions = 0;
  advance(700);
  EXPECT_TRUE(stays_silent(30ms)) << "A non-zero reading must restart the grace: 0.7 s, a session, then 0.7 s of zero is "
                                     "less than one continuous second.";

  advance(500);
  EXPECT_TRUE(saw("exit_graceful")) << "After one continuous second of zero the exit must start (control).";
}

TEST_F(HandoffDrainControllerTest, DeadlineReasonWinsWhenBothHold) {
  rig.sessions = 0;
  begin_now();
  advance(11000, 500);
  ctrl->handed_over();
  ASSERT_TRUE(saw("exit_graceful"));
  EXPECT_EQ(rig.count("end_sessions:drain-deadline"), 1)
    << "When the deadline passed and the sessions are zero, the reason must be drain-deadline.";
  EXPECT_EQ(rig.count("end_sessions:stop"), 0) << "The reason stop must not be used when the deadline passed.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Deadline
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffDrainControllerTest, DeadlineIsCountedFromSwitchTime) {
  rig.sessions = 2;
  rig.clock_ms = 4000;
  ctrl->begin(rig.at(2000));  // switch at 2 s, deadline 12 s
  ctrl->handed_over();
  advance(7000, 500);  // clock 11 s
  EXPECT_TRUE(stays_silent(30ms)) << "At 11 s the deadline (switch 2 s + 10 s) has not passed.";

  advance(2000, 500);  // clock 13 s
  EXPECT_TRUE(saw("exit_graceful")) << "After switch_time + deadline_s the exit must start.";
  EXPECT_EQ(rig.count("end_sessions:drain-deadline"), 1);
}

TEST_F(HandoffDrainControllerTest, SecondBeginInSameCycleChangesNothing) {
  rig.sessions = 2;
  begin_now();  // switch 0, deadline 10 s
  ctrl->handed_over();
  advance(5000, 500);
  begin_now();  // would move the deadline to 15 s if it counted
  advance(5500, 500);  // clock 10.5 s
  EXPECT_TRUE(saw("exit_graceful")) << "A second begin() in one cycle must not move the deadline: the first switch "
                                       "time wins.";
}

// ---------------------------------------------------------------------------------------------------------------------
// cancel()
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffDrainControllerTest, CancelDisarmsTimerAndExit) {
  // Control first: without cancel the same scenario exits.
  rig.sessions = 2;
  begin_now();
  ctrl->handed_over();
  advance(11000, 1000);
  ASSERT_TRUE(saw("exit_graceful")) << "Control: the scenario without cancel() must exit.";

  // Now the same with cancel().
  rig.reset();
  rig.clock_ms = 0;
  make();
  begin_now();
  ctrl->handed_over();
  EXPECT_TRUE(ctrl->cancel()) << "cancel() while draining must return true.";
  EXPECT_EQ(ctrl->phase(), drain_phase_e::idle) << "cancel() must return the phase to idle.";
  advance(11000, 1000);
  EXPECT_TRUE(stays_silent()) << "After cancel() the deadline timer and the exit must be disarmed: no end_sessions, "
                                 "release_devices, arm_limit or exit_graceful.";
}

TEST_F(HandoffDrainControllerTest, CancelWithZeroSessionsDisarmsGraceExit) {
  rig.sessions = 0;
  begin_now();
  ctrl->handed_over();
  EXPECT_TRUE(ctrl->cancel());
  advance(3000);
  EXPECT_TRUE(stays_silent()) << "After cancel() a zero-session count must not start the exit.";
}

TEST_F(HandoffDrainControllerTest, CancelDisarmsLimit) {
  begin_now();
  EXPECT_TRUE(ctrl->cancel());
  ctrl->on_limit_expired();
  EXPECT_EQ(rig.count("arm_limit"), 0) << "No limit may be armed before the exit starts.";
  EXPECT_EQ(rig.count("force_exit"), 0) << "on_limit_expired() after cancel() must not force an exit.";
  EXPECT_EQ(rig.count("log_error"), 0) << "on_limit_expired() after cancel() must log nothing.";
}

TEST_F(HandoffDrainControllerTest, CancelWithoutBeginReturnsTrue) {
  EXPECT_TRUE(ctrl->cancel()) << "cancel() of a controller that was never begun returns true (nothing to stop).";
  EXPECT_EQ(ctrl->phase(), drain_phase_e::idle);
}

TEST_F(HandoffDrainControllerTest, CancelTwiceIsHarmless) {
  begin_now();
  EXPECT_TRUE(ctrl->cancel());
  EXPECT_TRUE(ctrl->cancel()) << "A second cancel() returns true as well.";
  EXPECT_EQ(ctrl->phase(), drain_phase_e::idle);
}

TEST_F(HandoffDrainControllerTest, CancelAfterExitStartedReturnsFalseAndStopsNothing) {
  rig.sessions = 1;
  rig.hold_end_sessions = true;
  begin_now();
  ctrl->handed_over();
  advance(11000, 1000);
  ASSERT_TRUE(wait_until([&] {
    return rig.in_end_sessions.load();
  }))
    << "Precondition: the exit sequence started and is inside end_sessions.";
  EXPECT_EQ(ctrl->phase(), drain_phase_e::exiting);

  EXPECT_FALSE(ctrl->cancel()) << "cancel() while exiting must return false at once (the exit is committed).";
  EXPECT_EQ(rig.count("arm_limit"), 0) << "cancel() must not itself arm or skip anything.";

  rig.hold_end_sessions = false;
  EXPECT_TRUE(saw("exit_graceful")) << "The committed exit must continue after a refused cancel().";
  EXPECT_FALSE(ctrl->cancel()) << "cancel() after the exit finished (phase done) must return false too.";
}

TEST_F(HandoffDrainControllerTest, BeginAfterCancelIsFreshCycleWithoutHandover) {
  rig.sessions = 0;
  begin_now();
  ctrl->handed_over();
  ASSERT_TRUE(ctrl->cancel());

  begin_now();  // fresh cycle: handed_over is false again
  EXPECT_EQ(ctrl->phase(), drain_phase_e::draining) << "begin() after cancel() must start a draining cycle again.";
  advance(3000);
  EXPECT_TRUE(stays_silent()) << "The handover of the cancelled cycle must not carry over: a new cycle needs a new "
                                 "handed_over() before it may exit.";

  // Positive control: the new cycle exits after its own handed_over().
  ctrl->handed_over();
  EXPECT_TRUE(saw("exit_graceful")) << "After handed_over() of the new cycle the exit must happen.";
}

TEST_F(HandoffDrainControllerTest, BeginAfterCancelHasNewDeadlineAndNoLatch) {
  rig.sessions = 2;
  begin_now();  // switch 0, deadline 10 s
  advance(11000, 1000);  // the first cycle's deadline passed (latched)
  ASSERT_TRUE(ctrl->cancel());

  begin_now();  // switch 11 s, deadline 21 s
  ctrl->handed_over();
  advance(6000, 500);  // clock 17 s: past the old deadline, before the new one
  EXPECT_TRUE(stays_silent(30ms)) << "The latch of the cancelled cycle's deadline must be gone, and the deadline must "
                                     "be the new switch time + deadline_s.";

  advance(5000, 500);  // clock 22 s
  EXPECT_TRUE(saw("exit_graceful")) << "The new cycle's own deadline must start the exit (control).";
  EXPECT_EQ(rig.count("end_sessions:drain-deadline"), 1);
}

TEST_F(HandoffDrainControllerTest, BeginWhileExitingOrDoneChangesNothing) {
  rig.sessions = 0;
  begin_now();
  ctrl->handed_over();
  advance(1500);
  ASSERT_TRUE(saw("exit_graceful"));
  ASSERT_TRUE(wait_until([&] {
    return ctrl->phase() == drain_phase_e::done;
  }));
  const auto before = rig.snapshot();

  begin_now();
  ctrl->handed_over();
  advance(3000);
  EXPECT_EQ(ctrl->phase(), drain_phase_e::done) << "begin() while done must change nothing.";
  EXPECT_EQ(rig.snapshot(), before) << "begin() while done must not run the sequence again.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Exit order and the bounded exit
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffDrainControllerTest, ExitSequenceOrder) {
  rig.sessions = 0;
  begin_now();
  ctrl->handed_over();
  advance(1500);
  ASSERT_TRUE(saw("exit_graceful"));

  const std::vector<std::string> expected {
    "ensure_unmap_scope",
    "end_sessions:stop",
    "release_devices",
    "arm_limit",
    "exit_graceful",
  };
  EXPECT_EQ(rig.snapshot(), expected) << "The exit must run: ensure the unmap scope, end sessions, release devices, "
                                         "arm the limit, and only then stop the servers (exit_graceful), each once.";
  EXPECT_EQ(rig.count("force_exit"), 0) << "Nothing may force the exit before the limit expires.";
  std::lock_guard lock(rig.mutex);
  EXPECT_EQ(rig.armed_limit, 10000ms) << "The forced-shutdown limit must be the 10 s of the signal handlers.";
}

TEST_F(HandoffDrainControllerTest, LimitIsNotArmedWhileSessionsAreStillEnding) {
  rig.sessions = 1;
  rig.hold_end_sessions = true;
  begin_now();
  ctrl->handed_over();
  advance(11000, 1000);
  ASSERT_TRUE(wait_until([&] {
    return rig.in_end_sessions.load();
  }));
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(rig.count("arm_limit"), 0) << "Ending sessions is outside the limit: it must not be armed while "
                                          "end_sessions has not returned.";
  EXPECT_EQ(rig.count("release_devices"), 0) << "Devices are released only after the sessions ended.";
  EXPECT_EQ(rig.count("exit_graceful"), 0);

  rig.hold_end_sessions = false;
  ASSERT_TRUE(saw("exit_graceful"));
  const auto events = rig.snapshot();
  EXPECT_LT(index_of(events, "end_sessions:drain-deadline"), index_of(events, "release_devices"));
  EXPECT_LT(index_of(events, "release_devices"), index_of(events, "arm_limit"));
  EXPECT_LT(index_of(events, "arm_limit"), index_of(events, "exit_graceful"));
}

TEST_F(HandoffDrainControllerTest, UnmapScopeIsEnsuredBeforeSessionsAreEnded) {
  rig.sessions = 1;
  begin_now();
  EXPECT_EQ(rig.count("ensure_unmap_scope"), 0) << "Nothing may run while draining before the handover.";
  ctrl->handed_over();
  advance(11000, 1000);
  ASSERT_TRUE(saw("exit_graceful"));
  const auto events = rig.snapshot();
  ASSERT_GE(index_of(events, "ensure_unmap_scope"), 0) << "The exit must ensure the UPnP unmap scope.";
  EXPECT_LT(index_of(events, "ensure_unmap_scope"), index_of(events, "end_sessions:drain-deadline"))
    << "The unmap scope must be ensured before any session ends, devices are released or any exit path can run.";
  EXPECT_LT(index_of(events, "ensure_unmap_scope"), index_of(events, "exit_graceful"));
}

TEST_F(HandoffDrainControllerTest, LimitExpiryLogsStageAndForcesExit) {
  rig.sessions = 0;
  handoff::note_shutdown_stage("RTSP server thread");
  begin_now();
  ctrl->handed_over();
  advance(1500);
  ASSERT_TRUE(saw("exit_graceful"));
  EXPECT_EQ(rig.count("force_exit"), 0) << "Control: nothing forces the exit before the limit expires.";
  EXPECT_EQ(rig.count("log_error"), 0) << "Control: nothing is logged as an error before the limit expires.";

  std::function<void()> expiry;
  {
    std::lock_guard lock(rig.mutex);
    expiry = rig.expiry;
  }
  ASSERT_TRUE(static_cast<bool>(expiry)) << "arm_limit must receive the on_expiry function.";
  expiry();

  const int forced = rig.count("force_exit");
  std::lock_guard lock(rig.mutex);
  ASSERT_EQ(rig.errors.size(), 1u) << "Limit expiry must log exactly one Error line.";
  EXPECT_NE(rig.errors[0].find("RTSP server thread"), std::string::npos)
    << "The Error must name the stop that did not finish (the last note_shutdown_stage name): " << rig.errors[0];
  EXPECT_NE(rig.errors[0].find("did not finish"), std::string::npos) << rig.errors[0];
  EXPECT_NE(rig.errors[0].find("10"), std::string::npos) << "The Error must name the 10 s limit: " << rig.errors[0];
  EXPECT_EQ(rig.force_exit_code, 1) << "The forced exit must use code 1.";
  EXPECT_EQ(forced, 1) << "The exit must go through the force_exit seam, once.";
}

TEST_F(HandoffDrainControllerTest, LimitExpiryWithoutStageNamesUnknownStop) {
  rig.sessions = 0;
  begin_now();
  ctrl->handed_over();
  advance(1500);
  ASSERT_TRUE(saw("arm_limit"));
  ctrl->on_limit_expired();
  std::lock_guard lock(rig.mutex);
  ASSERT_EQ(rig.errors.size(), 1u);
  EXPECT_NE(rig.errors[0].find("unknown stop"), std::string::npos)
    << "With no stage given, the Error must say 'unknown stop': " << rig.errors[0];
}

TEST_F(HandoffDrainControllerTest, LimitExpiryRunsAtMostOnce) {
  rig.sessions = 0;
  begin_now();
  ctrl->handed_over();
  advance(1500);
  ASSERT_TRUE(saw("arm_limit"));
  ctrl->on_limit_expired();
  ctrl->on_limit_expired();
  EXPECT_EQ(rig.count("force_exit"), 1) << "A second on_limit_expired() must do nothing.";
  EXPECT_EQ(rig.count("log_error"), 1) << "A second on_limit_expired() must log nothing.";
}

TEST_F(HandoffDrainControllerTest, LimitExpiryIsNoOpBeforeBegin) {
  ctrl->on_limit_expired();
  EXPECT_TRUE(stays_silent(20ms)) << "on_limit_expired() with no cycle begun must log nothing and not exit.";
}

TEST_F(HandoffDrainControllerTest, LimitExpiryIsNoOpWhileDrainingBeforeTheLimitWasArmed) {
  rig.sessions = 2;
  begin_now();
  ctrl->handed_over();
  ctrl->on_limit_expired();
  EXPECT_TRUE(stays_silent(30ms)) << "The limit is armed only by the exit sequence: an on_limit_expired() before that "
                                     "must do nothing and must not start the exit.";
  EXPECT_EQ(ctrl->phase(), drain_phase_e::draining);
}

TEST_F(HandoffDrainControllerTest, LimitExpiryIsNoOpWhileSessionsAreStillEnding) {
  rig.sessions = 1;
  rig.hold_end_sessions = true;
  begin_now();
  ctrl->handed_over();
  advance(11000, 1000);
  ASSERT_TRUE(wait_until([&] {
    return rig.in_end_sessions.load();
  }));
  ctrl->on_limit_expired();
  EXPECT_EQ(rig.count("force_exit"), 0) << "The limit was not armed yet (sessions are still ending): on_limit_expired() "
                                           "must be a no-op.";
  EXPECT_EQ(rig.count("log_error"), 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shutdown (SIGTERM) overlap
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffDrainControllerTest, ShutdownRequestedBeforeExitStartsMeansNoSequence) {
  // Control: without a shutdown request the same scenario runs the whole sequence.
  rig.sessions = 0;
  begin_now();
  ctrl->handed_over();
  advance(1500);
  ASSERT_TRUE(saw("exit_graceful")) << "Control: the scenario without a shutdown request must exit.";
  ASSERT_EQ(rig.count("end_sessions"), 1) << "Control: the sequence ends the sessions.";

  rig.reset();
  rig.clock_ms = 0;
  rig.shutdown = true;
  make();
  begin_now();
  ctrl->handed_over();
  advance(11000, 1000);
  ASSERT_TRUE(saw("ensure_unmap_scope")) << "On the shutdown path the unmap scope must still be ensured first.";
  std::this_thread::sleep_for(60ms);
  EXPECT_EQ(rig.count("ensure_unmap_scope"), 1) << "The unmap scope must be ensured exactly once.";
  EXPECT_EQ(rig.count("end_sessions"), 0) << "A shutdown request: the signal path ends the sessions, not the controller.";
  EXPECT_EQ(rig.count("release_devices"), 0) << "A shutdown request: no device release by the controller.";
  EXPECT_EQ(rig.count("arm_limit"), 0) << "The controller must never arm a second limit after a shutdown request.";
  EXPECT_EQ(rig.count("exit_graceful"), 0) << "A shutdown request: the signal path owns the exit.";
  EXPECT_EQ(rig.count("force_exit"), 0);
  EXPECT_EQ(rig.snapshot().size(), 1u) << "ensure_unmap_scope must be the only effect on the shutdown path.";
}

TEST_F(HandoffDrainControllerTest, CancelFromShutdownPathJoinsTheThread) {
  rig.sessions = 0;
  rig.shutdown = true;
  begin_now();
  ctrl->handed_over();
  advance(1500);
  ASSERT_TRUE(saw("ensure_unmap_scope"));
  std::this_thread::sleep_for(30ms);
  EXPECT_EQ(rig.snapshot(), std::vector<std::string>({"ensure_unmap_scope"}))
    << "On the shutdown path the only effect is ensure_unmap_scope, once.";
  // SPEC-QUESTION: the phase after the controller ended because of a shutdown request is not stated by the spec
  // (drain.h says the controller ends); only the result of cancel() is asserted.
  EXPECT_TRUE(ctrl->cancel()) << "cancel() from the shutdown path must join the thread and return true when the exit "
                                 "sequence never started.";
  EXPECT_EQ(rig.count("ensure_unmap_scope"), 1) << "cancel() must not ensure the scope again.";
}

TEST_F(HandoffDrainControllerTest, DestructorWhileDrainingEndsWithoutEffects) {
  rig.sessions = 2;
  begin_now();
  ctrl->handed_over();
  ctrl.reset();
  advance(11000, 1000);
  EXPECT_TRUE(stays_silent(20ms)) << "Destroying a draining controller must stop it without running the exit.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Production defaults of the unmap scope (ensure_unmap_scope)
// ---------------------------------------------------------------------------------------------------------------------

namespace {
  /**
   * @brief Fixture for the default `ensure_unmap_scope`: everything but that one effect is faked.
   */
  class HandoffDrainDefaultUnmapScopeTest: public ::testing::Test {
  protected:
    void SetUp() override {
      handoff::runtime().reset_for_test();
      _saved_port = config::sunshine.port;
      config::sunshine.port = 47989;
      handoff::settings_t settings;
      settings.enabled = true;
      settings.udp_offset = 100;
      settings.deadline_s = 10;
      handoff::runtime().configure(settings);
      _saved_scope = upnp::unmap_scope();
    }

    void TearDown() override {
      upnp::set_unmap_scope(_saved_scope);
      config::sunshine.port = _saved_port;
      handoff::runtime().reset_for_test();
    }

    /**
     * @brief Run one full exit with the default `ensure_unmap_scope`.
     *
     * @return `true` when the exit sequence reached the exit seam.
     */
    bool run_exit(bool shutdown = false) {
      rig_t rig;
      rig.shutdown = shutdown;
      auto ops = rig.ops();
      ops.ensure_unmap_scope = nullptr;  // production default
      handoff::drain_timing_t timing;
      timing.tick = 1ms;
      timing.zero_grace = 10ms;
      handoff::drain_controller_t ctrl(ops, timing);
      ctrl.begin(rig.at(0));
      ctrl.handed_over();
      for (int i = 0; i < 50; ++i) {
        rig.clock_ms += 20;
        std::this_thread::sleep_for(3ms);
        if (shutdown ? upnp::unmap_scope() != upnp::unmap_scope_e::all : rig.count("exit_graceful") > 0) {
          return true;
        }
      }
      return wait_until([&] {
        return shutdown ? upnp::unmap_scope() != upnp::unmap_scope_e::all : rig.count("exit_graceful") > 0;
      });
    }

    std::uint16_t _saved_port {0};  ///< Port to restore.
    upnp::unmap_scope_e _saved_scope {upnp::unmap_scope_e::all};  ///< Scope to restore.
  };
}  // namespace

TEST_F(HandoffDrainDefaultUnmapScopeTest, ScopeAllIsNarrowedBeforeExit) {
  upnp::set_unmap_scope(upnp::unmap_scope_e::all);
  ASSERT_TRUE(run_exit());
  // SPEC-QUESTION: drain.h says the default sets `udp_only`, or `none` when both processes share one UDP base. With
  // the default offset of 100 the bases differ, so `udp_only` is asserted; the test only requires "not all" strictly.
  EXPECT_NE(upnp::unmap_scope(), upnp::unmap_scope_e::all)
    << "The default ensure_unmap_scope must narrow a scope that is still all (the exit must never unmap the shared TCP ports).";
  EXPECT_EQ(upnp::unmap_scope(), upnp::unmap_scope_e::udp_only);
}

TEST_F(HandoffDrainDefaultUnmapScopeTest, ShutdownPathNarrowsScopeAll) {
  upnp::set_unmap_scope(upnp::unmap_scope_e::all);
  ASSERT_TRUE(run_exit(true)) << "When a shutdown request ends the sequence, the default ensure_unmap_scope must still "
                                 "narrow a scope of all before the controller ends.";
  EXPECT_EQ(upnp::unmap_scope(), upnp::unmap_scope_e::udp_only);
}

TEST_F(HandoffDrainDefaultUnmapScopeTest, ScopeUdpOnlyIsKept) {
  upnp::set_unmap_scope(upnp::unmap_scope_e::udp_only);
  ASSERT_TRUE(run_exit());
  EXPECT_EQ(upnp::unmap_scope(), upnp::unmap_scope_e::udp_only) << "An already narrowed scope must be left alone.";
}

TEST_F(HandoffDrainDefaultUnmapScopeTest, ScopeNoneIsKept) {
  upnp::set_unmap_scope(upnp::unmap_scope_e::none);
  ASSERT_TRUE(run_exit());
  EXPECT_EQ(upnp::unmap_scope(), upnp::unmap_scope_e::none) << "A scope of none must never be changed.";
}

// ---------------------------------------------------------------------------------------------------------------------
// shutdown stage and device release registration
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffDrainStageTest, ShutdownStageIsLastNoted) {
  handoff::reset_drain_for_test();
  EXPECT_EQ(handoff::shutdown_stage(), "") << "With no stage noted the name is empty.";
  handoff::note_shutdown_stage("client server thread (nvhttp)");
  EXPECT_EQ(handoff::shutdown_stage(), "client server thread (nvhttp)");
  handoff::note_shutdown_stage("task pool");
  EXPECT_EQ(handoff::shutdown_stage(), "task pool") << "The last name given must win.";
  handoff::reset_drain_for_test();
  EXPECT_EQ(handoff::shutdown_stage(), "") << "reset_drain_for_test() must forget the stage.";
}

TEST(HandoffDrainStageTest, ShutdownStageIsThreadSafe) {
  handoff::reset_drain_for_test();
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([i] {
      for (int n = 0; n < 200; ++n) {
        handoff::note_shutdown_stage(i % 2 ? "web UI server thread (confighttp)" : "RTSP server thread");
        const auto stage = handoff::shutdown_stage();
        EXPECT_TRUE(stage.empty() || stage == "web UI server thread (confighttp)" || stage == "RTSP server thread")
          << "A torn stage name was read: " << stage;
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }
  handoff::reset_drain_for_test();
}

TEST_F(HandoffDrainControllerTest, DefaultReleaseDevicesCallsRegisteredFunction) {
  std::atomic<int> released {0};
  handoff::register_device_release([&released] {
    ++released;
  });
  auto ops = rig.ops();
  ops.release_devices = nullptr;  // production default: the registered function
  handoff::drain_timing_t timing;
  timing.tick = 1ms;
  timing.zero_grace = 10ms;
  ctrl.reset();
  ctrl = std::make_unique<handoff::drain_controller_t>(ops, timing);
  begin_now();
  ctrl->handed_over();
  advance(200, 20);
  ASSERT_TRUE(saw("exit_graceful"));
  EXPECT_EQ(released.load(), 1) << "The default release_devices must call the function given to "
                                   "register_device_release(), once.";
  const auto events = rig.snapshot();
  EXPECT_LT(index_of(events, "end_sessions:stop"), index_of(events, "arm_limit"));
  ctrl.reset();
  handoff::register_device_release({});
}

TEST_F(HandoffDrainControllerTest, SecondRegistrationReplacesTheFirst) {
  std::atomic<int> first {0};
  std::atomic<int> second {0};
  handoff::register_device_release([&first] {
    ++first;
  });
  handoff::register_device_release([&second] {
    ++second;
  });
  auto ops = rig.ops();
  ops.release_devices = nullptr;
  handoff::drain_timing_t timing;
  timing.tick = 1ms;
  timing.zero_grace = 10ms;
  ctrl.reset();
  ctrl = std::make_unique<handoff::drain_controller_t>(ops, timing);
  begin_now();
  ctrl->handed_over();
  advance(200, 20);
  ASSERT_TRUE(saw("exit_graceful"));
  EXPECT_EQ(first.load(), 0) << "A second registration replaces the first.";
  EXPECT_EQ(second.load(), 1);
  ctrl.reset();
  handoff::register_device_release({});
}

// ---------------------------------------------------------------------------------------------------------------------
// The session notifier
// ---------------------------------------------------------------------------------------------------------------------

namespace {
  /**
   * @brief Recorder for the notifier's effects.
   */
  struct notifier_rig_t {
    std::atomic<int> sessions {2};  ///< Fake session count.
    std::atomic<mode_e> mode {mode_e::normal};  ///< Fake mode.
    std::atomic<state_e> current {state_e::none};  ///< Fake own handoff state.
    std::atomic<bool> has_peer {false};  ///< Whether `peer_state` returns a value.
    std::atomic<state_e> peer {state_e::none};  ///< The peer's state.
    std::atomic<bool> shutdown {false};  ///< Fake shutdown flag.

    std::mutex mutex;  ///< Guards the vectors.
    std::vector<int> written_sessions;  ///< Values given to `write_sessions`.
    std::vector<state_e> sent;  ///< Values given to `send_state`.
    std::vector<state_e> written_states;  ///< Values given to `write_state`.

    /**
     * @brief Effects wired to this rig.
     *
     * @return The ops.
     */
    handoff::notifier_ops_t ops() {
      handoff::notifier_ops_t o;
      o.sessions = [this] {
        return sessions.load();
      };
      o.write_sessions = [this](int n) {
        std::lock_guard lock(mutex);
        written_sessions.push_back(n);
      };
      o.mode = [this] {
        return mode.load();
      };
      o.current_state = [this] {
        return current.load();
      };
      o.write_state = [this](state_e s) {
        std::lock_guard lock(mutex);
        written_states.push_back(s);
      };
      o.send_state = [this](state_e s) {
        std::lock_guard lock(mutex);
        sent.push_back(s);
      };
      o.peer_state = [this]() -> std::optional<state_e> {
        if (!has_peer.load()) {
          return std::nullopt;
        }
        return peer.load();
      };
      o.shutdown_requested = [this] {
        return shutdown.load();
      };
      return o;
    }

    /**
     * @brief Copy of the written session counts.
     *
     * @return Values.
     */
    std::vector<int> sessions_written() {
      std::lock_guard lock(mutex);
      return written_sessions;
    }

    /**
     * @brief Copy of the sent states.
     *
     * @return Values.
     */
    std::vector<state_e> states_sent() {
      std::lock_guard lock(mutex);
      return sent;
    }

    /**
     * @brief Copy of the states written to the instance file.
     *
     * @return Values.
     */
    std::vector<state_e> states_written() {
      std::lock_guard lock(mutex);
      return written_states;
    }
  };

  /**
   * @brief Fixture: a notifier whose period is long (5 s), so only pokes drive passes, unless a test says otherwise.
   */
  class HandoffDrainNotifierTest: public ::testing::Test {
  protected:
    void SetUp() override {
      handoff::runtime().reset_for_test();
      make(5000ms);
    }

    void TearDown() override {
      if (notifier) {
        notifier->stop();
      }
      notifier.reset();
      handoff::runtime().reset_for_test();
    }

    /**
     * @brief Build the notifier.
     *
     * @param period Notifier period.
     */
    void make(std::chrono::milliseconds period) {
      notifier.reset();
      handoff::notifier_timing_t timing;
      timing.period = period;
      notifier = std::make_unique<handoff::session_notifier_t>(rig.ops(), timing);
    }

    /**
     * @brief Poke and wait for one more pass.
     *
     * @return `true` when a pass completed within a second.
     */
    bool pass() {
      const auto before = notifier->passes();
      notifier->poke();
      return wait_until([&] {
        return notifier->passes() > before;
      });
    }

    /**
     * @brief Start and wait for the first pass.
     */
    void start_and_settle() {
      notifier->start();
      ASSERT_TRUE(wait_until([&] {
        return notifier->passes() >= 1;
      }))
        << "The first pass must run at once after start().";
    }

    notifier_rig_t rig;  ///< Effects.
    std::unique_ptr<handoff::session_notifier_t> notifier;  ///< Notifier under test.
  };
}  // namespace

TEST_F(HandoffDrainNotifierTest, FirstPassWritesSessionsAtOnce) {
  start_and_settle();
  EXPECT_EQ(rig.sessions_written(), std::vector<int>({2})) << "The first pass must always write the session count.";
}

TEST_F(HandoffDrainNotifierTest, UnchangedCountIsNotRewritten) {
  start_and_settle();
  ASSERT_TRUE(pass());
  ASSERT_TRUE(pass());
  EXPECT_EQ(rig.sessions_written().size(), 1u) << "An unchanged session count must not be written again.";

  // Control: a changed count is written.
  rig.sessions = 3;
  ASSERT_TRUE(pass());
  EXPECT_EQ(rig.sessions_written(), std::vector<int>({2, 3})) << "A changed session count must be written.";
}

TEST_F(HandoffDrainNotifierTest, PokeWakesBeforeThePeriod) {
  start_and_settle();
  rig.sessions = 7;
  EXPECT_TRUE(pass()) << "poke() must wake the notifier without waiting for its 5 s period.";
  EXPECT_EQ(rig.sessions_written().back(), 7);
}

TEST_F(HandoffDrainNotifierTest, PokeBeforeStartGivesAPassAtStart) {
  notifier->poke();
  notifier->start();
  EXPECT_TRUE(wait_until([&] {
    return notifier->passes() >= 1;
  }))
    << "poke() before start() must be harmless and the first pass must happen at start().";
}

TEST_F(HandoffDrainNotifierTest, PeriodDrivesPassesWithoutPoke) {
  make(20ms);
  start_and_settle();
  const auto first = notifier->passes();
  EXPECT_TRUE(wait_until([&] {
    return notifier->passes() >= first + 2;
  }))
    << "Without pokes the notifier must pass at least once per period.";
}

TEST_F(HandoffDrainNotifierTest, StartIsIdempotent) {
  start_and_settle();
  notifier->start();
  ASSERT_TRUE(pass());
  EXPECT_EQ(rig.sessions_written().size(), 1u) << "A second start() must do nothing.";
}

TEST_F(HandoffDrainNotifierTest, StopIsIdempotentAndSafeWithoutStart) {
  notifier->stop();  // never started
  start_and_settle();
  notifier->stop();
  notifier->stop();
  const auto after = notifier->passes();
  notifier->poke();
  std::this_thread::sleep_for(40ms);
  EXPECT_EQ(notifier->passes(), after) << "poke() after stop() must do nothing.";
}

TEST_F(HandoffDrainNotifierTest, DrainingSendsStateOnFirstPassOnly) {
  rig.mode = mode_e::draining;
  rig.current = state_e::draining;
  start_and_settle();
  EXPECT_EQ(rig.states_sent(), std::vector<state_e>({state_e::draining}))
    << "A draining process must send HANDOFF_STATE on its first pass (nothing sent yet counts as different).";
  ASSERT_TRUE(pass());
  ASSERT_TRUE(pass());
  EXPECT_EQ(rig.states_sent().size(), 1u) << "An unchanged count and state must not be sent again.";
}

TEST_F(HandoffDrainNotifierTest, DrainingResendsOnCountChange) {
  rig.mode = mode_e::draining;
  rig.current = state_e::draining;
  start_and_settle();
  rig.sessions = 1;
  ASSERT_TRUE(pass());
  EXPECT_EQ(rig.states_sent().size(), 2u) << "A changed session count must make a draining process send its state.";
}

TEST_F(HandoffDrainNotifierTest, DrainingResendsOnStateChange) {
  rig.mode = mode_e::draining;
  rig.current = state_e::draining;
  start_and_settle();
  rig.current = state_e::handover_failed;
  ASSERT_TRUE(pass());
  EXPECT_EQ(rig.states_sent(), std::vector<state_e>({state_e::draining, state_e::handover_failed}))
    << "A changed state must be sent.";
}

TEST_F(HandoffDrainNotifierTest, NonDrainingNeverSendsState) {
  rig.mode = mode_e::normal;
  start_and_settle();
  rig.sessions = 5;
  ASSERT_TRUE(pass());
  EXPECT_TRUE(rig.states_sent().empty()) << "Only a draining process sends HANDOFF_STATE.";

  // Control: the same notifier sends once the mode is draining.
  rig.mode = mode_e::draining;
  rig.current = state_e::draining;
  ASSERT_TRUE(pass());
  EXPECT_FALSE(rig.states_sent().empty()) << "Control: a draining process must send HANDOFF_STATE.";
}

TEST_F(HandoffDrainNotifierTest, DrainingNeverMirrorsPeer) {
  rig.mode = mode_e::draining;
  rig.current = state_e::draining;
  rig.has_peer = true;
  rig.peer = state_e::kill_pending;
  start_and_settle();
  ASSERT_TRUE(pass());
  EXPECT_TRUE(rig.states_written().empty()) << "A draining process never mirrors a peer into its instance file.";
}

TEST_F(HandoffDrainNotifierTest, NonDrainingMirrorsDifferingPeerState) {
  rig.has_peer = true;
  rig.peer = state_e::draining;
  rig.current = state_e::none;
  start_and_settle();
  EXPECT_EQ(rig.states_written(), std::vector<state_e>({state_e::draining}))
    << "An active process with a draining peer must mirror the peer's state into its own file.";
}

TEST_F(HandoffDrainNotifierTest, NoPeerWritesNoState) {
  rig.has_peer = false;
  rig.current = state_e::kill_pending;
  start_and_settle();
  ASSERT_TRUE(pass());
  EXPECT_TRUE(rig.states_written().empty()) << "With no peer, the state the old side set itself (kill_pending) must "
                                               "never be overwritten.";

  // Control: with a peer whose state differs the notifier does write.
  rig.has_peer = true;
  rig.peer = state_e::none;
  ASSERT_TRUE(pass());
  EXPECT_EQ(rig.states_written(), std::vector<state_e>({state_e::none})) << "Control: a vanished peer reads none and "
                                                                            "must bring the file back to none.";
}

TEST_F(HandoffDrainNotifierTest, EqualPeerStateIsNotWritten) {
  rig.has_peer = true;
  rig.peer = state_e::draining;
  rig.current = state_e::draining;
  start_and_settle();
  ASSERT_TRUE(pass());
  EXPECT_TRUE(rig.states_written().empty()) << "A peer state equal to the file's must not be rewritten.";
}

TEST_F(HandoffDrainNotifierTest, ShutdownRequestMakesLastPassAndEnds) {
  start_and_settle();
  const auto before = notifier->passes();
  if (g_global_notifier_started.load()) {
    g_global_notifier_may_have_ended = true;
  }
  handoff::runtime().request_shutdown();
  ASSERT_TRUE(wait_until([&] {
    return notifier->passes() > before;
  }))
    << "A readable shutdown fd must make the notifier do one last pass.";
  const auto ended = notifier->passes();
  rig.sessions = 9;
  notifier->poke();
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(notifier->passes(), ended) << "After the shutdown the notifier stays ended.";
}

namespace {
  /**
   * @brief A draining-peer double with a settable existence and announced state.
   */
  class fake_session_peer_t: public handoff::session_peer_t {
  public:
    bool exists() override {
      return present.load();
    }

    bool gone() override {
      return !present.load();
    }

    reply_e end_all(std::chrono::milliseconds) override {
      return reply_e::no_reply;
    }

    std::optional<std::vector<handoff::protocol::session_entry_t>> list(std::chrono::milliseconds) override {
      return std::nullopt;
    }

    std::optional<rtsp_stream::close_result_e> close(std::uint32_t, std::chrono::milliseconds) override {
      return std::nullopt;
    }

    std::optional<std::uint32_t> close_cert(std::string_view, std::chrono::milliseconds) override {
      return std::nullopt;
    }

    state_e state() override {
      return announced.load();
    }

    std::atomic<bool> present {true};  ///< `exists()`.
    std::atomic<state_e> announced {state_e::none};  ///< `state()`.
  };
}  // namespace

TEST(HandoffDrainDefaultPeerTest, DefaultPeerStateMirrorsSessionPeerIntoInstanceFile) {
  handoff::runtime().reset_for_test();
  const auto dir = std::filesystem::temp_directory_path() / ("sunshine-drain-peer-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  handoff::instance_t initial;
  initial.pid = ::getpid();
  initial.start_time = 1;
  initial.role = handoff::role_e::active;
  initial.udp_base = 47989;
  ASSERT_TRUE(handoff::runtime().open_instance(dir, initial));

  auto peer = std::make_shared<fake_session_peer_t>();
  peer->announced = state_e::draining;
  handoff::runtime().set_peer(peer);

  // Only the session count is faked; current_state, write_state and peer_state take their production defaults.
  std::atomic<int> sessions {0};
  handoff::notifier_ops_t ops;
  ops.sessions = [&sessions] {
    return sessions.load();
  };
  handoff::notifier_timing_t timing;
  timing.period = 5000ms;
  auto current = [] {
    const auto i = handoff::runtime().instance_current();
    return i ? i->handoff : state_e::none;
  };
  {
    handoff::session_notifier_t notifier(ops, timing);
    notifier.start();
    ASSERT_TRUE(wait_until([&] {
      return notifier.passes() >= 1;
    }));
    EXPECT_EQ(current(), state_e::draining) << "The default peer_state must read the session peer's announced state "
                                               "and the default write_state must put it in the instance file.";

    peer->announced = state_e::handover_failed;
    notifier.poke();
    EXPECT_TRUE(wait_until([&] {
      return current() == state_e::handover_failed;
    }))
      << "A changed announced state must follow into the file.";

    peer->present = false;
    notifier.poke();
    EXPECT_TRUE(wait_until([&] {
      return current() == state_e::none;
    }))
      << "A peer that no longer exists reads none, which returns the file to none.";

    handoff::runtime().set_peer(nullptr);
    handoff::runtime().instance_set_state(state_e::kill_pending);
    const auto before = notifier.passes();
    notifier.poke();
    ASSERT_TRUE(wait_until([&] {
      return notifier.passes() > before;
    }));
    EXPECT_EQ(current(), state_e::kill_pending) << "With no peer the state the old side set itself must be kept.";
    notifier.stop();
  }
  // SPEC-QUESTION: the default mapping for a peer that is a plain drain_peer_t (no session_peer_t) is not asserted:
  // drain.h's default text reads `peer->state()` and does not say what a plain double reports.
  handoff::runtime().set_peer(nullptr);
  handoff::runtime().instance_remove();
  handoff::runtime().reset_for_test();
  std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------------------------------------------------
// Controller and notifier together
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffDrainControllerTest, NotifierKeepsRunningAfterCancelAndSendsNoStateAfterRollback) {
  notifier_rig_t nrig;
  nrig.mode = mode_e::draining;
  nrig.current = state_e::draining;
  handoff::notifier_timing_t ntiming;
  ntiming.period = 5000ms;
  handoff::session_notifier_t notifier(nrig.ops(), ntiming);
  notifier.start();
  ASSERT_TRUE(wait_until([&] {
    return notifier.passes() >= 1;
  }));
  ASSERT_EQ(nrig.states_sent().size(), 1u) << "Control: while draining the notifier sends HANDOFF_STATE.";

  rig.sessions = 2;
  begin_now();
  ctrl->handed_over();
  ASSERT_TRUE(ctrl->cancel()) << "The rollback cancels the drain.";
  nrig.mode = mode_e::normal;  // the rollback made this process the active one again
  nrig.current = state_e::none;
  nrig.sessions = 4;

  const auto before = notifier.passes();
  notifier.poke();
  ASSERT_TRUE(wait_until([&] {
    return notifier.passes() > before;
  }))
    << "The notifier must keep running after cancel().";
  EXPECT_EQ(nrig.states_sent().size(), 1u) << "After cancel() (the rollback) no HANDOFF_STATE may be sent.";
  EXPECT_EQ(nrig.sessions_written().back(), 4) << "The notifier must go on writing the session count.";
  notifier.stop();
}

TEST_F(HandoffDrainControllerTest, SlowEndSessionsDoesNotDelayTheNotifier) {
  notifier_rig_t nrig;
  handoff::notifier_timing_t ntiming;
  ntiming.period = 5000ms;
  handoff::session_notifier_t notifier(nrig.ops(), ntiming);
  notifier.start();
  ASSERT_TRUE(wait_until([&] {
    return notifier.passes() >= 1;
  }));

  rig.sessions = 1;
  rig.hold_end_sessions = true;
  begin_now();
  ctrl->handed_over();
  advance(11000, 1000);
  ASSERT_TRUE(wait_until([&] {
    return rig.in_end_sessions.load();
  }))
    << "Precondition: end_sessions is blocked (a long session join).";

  nrig.sessions = 6;
  const auto before = notifier.passes();
  notifier.poke();
  EXPECT_TRUE(wait_until([&] {
    return notifier.passes() > before;
  }))
    << "A long end_sessions must not delay the notifier.";
  EXPECT_EQ(nrig.sessions_written().back(), 6);
  rig.hold_end_sessions = false;
  notifier.stop();
}

// ---------------------------------------------------------------------------------------------------------------------
// hooks::sessions_changed() wakes the (process-wide) notifier
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffDrainHooksTest, SessionsChangedWakesTheDrainNotifier) {
  handoff::runtime().reset_for_test();
  auto &notifier = handoff::session_notifier();
  notifier.start();
  // The process-wide notifier is never stopped here: after stop() it stays stopped and start() does nothing, so
  // stopping it would break a repeat. start() is idempotent while it runs.
  // SPEC-QUESTION: a shutdown request made by another test while this notifier runs ends it for good ("it stays
  // ended"). g_global_notifier_may_have_ended records that case in this file; the test is skipped then.
  if (g_global_notifier_may_have_ended.load()) {
    GTEST_SKIP() << "The process-wide notifier was ended by a shutdown request in an earlier test.";
  }
  g_global_notifier_started = true;
  ASSERT_TRUE(wait_until([&] {
    return notifier.passes() >= 1;
  }))
    << "The process-wide notifier must run its first pass after start().";

  // The default period is 500 ms, so a pass within 300 ms of the call can only come from the poke.
  const auto before = notifier.passes();
  handoff::hooks::sessions_changed();
  EXPECT_TRUE(wait_until(
    [&] {
      return notifier.passes() > before;
    },
    300ms
  )) << "hooks::sessions_changed() must wake the session notifier for a pass right now.";
  handoff::runtime().reset_for_test();
}

TEST(HandoffDrainHooksTest, RunningSessionCountIsZeroWithNoSessions) {
  EXPECT_EQ(stream::session::running_session_count(), 0)
    << "The lock-free accessor must read 0 when no session was started.";
}

#endif  // __linux__
