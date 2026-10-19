/**
 * @file tests/unit/test_rtsp_handoff.cpp
 * @brief Blind tests for the RTSP side of the drain handoff (wave C, package P10).
 *
 * Covers `adopt_listener`, `pause_accept`, `resume_accept`, `close_listener`, `listener_fd`, `set_listening_callback`,
 * `has_pending_launch`, `drop_pending_launch`, `terminate_sessions_by_cert_hash` and `total_session_count`, using only the
 * documented `rtsp_stream::testing` seams, loopback sockets and fake sessions. Written from the contract in `src/rtsp.h`
 * and `docs/drain-handoff.md` only.
 *
 * SPEC-QUESTION: no seam sets the client certificate of a fake session, and the tests that need one do it by writing
 * through the reference that `stream::session::client_cert` returns. If that reference is not the stored string, those
 * tests need a seam.
 *
 * SPEC-QUESTION: no seam runs code on the loop thread, so "the call returns false on the loop thread" is covered only for
 * the listening callback (see `ListeningCallbackCannotPauseResumeOrClose`).
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <latch>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
  // platform includes
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

// lib includes
#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>

// local includes
#include <src/config.h>
#include <src/crypto.h>
#include <src/rtsp.h>
#include <src/stream.h>

namespace {
  using namespace std::chrono_literals;

  /**
   * @brief Name of the log file the unit-test logger writes to.
   */
  constexpr auto TEST_LOG_FILE = "test_sunshine.log";

  /**
   * @brief Upper bound for waiting on an asynchronous condition.
   */
  constexpr auto WAIT_LIMIT = 2000ms;

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
   * @brief Poll a condition until it is true or the limit passes.
   *
   * @param condition Condition to poll.
   * @param limit Longest time to wait.
   * @return `true` when the condition became true.
   */
  bool wait_until(const std::function<bool()> &condition, std::chrono::milliseconds limit = WAIT_LIMIT) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
      if (condition()) {
        return true;
      }
      std::this_thread::sleep_for(2ms);
    }
    return condition();
  }

  /**
   * @brief Lowercase hexadecimal SHA-256 of a string, the form `terminate_sessions_by_cert_hash` expects.
   *
   * @param text Text to hash.
   * @return 64 lowercase hex characters.
   */
  std::string sha256_hex(const std::string &text) {
    constexpr auto digits = "0123456789abcdef";
    const auto digest = crypto::hash(text);
    std::string out;
    for (const auto byte : digest) {
      out.push_back(digits[byte >> 4]);
      out.push_back(digits[byte & 0x0f]);
    }
    return out;
  }

  /**
   * @brief Insert a fake session and give it a client certificate.
   *
   * @param id Launch session id.
   * @param state Initial state.
   * @param cert Certificate text to store in the session.
   * @param gate Optional join gate.
   * @return The session.
   */
  std::shared_ptr<stream::session_t> insert_session_with_cert(
    std::uint32_t id,
    stream::session::state_e state,
    const std::string &cert,
    std::shared_future<void> gate = {}
  ) {
    auto session = rtsp_stream::testing::insert_fake_session(id, state, std::move(gate));
    if (session) {
      const_cast<std::string &>(stream::session::client_cert(*session)) = cert;
    }
    return session;
  }

  /**
   * @brief Make a launch session for `launch_session_raise`.
   *
   * @param id Launch session id.
   * @return A launch session with all fields value-initialised.
   */
  std::shared_ptr<rtsp_stream::launch_session_t> make_launch(std::uint32_t id) {
    auto launch = std::make_shared<rtsp_stream::launch_session_t>();
    launch->id = id;
    return launch;
  }

  /**
   * @brief Ids of the sessions currently listed.
   *
   * @return Sorted ids.
   */
  std::vector<std::uint32_t> listed_ids() {
    std::vector<std::uint32_t> ids;
    for (const auto &info : rtsp_stream::list_sessions()) {
      ids.push_back(info.id);
    }
    std::ranges::sort(ids);
    return ids;
  }

  /**
   * @brief Holds a join open until released; the destructor releases it.
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
     * @brief Shared future to pass to `insert_fake_session`.
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
    std::shared_future<void> future_;  ///< Shared view of the promise.
    bool released_ {false};  ///< Whether `release()` already ran.
  };

#ifdef __linux__
  /**
   * @brief A bound, listening loopback TCP socket created by the test (stands in for the inherited RTSP listener).
   *
   * It is created without close-on-exec so that a close-on-exec duplicate can be told apart.
   */
  class loopback_listener_t {
  public:
    loopback_listener_t() {
      fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd_ < 0) {
        return;
      }
      const int one = 1;
      ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
      sockaddr_in addr {};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = 0;
      if (::bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(fd_, 16) != 0) {
        ::close(fd_);
        fd_ = -1;
        return;
      }
      socklen_t len = sizeof(addr);
      if (::getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &len) == 0) {
        port_ = ntohs(addr.sin_port);
      }
    }

    loopback_listener_t(const loopback_listener_t &) = delete;
    loopback_listener_t &operator=(const loopback_listener_t &) = delete;

    ~loopback_listener_t() {
      close_fd();
    }

    /**
     * @brief The descriptor, or -1.
     *
     * @return The listening descriptor.
     */
    [[nodiscard]] int fd() const {
      return fd_;
    }

    /**
     * @brief The bound port.
     *
     * @return Port in host order.
     */
    [[nodiscard]] unsigned short port() const {
      return port_;
    }

    /**
     * @brief Close the caller's copy of the descriptor.
     */
    void close_fd() {
      if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
      }
    }

  private:
    int fd_ {-1};  ///< The descriptor.
    unsigned short port_ {0};  ///< The bound port.
  };

  /**
   * @brief A client connection to the loopback port; closed on destruction.
   */
  class loopback_client_t {
  public:
    /**
     * @brief Connect to 127.0.0.1 on `port`. The kernel completes the connect from the backlog even if nobody accepts.
     *
     * @param port Port to connect to.
     */
    explicit loopback_client_t(unsigned short port) {
      fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd_ < 0) {
        return;
      }
      sockaddr_in addr {};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = htons(port);
      if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd_);
        fd_ = -1;
      }
    }

    loopback_client_t(const loopback_client_t &) = delete;
    loopback_client_t &operator=(const loopback_client_t &) = delete;

    ~loopback_client_t() {
      if (fd_ >= 0) {
        ::close(fd_);
      }
    }

    /**
     * @brief Whether the connect succeeded.
     *
     * @return `true` when connected.
     */
    [[nodiscard]] bool connected() const {
      return fd_ >= 0;
    }

    /**
     * @brief Wait until the server closed the connection (EOF).
     *
     * @param limit Longest wait.
     * @return `true` when the peer closed the connection.
     */
    [[nodiscard]] bool server_closed(std::chrono::milliseconds limit) const {
      pollfd p {fd_, POLLIN, 0};
      if (::poll(&p, 1, static_cast<int>(limit.count())) <= 0) {
        return false;
      }
      char byte;
      return ::recv(fd_, &byte, 1, 0) == 0;
    }

  private:
    int fd_ {-1};  ///< The client descriptor.
  };
#endif
}  // namespace

/**
 * @brief Fixture that resets the RTSP server and the config values it changes, before and after each test.
 */
class RtspHandoffTest: public BaseTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
protected:
  /**
   * @brief Reset the RTSP server and set a long launch expiry.
   */
  void SetUp() override {
    BaseTest::SetUp();
    saved_ping_timeout_ = config::stream.ping_timeout;
    config::stream.ping_timeout = 30s;
    rtsp_stream::testing::reset_server();
    rtsp_stream::testing::remove_all();
  }

  /**
   * @brief Reset the RTSP server and restore the config.
   */
  void TearDown() override {
    rtsp_stream::testing::reset_server();
    rtsp_stream::testing::remove_all();
    config::stream.ping_timeout = saved_ping_timeout_;
    BaseTest::TearDown();
  }

private:
  std::chrono::milliseconds saved_ping_timeout_ {};  ///< ping_timeout to restore.
};

// ---------------------------------------------------------------------------------------------------------------------
// total_session_count and pending launch (all platforms)
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Test: with no draining process, `total_session_count()` is 0 with no sessions.
 */
TEST_F(RtspHandoffTest, TotalSessionCountWithNoPeerAndNoSessionsIsZero) {
  EXPECT_EQ(rtsp_stream::session_count(), 0);
  EXPECT_EQ(rtsp_stream::total_session_count(), 0) << "with no peer the total must equal session_count()";
}

/**
 * @brief Test: with no draining process, `total_session_count()` equals `session_count()` and returns promptly.
 */
TEST_F(RtspHandoffTest, TotalSessionCountWithNoPeerEqualsSessionCount) {
  ASSERT_NE(rtsp_stream::testing::insert_fake_session(301, stream::session::state_e::RUNNING), nullptr);
  ASSERT_NE(rtsp_stream::testing::insert_fake_session(302, stream::session::state_e::RUNNING), nullptr);

  const auto start = std::chrono::steady_clock::now();
  const int total = rtsp_stream::total_session_count();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(total, 2) << "with no peer the total must equal session_count()";
  EXPECT_EQ(total, rtsp_stream::session_count());
  EXPECT_LT(elapsed, 400ms) << "with no peer nothing is sent, so the call must not wait for a reply";
}

/**
 * @brief Test: nothing is pending at the start, and `drop_pending_launch()` then returns false.
 */
TEST_F(RtspHandoffTest, NoPendingLaunchInitially) {
  EXPECT_FALSE(rtsp_stream::has_pending_launch());
  EXPECT_FALSE(rtsp_stream::drop_pending_launch()) << "dropping with nothing pending must return false";
}

/**
 * @brief Test: a raised launch is pending until `drop_pending_launch()` removes it; a second drop returns false.
 */
TEST_F(RtspHandoffTest, RaiseThenDropClearsPendingLaunch) {
  rtsp_stream::launch_session_raise(make_launch(401));
  EXPECT_TRUE(rtsp_stream::has_pending_launch()) << "a raised launch is pending";

  EXPECT_TRUE(rtsp_stream::drop_pending_launch()) << "the first drop removes the pending launch";
  EXPECT_FALSE(rtsp_stream::has_pending_launch());
  EXPECT_FALSE(rtsp_stream::drop_pending_launch()) << "a second drop in a row must return false";
}

/**
 * @brief Test: `launch_session_clear` of the raised id ends the pending state.
 */
TEST_F(RtspHandoffTest, LaunchSessionClearEndsPendingLaunch) {
  rtsp_stream::launch_session_raise(make_launch(402));
  ASSERT_TRUE(rtsp_stream::has_pending_launch());

  rtsp_stream::launch_session_clear(402);

  EXPECT_FALSE(rtsp_stream::has_pending_launch()) << "launch_session_clear must end the pending launch";
  EXPECT_FALSE(rtsp_stream::drop_pending_launch());
}

/**
 * @brief Test: drop then raise leaves the new launch pending, and a drop removes that one.
 */
TEST_F(RtspHandoffTest, DropThenRaiseLeavesNewLaunchPending) {
  rtsp_stream::launch_session_raise(make_launch(403));
  ASSERT_TRUE(rtsp_stream::drop_pending_launch());
  rtsp_stream::launch_session_raise(make_launch(404));

  EXPECT_TRUE(rtsp_stream::has_pending_launch()) << "a launch raised after a drop must stay pending";
  EXPECT_TRUE(rtsp_stream::drop_pending_launch());
  EXPECT_FALSE(rtsp_stream::has_pending_launch());
}

/**
 * @brief Test: dropping the pending launch leaves sessions alone.
 */
TEST_F(RtspHandoffTest, DropPendingLaunchLeavesSessionsAlone) {
  auto session = rtsp_stream::testing::insert_fake_session(405, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  rtsp_stream::launch_session_raise(make_launch(406));

  EXPECT_TRUE(rtsp_stream::drop_pending_launch());

  EXPECT_EQ(rtsp_stream::session_count(), 1);
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::RUNNING);
}

/**
 * @brief Test: concurrent raises and drops end in a consistent state (each outcome as if one ran entirely before the other).
 */
TEST_F(RtspHandoffTest, ConcurrentRaiseAndDropStayConsistent) {
  constexpr int rounds = 300;
  std::latch start(2);
  std::atomic<int> drops_true {0};
  std::atomic<int> raises {0};

  std::thread raiser([&]() {
    start.arrive_and_wait();
    for (int i = 0; i < rounds; ++i) {
      rtsp_stream::launch_session_raise(make_launch(static_cast<std::uint32_t>(1000 + i)));
      ++raises;
    }
  });
  std::thread dropper([&]() {
    start.arrive_and_wait();
    for (int i = 0; i < rounds; ++i) {
      if (rtsp_stream::drop_pending_launch()) {
        ++drops_true;
      }
    }
  });
  raiser.join();
  dropper.join();

  EXPECT_EQ(raises.load(), rounds);
  const bool pending = rtsp_stream::has_pending_launch();
  EXPECT_EQ(rtsp_stream::drop_pending_launch(), pending) << "the final drop must agree with has_pending_launch()";
  EXPECT_FALSE(rtsp_stream::has_pending_launch());
  EXPECT_LE(drops_true.load(), rounds);
}

/**
 * @brief Test: arguments of `terminate_sessions_by_cert_hash` that are not exactly 64 lowercase hex characters do nothing.
 */
TEST_F(RtspHandoffTest, CertHashInvalidFormsTerminateNothing) {
  const std::string cert = "cert-invalid-forms";
  auto session = insert_session_with_cert(501, stream::session::state_e::RUNNING, cert);
  ASSERT_NE(session, nullptr);
  const std::string good = sha256_hex(cert);
  ASSERT_EQ(good.size(), 64U);

  std::string upper = good;
  std::ranges::transform(upper, upper.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  std::string bad_char = good;
  bad_char[10] = 'g';

  const std::vector<std::pair<std::string, std::string>> cases {
    {"empty", ""},
    {"63 characters", good.substr(0, 63)},
    {"65 characters", good + "0"},
    {"uppercase digits", upper},
    {"non-hex character", bad_char},
    {"leading space", " " + good.substr(0, 63)},
    {"trailing newline", good + "\n"},
  };

  const std::uintmax_t offset = test_log_size();
  for (const auto &[name, text] : cases) {
    EXPECT_EQ(rtsp_stream::terminate_sessions_by_cert_hash(text), 0U) << "invalid hash (" << name << ") must return 0";
  }

  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::RUNNING) << "an invalid hash must not stop any session";
  EXPECT_EQ(rtsp_stream::session_count(), 1);
  const auto log = test_log_since(offset);
  EXPECT_EQ(log.find("Error: "), std::string::npos) << "an invalid hash must not log above Debug";
  EXPECT_EQ(log.find("Warning: "), std::string::npos) << "an invalid hash must not log above Debug";
}

/**
 * @brief Test: a valid hash that matches no session returns 0 and leaves sessions alone.
 */
TEST_F(RtspHandoffTest, CertHashWithNoMatchReturnsZero) {
  auto session = insert_session_with_cert(502, stream::session::state_e::RUNNING, "cert-one");
  ASSERT_NE(session, nullptr);

  EXPECT_EQ(rtsp_stream::terminate_sessions_by_cert_hash(sha256_hex("cert-other")), 0U);
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::RUNNING);
  EXPECT_EQ(rtsp_stream::session_count(), 1);
}

/**
 * @brief Test: a session with an empty certificate never matches, even for the hash of the empty string.
 */
TEST_F(RtspHandoffTest, CertHashOfEmptyStringNeverMatchesEmptyCertificate) {
  auto session = insert_session_with_cert(503, stream::session::state_e::RUNNING, "");
  ASSERT_NE(session, nullptr);

  EXPECT_EQ(rtsp_stream::terminate_sessions_by_cert_hash(sha256_hex("")), 0U) << "empty certificates never match";
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::RUNNING);
}

/**
 * @brief Test: matching sessions are stopped as `terminated` by default, joined and erased, and counted; others stay.
 */
TEST_F(RtspHandoffTest, CertHashTerminatesMatchingSessionsOnly) {
  const std::string cert = "cert-match";
  auto first = insert_session_with_cert(511, stream::session::state_e::RUNNING, cert);
  auto second = insert_session_with_cert(512, stream::session::state_e::RUNNING, cert);
  auto other = insert_session_with_cert(513, stream::session::state_e::RUNNING, "cert-different");
  ASSERT_TRUE(first && second && other);

  EXPECT_EQ(rtsp_stream::terminate_sessions_by_cert_hash(sha256_hex(cert)), 2U);

  EXPECT_EQ(stream::session::stop_reason(*first), stream::stop_reason_e::terminated) << "the default reason is `terminated`";
  EXPECT_EQ(stream::session::stop_reason(*second), stream::stop_reason_e::terminated);
  EXPECT_EQ(stream::session::state(*other), stream::session::state_e::RUNNING) << "a session of another client is untouched";
  EXPECT_EQ(listed_ids(), (std::vector<std::uint32_t> {513})) << "matched sessions must be erased, others kept";
}

/**
 * @brief Test: the `reason` argument is recorded for the sessions the call stops.
 */
TEST_F(RtspHandoffTest, CertHashRecordsGivenReason) {
  const std::string cert = "cert-reason";
  auto session = insert_session_with_cert(514, stream::session::state_e::RUNNING, cert);
  ASSERT_NE(session, nullptr);

  EXPECT_EQ(rtsp_stream::terminate_sessions_by_cert_hash(sha256_hex(cert), stream::stop_reason_e::api_close), 1U);
  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::api_close);
}

/**
 * @brief Test: a matching session that was already stopping is erased but not counted, and keeps its first reason.
 */
TEST_F(RtspHandoffTest, CertHashDoesNotCountAlreadyStoppingSession) {
  const std::string cert = "cert-stopping";
  auto running = insert_session_with_cert(521, stream::session::state_e::RUNNING, cert);
  auto stopping = insert_session_with_cert(522, stream::session::state_e::RUNNING, cert);
  ASSERT_TRUE(running && stopping);
  ASSERT_TRUE(stream::session::stop(*stopping, stream::stop_reason_e::ping_timeout));

  EXPECT_EQ(rtsp_stream::terminate_sessions_by_cert_hash(sha256_hex(cert)), 1U) << "only the session this call moved to stopping counts";
  EXPECT_EQ(stream::session::stop_reason(*stopping), stream::stop_reason_e::ping_timeout) << "the first stop reason is kept";
  EXPECT_EQ(stream::session::stop_reason(*running), stream::stop_reason_e::terminated);
  EXPECT_TRUE(listed_ids().empty()) << "the already-stopping match is still joined and erased";
}

/**
 * @brief Test: when every match was already stopping, the count is 0.
 */
TEST_F(RtspHandoffTest, CertHashCountIsZeroWhenAllMatchesWereStopping) {
  const std::string cert = "cert-all-stopping";
  auto session = insert_session_with_cert(523, stream::session::state_e::RUNNING, cert);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(stream::session::stop(*session, stream::stop_reason_e::ping_timeout));

  EXPECT_EQ(rtsp_stream::terminate_sessions_by_cert_hash(sha256_hex(cert)), 0U);
  EXPECT_TRUE(listed_ids().empty());
}

/**
 * @brief Test: concurrent calls with the same hash end each session once; the counts add up to the session count.
 */
TEST_F(RtspHandoffTest, CertHashConcurrentCallsCountEachSessionOnce) {
  const std::string cert = "cert-concurrent";
  constexpr std::uint32_t sessions = 6;
  for (std::uint32_t i = 0; i < sessions; ++i) {
    ASSERT_NE(insert_session_with_cert(530 + i, stream::session::state_e::RUNNING, cert), nullptr);
  }
  const auto hash = sha256_hex(cert);
  std::latch start(2);
  std::size_t a = 0;
  std::size_t b = 0;
  std::thread first([&]() {
    start.arrive_and_wait();
    a = rtsp_stream::terminate_sessions_by_cert_hash(hash);
  });
  std::thread second([&]() {
    start.arrive_and_wait();
    b = rtsp_stream::terminate_sessions_by_cert_hash(hash);
  });
  first.join();
  second.join();

  EXPECT_EQ(a + b, static_cast<std::size_t>(sessions)) << "each session is moved to stopping by exactly one call";
  EXPECT_TRUE(listed_ids().empty());
}

/**
 * @brief Test: the call returns only after the matching session is joined, while listing stays prompt (join is outside the slots lock).
 */
TEST_F(RtspHandoffTest, CertHashJoinsOutsideSlotsLock) {
  const std::string cert = "cert-held-join";
  join_gate_t gate;
  auto session = insert_session_with_cert(540, stream::session::state_e::RUNNING, cert, gate.future());
  ASSERT_NE(session, nullptr);

  auto call = std::async(std::launch::async, [&]() {
    return rtsp_stream::terminate_sessions_by_cert_hash(sha256_hex(cert));
  });
  std::this_thread::sleep_for(60ms);
  const bool returned_early = call.wait_for(0ms) == std::future_status::ready;
  auto list_call = std::async(std::launch::async, []() {
    return rtsp_stream::list_sessions();
  });
  const bool list_prompt = list_call.wait_for(2000ms) == std::future_status::ready;

  gate.release();
  ASSERT_EQ(call.wait_for(2000ms), std::future_status::ready);

  EXPECT_FALSE(returned_early) << "the call must not return before the session is joined";
  EXPECT_TRUE(list_prompt) << "list_sessions() must not wait for the held join";
  EXPECT_EQ(call.get(), 1U);
}

// ---------------------------------------------------------------------------------------------------------------------
// Listener adoption, pause, resume, close (Linux)
// ---------------------------------------------------------------------------------------------------------------------

#ifdef __linux__

/**
 * @brief Fixture that opens a loopback listener and drives the server through the test seams.
 */
class RtspHandoffListenerTest: public RtspHandoffTest {
protected:
  /**
   * @brief Open the listener, expecting success, and optionally start the loop.
   *
   * @param paused Whether accepting starts paused.
   * @param loop Whether to start the loop thread.
   */
  void open(bool paused, bool loop = true) {
    ASSERT_GE(listener.fd(), 0) << "test could not create a loopback listener";
    ASSERT_TRUE(rtsp_stream::testing::open_listener(listener.fd(), paused));
    if (loop) {
      rtsp_stream::testing::start_loop();
    }
  }

  /**
   * @brief Wait until exactly one accept is outstanding.
   *
   * @return `true` when it happened in time.
   */
  static bool wait_armed() {
    return wait_until([]() {
      return rtsp_stream::testing::outstanding_accepts() == 1;
    });
  }

  /**
   * @brief Wait until no accept is outstanding.
   *
   * @return `true` when it happened in time.
   */
  static bool wait_disarmed() {
    return wait_until([]() {
      return rtsp_stream::testing::outstanding_accepts() == 0;
    });
  }

  /**
   * @brief Wait until `handle_accept` handled at least `n` accepts.
   *
   * @param n Number of accepts.
   * @return `true` when it happened in time.
   */
  static bool wait_accepted(std::uint64_t n) {
    return wait_until([n]() {
      return rtsp_stream::testing::accepted_connections() >= n;
    });
  }

  loopback_listener_t listener;  ///< Test-owned listener.
};

/**
 * @brief Test: `adopt_listener` rejects a negative descriptor and records nothing.
 */
TEST_F(RtspHandoffListenerTest, AdoptListenerRejectsNegativeFd) {
  EXPECT_FALSE(rtsp_stream::adopt_listener(-1, false));
  ASSERT_GE(listener.fd(), 0);
  EXPECT_TRUE(rtsp_stream::adopt_listener(listener.fd(), false)) << "the rejected call must have recorded nothing";
}

/**
 * @brief Test: `adopt_listener` can be recorded only once.
 */
TEST_F(RtspHandoffListenerTest, AdoptListenerSecondCallIsRejected) {
  ASSERT_GE(listener.fd(), 0);
  EXPECT_TRUE(rtsp_stream::adopt_listener(listener.fd(), true));
  EXPECT_FALSE(rtsp_stream::adopt_listener(listener.fd(), false)) << "a second adoption must be rejected";
}

/**
 * @brief Test: `reset_server` drops the adoption record so a new one is accepted.
 */
TEST_F(RtspHandoffListenerTest, ResetServerDropsAdoptionRecord) {
  ASSERT_GE(listener.fd(), 0);
  ASSERT_TRUE(rtsp_stream::adopt_listener(listener.fd(), false));
  rtsp_stream::testing::reset_server();
  EXPECT_TRUE(rtsp_stream::adopt_listener(listener.fd(), false));
}

/**
 * @brief Test: `listener_fd()` is -1 before the listener exists.
 */
TEST_F(RtspHandoffListenerTest, ListenerFdIsMinusOneBeforeOpen) {
  EXPECT_EQ(rtsp_stream::listener_fd(), -1);
}

/**
 * @brief Test: the server uses a close-on-exec duplicate of the caller's fd on the same socket and leaves the caller's fd open.
 */
TEST_F(RtspHandoffListenerTest, ListenerFdIsCloseOnExecDuplicate) {
  ASSERT_GE(listener.fd(), 0);
  ASSERT_EQ(::fcntl(listener.fd(), F_GETFD) & FD_CLOEXEC, 0) << "test precondition: the original has no close-on-exec";
  open(true, false);

  const int dup_fd = rtsp_stream::listener_fd();
  ASSERT_GE(dup_fd, 0);
  EXPECT_NE(dup_fd, listener.fd()) << "the server must own a duplicate, not the caller's descriptor";
  EXPECT_NE(::fcntl(dup_fd, F_GETFD) & FD_CLOEXEC, 0) << "the duplicate must be close-on-exec (F_DUPFD_CLOEXEC)";
  EXPECT_NE(::fcntl(listener.fd(), F_GETFD), -1) << "the caller's descriptor must stay open";
  EXPECT_EQ(::fcntl(listener.fd(), F_GETFD) & FD_CLOEXEC, 0) << "the caller's descriptor must not be modified";

  sockaddr_in addr {};
  socklen_t len = sizeof(addr);
  ASSERT_EQ(::getsockname(dup_fd, reinterpret_cast<sockaddr *>(&addr), &len), 0);
  EXPECT_EQ(ntohs(addr.sin_port), listener.port()) << "the duplicate must refer to the same socket";
}

/**
 * @brief Test: the caller may close its fd after the listener was opened; the server keeps accepting.
 */
TEST_F(RtspHandoffListenerTest, CallerMayCloseItsFdAfterOpen) {
  open(false);
  const auto port = listener.port();
  listener.close_fd();

  loopback_client_t client(port);
  ASSERT_TRUE(client.connected());
  EXPECT_TRUE(wait_accepted(1)) << "the server's duplicate must keep the listener alive";
}

/**
 * @brief Test: `open_listener` rejects an invalid descriptor and a descriptor that is not a socket; nothing changes.
 */
TEST_F(RtspHandoffListenerTest, OpenListenerRejectsInvalidDescriptors) {
  EXPECT_FALSE(rtsp_stream::testing::open_listener(-1, false));
  EXPECT_EQ(rtsp_stream::listener_fd(), -1);

  int pipe_fds[2];
  ASSERT_EQ(::pipe(pipe_fds), 0);
  EXPECT_FALSE(rtsp_stream::testing::open_listener(pipe_fds[0], false)) << "a pipe is not a listening stream socket";
  EXPECT_EQ(rtsp_stream::listener_fd(), -1) << "a failed open must change nothing";
  ::close(pipe_fds[0]);
  ::close(pipe_fds[1]);

  ASSERT_GE(listener.fd(), 0);
  EXPECT_TRUE(rtsp_stream::testing::open_listener(listener.fd(), true)) << "a valid open must still work after failures";
}

/**
 * @brief Test: a second `open_listener` without `reset_server` returns false and keeps the first listener.
 */
TEST_F(RtspHandoffListenerTest, OpenListenerTwiceIsRejected) {
  open(true, false);
  const int first = rtsp_stream::listener_fd();
  loopback_listener_t second;
  ASSERT_GE(second.fd(), 0);

  EXPECT_FALSE(rtsp_stream::testing::open_listener(second.fd(), true));
  EXPECT_EQ(rtsp_stream::listener_fd(), first);
}

/**
 * @brief Test: `reset_server` closes the listener (`listener_fd()` is -1) and allows a fresh open.
 */
TEST_F(RtspHandoffListenerTest, ResetServerClosesListenerAndAllowsReopen) {
  open(true);
  ASSERT_GE(rtsp_stream::listener_fd(), 0);

  rtsp_stream::testing::reset_server();

  EXPECT_EQ(rtsp_stream::listener_fd(), -1);
  EXPECT_EQ(rtsp_stream::testing::accepted_connections(), 0U);
  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U);
  EXPECT_TRUE(rtsp_stream::testing::open_listener(listener.fd(), true)) << "reset_server must allow a new open_listener";
}

/**
 * @brief Test: the listening callback runs once from `open_listener` with the listener's local port, before the first accept.
 */
TEST_F(RtspHandoffListenerTest, ListeningCallbackReceivesPort) {
  std::vector<unsigned short> ports;
  rtsp_stream::set_listening_callback([&](unsigned short port) {
    ports.push_back(port);
  });
  open(true, false);

  ASSERT_EQ(ports.size(), 1U);
  EXPECT_EQ(ports[0], listener.port());
}

/**
 * @brief Test: a second `set_listening_callback` replaces the first; an empty function removes the registration.
 */
TEST_F(RtspHandoffListenerTest, ListeningCallbackIsReplacedAndRemoved) {
  int first = 0;
  int second = 0;
  rtsp_stream::set_listening_callback([&](unsigned short) {
    ++first;
  });
  rtsp_stream::set_listening_callback([&](unsigned short) {
    ++second;
  });
  open(true, false);
  EXPECT_EQ(first, 0) << "the replaced callback must not run";
  EXPECT_EQ(second, 1);

  rtsp_stream::testing::reset_server();
  int third = 0;
  rtsp_stream::set_listening_callback([&](unsigned short) {
    ++third;
  });
  rtsp_stream::set_listening_callback({});
  ASSERT_TRUE(rtsp_stream::testing::open_listener(listener.fd(), true));
  EXPECT_EQ(third, 0) << "an empty function must remove the registration";
}

/**
 * @brief Test: the callback is not called when the open fails.
 */
TEST_F(RtspHandoffListenerTest, ListeningCallbackNotCalledOnFailedOpen) {
  int calls = 0;
  rtsp_stream::set_listening_callback([&](unsigned short) {
    ++calls;
  });
  EXPECT_FALSE(rtsp_stream::testing::open_listener(-1, false));
  EXPECT_EQ(calls, 0);
}

/**
 * @brief Test: from inside the listening callback, pause, resume and close return false at once.
 *
 * SPEC-QUESTION: the contract says the callback runs on the thread that runs `start()`, and that these calls return false
 * there. This test assumes `open_listener` (the seam) does the same on the calling thread.
 */
TEST_F(RtspHandoffListenerTest, ListeningCallbackCannotPauseResumeOrClose) {
  std::array<bool, 3> results {true, true, true};
  std::chrono::steady_clock::duration elapsed {};
  rtsp_stream::set_listening_callback([&](unsigned short) {
    const auto start = std::chrono::steady_clock::now();
    results[0] = rtsp_stream::pause_accept(1000ms);
    results[1] = rtsp_stream::resume_accept(1000ms);
    results[2] = rtsp_stream::close_listener(1000ms);
    elapsed = std::chrono::steady_clock::now() - start;
  });
  open(true, false);

  EXPECT_FALSE(results[0]) << "pause_accept from the listening callback must return false";
  EXPECT_FALSE(results[1]) << "resume_accept from the listening callback must return false";
  EXPECT_FALSE(results[2]) << "close_listener from the listening callback must return false";
  EXPECT_LT(elapsed, 500ms) << "these calls must return at once, not wait for their timeout";
}

/**
 * @brief Test: unpaused, a connection is accepted and, with no pending launch, closed by the server.
 */
TEST_F(RtspHandoffListenerTest, UnpausedAcceptsAndClosesConnectionWithoutPendingLaunch) {
  open(false);
  ASSERT_TRUE(wait_armed());

  loopback_client_t client(listener.port());
  ASSERT_TRUE(client.connected());

  EXPECT_TRUE(wait_accepted(1));
  EXPECT_TRUE(client.server_closed(1000ms)) << "without a pending launch the accepted connection is closed";
  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U);
}

/**
 * @brief Test: opened paused, nothing is accepted; a connection waits in the backlog and is accepted after `resume_accept`.
 */
TEST_F(RtspHandoffListenerTest, PausedOpenAcceptsNothingUntilResume) {
  open(true);
  EXPECT_EQ(rtsp_stream::testing::outstanding_accepts(), 0) << "a paused open must arm no accept";
  loopback_client_t client(listener.port());
  ASSERT_TRUE(client.connected()) << "the kernel backlog must take the connection";

  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(rtsp_stream::testing::accepted_connections(), 0U) << "a paused listener must accept nothing";

  EXPECT_TRUE(rtsp_stream::resume_accept());
  EXPECT_TRUE(wait_accepted(1)) << "the queued connection must be accepted after resume";
  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U);
}

/**
 * @brief Test: `pause_accept` returns true, disarms the accept, and logs no Error, counts no accept error, ends no session.
 */
TEST_F(RtspHandoffListenerTest, PauseIsSilentAndKeepsSessions) {
  auto session = rtsp_stream::testing::insert_fake_session(601, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  open(false);
  ASSERT_TRUE(wait_armed());
  const std::uintmax_t offset = test_log_size();

  ASSERT_TRUE(rtsp_stream::pause_accept());
  EXPECT_TRUE(wait_disarmed()) << "after a pause no accept stays outstanding";
  std::this_thread::sleep_for(100ms);

  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U) << "the aborted accept of a pause is not an error";
  EXPECT_EQ(test_log_since(offset).find("Error: "), std::string::npos) << "a pause must log no Error";
  EXPECT_EQ(rtsp_stream::session_count(), 1) << "a pause must not call clear() on the sessions";
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::RUNNING);
  EXPECT_GE(rtsp_stream::listener_fd(), 0) << "a pause must not close the listener";
}

/**
 * @brief Test: connections made while paused are not accepted, and are accepted after `resume_accept`.
 */
TEST_F(RtspHandoffListenerTest, PauseConnectResumeAcceptsQueuedConnection) {
  open(false);
  ASSERT_TRUE(wait_armed());
  ASSERT_TRUE(rtsp_stream::pause_accept());
  ASSERT_TRUE(wait_disarmed());

  loopback_client_t client(listener.port());
  ASSERT_TRUE(client.connected());
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(rtsp_stream::testing::accepted_connections(), 0U);

  ASSERT_TRUE(rtsp_stream::resume_accept());
  EXPECT_TRUE(wait_accepted(1));
  EXPECT_TRUE(wait_armed()) << "after resume exactly one accept is armed again";
  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U);
}

/**
 * @brief Test: pause and resume are idempotent and resume never arms a second accept.
 */
TEST_F(RtspHandoffListenerTest, PauseAndResumeAreIdempotent) {
  open(false);
  ASSERT_TRUE(wait_armed());

  EXPECT_TRUE(rtsp_stream::resume_accept()) << "resume on a server that is not paused returns true";
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(rtsp_stream::testing::outstanding_accepts(), 1) << "resume on a running server must arm nothing";

  EXPECT_TRUE(rtsp_stream::pause_accept());
  EXPECT_TRUE(rtsp_stream::pause_accept()) << "a second pause returns true";
  EXPECT_TRUE(wait_disarmed());

  EXPECT_TRUE(rtsp_stream::resume_accept());
  EXPECT_TRUE(rtsp_stream::resume_accept()) << "a second resume returns true";
  EXPECT_TRUE(wait_armed());
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(rtsp_stream::testing::outstanding_accepts(), 1) << "never a second outstanding accept";
  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U);
}

/**
 * @brief Test: an idle running loop does not spin.
 */
TEST_F(RtspHandoffListenerTest, IdleLoopDoesNotSpin) {
  open(false);
  ASSERT_TRUE(wait_armed());
  std::this_thread::sleep_for(100ms);

  const auto before = rtsp_stream::testing::loop_iterations();
  std::this_thread::sleep_for(300ms);
  const auto grown = rtsp_stream::testing::loop_iterations() - before;

  EXPECT_LT(grown, 50U) << "an idle loop must block in run_one(); iterate() returned " << grown << " times in 300 ms";
}

/**
 * @brief Test: a paused loop (no session) does not spin, after a pause and when opened paused.
 */
TEST_F(RtspHandoffListenerTest, PausedLoopDoesNotSpin) {
  open(false);
  ASSERT_TRUE(wait_armed());
  ASSERT_TRUE(rtsp_stream::pause_accept());
  ASSERT_TRUE(wait_disarmed());
  std::this_thread::sleep_for(100ms);

  const auto before = rtsp_stream::testing::loop_iterations();
  std::this_thread::sleep_for(300ms);
  const auto grown = rtsp_stream::testing::loop_iterations() - before;

  EXPECT_LT(grown, 50U) << "the work guard must keep a paused loop idle; iterate() returned " << grown << " times in 300 ms";
}

/**
 * @brief Test: a loop that was opened paused and never resumed does not spin.
 */
TEST_F(RtspHandoffListenerTest, OpenedPausedLoopDoesNotSpin) {
  open(true);
  std::this_thread::sleep_for(100ms);

  const auto before = rtsp_stream::testing::loop_iterations();
  std::this_thread::sleep_for(300ms);
  const auto grown = rtsp_stream::testing::loop_iterations() - before;

  EXPECT_LT(grown, 50U) << "iterate() returned " << grown << " times in 300 ms while opened paused";
}

/**
 * @brief Test: a paused loop with a session present does not spin.
 */
TEST_F(RtspHandoffListenerTest, PausedLoopWithSessionDoesNotSpin) {
  ASSERT_NE(rtsp_stream::testing::insert_fake_session(602, stream::session::state_e::RUNNING), nullptr);
  open(false);
  ASSERT_TRUE(rtsp_stream::pause_accept());
  std::this_thread::sleep_for(100ms);

  const auto before = rtsp_stream::testing::loop_iterations();
  std::this_thread::sleep_for(300ms);
  const auto grown = rtsp_stream::testing::loop_iterations() - before;

  EXPECT_LT(grown, 50U) << "iterate() returned " << grown << " times in 300 ms";
}

/**
 * @brief Test: two threads pausing at once both get true and leave the server paused.
 */
TEST_F(RtspHandoffListenerTest, ConcurrentPausesBothReturnTrue) {
  open(false);
  ASSERT_TRUE(wait_armed());
  std::latch start(2);
  bool a = false;
  bool b = false;
  std::thread first([&]() {
    start.arrive_and_wait();
    a = rtsp_stream::pause_accept();
  });
  std::thread second([&]() {
    start.arrive_and_wait();
    b = rtsp_stream::pause_accept();
  });
  first.join();
  second.join();

  EXPECT_TRUE(a);
  EXPECT_TRUE(b) << "the second pause is a barrier and returns true";
  EXPECT_TRUE(wait_disarmed());
  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U);
}

/**
 * @brief Test: resume right after pause (before the aborted completion was necessarily handled) ends accepting, with one accept.
 */
TEST_F(RtspHandoffListenerTest, ResumeRightAfterPauseEndsAcceptingWithOneAccept) {
  open(false);
  ASSERT_TRUE(wait_armed());

  ASSERT_TRUE(rtsp_stream::pause_accept());
  ASSERT_TRUE(rtsp_stream::resume_accept());

  EXPECT_TRUE(wait_armed()) << "the later resume decides the final state: accepting";
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(rtsp_stream::testing::outstanding_accepts(), 1);
  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U) << "the aborted completion of the pause must stay silent";

  loopback_client_t client(listener.port());
  ASSERT_TRUE(client.connected());
  EXPECT_TRUE(wait_accepted(1));
}

/**
 * @brief Test: resume then pause ends paused.
 */
TEST_F(RtspHandoffListenerTest, ResumeThenPauseEndsPaused) {
  open(false);
  ASSERT_TRUE(wait_armed());

  ASSERT_TRUE(rtsp_stream::resume_accept());
  ASSERT_TRUE(rtsp_stream::pause_accept());

  EXPECT_TRUE(wait_disarmed()) << "the later pause decides the final state: paused";
  loopback_client_t client(listener.port());
  ASSERT_TRUE(client.connected());
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(rtsp_stream::testing::accepted_connections(), 0U);
}

/**
 * @brief Test: many rapid pause and resume cycles leave one accept, no error, and a working listener.
 */
TEST_F(RtspHandoffListenerTest, RapidPauseResumeCyclesKeepOneAccept) {
  open(false);
  ASSERT_TRUE(wait_armed());
  std::atomic<bool> done {false};
  std::atomic<int> max_outstanding {0};
  std::thread sampler([&]() {
    while (!done) {
      max_outstanding = std::max(max_outstanding.load(), rtsp_stream::testing::outstanding_accepts());
    }
  });

  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(rtsp_stream::pause_accept());
    ASSERT_TRUE(rtsp_stream::resume_accept());
  }
  EXPECT_TRUE(wait_armed());
  done = true;
  sampler.join();

  EXPECT_LE(max_outstanding.load(), 1) << "at most one accept may be outstanding";
  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U) << "pausing and resuming must produce no accept error";
  loopback_client_t client(listener.port());
  ASSERT_TRUE(client.connected());
  EXPECT_TRUE(wait_accepted(1));
}

/**
 * @brief Test: pause and resume from two threads at once never produce a second accept or an error.
 */
TEST_F(RtspHandoffListenerTest, ConcurrentPauseAndResumeNeverDoubleArm) {
  open(false);
  ASSERT_TRUE(wait_armed());
  std::atomic<bool> done {false};
  std::atomic<int> max_outstanding {0};
  std::thread sampler([&]() {
    while (!done) {
      max_outstanding = std::max(max_outstanding.load(), rtsp_stream::testing::outstanding_accepts());
    }
  });
  std::latch start(2);
  std::thread pauser([&]() {
    start.arrive_and_wait();
    for (int i = 0; i < 60; ++i) {
      rtsp_stream::pause_accept();
    }
  });
  std::thread resumer([&]() {
    start.arrive_and_wait();
    for (int i = 0; i < 60; ++i) {
      rtsp_stream::resume_accept();
    }
  });
  pauser.join();
  resumer.join();

  ASSERT_TRUE(rtsp_stream::resume_accept());
  EXPECT_TRUE(wait_armed());
  done = true;
  sampler.join();

  EXPECT_LE(max_outstanding.load(), 1) << "at most one accept may be outstanding";
  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U);
}

/**
 * @brief Test: a pause that timed out (loop not running) returns false, and its handler still runs once the loop starts.
 */
TEST_F(RtspHandoffListenerTest, PauseTimeoutReturnsFalseAndHandlerRunsLater) {
  open(false, false);

  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(rtsp_stream::pause_accept(60ms)) << "no loop thread runs, so the call must time out";
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(elapsed, 40ms);
  EXPECT_LT(elapsed, 1500ms);

  rtsp_stream::testing::start_loop();
  EXPECT_TRUE(wait_disarmed()) << "the queued pause handler still runs later";
  loopback_client_t client(listener.port());
  ASSERT_TRUE(client.connected());
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(rtsp_stream::testing::accepted_connections(), 0U);
}

/**
 * @brief Test: before the loop thread has started, a pause waits for it (within its timeout).
 */
TEST_F(RtspHandoffListenerTest, PauseWaitsForLoopThreadToStart) {
  open(false, false);
  auto call = std::async(std::launch::async, []() {
    return rtsp_stream::pause_accept(2000ms);
  });
  std::this_thread::sleep_for(100ms);
  EXPECT_NE(call.wait_for(0ms), std::future_status::ready) << "the call must wait while no loop thread runs";

  rtsp_stream::testing::start_loop();

  ASSERT_EQ(call.wait_for(2000ms), std::future_status::ready);
  EXPECT_TRUE(call.get());
}

/**
 * @brief Test: `stop()` while a call waits makes it return false promptly, not after its timeout.
 */
TEST_F(RtspHandoffListenerTest, StopWhileCallWaitsReturnsFalsePromptly) {
  open(false, false);
  const auto start = std::chrono::steady_clock::now();
  auto call = std::async(std::launch::async, []() {
    return rtsp_stream::pause_accept(5000ms);
  });
  std::this_thread::sleep_for(100ms);

  rtsp_stream::testing::stop_loop();

  ASSERT_EQ(call.wait_for(1500ms), std::future_status::ready) << "the waiting call must return promptly after stop()";
  EXPECT_FALSE(call.get());
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2500ms);
}

/**
 * @brief Test: after `stop()`, pause, resume and close return false at once.
 */
TEST_F(RtspHandoffListenerTest, AfterStopCallsReturnFalseAtOnce) {
  open(false);
  ASSERT_TRUE(wait_armed());
  rtsp_stream::testing::stop_loop();

  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(rtsp_stream::pause_accept(3000ms));
  EXPECT_FALSE(rtsp_stream::resume_accept(3000ms));
  EXPECT_FALSE(rtsp_stream::close_listener(3000ms));
  EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms) << "after stop() the calls must not post and wait";
}

/**
 * @brief Test: pause, resume and close return false when no listener exists.
 */
TEST_F(RtspHandoffListenerTest, CallsWithoutListenerReturnFalse) {
  EXPECT_FALSE(rtsp_stream::pause_accept(200ms));
  EXPECT_FALSE(rtsp_stream::resume_accept(200ms));
  EXPECT_FALSE(rtsp_stream::close_listener(200ms));
}

/**
 * @brief Test: `stop_loop` twice, and without a loop, is safe.
 */
TEST_F(RtspHandoffListenerTest, StopLoopIsSafeTwiceAndWithoutLoop) {
  rtsp_stream::testing::stop_loop();
  open(false);
  rtsp_stream::testing::stop_loop();
  rtsp_stream::testing::stop_loop();
  SUCCEED();
}

/**
 * @brief Test: `stop_loop` ends the sessions with `terminated` and closes the listener.
 */
TEST_F(RtspHandoffListenerTest, StopLoopEndsSessionsAndClosesListener) {
  auto session = rtsp_stream::testing::insert_fake_session(603, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  open(false);

  rtsp_stream::testing::stop_loop();

  EXPECT_EQ(stream::session::stop_reason(*session), stream::stop_reason_e::terminated);
  EXPECT_EQ(rtsp_stream::listener_fd(), -1) << "stop() closes the listener";
}

/**
 * @brief Test: `close_listener` closes the server's copy, `listener_fd()` becomes -1, resume and pause return false.
 */
TEST_F(RtspHandoffListenerTest, CloseListenerClosesCopy) {
  open(false);
  ASSERT_TRUE(wait_armed());

  EXPECT_TRUE(rtsp_stream::close_listener());

  EXPECT_EQ(rtsp_stream::listener_fd(), -1);
  EXPECT_FALSE(rtsp_stream::resume_accept()) << "resume after close_listener returns false";
  EXPECT_FALSE(rtsp_stream::pause_accept()) << "pause after close_listener has nothing to pause";
  EXPECT_TRUE(wait_disarmed());
  EXPECT_NE(::fcntl(listener.fd(), F_GETFD), -1) << "the caller's descriptor is not closed";
}

/**
 * @brief Test: `close_listener` is idempotent.
 */
TEST_F(RtspHandoffListenerTest, CloseListenerIsIdempotent) {
  open(false);
  ASSERT_TRUE(rtsp_stream::close_listener());
  EXPECT_TRUE(rtsp_stream::close_listener()) << "closing an already closed listener returns true";
  EXPECT_EQ(rtsp_stream::listener_fd(), -1);
}

/**
 * @brief Test: `close_listener` after a pause works.
 */
TEST_F(RtspHandoffListenerTest, CloseListenerAfterPause) {
  open(false);
  ASSERT_TRUE(rtsp_stream::pause_accept());
  EXPECT_TRUE(rtsp_stream::close_listener());
  EXPECT_EQ(rtsp_stream::listener_fd(), -1);
}

/**
 * @brief Test: `close_listener` ends no session, logs no Error, counts no accept error, leaves a pending launch alone, and the socket stays open for other holders.
 */
TEST_F(RtspHandoffListenerTest, CloseListenerKeepsSessionsAndPendingLaunch) {
  auto session = rtsp_stream::testing::insert_fake_session(604, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  open(false);
  ASSERT_TRUE(wait_armed());
  rtsp_stream::launch_session_raise(make_launch(605));
  const std::uintmax_t offset = test_log_size();

  ASSERT_TRUE(rtsp_stream::close_listener());
  EXPECT_TRUE(wait_disarmed());
  std::this_thread::sleep_for(100ms);

  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U) << "the aborted accept of close_listener is silent";
  EXPECT_EQ(test_log_since(offset).find("Error: "), std::string::npos);
  EXPECT_EQ(rtsp_stream::session_count(), 1) << "close_listener must not end sessions";
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::RUNNING);
  EXPECT_TRUE(rtsp_stream::has_pending_launch()) << "close_listener leaves a pending launch alone";

  loopback_client_t client(listener.port());
  EXPECT_TRUE(client.connected()) << "the socket stays open in every other holder";
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(rtsp_stream::testing::accepted_connections(), 0U) << "the closed server accepts nothing";
}

/**
 * @brief Test: pause then close then resume: the close wins and resume returns false, with no second accept.
 */
TEST_F(RtspHandoffListenerTest, CloseAfterPauseThenResumeArmsNothing) {
  open(false);
  ASSERT_TRUE(wait_armed());

  ASSERT_TRUE(rtsp_stream::pause_accept());
  ASSERT_TRUE(rtsp_stream::close_listener());
  EXPECT_FALSE(rtsp_stream::resume_accept());

  EXPECT_TRUE(wait_disarmed());
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(rtsp_stream::testing::outstanding_accepts(), 0);
  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U);
}

/**
 * @brief Test: a real accept error (not a pause abort) logs an Error and clears every session.
 */
TEST_F(RtspHandoffListenerTest, RealAcceptErrorLogsErrorAndClearsSessions) {
  auto session = rtsp_stream::testing::insert_fake_session(611, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  open(false);
  ASSERT_TRUE(wait_armed());
  const std::uintmax_t offset = test_log_size();

  rtsp_stream::testing::inject_accept_error(boost::system::errc::make_error_code(boost::system::errc::io_error));
  loopback_client_t client(listener.port());
  ASSERT_TRUE(client.connected());

  ASSERT_TRUE(wait_until([]() {
    return rtsp_stream::testing::accept_errors() == 1;
  }))
    << "a real error must be counted as an accept error";
  EXPECT_TRUE(wait_until([]() {
    return rtsp_stream::session_count() == 0;
  }))
    << "a real accept error must clear() all sessions";
  EXPECT_GE(count_occurrences(test_log_since(offset), "Error: "), 1U) << "a real accept error must log an Error";
  EXPECT_TRUE(wait_disarmed()) << "after a real error no accept is re-armed";
}

/**
 * @brief Test: an injected `operation_aborted` that no pause cancelled is still a real error.
 */
TEST_F(RtspHandoffListenerTest, UnpausedOperationAbortedIsARealError) {
  auto session = rtsp_stream::testing::insert_fake_session(612, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  open(false);
  ASSERT_TRUE(wait_armed());
  const std::uintmax_t offset = test_log_size();

  rtsp_stream::testing::inject_accept_error(boost::asio::error::make_error_code(boost::asio::error::operation_aborted));
  loopback_client_t client(listener.port());
  ASSERT_TRUE(client.connected());

  EXPECT_TRUE(wait_until([]() {
    return rtsp_stream::testing::accept_errors() == 1;
  }))
    << "only an accept cancelled by a pause is silent; any other operation_aborted is an error";
  EXPECT_TRUE(wait_until([]() {
    return rtsp_stream::session_count() == 0;
  }));
  EXPECT_GE(count_occurrences(test_log_since(offset), "Error: "), 1U);
}

/**
 * @brief Test: after a pause and a resume, a real accept error is still an Error plus `clear()`.
 */
TEST_F(RtspHandoffListenerTest, RealErrorAfterPauseAndResumeStillLogsAndClears) {
  auto session = rtsp_stream::testing::insert_fake_session(613, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  open(false);
  ASSERT_TRUE(wait_armed());
  ASSERT_TRUE(rtsp_stream::pause_accept());
  ASSERT_TRUE(wait_disarmed());
  ASSERT_TRUE(rtsp_stream::resume_accept());
  ASSERT_TRUE(wait_armed());
  ASSERT_EQ(rtsp_stream::testing::accept_errors(), 0U);
  const std::uintmax_t offset = test_log_size();

  rtsp_stream::testing::inject_accept_error(boost::system::errc::make_error_code(boost::system::errc::io_error));
  loopback_client_t client(listener.port());
  ASSERT_TRUE(client.connected());

  EXPECT_TRUE(wait_until([]() {
    return rtsp_stream::testing::accept_errors() == 1;
  }))
    << "a pause must not leave a flag that hides a later real error";
  EXPECT_TRUE(wait_until([]() {
    return rtsp_stream::session_count() == 0;
  }));
  EXPECT_GE(count_occurrences(test_log_since(offset), "Error: "), 1U);
}

/**
 * @brief Test: an error injected while paused stays pending and is reported by the first completion after resume.
 */
TEST_F(RtspHandoffListenerTest, ErrorInjectedWhilePausedHitsFirstCompletionAfterResume) {
  auto session = rtsp_stream::testing::insert_fake_session(614, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  open(true);
  rtsp_stream::testing::inject_accept_error(boost::system::errc::make_error_code(boost::system::errc::io_error));
  loopback_client_t client(listener.port());
  ASSERT_TRUE(client.connected());
  std::this_thread::sleep_for(50ms);
  ASSERT_EQ(rtsp_stream::testing::accept_errors(), 0U) << "no accept runs while paused";

  ASSERT_TRUE(rtsp_stream::resume_accept());

  EXPECT_TRUE(wait_until([]() {
    return rtsp_stream::testing::accept_errors() == 1;
  }));
  EXPECT_TRUE(wait_until([]() {
    return rtsp_stream::session_count() == 0;
  }));
}

/**
 * @brief Test: injecting a default-constructed error code removes a pending injection.
 */
TEST_F(RtspHandoffListenerTest, InjectingNoErrorRemovesPendingInjection) {
  auto session = rtsp_stream::testing::insert_fake_session(615, stream::session::state_e::RUNNING);
  ASSERT_NE(session, nullptr);
  open(false);
  ASSERT_TRUE(wait_armed());

  rtsp_stream::testing::inject_accept_error(boost::system::errc::make_error_code(boost::system::errc::io_error));
  rtsp_stream::testing::inject_accept_error(boost::system::error_code {});
  loopback_client_t client(listener.port());
  ASSERT_TRUE(client.connected());

  EXPECT_TRUE(wait_accepted(1));
  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U);
  EXPECT_EQ(rtsp_stream::session_count(), 1);
}

/**
 * @brief Test: `reset_server` clears a pending injection.
 */
TEST_F(RtspHandoffListenerTest, ResetServerClearsPendingInjection) {
  rtsp_stream::testing::inject_accept_error(boost::system::errc::make_error_code(boost::system::errc::io_error));
  rtsp_stream::testing::reset_server();
  open(false);
  ASSERT_TRUE(wait_armed());

  loopback_client_t client(listener.port());
  ASSERT_TRUE(client.connected());

  EXPECT_TRUE(wait_accepted(1));
  EXPECT_EQ(rtsp_stream::testing::accept_errors(), 0U) << "reset_server must drop the injected error";
}

/**
 * @brief Test: a launch that expires (`ping_timeout`) stops being pending.
 */
TEST_F(RtspHandoffListenerTest, PendingLaunchExpires) {
  config::stream.ping_timeout = 80ms;
  open(false);
  rtsp_stream::launch_session_raise(make_launch(621));
  ASSERT_TRUE(rtsp_stream::has_pending_launch());

  EXPECT_TRUE(wait_until([]() {
    return !rtsp_stream::has_pending_launch();
  }))
    << "the launch must expire after ping_timeout";
  EXPECT_FALSE(rtsp_stream::drop_pending_launch());
}

/**
 * @brief Test: a drop works while the loop is not running, and the late expiry of the dropped launch never discards a new one.
 */
TEST_F(RtspHandoffListenerTest, DropThenRaiseSurvivesExpiryOfDroppedLaunch) {
  config::stream.ping_timeout = 400ms;
  open(false);
  rtsp_stream::launch_session_raise(make_launch(622));
  ASSERT_TRUE(rtsp_stream::drop_pending_launch());
  rtsp_stream::launch_session_raise(make_launch(623));

  std::this_thread::sleep_for(150ms);
  EXPECT_TRUE(rtsp_stream::has_pending_launch()) << "the cancelled timer of the dropped launch must not remove the new launch";

  EXPECT_TRUE(wait_until([]() {
    return !rtsp_stream::has_pending_launch();
  }))
    << "the new launch must still expire on its own timer";
}

/**
 * @brief Test: repeated raise, drop, raise cycles with a running loop and a short expiry keep the state consistent.
 */
TEST_F(RtspHandoffListenerTest, RaceDropWithExpiryStaysConsistent) {
  config::stream.ping_timeout = 5ms;
  open(false);
  int dropped = 0;
  for (int i = 0; i < 100; ++i) {
    rtsp_stream::launch_session_raise(make_launch(static_cast<std::uint32_t>(2000 + i)));
    if (rtsp_stream::drop_pending_launch()) {
      ++dropped;
    }
    EXPECT_FALSE(rtsp_stream::drop_pending_launch()) << "after a drop nothing is pending";
  }
  EXPECT_GT(dropped, 0) << "at least some drops must beat the 5 ms expiry";

  config::stream.ping_timeout = 30s;
  rtsp_stream::launch_session_raise(make_launch(3000));
  std::this_thread::sleep_for(50ms);
  EXPECT_TRUE(rtsp_stream::has_pending_launch()) << "an old expiry must not discard a launch raised later";
  EXPECT_TRUE(rtsp_stream::drop_pending_launch());
}

/**
 * @brief Test: pause with a pending launch leaves the launch alone.
 */
TEST_F(RtspHandoffListenerTest, PauseLeavesPendingLaunchAlone) {
  open(false);
  rtsp_stream::launch_session_raise(make_launch(624));

  ASSERT_TRUE(rtsp_stream::pause_accept());

  EXPECT_TRUE(rtsp_stream::has_pending_launch());
}

#endif  // __linux__
