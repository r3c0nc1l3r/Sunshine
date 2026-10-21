/**
 * @file tests/unit/handoff/test_close_app.cpp
 * @brief Blind tests for the `close-app` verb handler `handoff::close_app()` (src/handoff/close_app.h), written from
 *        the spec in docs/drain-handoff.md ("The `close-app` verb") and the header only.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <stdexcept>
  #include <string>
  #include <string_view>

  // local includes
  #include "src/handoff/close_app.h"
  #include "src/handoff/control.h"
  #include "src/handoff/end_all.h"

using namespace std::chrono_literals;
namespace hf = handoff;

namespace {
  /**
   * @brief Recording fake of the close-app operations (`running` and `terminate`).
   */
  struct fake_ops_t {
    int running_result {0};  ///< Value that `running` returns.
    int running_calls {0};  ///< Number of `running` calls.
    hf::terminate_permission_e terminate_result {hf::terminate_permission_e::no_drain};  ///< Value `terminate` returns.
    int terminate_calls {0};  ///< Number of `terminate` calls.
    std::string terminate_action;  ///< The `action` argument of the last `terminate` call.

    /**
     * @brief Build the operations over this fake. The fake must outlive the returned value.
     *
     * @return Operations that record their calls here.
     */
    hf::close_app_ops_t make() {
      hf::close_app_ops_t ops;
      ops.running = [this]() {
        ++running_calls;
        return running_result;
      };
      ops.terminate = [this](std::chrono::milliseconds budget, std::string_view action) {
        (void) budget;
        ++terminate_calls;
        terminate_action = std::string {action};
        return terminate_result;
      };
      return ops;
    }
  };
}  // namespace

/**
 * @brief Tests for the `close-app` handler with injected operations.
 */
class HandoffCloseAppTest: public testing::Test {};

/**
 * @brief An app that is running is closed: the reply is `closed` with `closed_app` true, and `terminate` is called once
 * with the action "close app".
 */
TEST_F(HandoffCloseAppTest, RunningAppIsClosedAndReportsClosedApp) {
  fake_ops_t fake;
  fake.running_result = 4242;
  fake.terminate_result = hf::terminate_permission_e::ended;

  const hf::control_reply_t reply = hf::close_app(1234, fake.make());

  EXPECT_EQ(reply.kind, hf::reply_kind_e::closed) << "close-app with a running app and an allowed terminate must reply with kind closed";
  EXPECT_TRUE(reply.closed_app) << "closed_app must be true when an app was running (it was above 0)";
  EXPECT_EQ(fake.terminate_calls, 1) << "terminate must be called exactly once, got " << fake.terminate_calls;
  EXPECT_EQ(fake.terminate_action, "close app") << "terminate must be called with the action \"close app\", got \"" << fake.terminate_action << "\"";
}

/**
 * @brief With no app running, `terminate` is still called once (it is a no-op there), and the reply is `closed` with
 * `closed_app` false.
 */
TEST_F(HandoffCloseAppTest, NoRunningAppStillCallsTerminateOnceAndReportsClosedNone) {
  fake_ops_t fake;
  fake.running_result = 0;
  fake.terminate_result = hf::terminate_permission_e::no_drain;

  const hf::control_reply_t reply = hf::close_app(1234, fake.make());

  EXPECT_EQ(reply.kind, hf::reply_kind_e::closed) << "close-app with no app and an allowed terminate must reply with kind closed";
  EXPECT_FALSE(reply.closed_app) << "closed_app must be false when no app was running (running() returned 0)";
  EXPECT_EQ(fake.terminate_calls, 1) << "terminate must still be called exactly once when no app runs, got " << fake.terminate_calls;
  EXPECT_EQ(fake.terminate_action, "close app") << "the action must be \"close app\" also with no app running";
}

/**
 * @brief When the terminate gate refuses, the reply is `refused` with the reason "previous process still holds the app".
 */
TEST_F(HandoffCloseAppTest, TerminateRefusedGivesRefusedReply) {
  fake_ops_t fake;
  fake.running_result = 4242;
  fake.terminate_result = hf::terminate_permission_e::refused;

  const hf::control_reply_t reply = hf::close_app(1234, fake.make());

  EXPECT_EQ(reply.kind, hf::reply_kind_e::refused) << "a refused terminate must give kind refused, not closed";
  EXPECT_EQ(reply.reason, "previous process still holds the app") << "the refusal reason must be exactly \"previous process still holds the app\", got \"" << reply.reason << "\"";
  EXPECT_EQ(fake.terminate_calls, 1) << "terminate must be called once and then refuse, got " << fake.terminate_calls;
}

/**
 * @brief An exception from `running` is not caught by `close_app()`; it propagates to the caller (the control server
 * answers `refused internal error`).
 */
TEST_F(HandoffCloseAppTest, ExceptionFromRunningPropagates) {
  hf::close_app_ops_t ops;
  ops.running = []() -> int {
    throw std::runtime_error("test running failure");
  };
  ops.terminate = [](std::chrono::milliseconds, std::string_view) {
    return hf::terminate_permission_e::no_drain;
  };

  EXPECT_THROW(hf::close_app(1234, ops), std::runtime_error) << "an exception from running() must propagate out of close_app()";
}

#endif  // __linux__
