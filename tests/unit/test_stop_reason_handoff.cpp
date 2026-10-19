/**
 * @file tests/unit/test_stop_reason_handoff.cpp
 * @brief Tests for the drain-handoff session stop reasons (P05): the `stop` and `drain-deadline` names, the mapping
 *        from a shutdown reason, and the reason parameter of `terminate_sessions` and `clear`.
 *
 * Sessions are fake: `rtsp_stream::testing::insert_fake_session` creates them without worker threads, so a join
 * finishes at once. Every test removes its sessions in TearDown, so no state carries into the next iteration.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// lib includes
#include <src/logging.h>

// local includes
#include <src/handoff/types.h>
#include <src/rtsp.h>
#include <src/stream.h>
#include <src/stream_liveness.h>

#ifdef __linux__
  #include <src/handoff/hooks.h>
  #include <src/handoff/runtime.h>
  #include <src/handoff/settings.h>
#endif

namespace {
  /**
   * @brief Name of the log file the unit-test logger writes to.
   */
  constexpr auto TEST_LOG_FILE = "test_sunshine.log";

  /**
   * @brief Current size of the unit-test log file, after flushing pending log output.
   *
   * @return Size in bytes, or 0 when the file does not exist yet.
   */
  std::uintmax_t stop_test_log_size() {
    logging::log_flush();
    std::error_code ec;
    const auto size = std::filesystem::file_size(TEST_LOG_FILE, ec);
    return ec ? 0 : size;
  }

  /**
   * @brief Log text written after a byte offset of the unit-test log file.
   *
   * @param offset Byte offset returned by `stop_test_log_size()` before the action under test.
   * @return Text appended to the log since that offset.
   */
  std::string stop_test_log_since(std::uintmax_t offset) {
    logging::log_flush();
    std::ifstream input(TEST_LOG_FILE, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (offset >= content.size()) {
      return {};
    }
    return content.substr(static_cast<std::size_t>(offset));
  }

  /**
   * @brief Count how many times a fragment occurs in a text.
   *
   * @param text Text to search.
   * @param fragment Fragment to count; must not be empty.
   * @return Number of non-overlapping occurrences.
   */
  std::size_t count_fragment(const std::string &text, std::string_view fragment) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(fragment, pos)) != std::string::npos) {
      ++count;
      pos += fragment.size();
    }
    return count;
  }

  /**
   * @brief Every stop reason enumerator, from `unknown` to `drain_deadline`, in declaration order.
   *
   * @return The enumerators as a vector.
   */
  std::vector<stream::stop_reason_e> all_stop_reasons() {
    return {
      stream::stop_reason_e::unknown,
      stream::stop_reason_e::disconnect,
      stream::stop_reason_e::ping_timeout,
      stream::stop_reason_e::media_ping_timeout,
      stream::stop_reason_e::initial_ping_timeout,
      stream::stop_reason_e::protocol_error,
      stream::stop_reason_e::api_close,
      stream::stop_reason_e::stream_ended,
      stream::stop_reason_e::terminated,
      stream::stop_reason_e::stop,
      stream::stop_reason_e::drain_deadline,
    };
  }
}  // namespace

// ---------------------------------------------------------------------------
// Names and the shutdown mapping (no sessions)
// ---------------------------------------------------------------------------

/**
 * @brief Test: the new `stop` enumerator is named `stop` in the session-end log line.
 */
TEST(HandoffStopReasonTest, StopEnumeratorIsNamedStop) {
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::stop), std::string_view {"stop"})
    << "to_string(stop_reason_e::stop) must return the lowercase name \"stop\"";
}

/**
 * @brief Test: the new `drain_deadline` enumerator is named `drain-deadline` (dash, not underscore).
 */
TEST(HandoffStopReasonTest, DrainDeadlineEnumeratorIsNamedDrainDeadline) {
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::drain_deadline), std::string_view {"drain-deadline"})
    << "to_string(stop_reason_e::drain_deadline) must return \"drain-deadline\" with a dash";
}

/**
 * @brief Test: the existing names did not change when the two new enumerators were added.
 */
TEST(HandoffStopReasonTest, ExistingNamesAreUnchanged) {
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::unknown), std::string_view {"unknown"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::disconnect), std::string_view {"disconnect"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::ping_timeout), std::string_view {"ping-timeout"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::media_ping_timeout), std::string_view {"media-ping-timeout"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::initial_ping_timeout), std::string_view {"initial-ping-timeout"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::protocol_error), std::string_view {"protocol-error"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::api_close), std::string_view {"api-close"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::stream_ended), std::string_view {"stream-ended"});
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::terminated), std::string_view {"terminated"});
}

/**
 * @brief Test: every enumerator, including the two new ones, has a distinct non-empty name.
 */
TEST(HandoffStopReasonTest, AllEnumeratorsHaveDistinctNonEmptyNames) {
  std::set<std::string_view> names;
  const auto reasons = all_stop_reasons();
  for (const auto reason : reasons) {
    const auto name = stream::to_string(reason);
    EXPECT_FALSE(name.empty()) << "a stop reason has an empty name; every enumerator needs a log name";
    names.insert(name);
  }
  EXPECT_EQ(names.size(), reasons.size()) << "two stop reasons share a name, so their session-end log lines cannot be told apart";
}

/**
 * @brief Test: the new names are not the `unknown` fallback, so a missing switch case would show up.
 */
TEST(HandoffStopReasonTest, NewReasonsDoNotFallBackToUnknown) {
  EXPECT_NE(stream::to_string(stream::stop_reason_e::stop), std::string_view {"unknown"})
    << "stop has no case in to_string and falls back to \"unknown\"";
  EXPECT_NE(stream::to_string(stream::stop_reason_e::drain_deadline), std::string_view {"unknown"})
    << "drain_deadline has no case in to_string and falls back to \"unknown\"";
}

/**
 * @brief Test: values just past the new last enumerator are still reported as `unknown`.
 */
TEST(HandoffStopReasonTest, ValuePastLastNewEnumeratorIsUnknown) {
  const auto past = static_cast<stream::stop_reason_e>(static_cast<int>(stream::stop_reason_e::drain_deadline) + 1);
  EXPECT_EQ(stream::to_string(past), std::string_view {"unknown"})
    << "a value after drain_deadline must map to \"unknown\", not to a stale name";
}

/**
 * @brief Test: the shutdown reason `terminated` maps to the stop reason `terminated`.
 */
TEST(HandoffStopReasonTest, ToStopReasonMapsTerminatedToTerminated) {
  EXPECT_EQ(rtsp_stream::to_stop_reason(handoff::shutdown_reason_e::terminated), stream::stop_reason_e::terminated)
    << "an ordinary shutdown must keep recording terminated";
}

/**
 * @brief Test: the shutdown reason `stop` maps to the stop reason `stop`.
 */
TEST(HandoffStopReasonTest, ToStopReasonMapsStopToStop) {
  EXPECT_EQ(rtsp_stream::to_stop_reason(handoff::shutdown_reason_e::stop), stream::stop_reason_e::stop)
    << "a draining process that was stopped must record stop";
}

/**
 * @brief Test: the mapping is pure, so repeated calls give the same answer.
 */
TEST(HandoffStopReasonTest, ToStopReasonIsRepeatable) {
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(rtsp_stream::to_stop_reason(handoff::shutdown_reason_e::stop), stream::stop_reason_e::stop)
      << "to_stop_reason(stop) changed on repeated call " << i;
    EXPECT_EQ(rtsp_stream::to_stop_reason(handoff::shutdown_reason_e::terminated), stream::stop_reason_e::terminated)
      << "to_stop_reason(terminated) changed on repeated call " << i;
  }
}

// ---------------------------------------------------------------------------
// terminate_sessions(reason) and clear(all, reason), through the fake-session seam
// ---------------------------------------------------------------------------

/**
 * @brief Test fixture for stop-reason tests that use fake sessions. Removes every fake session after each test.
 */
class HandoffStopReasonSessionTest: public BaseTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
protected:
  /**
   * @brief Remove all fake sessions and restore the base test state.
   */
  void TearDown() override {
    rtsp_stream::testing::remove_all();
    BaseTest::TearDown();
  }
};

/**
 * @brief Test: `terminate_sessions()` with no argument records `terminated` on a running session and removes it.
 */
TEST_F(HandoffStopReasonSessionTest, TerminateSessionsDefaultReasonIsTerminated) {
  auto session = rtsp_stream::testing::insert_fake_session(1101, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);

  rtsp_stream::terminate_sessions();

  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::terminated)
    << "terminate_sessions() without a reason must record terminated, as it did before the parameter existed";
  EXPECT_EQ(rtsp_stream::session_count(), 0) << "terminate_sessions() must leave no session in the slots";
}

/**
 * @brief Test: `terminate_sessions(stop)` records `stop` on each running session it ends.
 */
TEST_F(HandoffStopReasonSessionTest, TerminateSessionsWithStopRecordsStop) {
  auto first = rtsp_stream::testing::insert_fake_session(1102, stream::session::state_e::RUNNING);
  auto second = rtsp_stream::testing::insert_fake_session(1103, stream::session::state_e::RUNNING);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  rtsp_stream::terminate_sessions(stream::stop_reason_e::stop);

  EXPECT_EQ(stream::session::stop_reason(*first), stream::stop_reason_e::stop)
    << "the first running session must record stop when terminate_sessions(stop) ends it";
  EXPECT_EQ(stream::session::stop_reason(*second), stream::stop_reason_e::stop)
    << "the second running session must record stop when terminate_sessions(stop) ends it";
  EXPECT_EQ(rtsp_stream::session_count(), 0) << "both sessions must be joined and removed";
}

/**
 * @brief Test: `terminate_sessions(drain_deadline)` records `drain-deadline` on each session it ends.
 */
TEST_F(HandoffStopReasonSessionTest, TerminateSessionsWithDrainDeadlineRecordsDrainDeadline) {
  auto session = rtsp_stream::testing::insert_fake_session(1104, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);

  rtsp_stream::terminate_sessions(stream::stop_reason_e::drain_deadline);

  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::drain_deadline)
    << "terminate_sessions(drain_deadline) must record drain_deadline on the session it ends";
  EXPECT_EQ(rtsp_stream::session_count(), 0) << "the drain-deadline session must be removed";
}

/**
 * @brief Test: a session already stopped with another reason keeps its first reason when `terminate_sessions(stop)` runs.
 */
TEST_F(HandoffStopReasonSessionTest, TerminateSessionsKeepsFirstRecordedReason) {
  auto session = rtsp_stream::testing::insert_fake_session(1105, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(stream::session::stop(*session, stream::stop_reason_e::api_close))
    << "setup failed: the first stop of a running session must succeed";

  rtsp_stream::terminate_sessions(stream::stop_reason_e::stop);

  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::api_close)
    << "a session that is already stopping must keep the reason it recorded first";
  EXPECT_EQ(rtsp_stream::session_count(), 0) << "the stopping session must still be joined and removed";
}

/**
 * @brief Test: `terminate_sessions` with no sessions present returns without error and leaves the slots empty.
 */
TEST_F(HandoffStopReasonSessionTest, TerminateSessionsWithNoSessionsIsHarmless) {
  EXPECT_EQ(rtsp_stream::session_count(), 0) << "setup: the slots must start empty";

  rtsp_stream::terminate_sessions(stream::stop_reason_e::stop);
  rtsp_stream::terminate_sessions(stream::stop_reason_e::drain_deadline);

  EXPECT_EQ(rtsp_stream::session_count(), 0) << "terminate_sessions with no sessions must not add any";
}

/**
 * @brief Test: the session-end log line carries the reason given to `terminate_sessions`.
 */
TEST_F(HandoffStopReasonSessionTest, TerminateSessionsWithStopLogsStopReason) {
  auto session = rtsp_stream::testing::insert_fake_session(1106, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  const std::uintmax_t offset = stop_test_log_size();

  rtsp_stream::terminate_sessions(stream::stop_reason_e::stop);

  const std::string logged = stop_test_log_since(offset);
  EXPECT_EQ(count_fragment(logged, "Session ended: reason=stop"), 1U)
    << "exactly one session-end line with reason=stop must be logged for the one session ended";
}

/**
 * @brief Test: `clear(true, stop)` records `stop` on a running session and removes it.
 */
TEST_F(HandoffStopReasonSessionTest, ClearAllWithStopRecordsStop) {
  auto session = rtsp_stream::testing::insert_fake_session(1201, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);

  rtsp_stream::testing::clear(true, stream::stop_reason_e::stop);

  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::stop)
    << "clear(true, stop) must record stop on the running session it ends";
  EXPECT_EQ(rtsp_stream::session_count(), 0) << "clear(true, ...) must remove every session";
}

/**
 * @brief Test: `clear(true)` with no reason still records `terminated`, the reason before the parameter existed.
 */
TEST_F(HandoffStopReasonSessionTest, ClearAllDefaultReasonIsTerminated) {
  auto session = rtsp_stream::testing::insert_fake_session(1202, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);

  rtsp_stream::testing::clear(true);

  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::terminated)
    << "clear(true) without a reason must keep recording terminated";
}

/**
 * @brief Test: `clear(true, drain_deadline)` records `drain-deadline` and logs that reason.
 */
TEST_F(HandoffStopReasonSessionTest, ClearAllWithDrainDeadlineRecordsAndLogsIt) {
  auto session = rtsp_stream::testing::insert_fake_session(1203, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  const std::uintmax_t offset = stop_test_log_size();

  rtsp_stream::testing::clear(true, stream::stop_reason_e::drain_deadline);

  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::drain_deadline)
    << "clear(true, drain_deadline) must record drain_deadline";
  EXPECT_EQ(count_fragment(stop_test_log_since(offset), "Session ended: reason=drain-deadline"), 1U)
    << "the session-end line must name the reason drain-deadline";
}

/**
 * @brief Test: `clear(false, stop)` ends only the stopping sessions and leaves a running session untouched.
 */
TEST_F(HandoffStopReasonSessionTest, ClearFalseWithStopEndsOnlyStoppingSessions) {
  auto running = rtsp_stream::testing::insert_fake_session(1204, stream::session::state_e::RUNNING);
  auto stopping = rtsp_stream::testing::insert_fake_session(1205, stream::session::state_e::STOPPING);
  ASSERT_NE(running, nullptr);
  ASSERT_NE(stopping, nullptr);

  rtsp_stream::testing::clear(false, stream::stop_reason_e::stop);

  EXPECT_EQ(stream::session::state(*running), stream::session::state_e::RUNNING)
    << "clear(false, ...) must not stop a running session";
  EXPECT_EQ(stream::session::stop_reason(*running), stream::stop_reason_e::unknown)
    << "clear(false, ...) must not record a reason on a running session it did not end";
  EXPECT_EQ(rtsp_stream::session_count(), 1) << "only the running session may remain after clear(false, ...)";
}

/**
 * @brief Test: `clear(false, drain_deadline)` keeps the reason a stopping session recorded before the call.
 */
TEST_F(HandoffStopReasonSessionTest, ClearFalseKeepsReasonOfAlreadyStoppingSession) {
  auto session = rtsp_stream::testing::insert_fake_session(1206, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(stream::session::stop(*session, stream::stop_reason_e::ping_timeout))
    << "setup failed: the first stop of a running session must succeed";

  rtsp_stream::testing::clear(false, stream::stop_reason_e::drain_deadline);

  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::ping_timeout)
    << "a stopping session must keep the first recorded reason, not take drain_deadline";
  EXPECT_EQ(rtsp_stream::session_count(), 0) << "the stopping session must be ended and removed by clear(false, ...)";
}

/**
 * @brief Test: `clear(true, stop)` keeps the reason a session recorded first.
 */
TEST_F(HandoffStopReasonSessionTest, ClearAllKeepsFirstRecordedReason) {
  auto session = rtsp_stream::testing::insert_fake_session(1207, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(stream::session::stop(*session, stream::stop_reason_e::api_close))
    << "setup failed: the first stop of a running session must succeed";

  rtsp_stream::testing::clear(true, stream::stop_reason_e::stop);

  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::api_close)
    << "clear(true, stop) must not overwrite the reason recorded first";
}

/**
 * @brief Test: a stop reason recorded through `terminate_sessions` is what the session-end log names.
 */
TEST_F(HandoffStopReasonSessionTest, DrainDeadlineSessionNeverLogsTerminated) {
  auto session = rtsp_stream::testing::insert_fake_session(1208, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  const std::uintmax_t offset = stop_test_log_size();

  rtsp_stream::terminate_sessions(stream::stop_reason_e::drain_deadline);

  const std::string logged = stop_test_log_since(offset);
  EXPECT_EQ(count_fragment(logged, "reason=terminated"), 0U)
    << "a session ended with drain_deadline must not be logged as terminated";
  EXPECT_EQ(count_fragment(logged, "reason=drain-deadline"), 1U)
    << "the session ended with drain_deadline must be logged once with reason=drain-deadline";
}

#ifdef __linux__
/**
 * @brief Test fixture for the shutdown path. Returns the handoff runtime to a fresh process and removes fake sessions.
 *
 * The RTSP loop's exit `clear()` is not reachable from a test (it runs inside `rtsp_stream::start()`), so these tests
 * drive the reason the way that path does: the runtime's shutdown reason, mapped by `to_stop_reason`, passed to
 * `testing::clear`.
 */
class HandoffShutdownReasonTest: public BaseTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
protected:
  /**
   * @brief Start from a fresh runtime (mode disabled, no shutdown latched).
   */
  void SetUp() override {
    handoff::runtime().reset_for_test();
    BaseTest::SetUp();
  }

  /**
   * @brief Remove fake sessions and return the runtime to a fresh process.
   */
  void TearDown() override {
    rtsp_stream::testing::remove_all();
    handoff::runtime().reset_for_test();
    BaseTest::TearDown();
  }
};

/**
 * @brief Test: with the feature disabled, a shutdown request leaves the shutdown reason `terminated`, and the sessions
 *        ended through the shutdown path record `terminated`.
 */
TEST_F(HandoffShutdownReasonTest, DisabledFeatureShutdownRecordsTerminated) {
  ASSERT_EQ(handoff::hooks::shutdown_reason(), handoff::shutdown_reason_e::terminated)
    << "a fresh process with the feature disabled must report the shutdown reason terminated";

  handoff::runtime().request_shutdown();

  ASSERT_EQ(handoff::hooks::shutdown_reason(), handoff::shutdown_reason_e::terminated)
    << "with the feature disabled a requested shutdown must still report terminated";
  auto session = rtsp_stream::testing::insert_fake_session(1301, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);

  rtsp_stream::testing::clear(true, rtsp_stream::to_stop_reason(handoff::hooks::shutdown_reason()));

  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::terminated)
    << "a normal shutdown must end its sessions with terminated";
}

/**
 * @brief Test: a draining process reports the shutdown reason `stop` before and after a shutdown request, and the
 *        sessions ended through the shutdown path record `stop`.
 */
TEST_F(HandoffShutdownReasonTest, DrainingShutdownRecordsStop) {
  handoff::settings_t settings;
  settings.enabled = true;
  handoff::runtime().configure(settings);
  ASSERT_TRUE(handoff::runtime().transition(handoff::mode_e::normal, handoff::mode_e::draining))
    << "setup failed: a normal process must be able to move to draining";

  EXPECT_EQ(handoff::hooks::shutdown_reason(), handoff::shutdown_reason_e::stop)
    << "a draining process must report the shutdown reason stop even before a shutdown is requested";

  handoff::runtime().request_shutdown();

  ASSERT_EQ(handoff::hooks::shutdown_reason(), handoff::shutdown_reason_e::stop)
    << "after a shutdown request on a draining process the reason must stay stop";
  auto session = rtsp_stream::testing::insert_fake_session(1302, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);

  rtsp_stream::testing::clear(true, rtsp_stream::to_stop_reason(handoff::hooks::shutdown_reason()));

  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::stop)
    << "a draining session ended by the shutdown path must record stop";
}

/**
 * @brief Test: an enabled, normal process that is shut down records `terminated`, not `stop`.
 */
TEST_F(HandoffShutdownReasonTest, EnabledNormalShutdownRecordsTerminated) {
  handoff::settings_t settings;
  settings.enabled = true;
  handoff::runtime().configure(settings);

  handoff::runtime().request_shutdown();

  EXPECT_EQ(rtsp_stream::to_stop_reason(handoff::hooks::shutdown_reason()), stream::stop_reason_e::terminated)
    << "a normal (non-draining) process must map its shutdown to terminated";
}
#endif
