/**
 * @file tests/unit/test_session_close.cpp
 * @brief Tests for session close results (L1), clear without a join under the slots lock (L2), and the RTSP test seams.
 *
 * Sessions are fake: `rtsp_stream::testing::insert_fake_session` creates them without worker threads, so joins finish
 * at once unless a test passes a join gate. Every test removes its sessions in TearDown.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <latch>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

// local includes
#include <src/rtsp.h>
#include <src/stream.h>
#include <src/stream_liveness.h>

namespace {
  /**
   * @brief Name of the log file the unit-test logger writes to.
   */
  constexpr auto TEST_LOG_FILE = "test_sunshine.log";

  /**
   * @brief Upper bound for a call that must not wait for a held join.
   */
  constexpr auto PROMPT_LIMIT = std::chrono::seconds {5};

  /**
   * @brief Time given to a background clear to reach the join before the test checks the held state.
   */
  constexpr auto SETTLE_TIME = std::chrono::milliseconds {50};

  /**
   * @brief Current size of the unit-test log file, after flushing pending log output.
   *
   * @return Size in bytes, or 0 when the file does not exist yet.
   */
  std::uintmax_t test_log_size() {
    logging::log_flush();
    std::error_code ec;
    const auto size = std::filesystem::file_size(TEST_LOG_FILE, ec);
    return ec ? 0 : size;
  }

  /**
   * @brief Log text written after a given byte offset of the unit-test log file.
   *
   * @param offset Byte offset returned by `test_log_size()` before the action under test.
   * @return Text appended to the log since that offset.
   */
  std::string test_log_since(std::uintmax_t offset) {
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
   * @param needle Substring to count.
   * @return Number of non-overlapping occurrences.
   */
  std::size_t count_occurrences(const std::string &text, const std::string &needle) {
    std::size_t count = 0;
    for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size())) {
      ++count;
    }
    return count;
  }

  /**
   * @brief Holds a join open: a fake session whose gate is passed to `insert_fake_session` joins only after `release()`.
   *
   * The destructor releases the gate, so a failing assertion cannot leave a background join blocked.
   */
  class join_gate_t {
  public:
    join_gate_t():
        future_(promise_.get_future().share()) {}

    join_gate_t(const join_gate_t &) = delete;
    join_gate_t &operator=(const join_gate_t &) = delete;

    ~join_gate_t() {
      release();
    }

    /**
     * @brief Shared future to pass to `insert_fake_session` as the join gate.
     *
     * @return Future that becomes ready when the gate is released.
     */
    [[nodiscard]] std::shared_future<void> future() const {
      return future_;
    }

    /**
     * @brief Open the gate. Calling it again does nothing.
     */
    void release() {
      if (!released_) {
        released_ = true;
        promise_.set_value();
      }
    }

  private:
    std::promise<void> promise_;  ///< Promise that opens the gate.
    std::shared_future<void> future_;  ///< Shared view of the promise handed to fake sessions.
    bool released_ {false};  ///< Whether `release()` already ran.
  };
}  // namespace

/**
 * @brief Test fixture for session close, clear and seam tests. Removes every fake session after each test.
 */
class SessionCloseTest: public BaseTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
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
 * @brief Test: closing a RUNNING session returns `closed`, stops it with reason `api-close`, and moves it to STOPPING.
 */
TEST_F(SessionCloseTest, RunningSessionCloseReturnsClosedWithApiCloseReason) {
  constexpr std::uint32_t id = 101;
  auto session = rtsp_stream::testing::insert_fake_session(id, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);

  EXPECT_EQ(rtsp_stream::request_session_close(id), rtsp_stream::close_result_e::closed);
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::STOPPING);
  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::api_close);
  EXPECT_EQ(stream::to_string(stream::stop_reason_e::api_close), "api-close");
}

/**
 * @brief Test: a second close of the same RUNNING session returns `already_stopping` and keeps the first reason.
 */
TEST_F(SessionCloseTest, SecondCloseOfSameSessionIsAlreadyStoppingAndKeepsFirstReason) {
  constexpr std::uint32_t id = 102;
  auto session = rtsp_stream::testing::insert_fake_session(id, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);

  ASSERT_EQ(rtsp_stream::request_session_close(id), rtsp_stream::close_result_e::closed);
  EXPECT_EQ(rtsp_stream::request_session_close(id), rtsp_stream::close_result_e::already_stopping);
  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::api_close);
}

/**
 * @brief Test: closing a session that another stop already moved to STOPPING returns `already_stopping` and keeps that reason.
 */
TEST_F(SessionCloseTest, CloseOfSessionStoppedForAnotherReasonIsAlreadyStoppingAndKeepsReason) {
  constexpr std::uint32_t id = 103;
  auto session = rtsp_stream::testing::insert_fake_session(id, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(stream::session::stop(*session, stream::stop_reason_e::ping_timeout));

  EXPECT_EQ(rtsp_stream::request_session_close(id), rtsp_stream::close_result_e::already_stopping);
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::STOPPING);
  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::ping_timeout);
}

/**
 * @brief Test: a session that is STARTING has not started yet, so closing it returns `not_found` and changes nothing.
 */
TEST_F(SessionCloseTest, StartingSessionIsNotFoundAndUnchanged) {
  constexpr std::uint32_t id = 104;
  auto session = rtsp_stream::testing::insert_fake_session(id, stream::session::state_e::STARTING);
  ASSERT_NE(session, nullptr);

  EXPECT_EQ(rtsp_stream::request_session_close(id), rtsp_stream::close_result_e::not_found);
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::STARTING);
  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::unknown);
}

/**
 * @brief Test: a STOPPED session is not in a live state, so closing it returns `not_found`.
 */
TEST_F(SessionCloseTest, StoppedSessionIsNotFound) {
  constexpr std::uint32_t id = 105;
  auto session = rtsp_stream::testing::insert_fake_session(id, stream::session::state_e::STOPPED);
  ASSERT_NE(session, nullptr);

  EXPECT_EQ(rtsp_stream::request_session_close(id), rtsp_stream::close_result_e::not_found);
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::STOPPED);
}

/**
 * @brief Test: closing an id that no session has returns `not_found`, and other sessions are untouched.
 */
TEST_F(SessionCloseTest, UnknownIdIsNotFoundAndOtherSessionIsUntouched) {
  constexpr std::uint32_t present_id = 106;
  constexpr std::uint32_t absent_id = 107;
  auto session = rtsp_stream::testing::insert_fake_session(present_id, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);

  EXPECT_EQ(rtsp_stream::request_session_close(absent_id), rtsp_stream::close_result_e::not_found);
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::RUNNING);
}

/**
 * @brief Test: `stream::session::stop` returns `true` only for the call that moves RUNNING to STOPPING, and keeps the first reason.
 */
TEST_F(SessionCloseTest, StopReturnsTrueOnlyForFirstStop) {
  constexpr std::uint32_t id = 108;
  auto session = rtsp_stream::testing::insert_fake_session(id, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);

  EXPECT_TRUE(stream::session::stop(*session, stream::stop_reason_e::api_close));
  EXPECT_FALSE(stream::session::stop(*session, stream::stop_reason_e::ping_timeout));
  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::api_close);
}

/**
 * @brief Test: the list holds RUNNING and STOPPING fake sessions ordered by id, with the fixed transport and category values.
 */
TEST_F(SessionCloseTest, ListedSessionsHaveFixedFieldsAndAreOrderedById) {
  const auto before = std::chrono::system_clock::now();
  auto later = rtsp_stream::testing::insert_fake_session(121, stream::session::state_e::RUNNING);
  auto earlier = rtsp_stream::testing::insert_fake_session(120, stream::session::state_e::STOPPING);
  const auto after = std::chrono::system_clock::now();
  ASSERT_NE(later, nullptr);
  ASSERT_NE(earlier, nullptr);

  const auto sessions = rtsp_stream::list_sessions();
  ASSERT_EQ(sessions.size(), 2U);
  EXPECT_EQ(sessions[0].id, 120U);
  EXPECT_EQ(sessions[1].id, 121U);
  for (const auto &info : sessions) {
    EXPECT_EQ(info.transport, "udp/ipv4");
    EXPECT_EQ(info.client_category, "local");
    EXPECT_GE(info.started, before);
    EXPECT_LE(info.started, after);
  }
}

/**
 * @brief Test: `clear(false)` ends only STOPPING sessions, so a RUNNING session survives it.
 */
TEST_F(SessionCloseTest, ClearFalseEndsOnlyStoppingSessions) {
  auto running = rtsp_stream::testing::insert_fake_session(131, stream::session::state_e::RUNNING);
  auto stopping = rtsp_stream::testing::insert_fake_session(132, stream::session::state_e::STOPPING);
  ASSERT_NE(running, nullptr);
  ASSERT_NE(stopping, nullptr);

  rtsp_stream::testing::clear(false);

  EXPECT_EQ(rtsp_stream::session_count(), 1);
  const auto sessions = rtsp_stream::list_sessions();
  ASSERT_EQ(sessions.size(), 1U);
  EXPECT_EQ(sessions[0].id, 131U);
  EXPECT_EQ(stream::session::state(*running), stream::session::state_e::RUNNING);
}

/**
 * @brief Test: `clear(true)` ends a RUNNING session as `terminated`, removes it, and logs one session-end line.
 */
TEST_F(SessionCloseTest, ClearAllEndsRunningSessionAsTerminatedAndLogsIt) {
  constexpr std::uint32_t id = 141;
  auto session = rtsp_stream::testing::insert_fake_session(id, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  const std::uintmax_t offset = test_log_size();

  rtsp_stream::testing::clear(true);

  EXPECT_EQ(rtsp_stream::session_count(), 0);
  EXPECT_TRUE(rtsp_stream::list_sessions().empty());
  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::terminated);
  EXPECT_EQ(count_occurrences(test_log_since(offset), "Session ended: reason=terminated"), 1U);
}

/**
 * @brief Test: `remove_all()` empties the slots without any join or session-end log line.
 */
TEST_F(SessionCloseTest, RemoveAllEmptiesSlotsWithoutLogging) {
  auto first = rtsp_stream::testing::insert_fake_session(151, stream::session::state_e::RUNNING);
  auto second = rtsp_stream::testing::insert_fake_session(152, stream::session::state_e::STOPPING);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(rtsp_stream::list_sessions().size(), 2U);
  const std::uintmax_t offset = test_log_size();

  rtsp_stream::testing::remove_all();

  // Check the log before any session_count() call: session_count() ends STOPPING sessions and logs for each.
  EXPECT_EQ(count_occurrences(test_log_since(offset), "Session ended"), 0U);
  EXPECT_TRUE(rtsp_stream::list_sessions().empty());
  EXPECT_EQ(rtsp_stream::session_count(), 0);
}

/**
 * @brief Test: while a join is held, `list_sessions()` and `request_session_close()` return promptly.
 */
TEST_F(SessionCloseTest, ListAndCloseReturnPromptlyWhileJoinIsHeld) {
  constexpr std::uint32_t id = 161;
  std::jthread clearer;
  join_gate_t gate;
  auto session = rtsp_stream::testing::insert_fake_session(id, stream::session::state_e::STOPPING, gate.future());
  ASSERT_NE(session, nullptr);
  clearer = std::jthread([]() {
    rtsp_stream::testing::clear(false);
  });
  std::this_thread::sleep_for(SETTLE_TIME);

  auto list_call = std::async(std::launch::async, []() {
    return rtsp_stream::list_sessions();
  });
  auto close_call = std::async(std::launch::async, [id]() {
    return rtsp_stream::request_session_close(id);
  });
  const bool list_prompt = list_call.wait_for(PROMPT_LIMIT) == std::future_status::ready;
  const bool close_prompt = close_call.wait_for(PROMPT_LIMIT) == std::future_status::ready;

  // Release the join before any assertion can end the test, so the calls and the clear thread can finish.
  gate.release();
  clearer.join();

  ASSERT_TRUE(list_prompt) << "list_sessions() waited for the held join";
  ASSERT_TRUE(close_prompt) << "request_session_close() waited for the held join";
  EXPECT_EQ(list_call.get().size(), 1U);
  EXPECT_EQ(close_call.get(), rtsp_stream::close_result_e::already_stopping);
}

/**
 * @brief Test: while a join is held, the session still counts and is listed; after the join it is gone.
 */
TEST_F(SessionCloseTest, JoiningSessionStaysCountedAndListedUntilErased) {
  constexpr std::uint32_t id = 171;
  std::jthread clearer;
  join_gate_t gate;
  auto session = rtsp_stream::testing::insert_fake_session(id, stream::session::state_e::STOPPING, gate.future());
  ASSERT_NE(session, nullptr);
  clearer = std::jthread([]() {
    rtsp_stream::testing::clear(false);
  });
  std::this_thread::sleep_for(SETTLE_TIME);

  auto count_call = std::async(std::launch::async, []() {
    return rtsp_stream::session_count();
  });
  auto list_call = std::async(std::launch::async, []() {
    return rtsp_stream::list_sessions();
  });
  const bool count_prompt = count_call.wait_for(PROMPT_LIMIT) == std::future_status::ready;
  const bool list_prompt = list_call.wait_for(PROMPT_LIMIT) == std::future_status::ready;

  gate.release();
  clearer.join();

  ASSERT_TRUE(count_prompt) << "session_count() waited for the held join";
  ASSERT_TRUE(list_prompt) << "list_sessions() waited for the held join";
  EXPECT_EQ(count_call.get(), 1);
  const auto listed = list_call.get();
  ASSERT_EQ(listed.size(), 1U);
  EXPECT_EQ(listed[0].id, id);
  EXPECT_EQ(rtsp_stream::session_count(), 0);
}

/**
 * @brief Test: `clear(true)` returns only after the held session is joined and erased.
 */
TEST_F(SessionCloseTest, ClearAllReturnsOnlyAfterJoinedAndErased) {
  constexpr std::uint32_t id = 181;
  std::atomic<bool> returned {false};
  std::jthread clearer;
  join_gate_t gate;
  auto session = rtsp_stream::testing::insert_fake_session(id, stream::session::state_e::STOPPING, gate.future());
  ASSERT_NE(session, nullptr);
  clearer = std::jthread([&returned]() {
    rtsp_stream::testing::clear(true);
    returned = true;
  });
  std::this_thread::sleep_for(SETTLE_TIME);

  EXPECT_FALSE(returned.load()) << "clear(true) returned while the join was still held";

  gate.release();
  clearer.join();

  EXPECT_TRUE(returned.load());
  EXPECT_EQ(rtsp_stream::session_count(), 0);
}

/**
 * @brief Test: two concurrent clears on the same held session join it once and log its end once.
 */
TEST_F(SessionCloseTest, ConcurrentClearsJoinAndLogEachSessionOnce) {
  constexpr std::uint32_t id = 191;
  const std::uintmax_t offset = test_log_size();
  std::jthread first_clear;
  std::jthread second_clear;
  join_gate_t gate;
  auto session = rtsp_stream::testing::insert_fake_session(id, stream::session::state_e::STOPPING, gate.future());
  ASSERT_NE(session, nullptr);
  first_clear = std::jthread([]() {
    rtsp_stream::testing::clear(false);
  });
  second_clear = std::jthread([]() {
    rtsp_stream::testing::clear(true);
  });
  std::this_thread::sleep_for(SETTLE_TIME);

  gate.release();
  first_clear.join();
  second_clear.join();

  EXPECT_EQ(count_occurrences(test_log_since(offset), "Session ended"), 1U);
  EXPECT_EQ(rtsp_stream::session_count(), 0);
}

/**
 * @brief Test: `remove_all()` during a held join does not crash, and the clear logs nothing for a session it did not erase.
 *
 * The spec says a clear erases by value and logs only when it actually erased, so the removed session must not be logged.
 */
TEST_F(SessionCloseTest, RemoveDuringHeldJoinDoesNotCrashOrDoubleLog) {
  constexpr std::uint32_t id = 201;
  const std::uintmax_t offset = test_log_size();
  std::jthread clearer;
  join_gate_t gate;
  auto session = rtsp_stream::testing::insert_fake_session(id, stream::session::state_e::STOPPING, gate.future());
  ASSERT_NE(session, nullptr);
  clearer = std::jthread([]() {
    rtsp_stream::testing::clear(false);
  });
  std::this_thread::sleep_for(SETTLE_TIME);

  rtsp_stream::testing::remove_all();
  gate.release();
  clearer.join();

  // Check the log before session_count(), which ends STOPPING sessions and logs for each.
  EXPECT_EQ(count_occurrences(test_log_since(offset), "Session ended"), 0U);
  EXPECT_EQ(rtsp_stream::session_count(), 0);
}

/**
 * @brief Test: concurrent close, stop and clear on one RUNNING session produce exactly one winner, with no double end.
 *
 * Each of 200 iterations races two `request_session_close` calls, one `stream::session::stop(ping_timeout)` and one
 * `clear(false)`. The invariants are checked, not timings: exactly one stop wins; the recorded reason is the winner's;
 * the session ends up erased; and each iteration produces exactly one session-end log line.
 */
TEST_F(SessionCloseTest, ConcurrentCloseStopAndClearPickExactlyOneWinner) {
  constexpr std::uint32_t id = 301;
  constexpr int iterations = 200;
  const std::uintmax_t offset = test_log_size();

  for (int i = 0; i < iterations; ++i) {
    auto session = rtsp_stream::testing::insert_fake_session(id, stream::session::state_e::RUNNING);
    ASSERT_NE(session, nullptr);

    std::latch start_line {1};
    rtsp_stream::close_result_e first_close {rtsp_stream::close_result_e::not_found};
    rtsp_stream::close_result_e second_close {rtsp_stream::close_result_e::not_found};
    bool stop_won = false;

    std::vector<std::thread> threads;
    threads.emplace_back([&]() {
      start_line.wait();
      first_close = rtsp_stream::request_session_close(id);
    });
    threads.emplace_back([&]() {
      start_line.wait();
      second_close = rtsp_stream::request_session_close(id);
    });
    threads.emplace_back([&]() {
      start_line.wait();
      stop_won = stream::session::stop(*session, stream::stop_reason_e::ping_timeout);
    });
    threads.emplace_back([&start_line]() {
      start_line.wait();
      rtsp_stream::testing::clear(false);
    });
    start_line.count_down();
    for (auto &thread : threads) {
      thread.join();
    }

    const bool request_won = first_close == rtsp_stream::close_result_e::closed || second_close == rtsp_stream::close_result_e::closed;
    const int winners = static_cast<int>(first_close == rtsp_stream::close_result_e::closed) +
                        static_cast<int>(second_close == rtsp_stream::close_result_e::closed) + static_cast<int>(stop_won);
    EXPECT_EQ(winners, 1) << "iteration " << i << ": exactly one stop may win";
    EXPECT_EQ(stream::session::stop_reason(*session), request_won ? stream::stop_reason_e::api_close : stream::stop_reason_e::ping_timeout)
      << "iteration " << i << ": the recorded reason must be the winner's";

    rtsp_stream::testing::clear(true);
    EXPECT_EQ(rtsp_stream::session_count(), 0) << "iteration " << i << ": the session must be erased";
    if (::testing::Test::HasFailure()) {
      break;
    }
  }

  EXPECT_EQ(count_occurrences(test_log_since(offset), "Session ended"), static_cast<std::size_t>(iterations));
}
