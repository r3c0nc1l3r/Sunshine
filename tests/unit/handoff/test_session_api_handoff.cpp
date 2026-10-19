/**
 * @file tests/unit/handoff/test_session_api_handoff.cpp
 * @brief Tests for the Linux session API across both processes: the draining entries, the `handoff` key and the close
 * forwarding (`confighttp::session_entry_to_json`, `confighttp::sessions_reply`, `confighttp::close_session_any`).
 *
 * Written from the spec (`docs/drain-handoff.md`, "Visibility and control") and the P19 contract, using a fake
 * `session_peer_t` and the runtime's test seams.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <cstdint>
  #include <filesystem>
  #include <memory>
  #include <optional>
  #include <string>
  #include <string_view>
  #include <system_error>
  #include <vector>

  // lib includes
  #include <unistd.h>

  // local includes
  #include <src/confighttp.h>
  #include <src/handoff/instance_file.h>
  #include <src/handoff/peer_link.h>
  #include <src/handoff/protocol.h>
  #include <src/handoff/runtime.h>
  #include <src/rtsp.h>

namespace {
  /**
   * @brief Test double for the draining peer: fixed list and close replies, and records the close requests it gets.
   */
  class FakeSessionPeer final: public handoff::session_peer_t {
  public:
    std::optional<std::vector<handoff::protocol::session_entry_t>> list_reply;  ///< What `list` returns.
    std::optional<rtsp_stream::close_result_e> close_reply;  ///< What `close` returns.
    int close_calls = 0;  ///< Number of `close` calls.
    std::uint32_t last_closed_id = 0;  ///< Id of the last `close` call.

    bool exists() override {
      return true;
    }

    bool gone() override {
      return false;
    }

    handoff::drain_peer_t::reply_e end_all(std::chrono::milliseconds) override {
      return handoff::drain_peer_t::reply_e::no_reply;
    }

    std::optional<std::vector<handoff::protocol::session_entry_t>> list(std::chrono::milliseconds) override {
      return list_reply;
    }

    std::optional<rtsp_stream::close_result_e> close(std::uint32_t id, std::chrono::milliseconds) override {
      ++close_calls;
      last_closed_id = id;
      return close_reply;
    }

    std::optional<std::uint32_t> close_cert(std::string_view, std::chrono::milliseconds) override {
      return std::nullopt;
    }

    handoff::state_e state() override {
      return handoff::state_e::draining;
    }
  };

  /**
   * @brief Build a session entry of the draining process with fixed values.
   *
   * @param id Launch session id.
   * @param started_unix_ms Start time in Unix epoch milliseconds.
   * @return Entry with transport `udp/ipv6` and category `wan`.
   */
  handoff::protocol::session_entry_t make_entry(std::uint32_t id, std::int64_t started_unix_ms) {
    handoff::protocol::session_entry_t entry;
    entry.id = id;
    entry.started_unix_ms = started_unix_ms;
    entry.transport = "udp/ipv6";
    entry.client_category = "wan";
    return entry;
  }
}  // namespace

/**
 * @brief Test: a draining session entry converts to the five documented keys, with `started` in whole seconds.
 */
TEST(SessionApiHandoffJsonTest, SessionEntryToJsonHasFiveKeysAndDrainingTrue) {
  const nlohmann::json json = confighttp::session_entry_to_json(make_entry(9, 1700000000123));

  const nlohmann::json expected {
    {"id", 9},
    {"started", 1700000000},
    {"transport", "udp/ipv6"},
    {"client_category", "wan"},
    {"draining", true},
  };
  EXPECT_EQ(json, expected);
  EXPECT_EQ(json.size(), 5U);
}

/**
 * @brief Fixture: a clean handoff runtime (no peer, no instance) before and after each test, and a temporary
 * instances directory that is removed afterwards.
 */
class SessionApiHandoffTest: public ::testing::Test {
protected:
  std::filesystem::path _dir;  ///< Temporary instances directory.

  /**
   * @brief Reset the runtime and create an empty temporary directory.
   */
  void SetUp() override {
    handoff::runtime().reset_for_test();
    _dir = std::filesystem::temp_directory_path() / ("session_api_handoff_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
    std::filesystem::create_directories(_dir, ec);
  }

  /**
   * @brief Drop the peer and the instance writer, then remove the temporary directory.
   */
  void TearDown() override {
    handoff::runtime().reset_for_test();
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
  }
};

/**
 * @brief Test: with no peer and no instance, the sessions reply has `status` true, no sessions and `handoff` "none".
 */
TEST_F(SessionApiHandoffTest, SessionsReplyWithoutPeerOrInstanceIsOwnListAndNone) {
  const nlohmann::json reply = confighttp::sessions_reply();

  const nlohmann::json expected {
    {"status", true},
    {"sessions", nlohmann::json::array()},
    {"handoff", "none"},
  };
  EXPECT_EQ(reply, expected);
}

/**
 * @brief Test: draining entries from the peer's list are appended after this process's sessions and marked draining.
 */
TEST_F(SessionApiHandoffTest, SessionsReplyListsPeerEntriesAsDraining) {
  auto peer = std::make_shared<FakeSessionPeer>();
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {make_entry(11, 1700000001000), make_entry(12, 1700000002500)};
  handoff::runtime().set_peer(peer);

  const nlohmann::json reply = confighttp::sessions_reply();

  ASSERT_TRUE(reply.contains("sessions"));
  ASSERT_EQ(reply["sessions"].size(), 2U);
  EXPECT_EQ(reply["sessions"][0]["id"], 11);
  EXPECT_EQ(reply["sessions"][1]["id"], 12);
  for (const auto &entry : reply["sessions"]) {
    EXPECT_EQ(entry["draining"], true);
  }
}

/**
 * @brief Test: when the peer does not answer the list request, the reply still has `status` and only this process's
 * sessions (none here) and does not throw.
 */
TEST_F(SessionApiHandoffTest, SessionsReplyWithoutAnswerFromPeerShowsOnlyOwnSessions) {
  auto peer = std::make_shared<FakeSessionPeer>();
  peer->list_reply = std::nullopt;
  handoff::runtime().set_peer(peer);

  nlohmann::json reply;
  EXPECT_NO_THROW(reply = confighttp::sessions_reply());

  EXPECT_EQ(reply["status"], true);
  EXPECT_EQ(reply["sessions"], nlohmann::json::array());
}

/**
 * @brief Test: the top-level `handoff` key follows the instance state, and is "draining" after the state is set.
 */
TEST_F(SessionApiHandoffTest, SessionsReplyHandoffKeyFollowsInstanceState) {
  handoff::instance_t initial;
  initial.pid = ::getpid();
  initial.start_time = 1;
  initial.role = handoff::role_e::active;
  initial.udp_base = 47989;
  initial.sessions = 0;
  initial.handoff = handoff::state_e::none;
  ASSERT_TRUE(handoff::runtime().open_instance(_dir, initial)) << "Precondition: open_instance must succeed.";

  ASSERT_TRUE(handoff::runtime().instance_set_state(handoff::state_e::draining)) << "Precondition: state change must succeed.";

  EXPECT_EQ(confighttp::sessions_reply()["handoff"], "draining");
}

/**
 * @brief Test: closing an unknown id with no peer returns `not_found`.
 */
TEST_F(SessionApiHandoffTest, CloseSessionAnyWithoutPeerIsNotFound) {
  EXPECT_EQ(confighttp::close_session_any(12345), rtsp_stream::close_result_e::not_found);
}

/**
 * @brief Test: closing an unknown id is forwarded to the peer, whose `closed` answer is returned.
 */
TEST_F(SessionApiHandoffTest, CloseSessionAnyForwardsToPeerAndReturnsItsAnswer) {
  auto peer = std::make_shared<FakeSessionPeer>();
  peer->close_reply = rtsp_stream::close_result_e::closed;
  handoff::runtime().set_peer(peer);

  EXPECT_EQ(confighttp::close_session_any(12345), rtsp_stream::close_result_e::closed);
  EXPECT_EQ(peer->close_calls, 1);
  EXPECT_EQ(peer->last_closed_id, 12345U);
}

/**
 * @brief Test: when the peer does not answer the close request, the result is `not_found` and nothing throws.
 */
TEST_F(SessionApiHandoffTest, CloseSessionAnyWithoutAnswerFromPeerIsNotFound) {
  auto peer = std::make_shared<FakeSessionPeer>();
  peer->close_reply = std::nullopt;
  handoff::runtime().set_peer(peer);

  EXPECT_EQ(confighttp::close_session_any(12345), rtsp_stream::close_result_e::not_found);
  EXPECT_EQ(peer->close_calls, 1);
}

#endif  // __linux__
