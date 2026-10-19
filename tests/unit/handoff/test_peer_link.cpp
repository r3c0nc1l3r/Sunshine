/**
 * @file tests/unit/handoff/test_peer_link.cpp
 * @brief Blind tests for the active process's link to its draining peer (src/handoff/peer_link.h): `cert_sha256_hex`,
 * the common request flow of `list`, `close`, `close_cert` and `end_all`, the gone and unknown results, `state`,
 * `deliver` and `on_channel_closed`.
 *
 * The draining process is played by the test: it owns the far end of a real socketpair channel, reads the requests the
 * link sends and routes replies back through `peer_link_t::deliver`, as the channel reader would. The pidfd is an
 * `eventfd`: writing to it makes it readable, which is how the header says a test "kills" the peer.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <atomic>
  #include <cctype>
  #include <chrono>
  #include <cstdint>
  #include <functional>
  #include <future>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <thread>
  #include <type_traits>
  #include <variant>
  #include <vector>

  // lib includes
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/eventfd.h>
  #include <unistd.h>

  // local includes
  #include "src/handoff/peer_link.h"

using namespace std::chrono_literals;

namespace {
  using handoff::drain_peer_t;
  using handoff::peer_link_t;
  using handoff::state_e;
  namespace protocol = handoff::protocol;
  using clock_type = std::chrono::steady_clock;

  /// A valid lowercase 64-character hash for `close_cert`.
  const std::string valid_hash(64, 'a');

  /**
   * @brief Request id of a request message.
   *
   * @param message Message.
   * @return The id, or 0 when the message is not one of the four requests.
   */
  std::uint64_t request_id_of(const protocol::message_t &message) {
    return std::visit(
      [](const auto &m) -> std::uint64_t {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, protocol::sessions_request_t> || std::is_same_v<T, protocol::close_request_t> || std::is_same_v<T, protocol::close_cert_request_t> || std::is_same_v<T, protocol::end_all_t>) {
          return m.request_id;
        } else {
          return 0;
        }
      },
      message
    );
  }

  /**
   * @brief Milliseconds since `start`.
   *
   * @param start Start time.
   * @return Elapsed milliseconds.
   */
  std::chrono::milliseconds since(clock_type::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start);
  }

  /**
   * @brief Fixture: a link over a socketpair, a far end played by the test, and an eventfd as the pidfd.
   */
  class HandoffPeerLinkTest: public BaseTest {
  protected:
    using handler_t = std::function<std::optional<protocol::message_t>(const protocol::message_t &)>;

    void SetUp() override {
      BaseTest::SetUp();
      auto pair = handoff::make_channel_pair();
      ASSERT_TRUE(pair.has_value()) << "make_channel_pair must give a socketpair for the tests";
      _link_channel = std::make_shared<handoff::channel_t>(pair->first);
      _far = std::make_shared<handoff::channel_t>(pair->second);
      _event_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
      ASSERT_GE(_event_fd, 0) << "eventfd must be available";
      _link = std::make_unique<peer_link_t>(_link_channel, _event_fd);
    }

    void TearDown() override {
      stop_responder();
      _link.reset();
      _far.reset();
      _link_channel.reset();
      BaseTest::TearDown();
    }

    /// Make the "draining process" gone: the pidfd (eventfd) becomes readable.
    void make_gone() {
      const std::uint64_t one = 1;
      ASSERT_EQ(::write(_event_fd, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
    }

    /// Start the far end: every received message is recorded, and `handler`'s reply (if any) is delivered to the link.
    void start_responder(handler_t handler) {
      _stop = false;
      _responder = std::thread([this, handler = std::move(handler)] {
        while (!_stop.load()) {
          handoff::received_t received;
          if (_far->receive(received, 10ms) != handoff::recv_status_e::ok) {
            continue;
          }
          {
            std::lock_guard lock(_mutex);
            _requests.push_back(received.message);
          }
          if (handler) {
            if (auto reply = handler(received.message)) {
              _link->deliver(*reply);
            }
          }
        }
      });
    }

    /// Start the far end without replies: it only records requests.
    void start_recorder() {
      start_responder(nullptr);
    }

    void stop_responder() {
      _stop = true;
      if (_responder.joinable()) {
        _responder.join();
      }
    }

    /// Requests received so far by the far end.
    std::vector<protocol::message_t> requests() {
      std::lock_guard lock(_mutex);
      return _requests;
    }

    /// Wait (bounded) until the far end has received `count` requests.
    bool wait_requests(std::size_t count, std::chrono::milliseconds limit = 1000ms) {
      const auto deadline = clock_type::now() + limit;
      while (clock_type::now() < deadline) {
        if (requests().size() >= count) {
          return true;
        }
        std::this_thread::sleep_for(2ms);
      }
      return requests().size() >= count;
    }

    /// Whether the far end receives anything within `wait` (used to prove that nothing was sent).
    bool far_end_receives_within(std::chrono::milliseconds wait) {
      std::this_thread::sleep_for(wait);
      return !requests().empty();
    }

    /// Reply to every request of the four kinds with the given factory of reply messages.
    static handler_t reply_with(std::function<protocol::message_t(std::uint64_t)> make_reply) {
      return [make_reply = std::move(make_reply)](const protocol::message_t &request) -> std::optional<protocol::message_t> {
        const auto id = request_id_of(request);
        if (id == 0) {
          return std::nullopt;
        }
        return make_reply(id);
      };
    }

    std::shared_ptr<handoff::channel_t> _link_channel;  ///< The link's end.
    std::shared_ptr<handoff::channel_t> _far;  ///< The test's end (the draining process).
    int _event_fd {-1};  ///< The pidfd stand-in; owned by `_link`.
    std::unique_ptr<peer_link_t> _link;  ///< Object under test.
    std::thread _responder;  ///< Far end thread.
    std::atomic<bool> _stop {false};  ///< Stops the far end thread.
    std::mutex _mutex;  ///< Guards `_requests`.
    std::vector<protocol::message_t> _requests;  ///< Requests received by the far end.
  };

  const std::vector<protocol::session_entry_t> two_sessions {
    {7, 1700000000000, "udp/ipv4", "lan"},
    {9, 1700000001000, "udp/ipv6", "wan"},
  };
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// cert_sha256_hex
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffPeerLinkHashTest, EmptyInputGivesSha256OfEmptyString) {
  EXPECT_EQ(handoff::cert_sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
    << "An empty certificate must give the SHA-256 of the empty string in lowercase hex.";
}

TEST(HandoffPeerLinkHashTest, KnownVector) {
  EXPECT_EQ(handoff::cert_sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    << "cert_sha256_hex must be the lowercase hex SHA-256 of the exact bytes.";
}

TEST(HandoffPeerLinkHashTest, ResultIsSixtyFourLowercaseHexCharacters) {
  const auto hash = handoff::cert_sha256_hex("-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n");
  ASSERT_EQ(hash.size(), 64u) << "The hash must have 64 characters.";
  for (const char c : hash) {
    EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(c)))
      << "The hash must use lowercase hex digits only; got '" << c << "'.";
  }
}

TEST(HandoffPeerLinkHashTest, ExactBytesAreHashedWithoutTrimming) {
  EXPECT_NE(handoff::cert_sha256_hex("abc"), handoff::cert_sha256_hex("abc\n")) << "A trailing newline is part of the input and must change the hash.";
  EXPECT_NE(handoff::cert_sha256_hex("abc"), handoff::cert_sha256_hex(" abc")) << "A leading blank is part of the input and must change the hash.";
  EXPECT_EQ(handoff::cert_sha256_hex("abc"), handoff::cert_sha256_hex(std::string("abc"))) << "The same bytes must give the same hash.";
}

TEST(HandoffPeerLinkHashTest, HashIsAcceptedByCloseCert) {
  // The output of cert_sha256_hex must be exactly what close_cert accepts as valid (it is sent, so the far end sees it).
  auto pair = handoff::make_channel_pair();
  ASSERT_TRUE(pair.has_value());
  auto link_channel = std::make_shared<handoff::channel_t>(pair->first);
  handoff::channel_t far(pair->second);
  peer_link_t link(link_channel, -1);
  const auto hash = handoff::cert_sha256_hex("some certificate");
  const auto start = clock_type::now();
  const auto result = link.close_cert(hash, 30ms);
  EXPECT_FALSE(result.has_value()) << "No reply was given, so the result is unknown.";
  EXPECT_LT(since(start), 1000ms);
  handoff::received_t received;
  ASSERT_EQ(far.receive(received, 200ms), handoff::recv_status_e::ok) << "A hash from cert_sha256_hex must be valid for close_cert and be sent.";
  const auto *request = std::get_if<protocol::close_cert_request_t>(&received.message);
  ASSERT_NE(request, nullptr);
  EXPECT_EQ(request->cert_sha256, hash);
}

// ---------------------------------------------------------------------------------------------------------------------
// exists, gone, state, destructor
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffPeerLinkTest, ExistsAndNotGoneWhilePidfdIsQuiet) {
  EXPECT_TRUE(_link->exists()) << "A fresh link with a quiet pidfd must report the draining process as existing.";
  EXPECT_FALSE(_link->gone()) << "A quiet pidfd must not be gone.";
}

TEST_F(HandoffPeerLinkTest, ReadablePidfdMeansGone) {
  make_gone();
  EXPECT_TRUE(_link->gone()) << "A readable pidfd means the draining process is gone.";
  EXPECT_FALSE(_link->exists()) << "exists() must be !gone().";
}

TEST_F(HandoffPeerLinkTest, GoneIsSticky) {
  make_gone();
  ASSERT_TRUE(_link->gone());
  std::uint64_t counter = 0;
  ASSERT_EQ(::read(_event_fd, &counter, sizeof(counter)), static_cast<ssize_t>(sizeof(counter)));
  EXPECT_TRUE(_link->gone()) << "Once gone, the link must stay gone even if the fd stops being readable.";
  EXPECT_FALSE(_link->exists());
}

TEST(HandoffPeerLinkNoFdTest, PidfdMinusOneIsNeverGone) {
  peer_link_t link(nullptr, -1);
  EXPECT_FALSE(link.gone()) << "A pidfd of -1 can never show gone.";
  EXPECT_TRUE(link.exists()) << "A pidfd of -1 must keep exists() true so the terminate rule never terminates on a guess.";
}

TEST_F(HandoffPeerLinkTest, StateIsNoneInitially) {
  EXPECT_EQ(_link->state(), state_e::none) << "state() must be none until the first HANDOFF_STATE arrives.";
}

TEST_F(HandoffPeerLinkTest, StateFollowsDeliveredHandoffState) {
  EXPECT_TRUE(_link->deliver(protocol::handoff_state_t {"draining"})) << "HANDOFF_STATE is consumed by deliver.";
  EXPECT_EQ(_link->state(), state_e::draining);
  EXPECT_TRUE(_link->deliver(protocol::handoff_state_t {"handover_failed"}));
  EXPECT_EQ(_link->state(), state_e::handover_failed);
  EXPECT_TRUE(_link->deliver(protocol::handoff_state_t {"kill_pending"}));
  EXPECT_EQ(_link->state(), state_e::kill_pending);
  EXPECT_TRUE(_link->deliver(protocol::handoff_state_t {"none"}));
  EXPECT_EQ(_link->state(), state_e::none) << "The last announced state wins, including none.";
}

TEST_F(HandoffPeerLinkTest, StateIsNoneOnceGone) {
  ASSERT_TRUE(_link->deliver(protocol::handoff_state_t {"draining"}));
  ASSERT_EQ(_link->state(), state_e::draining);
  make_gone();
  EXPECT_EQ(_link->state(), state_e::none) << "state() must be none once the peer is gone.";
}

TEST_F(HandoffPeerLinkTest, StateDoesNotBlock) {
  const auto start = clock_type::now();
  for (int i = 0; i < 100; ++i) {
    (void) _link->state();
  }
  EXPECT_LT(since(start), 200ms) << "state() must never block.";
}

TEST(HandoffPeerLinkDestructorTest, DestructorClosesPidfd) {
  const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  ASSERT_GE(fd, 0);
  {
    peer_link_t link(nullptr, fd);
    EXPECT_NE(::fcntl(fd, F_GETFD), -1) << "The link must not close the pidfd while it lives.";
  }
  errno = 0;
  EXPECT_EQ(::fcntl(fd, F_GETFD), -1) << "The link owns the pidfd and must close it in the destructor.";
  EXPECT_EQ(errno, EBADF);
}

// ---------------------------------------------------------------------------------------------------------------------
// deliver
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffPeerLinkTest, DeliverIgnoresMessagesItDoesNotHandle) {
  EXPECT_FALSE(_link->deliver(protocol::main_t {})) << "MAIN is not for the link; deliver must return false.";
  EXPECT_FALSE(_link->deliver(protocol::pong_t {3}));
  EXPECT_FALSE(_link->deliver(protocol::ping_t {3}));
  EXPECT_FALSE(_link->deliver(protocol::unknown_t {"FUTURE"}));
  EXPECT_FALSE(_link->deliver(protocol::hello_t {}));
  EXPECT_FALSE(_link->deliver(protocol::commit_t {}));
  EXPECT_FALSE(_link->deliver(protocol::sessions_request_t {1}));
  EXPECT_FALSE(_link->deliver(protocol::end_all_t {1}));
  EXPECT_EQ(_link->state(), state_e::none) << "Ignored messages must not change the state.";
}

TEST_F(HandoffPeerLinkTest, DeliverOfReplyWithUnknownIdReturnsFalse) {
  EXPECT_FALSE(_link->deliver(protocol::sessions_reply_t {999, {}})) << "A SESSIONS reply nobody waits for is dropped (false).";
  EXPECT_FALSE(_link->deliver(protocol::close_reply_t {999, "closed"}));
  EXPECT_FALSE(_link->deliver(protocol::close_cert_reply_t {999, 1}));
  EXPECT_FALSE(_link->deliver(protocol::ended_t {999}));
  EXPECT_FALSE(_link->deliver(protocol::end_rejected_t {999}));
}

TEST_F(HandoffPeerLinkTest, LateReplyAfterTimeoutIsDropped) {
  start_recorder();
  EXPECT_FALSE(_link->list(40ms).has_value());
  ASSERT_TRUE(wait_requests(1));
  const auto sent = requests();
  ASSERT_EQ(sent.size(), 1u);
  const auto id = request_id_of(sent[0]);
  EXPECT_FALSE(_link->deliver(protocol::sessions_reply_t {id, two_sessions})) << "A reply after the waiter timed out must be dropped (false).";
}

// ---------------------------------------------------------------------------------------------------------------------
// list
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffPeerLinkTest, ListSendsSessionsRequestAndReturnsEntries) {
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::sessions_reply_t {id, two_sessions}};
  }));
  const auto result = _link->list(1000ms);
  ASSERT_TRUE(result.has_value()) << "A SESSIONS reply must give the entries.";
  EXPECT_EQ(*result, two_sessions);
  const auto sent = requests();
  ASSERT_EQ(sent.size(), 1u);
  const auto *request = std::get_if<protocol::sessions_request_t>(&sent[0]);
  ASSERT_NE(request, nullptr) << "list must send SESSIONS?.";
  EXPECT_NE(request->request_id, 0u) << "The request id must be a real id from the tracker (ids start at 1).";
}

TEST_F(HandoffPeerLinkTest, ListWithEmptyReplyGivesEmptyVectorNotNullopt) {
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::sessions_reply_t {id, {}}};
  }));
  const auto result = _link->list(1000ms);
  ASSERT_TRUE(result.has_value()) << "An empty SESSIONS reply is an answer: an empty vector, not 'unknown'.";
  EXPECT_TRUE(result->empty());
}

TEST_F(HandoffPeerLinkTest, ListTimeoutWhenNotGoneIsUnknown) {
  start_recorder();
  const auto start = clock_type::now();
  const auto result = _link->list(80ms);
  const auto elapsed = since(start);
  EXPECT_FALSE(result.has_value()) << "No reply and the peer is not gone: the count is unknown (nullopt).";
  EXPECT_GE(elapsed, 60ms) << "The call must wait for its timeout when the request was sent.";
  EXPECT_LT(elapsed, 1000ms);
  EXPECT_TRUE(wait_requests(1)) << "The request must have been sent.";
}

TEST_F(HandoffPeerLinkTest, ListTimeoutWhenGoneIsEmptyVector) {
  start_recorder();
  make_gone();
  const auto result = _link->list(50ms);
  ASSERT_TRUE(result.has_value()) << "A gone process has no sessions: an empty vector, not nullopt.";
  EXPECT_TRUE(result->empty());
}

TEST_F(HandoffPeerLinkTest, ListWrongTypedReplyCountsAsNoReply) {
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::ended_t {id}};
  }));
  const auto result = _link->list(1000ms);
  EXPECT_FALSE(result.has_value()) << "A reply of the wrong type for the id counts as no reply (nullopt while not gone).";
}

TEST_F(HandoffPeerLinkTest, ListWrongTypedReplyWhenGoneIsEmptyVector) {
  make_gone();
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::close_reply_t {id, "closed"}};
  }));
  const auto result = _link->list(1000ms);
  ASSERT_TRUE(result.has_value()) << "No usable reply and the peer gone: the gone result (empty vector).";
  EXPECT_TRUE(result->empty());
}

TEST_F(HandoffPeerLinkTest, ListReplyWithOtherIdDoesNotAnswer) {
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::sessions_reply_t {id + 1000, two_sessions}};
  }));
  EXPECT_FALSE(_link->list(60ms).has_value()) << "A reply whose id differs from the request's must not answer it.";
}

TEST_F(HandoffPeerLinkTest, ListWithZeroTimeoutSendsNothing) {
  start_recorder();
  const auto start = clock_type::now();
  EXPECT_FALSE(_link->list(0ms).has_value()) << "timeout 0: nothing is sent, unknown result while not gone.";
  EXPECT_LT(since(start), 300ms);
  EXPECT_FALSE(far_end_receives_within(60ms)) << "With timeout <= 0 nothing may be sent.";
}

TEST_F(HandoffPeerLinkTest, ListWithNegativeTimeoutSendsNothing) {
  start_recorder();
  EXPECT_FALSE(_link->list(-5ms).has_value());
  EXPECT_FALSE(far_end_receives_within(60ms)) << "With timeout <= 0 nothing may be sent.";
}

TEST_F(HandoffPeerLinkTest, ListWithZeroTimeoutWhenGoneIsEmptyVector) {
  make_gone();
  const auto result = _link->list(0ms);
  ASSERT_TRUE(result.has_value()) << "The gone check applies to the timeout <= 0 path too: empty vector.";
  EXPECT_TRUE(result->empty());
}

TEST_F(HandoffPeerLinkTest, ListOnClosedChannelFailsAtOnce) {
  _link_channel->close();
  const auto start = clock_type::now();
  EXPECT_FALSE(_link->list(2000ms).has_value()) << "A send on a closed channel fails: unknown result.";
  EXPECT_LT(since(start), 500ms) << "A request that cannot be sent must not wait for its timeout.";
}

TEST_F(HandoffPeerLinkTest, ListOnClosedChannelWhenGoneIsEmptyVector) {
  _link_channel->close();
  make_gone();
  const auto result = _link->list(2000ms);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->empty());
}

TEST_F(HandoffPeerLinkTest, ListSendFailureBecausePeerClosedFailsAtOnce) {
  _far->close();
  const auto start = clock_type::now();
  EXPECT_FALSE(_link->list(2000ms).has_value()) << "A failed send gives the unknown result.";
  EXPECT_LT(since(start), 500ms) << "A failed send must end the request at once, without waiting.";
}

TEST_F(HandoffPeerLinkTest, ListSendFailureBecauseBufferIsFullFailsAtOnce) {
  // Nobody reads the far end: fill the socket buffer until a send reports EAGAIN (false).
  bool full = false;
  for (int i = 0; i < 200000 && !full; ++i) {
    full = !_link_channel->send(protocol::ping_t {static_cast<std::uint64_t>(i)});
  }
  ASSERT_TRUE(full) << "Test setup: the socket buffer must fill up.";
  const auto start = clock_type::now();
  EXPECT_FALSE(_link->list(2000ms).has_value());
  EXPECT_LT(since(start), 500ms) << "An EAGAIN send must end the request at once (no retry, no queue).";
}

TEST_F(HandoffPeerLinkTest, ListWithNullChannelFailsAtOnce) {
  peer_link_t link(nullptr, -1);
  const auto start = clock_type::now();
  EXPECT_FALSE(link.list(2000ms).has_value()) << "A null channel makes every request fail without waiting.";
  EXPECT_LT(since(start), 500ms);
}

TEST_F(HandoffPeerLinkTest, RepeatedRequestsUseFreshIds) {
  start_recorder();
  (void) _link->list(20ms);
  (void) _link->list(20ms);
  (void) _link->close(1, 20ms);
  ASSERT_TRUE(wait_requests(3));
  const auto sent = requests();
  EXPECT_NE(request_id_of(sent[0]), request_id_of(sent[1])) << "Every request must get its own request id.";
  EXPECT_NE(request_id_of(sent[1]), request_id_of(sent[2]));
  EXPECT_NE(request_id_of(sent[0]), request_id_of(sent[2]));
}

// ---------------------------------------------------------------------------------------------------------------------
// close
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffPeerLinkTest, CloseMapsWireResults) {
  const std::vector<std::pair<std::string, rtsp_stream::close_result_e>> table {
    {"closed", rtsp_stream::close_result_e::closed},
    {"already-stopping", rtsp_stream::close_result_e::already_stopping},
    {"not-found", rtsp_stream::close_result_e::not_found},
  };
  for (const auto &[wire, expected] : table) {
    SCOPED_TRACE(wire);
    stop_responder();
    {
      std::lock_guard lock(_mutex);
      _requests.clear();
    }
    start_responder(reply_with([wire](std::uint64_t id) {
      return protocol::message_t {protocol::close_reply_t {id, wire}};
    }));
    const auto result = _link->close(42, 1000ms);
    ASSERT_TRUE(result.has_value()) << "A CLOSED reply must give a result for '" << wire << "'.";
    EXPECT_EQ(*result, expected) << "Wire result '" << wire << "' maps one to one.";
    const auto sent = requests();
    ASSERT_EQ(sent.size(), 1u);
    const auto *request = std::get_if<protocol::close_request_t>(&sent[0]);
    ASSERT_NE(request, nullptr) << "close must send CLOSE.";
    EXPECT_EQ(request->session_id, 42u) << "CLOSE must carry the session id.";
  }
}

TEST_F(HandoffPeerLinkTest, CloseTimeoutWhenNotGoneIsUnknown) {
  start_recorder();
  EXPECT_FALSE(_link->close(5, 50ms).has_value()) << "No reply, peer not gone: nullopt.";
}

TEST_F(HandoffPeerLinkTest, CloseTimeoutWhenGoneIsNotFound) {
  start_recorder();
  make_gone();
  const auto result = _link->close(5, 50ms);
  ASSERT_TRUE(result.has_value()) << "A gone peer has no session: not_found, not nullopt.";
  EXPECT_EQ(*result, rtsp_stream::close_result_e::not_found);
}

TEST_F(HandoffPeerLinkTest, CloseWrongTypedReplyCountsAsNoReply) {
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::sessions_reply_t {id, {}}};
  }));
  EXPECT_FALSE(_link->close(5, 1000ms).has_value()) << "A reply of the wrong type for the id counts as no reply.";
}

TEST_F(HandoffPeerLinkTest, CloseWithNonPositiveTimeoutSendsNothing) {
  start_recorder();
  EXPECT_FALSE(_link->close(5, 0ms).has_value());
  EXPECT_FALSE(_link->close(5, -1ms).has_value());
  EXPECT_FALSE(far_end_receives_within(60ms)) << "With timeout <= 0 nothing may be sent.";
  make_gone();
  const auto result = _link->close(5, 0ms);
  ASSERT_TRUE(result.has_value()) << "Timeout <= 0 with a gone peer: not_found.";
  EXPECT_EQ(*result, rtsp_stream::close_result_e::not_found);
}

TEST_F(HandoffPeerLinkTest, CloseOnClosedChannelFailsAtOnce) {
  _link_channel->close();
  const auto start = clock_type::now();
  EXPECT_FALSE(_link->close(5, 2000ms).has_value());
  EXPECT_LT(since(start), 500ms);
}

// ---------------------------------------------------------------------------------------------------------------------
// close_cert
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffPeerLinkTest, CloseCertSendsHashAndReturnsCount) {
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::close_cert_reply_t {id, 3}};
  }));
  const auto result = _link->close_cert(valid_hash, 1000ms);
  ASSERT_TRUE(result.has_value()) << "A CLOSED_CERT reply must give the count.";
  EXPECT_EQ(*result, 3u);
  const auto sent = requests();
  ASSERT_EQ(sent.size(), 1u);
  const auto *request = std::get_if<protocol::close_cert_request_t>(&sent[0]);
  ASSERT_NE(request, nullptr) << "close_cert must send CLOSE_CERT.";
  EXPECT_EQ(request->cert_sha256, valid_hash);
}

TEST_F(HandoffPeerLinkTest, CloseCertZeroCountIsAnAnswerNotUnknown) {
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::close_cert_reply_t {id, 0}};
  }));
  const auto result = _link->close_cert(valid_hash, 1000ms);
  ASSERT_TRUE(result.has_value()) << "A reply with count 0 is an answer, not nullopt.";
  EXPECT_EQ(*result, 0u);
}

TEST_F(HandoffPeerLinkTest, CloseCertTimeoutWhenNotGoneIsUnknown) {
  start_recorder();
  EXPECT_FALSE(_link->close_cert(valid_hash, 50ms).has_value()) << "No reply, peer not gone: nullopt.";
}

TEST_F(HandoffPeerLinkTest, CloseCertTimeoutWhenGoneIsZero) {
  start_recorder();
  make_gone();
  const auto result = _link->close_cert(valid_hash, 50ms);
  ASSERT_TRUE(result.has_value()) << "A gone peer: 0 sessions, not nullopt.";
  EXPECT_EQ(*result, 0u);
}

TEST_F(HandoffPeerLinkTest, CloseCertWrongTypedReplyCountsAsNoReply) {
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::ended_t {id}};
  }));
  EXPECT_FALSE(_link->close_cert(valid_hash, 1000ms).has_value());
}

TEST_F(HandoffPeerLinkTest, CloseCertInvalidHashIsNotSentAndGivesNullopt) {
  start_recorder();
  const std::vector<std::string> invalid {
    "",
    "abc",
    std::string(63, 'a'),
    std::string(65, 'a'),
    std::string(64, 'A'),
    std::string(63, 'a') + "g",
    std::string(63, 'a') + " ",
    std::string(63, 'a') + "\n",
  };
  for (const auto &hash : invalid) {
    SCOPED_TRACE("hash size " + std::to_string(hash.size()));
    const auto start = clock_type::now();
    EXPECT_FALSE(_link->close_cert(hash, 1000ms).has_value()) << "A value that is not exactly 64 lowercase hex characters must give nullopt.";
    EXPECT_LT(since(start), 300ms) << "An invalid hash must be rejected without waiting.";
  }
  EXPECT_FALSE(far_end_receives_within(60ms)) << "An invalid hash must not be sent.";
}

TEST_F(HandoffPeerLinkTest, CloseCertInvalidHashIsCheckedBeforeGone) {
  make_gone();
  EXPECT_FALSE(_link->close_cert("nothex", 100ms).has_value()) << "The argument check comes before the gone check: nullopt, not 0.";
}

TEST_F(HandoffPeerLinkTest, CloseCertInvalidHashIsCheckedBeforeTimeoutRule) {
  make_gone();
  EXPECT_FALSE(_link->close_cert("nothex", 0ms).has_value()) << "An invalid hash gives nullopt even with timeout 0 and a gone peer.";
  EXPECT_FALSE(_link->close_cert("nothex", -1ms).has_value());
}

TEST_F(HandoffPeerLinkTest, CloseCertWithNonPositiveTimeoutSendsNothing) {
  start_recorder();
  EXPECT_FALSE(_link->close_cert(valid_hash, 0ms).has_value());
  EXPECT_FALSE(_link->close_cert(valid_hash, -1ms).has_value());
  EXPECT_FALSE(far_end_receives_within(60ms)) << "With timeout <= 0 nothing may be sent.";
  make_gone();
  const auto result = _link->close_cert(valid_hash, 0ms);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0u) << "Timeout <= 0 with a gone peer: 0.";
}

TEST_F(HandoffPeerLinkTest, CloseCertOnClosedChannelFailsAtOnce) {
  _link_channel->close();
  const auto start = clock_type::now();
  EXPECT_FALSE(_link->close_cert(valid_hash, 2000ms).has_value());
  EXPECT_LT(since(start), 500ms);
}

// ---------------------------------------------------------------------------------------------------------------------
// end_all
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffPeerLinkTest, EndAllEndedReply) {
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::ended_t {id}};
  }));
  EXPECT_EQ(_link->end_all(1000ms), drain_peer_t::reply_e::ended) << "ENDED must give reply_e::ended.";
  const auto sent = requests();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<protocol::end_all_t>(sent[0])) << "end_all must send END_ALL.";
  EXPECT_NE(request_id_of(sent[0]), 0u);
}

TEST_F(HandoffPeerLinkTest, EndAllRejectedReply) {
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::end_rejected_t {id}};
  }));
  EXPECT_EQ(_link->end_all(1000ms), drain_peer_t::reply_e::rejected) << "END_REJECTED must give reply_e::rejected.";
}

TEST_F(HandoffPeerLinkTest, EndAllTimeoutIsNoReply) {
  start_recorder();
  const auto start = clock_type::now();
  EXPECT_EQ(_link->end_all(80ms), drain_peer_t::reply_e::no_reply);
  EXPECT_GE(since(start), 60ms) << "end_all must wait for its timeout when the request was sent.";
}

TEST_F(HandoffPeerLinkTest, EndAllDoesNotLookAtThePidfd) {
  start_recorder();
  make_gone();
  EXPECT_EQ(_link->end_all(50ms), drain_peer_t::reply_e::no_reply) << "end_all gives no_reply even when the peer is gone; the terminate rule polls gone() itself.";
  EXPECT_EQ(_link->end_all(0ms), drain_peer_t::reply_e::no_reply);
}

TEST_F(HandoffPeerLinkTest, EndAllWrongTypedReplyIsNoReply) {
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::sessions_reply_t {id, {}}};
  }));
  EXPECT_EQ(_link->end_all(1000ms), drain_peer_t::reply_e::no_reply) << "A reply of the wrong type counts as no reply.";
}

TEST_F(HandoffPeerLinkTest, EndAllNonPositiveTimeoutSendsNothing) {
  start_recorder();
  EXPECT_EQ(_link->end_all(0ms), drain_peer_t::reply_e::no_reply);
  EXPECT_EQ(_link->end_all(-10ms), drain_peer_t::reply_e::no_reply);
  EXPECT_FALSE(far_end_receives_within(60ms)) << "With timeout <= 0 nothing may be sent.";
}

TEST_F(HandoffPeerLinkTest, EndAllOnClosedChannelIsNoReplyAtOnce) {
  _link_channel->close();
  const auto start = clock_type::now();
  EXPECT_EQ(_link->end_all(2000ms), drain_peer_t::reply_e::no_reply);
  EXPECT_LT(since(start), 500ms);
}

TEST_F(HandoffPeerLinkTest, EndAllSendFailureIsNoReplyAtOnce) {
  _far->close();
  const auto start = clock_type::now();
  EXPECT_EQ(_link->end_all(2000ms), drain_peer_t::reply_e::no_reply);
  EXPECT_LT(since(start), 500ms) << "A failed send must not wait for the timeout.";
}

TEST_F(HandoffPeerLinkTest, ConcurrentEndAllSendTwoRequests) {
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::ended_t {id}};
  }));
  auto first = std::async(std::launch::async, [this] {
    return _link->end_all(1500ms);
  });
  auto second = std::async(std::launch::async, [this] {
    return _link->end_all(1500ms);
  });
  EXPECT_EQ(first.get(), drain_peer_t::reply_e::ended);
  EXPECT_EQ(second.get(), drain_peer_t::reply_e::ended);
  const auto sent = requests();
  ASSERT_EQ(sent.size(), 2u) << "Two concurrent end_all calls must send two requests.";
  EXPECT_NE(request_id_of(sent[0]), request_id_of(sent[1])) << "Each concurrent request has its own id.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Independent concurrent requests
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffPeerLinkTest, RepliesMayArriveInAnyOrder) {
  start_recorder();
  auto list_future = std::async(std::launch::async, [this] {
    return _link->list(2000ms);
  });
  ASSERT_TRUE(wait_requests(1));
  auto cert_future = std::async(std::launch::async, [this] {
    return _link->close_cert(valid_hash, 2000ms);
  });
  ASSERT_TRUE(wait_requests(2));
  const auto sent = requests();
  const auto list_id = request_id_of(sent[0]);
  const auto cert_id = request_id_of(sent[1]);
  ASSERT_TRUE(std::holds_alternative<protocol::sessions_request_t>(sent[0]));
  ASSERT_TRUE(std::holds_alternative<protocol::close_cert_request_t>(sent[1]));

  // Answer the later request first: it must complete while the list still waits.
  EXPECT_TRUE(_link->deliver(protocol::close_cert_reply_t {cert_id, 2}));
  ASSERT_EQ(cert_future.wait_for(1000ms), std::future_status::ready) << "close_cert must complete independently of the list in flight.";
  EXPECT_EQ(cert_future.get(), std::optional<std::uint32_t> {2});
  EXPECT_NE(list_future.wait_for(30ms), std::future_status::ready) << "The list must still wait for its own reply.";
  EXPECT_TRUE(_link->deliver(protocol::sessions_reply_t {list_id, two_sessions}));
  ASSERT_EQ(list_future.wait_for(1000ms), std::future_status::ready);
  const auto listed = list_future.get();
  ASSERT_TRUE(listed.has_value());
  EXPECT_EQ(*listed, two_sessions);
}

// ---------------------------------------------------------------------------------------------------------------------
// Gone while waiting
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffPeerLinkTest, PeerGoneDuringWaitGivesGoneResultAtTheEndOfTheWait) {
  start_recorder();
  auto future = std::async(std::launch::async, [this] {
    return _link->list(200ms);
  });
  ASSERT_TRUE(wait_requests(1));
  make_gone();
  ASSERT_EQ(future.wait_for(2000ms), std::future_status::ready);
  const auto result = future.get();
  ASSERT_TRUE(result.has_value()) << "Every path that ends without a reply checks gone() once: gone gives the empty list.";
  EXPECT_TRUE(result->empty());
}

// ---------------------------------------------------------------------------------------------------------------------
// on_channel_closed
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffPeerLinkTest, OnChannelClosedWakesAWaitingList) {
  start_recorder();
  auto future = std::async(std::launch::async, [this] {
    return _link->list(5000ms);
  });
  ASSERT_TRUE(wait_requests(1));
  const auto start = clock_type::now();
  _link->on_channel_closed();
  ASSERT_EQ(future.wait_for(1000ms), std::future_status::ready) << "on_channel_closed must release a waiting request promptly.";
  EXPECT_LT(since(start), 800ms);
  EXPECT_FALSE(future.get().has_value()) << "A woken request has no reply: unknown while the peer is not gone.";
}

TEST_F(HandoffPeerLinkTest, OnChannelClosedWakesAllKindsOfWaiters) {
  start_recorder();
  auto list_future = std::async(std::launch::async, [this] {
    return _link->list(5000ms);
  });
  auto close_future = std::async(std::launch::async, [this] {
    return _link->close(3, 5000ms);
  });
  auto cert_future = std::async(std::launch::async, [this] {
    return _link->close_cert(valid_hash, 5000ms);
  });
  auto end_future = std::async(std::launch::async, [this] {
    return _link->end_all(5000ms);
  });
  ASSERT_TRUE(wait_requests(4)) << "All four requests must be in flight.";
  _link->on_channel_closed();
  ASSERT_EQ(list_future.wait_for(1000ms), std::future_status::ready) << "list must be woken by on_channel_closed.";
  ASSERT_EQ(close_future.wait_for(1000ms), std::future_status::ready) << "close must be woken by on_channel_closed.";
  ASSERT_EQ(cert_future.wait_for(1000ms), std::future_status::ready) << "close_cert must be woken by on_channel_closed.";
  ASSERT_EQ(end_future.wait_for(1000ms), std::future_status::ready) << "end_all must be woken by on_channel_closed.";
  EXPECT_FALSE(list_future.get().has_value());
  EXPECT_FALSE(close_future.get().has_value());
  EXPECT_FALSE(cert_future.get().has_value());
  EXPECT_EQ(end_future.get(), drain_peer_t::reply_e::no_reply);
}

TEST_F(HandoffPeerLinkTest, WokenRequestChecksGoneOnce) {
  start_recorder();
  auto future = std::async(std::launch::async, [this] {
    return _link->close_cert(valid_hash, 5000ms);
  });
  ASSERT_TRUE(wait_requests(1));
  make_gone();
  _link->on_channel_closed();
  ASSERT_EQ(future.wait_for(1000ms), std::future_status::ready);
  const auto result = future.get();
  ASSERT_TRUE(result.has_value()) << "A wake by fail_all with the peer gone gives the gone result (0), not nullopt.";
  EXPECT_EQ(*result, 0u);
}

TEST_F(HandoffPeerLinkTest, RequestAfterOnChannelClosedFailsAtOnceWithoutSending) {
  start_recorder();
  _link->on_channel_closed();
  const auto start = clock_type::now();
  EXPECT_FALSE(_link->list(3000ms).has_value());
  EXPECT_FALSE(_link->close(1, 3000ms).has_value());
  EXPECT_FALSE(_link->close_cert(valid_hash, 3000ms).has_value());
  EXPECT_EQ(_link->end_all(3000ms), drain_peer_t::reply_e::no_reply);
  EXPECT_LT(since(start), 800ms) << "Requests started after on_channel_closed must fail at once.";
  EXPECT_FALSE(far_end_receives_within(60ms)) << "A closed link must not send (step 4 comes before the send).";
}

TEST_F(HandoffPeerLinkTest, RequestAfterOnChannelClosedWhenGoneGivesGoneResult) {
  _link->on_channel_closed();
  make_gone();
  const auto listed = _link->list(3000ms);
  ASSERT_TRUE(listed.has_value());
  EXPECT_TRUE(listed->empty());
  const auto closed = _link->close(1, 3000ms);
  ASSERT_TRUE(closed.has_value());
  EXPECT_EQ(*closed, rtsp_stream::close_result_e::not_found);
  const auto count = _link->close_cert(valid_hash, 3000ms);
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(*count, 0u);
  EXPECT_EQ(_link->end_all(3000ms), drain_peer_t::reply_e::no_reply);
}

TEST_F(HandoffPeerLinkTest, OnChannelClosedIsIdempotent) {
  _link->on_channel_closed();
  _link->on_channel_closed();
  const auto start = clock_type::now();
  EXPECT_FALSE(_link->list(3000ms).has_value());
  EXPECT_LT(since(start), 800ms);
}

TEST_F(HandoffPeerLinkTest, OnChannelClosedDoesNotChangeStateOrGone) {
  ASSERT_TRUE(_link->deliver(protocol::handoff_state_t {"draining"}));
  _link->on_channel_closed();
  EXPECT_FALSE(_link->gone()) << "A closed channel is not a gone process; only the pidfd says that.";
  EXPECT_TRUE(_link->exists());
  EXPECT_EQ(_link->state(), state_e::draining);
}

// ---------------------------------------------------------------------------------------------------------------------
// Unknown strings and late replies after no-send paths
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffPeerLinkTest, CloseUnknownResultStringCountsAsNoReply) {
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::close_reply_t {id, "bogus-result"}};
  }));
  EXPECT_FALSE(_link->close(5, 300ms).has_value()) << "A CLOSED result string the link does not know counts as no reply: nullopt while the peer is not gone.";
}

TEST_F(HandoffPeerLinkTest, CloseUnknownResultStringWhenGoneIsNotFound) {
  make_gone();
  start_responder(reply_with([](std::uint64_t id) {
    return protocol::message_t {protocol::close_reply_t {id, "bogus-result"}};
  }));
  const auto result = _link->close(5, 300ms);
  ASSERT_TRUE(result.has_value()) << "An unknown CLOSED result with the peer gone gives the gone result (not_found).";
  EXPECT_EQ(*result, rtsp_stream::close_result_e::not_found);
}

TEST_F(HandoffPeerLinkTest, UnknownHandoffStateStringLeavesStateUnchanged) {
  ASSERT_TRUE(_link->deliver(protocol::handoff_state_t {"draining"}));
  (void) _link->deliver(protocol::handoff_state_t {"bogus-state"});
  EXPECT_EQ(_link->state(), state_e::draining) << "An unknown HANDOFF_STATE string must leave state() unchanged.";
}

TEST_F(HandoffPeerLinkTest, UnknownHandoffStateStringFromNoneStaysNone) {
  (void) _link->deliver(protocol::handoff_state_t {"bogus-state"});
  EXPECT_EQ(_link->state(), state_e::none);
}

TEST_F(HandoffPeerLinkTest, LateDeliverAfterNoSendPathsReturnsFalse) {
  // After a path that sent nothing, a reply with any id (here the first ids the tracker hands out) is not consumed.
  EXPECT_FALSE(_link->list(0ms).has_value());
  _link->on_channel_closed();
  EXPECT_FALSE(_link->list(100ms).has_value());
  for (std::uint64_t id = 1; id <= 4; ++id) {
    SCOPED_TRACE(id);
    EXPECT_FALSE(_link->deliver(protocol::sessions_reply_t {id, two_sessions})) << "The id of a request that ended without a send must be forgotten: a late reply is dropped.";
    EXPECT_FALSE(_link->deliver(protocol::ended_t {id}));
  }
}

TEST_F(HandoffPeerLinkTest, LateDeliverAfterSendFailureReturnsFalse) {
  start_recorder();
  _far->close();
  EXPECT_FALSE(_link->list(500ms).has_value());
  for (std::uint64_t id = 1; id <= 3; ++id) {
    EXPECT_FALSE(_link->deliver(protocol::sessions_reply_t {id, {}})) << "The id of a request whose send failed must be forgotten.";
  }
}

#endif  // __linux__
