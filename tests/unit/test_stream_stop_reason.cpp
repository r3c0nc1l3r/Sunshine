/**
 * @file tests/unit/test_stream_stop_reason.cpp
 * @brief Test stop reasons recorded on a streaming session that has not started.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <cstdint>
#include <vector>

// local includes
#include <src/rtsp.h>
#include <src/stream.h>
#include <src/stream_liveness.h>

namespace {
  /**
   * @brief Build a launch session with a known id and 16-byte zero GCM key and IV.
   *
   * @param id Launch session id to assign.
   * @return Launch session ready to pass to `stream::session::alloc`.
   */
  rtsp_stream::launch_session_t make_launch_session(std::uint32_t id) {
    rtsp_stream::launch_session_t launch_session {};
    launch_session.id = id;
    launch_session.gcm_key = std::vector<std::uint8_t>(16, 0);
    launch_session.iv = std::vector<std::uint8_t>(16, 0);
    return launch_session;
  }
}  // namespace

/**
 * @brief A freshly allocated session that was never started has no recorded stop reason.
 */
TEST(StopReasonNotStartedTests, FreshSessionHasUnknownStopReason) {
  stream::config_t config {};
  auto launch_session = make_launch_session(1);

  auto session = stream::session::alloc(config, launch_session);
  ASSERT_NE(session, nullptr);

  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::unknown);
}

/**
 * @brief A freshly allocated session that was never started is not in the running state.
 */
TEST(StopReasonNotStartedTests, FreshSessionIsNotRunning) {
  stream::config_t config {};
  auto launch_session = make_launch_session(2);

  auto session = stream::session::alloc(config, launch_session);
  ASSERT_NE(session, nullptr);

  EXPECT_NE(stream::session::state(*session), stream::session::state_e::RUNNING);
}

/**
 * @brief Stopping a session that is not running records nothing and leaves its state unchanged.
 */
TEST(StopReasonNotStartedTests, StopOnNotStartedSessionRecordsNothing) {
  stream::config_t config {};
  auto launch_session = make_launch_session(3);

  auto session = stream::session::alloc(config, launch_session);
  ASSERT_NE(session, nullptr);

  const auto state_before = stream::session::state(*session);

  stream::session::stop(*session, stream::stop_reason_e::api_close);

  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::unknown);
  EXPECT_EQ(stream::session::state(*session), state_before);
}

/**
 * @brief The session id reported by `stream::session::id` equals the launch session id it was allocated with.
 */
TEST(StopReasonNotStartedTests, IdMatchesLaunchSessionId) {
  stream::config_t config {};
  constexpr std::uint32_t launch_id = 0x1234abcd;
  auto launch_session = make_launch_session(launch_id);

  auto session = stream::session::alloc(config, launch_session);
  ASSERT_NE(session, nullptr);

  EXPECT_EQ(stream::session::id(*session), launch_id);
}
