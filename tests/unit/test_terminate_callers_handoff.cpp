/**
 * @file tests/unit/test_terminate_callers_handoff.cpp
 * @brief Tests for the terminate rule at its callers (`confighttp` close app, update client, unpair, unpair-all;
 * `nvhttp` `/cancel`) and for the `/launch` guard, written from `src/handoff/terminate.h`, `peer_link.h`,
 * `runtime.h` and `docs/drain-handoff.md` ("App ownership and prep commands", "Visibility and control").
 *
 * Every test installs a fake `handoff::session_peer_t` through `handoff::runtime().set_peer()` and a fake terminate
 * through `handoff::set_terminate_for_test()`, so no app is started or killed and no real process or channel exists.
 *
 * Groups: `HandoffTerminateCallersTest` (the building blocks), `HandoffCallersConfigHttpTest` (the web UI
 * handlers through `confighttp::register_app_client_routes`) and `HandoffCallersNvhttpTest` (`/cancel`, `/launch` and
 * `/resume` through `nvhttp::test_support::*_https`). Every test sets a short `handoff::set_request_budget_for_test`.
 *
 * SPEC-QUESTION: the query argument names of `/launch` and `/cancel`, and the `updateClient` body `{uuid, enabled}`,
 * are guesses from upstream and the web UI script.
 */

// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

#ifdef __linux__

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <filesystem>
  #include <format>
  #include <fstream>
  #include <map>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <sstream>
  #include <string>
  #include <string_view>
  #include <system_error>
  #include <thread>
  #include <utility>
  #include <vector>

  // lib includes
  #include <boost/property_tree/ptree.hpp>
  #include <boost/property_tree/xml_parser.hpp>
  #include <nlohmann/json.hpp>
  #include <Simple-Web-Server/client_https.hpp>
  #include <Simple-Web-Server/crypto.hpp>
  #include <Simple-Web-Server/server_https.hpp>

  // local includes
  #include <src/config.h>
  #include <src/confighttp.h>
  #include <src/crypto.h>
  #include <src/handoff/end_all.h>
  #include <src/handoff/peer_link.h>
  #include <src/handoff/runtime.h>
  #include <src/handoff/terminate.h>
  #include <src/nvhttp.h>
  #include <src/process.h>
  #include <src/rtsp.h>
  #include <src/utility.h>

namespace {
  using namespace std::chrono_literals;

  namespace fs = std::filesystem;

  /// Short budget for the gate when a test passes it directly.
  constexpr std::chrono::milliseconds short_budget {120};

  /// Request budget set in every test, so a refusal waits only this long.
  constexpr std::chrono::milliseconds request_test_budget {150};

  /**
   * @brief Fake draining process with session knowledge. Records every request; replies come from public fields.
   *
   * Set the reply fields before the first request; counters and recorded arguments are safe to read afterwards.
   */
  class fake_session_peer_t final: public handoff::session_peer_t {
  public:
    std::atomic<bool> is_gone {false};  ///< What `gone()` reports (and `!exists()`).
    reply_e end_all_reply {reply_e::ended};  ///< Reply of `end_all`.
    std::optional<std::vector<handoff::protocol::session_entry_t>> list_reply {std::vector<handoff::protocol::session_entry_t> {}};  ///< Reply of `list`.
    std::optional<std::uint32_t> close_cert_reply {0u};  ///< Reply of `close_cert`.

    std::atomic<int> end_all_calls {0};  ///< Number of `end_all` calls.
    std::atomic<int> list_calls {0};  ///< Number of `list` calls.
    std::atomic<int> close_calls {0};  ///< Number of `close` calls.
    std::atomic<int> close_cert_calls {0};  ///< Number of `close_cert` calls.

    /**
     * @brief Whether a draining process exists.
     *
     * @return `!gone()`.
     */
    bool exists() override {
      return !is_gone.load();
    }

    /**
     * @brief Whether the draining process is gone.
     *
     * @return The `is_gone` flag.
     */
    bool gone() override {
      return is_gone.load();
    }

    /**
     * @brief Record the call and answer with `end_all_reply`.
     *
     * @param timeout Timeout, recorded.
     * @return `end_all_reply`.
     */
    reply_e end_all(std::chrono::milliseconds timeout) override {
      std::scoped_lock lock {_mutex};
      ++end_all_calls;
      last_end_all_timeout = timeout;
      return end_all_reply;
    }

    /**
     * @brief Record the call and answer with `list_reply`.
     *
     * @param timeout Timeout, recorded.
     * @return `list_reply`.
     */
    std::optional<std::vector<handoff::protocol::session_entry_t>> list(std::chrono::milliseconds timeout) override {
      std::scoped_lock lock {_mutex};
      ++list_calls;
      last_list_timeout = timeout;
      return list_reply;
    }

    /**
     * @brief Record the call; no reply.
     *
     * @return `std::nullopt`.
     */
    std::optional<rtsp_stream::close_result_e> close(std::uint32_t, std::chrono::milliseconds) override {
      ++close_calls;
      return std::nullopt;
    }

    /**
     * @brief Record the arguments and answer with `close_cert_reply`.
     *
     * @param cert_sha256 Hash, recorded.
     * @param timeout Timeout, recorded.
     * @return `close_cert_reply`.
     */
    std::optional<std::uint32_t> close_cert(std::string_view cert_sha256, std::chrono::milliseconds timeout) override {
      std::scoped_lock lock {_mutex};
      ++close_cert_calls;
      close_cert_hashes.emplace_back(cert_sha256);
      last_close_cert_timeout = timeout;
      return close_cert_reply;
    }

    /**
     * @brief No handoff state.
     *
     * @return `none`.
     */
    handoff::state_e state() override {
      return handoff::state_e::none;
    }

    std::vector<std::string> close_cert_hashes;  ///< Hashes given to `close_cert`.
    std::chrono::milliseconds last_end_all_timeout {0};  ///< Timeout of the last `end_all`.
    std::chrono::milliseconds last_list_timeout {0};  ///< Timeout of the last `list`.
    std::chrono::milliseconds last_close_cert_timeout {0};  ///< Timeout of the last `close_cert`.

  private:
    std::mutex _mutex;  ///< Guards the recorded values.
  };

  /**
   * @brief Fake draining process that is only a `drain_peer_t` (no session knowledge).
   */
  class fake_plain_peer_t final: public handoff::drain_peer_t {
  public:
    std::atomic<int> end_all_calls {0};  ///< Number of `end_all` calls.

    /**
     * @brief A draining process exists.
     *
     * @return `true`.
     */
    bool exists() override {
      return true;
    }

    /**
     * @brief Never gone.
     *
     * @return `false`.
     */
    bool gone() override {
      return false;
    }

    /**
     * @brief Record the call and answer `ended`.
     *
     * @return `ended`.
     */
    reply_e end_all(std::chrono::milliseconds) override {
      ++end_all_calls;
      return reply_e::ended;
    }
  };

  /**
   * @brief One draining session entry.
   *
   * @param id Launch session id.
   * @return Entry with fixed other fields.
   */
  handoff::protocol::session_entry_t make_entry(std::uint32_t id) {
    handoff::protocol::session_entry_t entry;
    entry.id = id;
    entry.started_unix_ms = 1700000000000;
    entry.transport = "udp/ipv4";
    entry.client_category = "lan";
    return entry;
  }

  /**
   * @brief An app description for `set_running_for_test`.
   *
   * @return App with no prep commands.
   */
  proc::ctx_t make_app() {
    proc::ctx_t app {};
    app.id = "1";
    app.name = "Caller test app";
    app.elevated = false;
    app.auto_detach = false;
    app.wait_all = false;
    app.exit_timeout = std::chrono::seconds {5};
    return app;
  }
}  // namespace

/**
 * @brief Fixture that resets every global the callers touch: runtime peer and mode, the terminate seam, the running
 * app and the RTSP slots.
 */
class HandoffCallerBase: public BaseTest {
protected:
  std::atomic<int> terminate_calls {0};  ///< Calls of the fake terminate.
  std::shared_ptr<fake_session_peer_t> peer;  ///< The fake peer (not installed until a test does it).

  /**
   * @brief Reset all global state and install the counting terminate.
   */
  void SetUp() override {
    BaseTest::SetUp();
    reset_globals();
    peer = std::make_shared<fake_session_peer_t>();
    terminate_calls = 0;
    handoff::set_request_budget_for_test(request_test_budget);
    handoff::set_terminate_for_test([this] {
      ++terminate_calls;
    });
  }

  /**
   * @brief Reset all global state again.
   */
  void TearDown() override {
    reset_globals();
    BaseTest::TearDown();
  }

  /**
   * @brief Put the runtime into mode `normal` with the feature enabled.
   */
  static void enter_normal() {
    handoff::settings_t settings;
    settings.enabled = true;
    handoff::runtime().configure(settings);
  }

  /**
   * @brief Put the runtime into mode `new_pending` through `early_init` with a fake environment.
   */
  static void enter_new_pending() {
    std::map<std::string, std::string> vars {
      {"SUNSHINE_HANDOFF_FDS", "https:3,http:4,webui:5,rtsp:6"},
      {"SUNSHINE_HANDOFF_CHANNEL", "7"},
      {"SUNSHINE_HANDOFF_UDP_BASE", "48100"},
      {"SUNSHINE_HANDOFF_GENERATION", "1"},
    };
    handoff::early_init_env_t env;
    env.getenv = [&vars](const char *name) -> const char * {
      auto it = vars.find(name);
      return it == vars.end() ? nullptr : it->second.c_str();
    };
    env.unsetenv = [&vars](const char *name) {
      vars.erase(name);
    };
    env.scrub_fds = [] {
    };
    ASSERT_EQ(handoff::runtime().early_init(env), "") << "precondition: early_init must accept a valid handoff start";
    ASSERT_EQ(handoff::runtime().mode(), handoff::mode_e::new_pending) << "precondition: mode must be new_pending";
  }

  /**
   * @brief Put the runtime into mode `draining`.
   */
  static void enter_draining() {
    enter_normal();
    ASSERT_TRUE(handoff::runtime().transition(handoff::mode_e::normal, handoff::mode_e::draining)) << "precondition: mode must be draining";
  }

  /**
   * @brief Install a peer into the runtime.
   *
   * @param installed Peer to install.
   */
  static void install_peer(std::shared_ptr<handoff::drain_peer_t> installed) {
    handoff::runtime().set_peer(std::move(installed));
  }

  /**
   * @brief Make `proc::proc` look like it runs a placebo app, without starting anything.
   */
  static void run_fake_app() {
    proc::proc.set_running_for_test(make_app(), 1, true, 0, 0);
  }

  /**
   * @brief Whether `proc::proc` still records a running app.
   *
   * @return `true` when `running()` is above 0.
   */
  static bool app_running() {
    return proc::proc.running() > 0;
  }

  /**
   * @brief Reset the runtime, the terminate seam, the app and the sessions.
   */
  static void reset_globals() {
    handoff::set_request_budget_for_test(0ms);
    handoff::set_terminate_for_test({});
    handoff::runtime().set_peer(nullptr);
    handoff::runtime().reset_for_test();
    // A released app is dropped without running anything (no undo, no tray, no display function).
    proc::proc.set_released(true);
    proc::proc.drop_released_app();
    proc::proc.set_released(false);
    rtsp_stream::testing::remove_all();
  }
};

/**
 * @brief Building-block tests: the rules every caller delegates to.
 */
class HandoffTerminateCallersTest: public HandoffCallerBase {};

TEST_F(HandoffTerminateCallersTest, GateWithoutPeerIsNoDrainAndSendsNothing) {
  enter_normal();
  EXPECT_EQ(handoff::terminate_gate(short_budget), handoff::terminate_permission_e::no_drain) << "no peer must give no_drain";
}

TEST_F(HandoffTerminateCallersTest, TerminateAppWithoutPeerTerminatesOnce) {
  enter_normal();
  EXPECT_EQ(handoff::terminate_app(short_budget, "close app"), handoff::terminate_permission_e::no_drain) << "no peer: permission must be no_drain";
  EXPECT_EQ(terminate_calls.load(), 1) << "no peer: the app must be terminated exactly once, as upstream does";
}

TEST_F(HandoffTerminateCallersTest, TerminateAppGrantedByEndedTerminatesOnce) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::ended;
  install_peer(peer);
  EXPECT_EQ(handoff::terminate_app(short_budget, "close app"), handoff::terminate_permission_e::ended) << "ENDED must give permission 'ended'";
  EXPECT_EQ(peer->end_all_calls.load(), 1) << "exactly one END_ALL must be sent";
  EXPECT_GT(peer->last_end_all_timeout, 0ms) << "END_ALL must carry a positive timeout";
  EXPECT_EQ(terminate_calls.load(), 1) << "after ENDED the app must be terminated exactly once";
}

TEST_F(HandoffTerminateCallersTest, TerminateAppRefusedDoesNotTerminate) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::no_reply;
  install_peer(peer);
  EXPECT_EQ(handoff::terminate_app(short_budget, "close app"), handoff::terminate_permission_e::refused) << "no reply from a living peer must be 'refused'";
  EXPECT_EQ(terminate_calls.load(), 0) << "a refusal must not terminate the app";
}

TEST_F(HandoffTerminateCallersTest, TerminateAppRejectedByLivingPeerDoesNotTerminate) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::rejected;
  install_peer(peer);
  EXPECT_EQ(handoff::terminate_app(short_budget, "close app"), handoff::terminate_permission_e::refused) << "END_REJECTED from a living peer must be 'refused'";
  EXPECT_EQ(terminate_calls.load(), 0) << "END_REJECTED must not terminate the app";
}

TEST_F(HandoffTerminateCallersTest, TerminateAppPeerGoneTerminates) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::no_reply;
  peer->is_gone = true;
  install_peer(peer);
  EXPECT_EQ(handoff::terminate_app(short_budget, "close app"), handoff::terminate_permission_e::peer_gone) << "a gone peer must give peer_gone";
  EXPECT_EQ(terminate_calls.load(), 1) << "a gone peer must allow the terminate";
}

TEST_F(HandoffTerminateCallersTest, GateInNewPendingModeSendsNothing) {
  enter_new_pending();
  install_peer(peer);
  EXPECT_EQ(handoff::terminate_gate(short_budget), handoff::terminate_permission_e::no_drain) << "mode new_pending must give no_drain";
  EXPECT_EQ(peer->end_all_calls.load(), 0) << "mode new_pending must send no END_ALL";
}

TEST_F(HandoffTerminateCallersTest, GateInDrainingModeSendsNothing) {
  enter_draining();
  install_peer(peer);
  EXPECT_EQ(handoff::terminate_gate(short_budget), handoff::terminate_permission_e::no_drain) << "mode draining must give no_drain";
  EXPECT_EQ(peer->end_all_calls.load(), 0) << "mode draining must send no END_ALL";
}

TEST_F(HandoffTerminateCallersTest, TerminateAppInNewPendingModeTerminatesWithoutAsking) {
  enter_new_pending();
  install_peer(peer);
  peer->end_all_reply = handoff::drain_peer_t::reply_e::no_reply;
  EXPECT_EQ(handoff::terminate_app(short_budget, "close app"), handoff::terminate_permission_e::no_drain) << "mode new_pending must give no_drain";
  EXPECT_EQ(terminate_calls.load(), 1) << "mode new_pending must terminate as before";
  EXPECT_EQ(peer->end_all_calls.load(), 0) << "mode new_pending must not send END_ALL";
}

TEST_F(HandoffTerminateCallersTest, CertHashIsLowercaseSha256Hex) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const std::string hash = handoff::cert_sha256_hex(credentials.x509);
  EXPECT_EQ(hash.size(), 64u) << "the hash must be 64 characters";
  EXPECT_EQ(hash.find_first_not_of("0123456789abcdef"), std::string::npos) << "the hash must be lowercase hex only";
  EXPECT_EQ(hash, util::hex(crypto::hash(credentials.x509)).to_string()) << "the hash must be the lowercase hex of crypto::hash over the exact PEM bytes";
}

TEST_F(HandoffTerminateCallersTest, LaunchRefusalEmptyWithoutPeer) {
  enter_normal();
  EXPECT_EQ(handoff::launch_refusal(), "") << "no peer: /launch must proceed";
}

TEST_F(HandoffTerminateCallersTest, LaunchRefusalEmptyWhenPeerHasNoSessions) {
  enter_normal();
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {};
  install_peer(peer);
  EXPECT_EQ(handoff::launch_refusal(), "") << "a peer that reports no sessions must not refuse /launch";
  EXPECT_EQ(peer->list_calls.load(), 1) << "exactly one SESSIONS? must be sent";
}

TEST_F(HandoffTerminateCallersTest, LaunchRefusalTextWhenPeerHasSessions) {
  enter_normal();
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {make_entry(41)};
  install_peer(peer);
  const std::string text = handoff::launch_refusal();
  EXPECT_NE(text, "") << "a peer that reports sessions must refuse /launch";
  EXPECT_EQ(text.find('\n'), std::string::npos) << "the refusal text must be one line";
  EXPECT_EQ(text.find("41"), std::string::npos) << "the refusal text must not carry session identities";
}

TEST_F(HandoffTerminateCallersTest, LaunchRefusalWhenSessionCountUnknown) {
  enter_normal();
  peer->list_reply = std::nullopt;
  install_peer(peer);
  EXPECT_NE(handoff::launch_refusal(), "") << "a timed-out SESSIONS? counts as non-zero and must refuse /launch";
}

TEST_F(HandoffTerminateCallersTest, LaunchRefusalWhenPeerHasNoSessionKnowledge) {
  enter_normal();
  install_peer(std::make_shared<fake_plain_peer_t>());
  EXPECT_NE(handoff::launch_refusal(), "") << "a drain_peer_t that is not a session_peer_t has unknown sessions and must refuse /launch";
}

TEST_F(HandoffTerminateCallersTest, LaunchRefusalEmptyAndSilentInNewPendingMode) {
  enter_new_pending();
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {make_entry(1)};
  install_peer(peer);
  EXPECT_EQ(handoff::launch_refusal(), "") << "mode new_pending must not refuse /launch";
  EXPECT_EQ(peer->list_calls.load(), 0) << "mode new_pending must send nothing";
}

TEST_F(HandoffTerminateCallersTest, LaunchRefusalEmptyAndSilentInDrainingMode) {
  enter_draining();
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {make_entry(1)};
  install_peer(peer);
  EXPECT_EQ(handoff::launch_refusal(), "") << "mode draining must not refuse /launch";
  EXPECT_EQ(peer->list_calls.load(), 0) << "mode draining must send nothing";
}

TEST_F(HandoffTerminateCallersTest, DrainingSessionCountFollowsThePeer) {
  enter_normal();
  EXPECT_EQ(handoff::draining_session_count(), std::optional<std::size_t> {0}) << "no peer: 0";
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {make_entry(1), make_entry(2)};
  install_peer(peer);
  EXPECT_EQ(handoff::draining_session_count(), std::optional<std::size_t> {2}) << "two entries: 2";
  peer->list_reply = std::nullopt;
  EXPECT_EQ(handoff::draining_session_count(), std::nullopt) << "no reply from a living peer: unknown";
  peer->is_gone = true;
  peer->list_reply = std::nullopt;
  EXPECT_EQ(handoff::draining_session_count(), std::optional<std::size_t> {0}) << "a gone peer has no sessions";
}

TEST_F(HandoffTerminateCallersTest, TotalSessionCountAddsThePeerSessions) {
  enter_normal();
  ASSERT_EQ(rtsp_stream::session_count(), 0) << "precondition: no own session";
  EXPECT_EQ(rtsp_stream::total_session_count(), 0) << "no peer: equals session_count()";

  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {make_entry(1), make_entry(2), make_entry(3)};
  install_peer(peer);
  EXPECT_EQ(rtsp_stream::total_session_count(), 3) << "own 0 plus 3 reported";
}

TEST_F(HandoffTerminateCallersTest, TotalSessionCountIsNonZeroWhenUnknown) {
  enter_normal();
  peer->list_reply = std::nullopt;
  install_peer(peer);
  EXPECT_GE(rtsp_stream::total_session_count(), 1) << "an unanswered SESSIONS? must never claim 'no sessions'";

  install_peer(std::make_shared<fake_plain_peer_t>());
  EXPECT_GE(rtsp_stream::total_session_count(), 1) << "a peer without session knowledge must never claim 'no sessions'";
}

TEST_F(HandoffTerminateCallersTest, TotalSessionCountEqualsOwnInNewPendingMode) {
  enter_new_pending();
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {make_entry(1), make_entry(2)};
  install_peer(peer);
  EXPECT_EQ(rtsp_stream::total_session_count(), rtsp_stream::session_count()) << "mode new_pending must ignore the peer";
  EXPECT_EQ(peer->list_calls.load(), 0) << "mode new_pending must send nothing";
}

TEST_F(HandoffTerminateCallersTest, ConcurrentTerminatesAreSerialisedAndEachCounted) {
  enter_normal();
  install_peer(peer);
  std::atomic<int> inside {0};
  std::atomic<int> max_inside {0};
  handoff::set_terminate_for_test([this, &inside, &max_inside] {
    const int now = ++inside;
    int seen = max_inside.load();
    while (now > seen && !max_inside.compare_exchange_weak(seen, now)) {}
    std::this_thread::sleep_for(20ms);
    --inside;
    ++terminate_calls;
  });

  std::vector<std::jthread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([] {
      handoff::terminate_app(short_budget, "close app");
    });
  }
  threads.clear();

  EXPECT_EQ(terminate_calls.load(), 4) << "each granted caller must terminate through the seam";
  EXPECT_EQ(max_inside.load(), 1) << "request-path terminates must never run concurrently with each other";
  EXPECT_EQ(peer->end_all_calls.load(), 4) << "each concurrent caller sends its own END_ALL";
}

namespace {
  /**
   * @brief Authorization header for the web UI test credentials.
   *
   * @return `Basic <base64>`.
   */
  std::string auth_header() {
    return "Basic " + SimpleWeb::Crypto::Base64::encode("testuser:testpass");
  }

  const std::string TEST_PRIVATE_KEY = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDLePNlWN06FLlM
ujWzIX8UICO7SWfH5DXlafVjpxwi/WCkdO6FxixqRNGu71wMvJXFbDlNR8fqX2xo
+eq17J3uFKn+qdjmP3L38bkqxhoJ/nCrXkeGyCTQ+Daug63ZYSJeW2Mmf+LAR5/i
/fWYfXpSlbcf5XJQPEWvENpLqWu+NOU50dJXIEVYpUXRx2+x4ZbwkH7tVJm94L+C
OUyiJKQPyWgU2aFsyJGwHFfePfSUpfYHqbHZV/ILpY59VJairBwE99bx/mBvMI7a
hBmJTSDuDffJcPDhFF5kZa0UkQPrPvhXcQaSRti7v0VonEQj8pTSnGYr9ktWKk92
wxDyn9S3AgMBAAECggEAbEhQ14WELg2rUz7hpxPTaiV0fo4hEcrMN+u8sKzVF3Xa
QYsNCNoe9urq3/r39LtDxU3D7PGfXYYszmz50Jk8ruAGW8WN7XKkv3i/fxjv8JOc
6EYDMKJAnYkKqLLhCQddX/Oof2udg5BacVWPpvhX6a1NSEc2H6cDupfwZEWkVhMi
bCC3JcNmjFa8N7ow1/5VQiYVTjpxfV7GY1GRe7vMvBucdQKH3tUG5PYXKXytXw/j
KDLaECiYVT89KbApkI0zhy7I5g3LRq0Rs5fmYLCjVebbuAL1W5CJHFJeFOgMKvnO
QSl7MfHkTnzTzUqwkwXjgNMGsTosV4UloL9gXVF6GQKBgQD5fI771WETkpaKjWBe
6XUVSS98IOAPbTGpb8CIhSjzCuztNAJ+0ey1zklQHonMFbdmcWTkTJoF3ECqAos9
vxB4ROg+TdqGDcRrXa7Twtmhv66QvYxttkaK3CqoLX8CCTnjgXBCijo6sCpo6H1T
+y55bBDpxZjNFT5BV3+YPBfWQwKBgQDQyNt+saTqJqxGYV7zWQtOqKORRHAjaJpy
m5035pky5wORsaxQY8HxbsTIQp9jBSw3SQHLHN/NAXDl2k7VAw/axMc+lj9eW+3z
2Hv5LVgj37jnJYEpYwehvtR0B4jZnXLyLwShoBdRPkGlC5fs9+oWjQZoDwMLZfTg
eZVOJm6SfQKBgQDfxYcB/kuKIKsCLvhHaSJpKzF6JoqRi6FFlkScrsMh66TCxSmP
0n58O0Cqqhlyge/z5LVXyBVGOF2Pn6SAh4UgOr4MVAwyvNp2aprKuTQ2zhSnIjx4
k0sGdZ+VJOmMS/YuRwUHya+cwDHp0s3Gq77tja5F38PD/s/OD8sUIqJGvQKBgBfI
6ghy4GC0ayfRa+m5GSqq14dzDntaLU4lIDIAGS/NVYDBhunZk3yXq99Mh6/WJQVf
Uc77yRsnsN7ekeB+as33YONmZm2vd1oyLV1jpwjfMcdTZHV8jKAGh1l4ikSQRUoF
xTdMb5uXxg6xVWtvisFq63HrU+N2iAESmMnAYxRZAoGAVEFJRRjPrSIUTCCKRiTE
br+cHqy6S5iYRxGl9riKySBKeU16fqUACIvUqmqlx4Secj3/Hn/VzYEzkxcSPwGi
qMgdS0R+tacca7NopUYaaluneKYdS++DNlT/m+KVHqLynQr54z1qBlThg9KGrpmM
LGZkXtQpx6sX7v3Kq56PkNk=
-----END PRIVATE KEY-----)";

  const std::string TEST_PUBLIC_CERT = R"(-----BEGIN CERTIFICATE-----
MIIC6zCCAdOgAwIBAgIBATANBgkqhkiG9w0BAQsFADA5MQswCQYDVQQGEwJJVDEW
MBQGA1UECgwNR2FtZXNPbldoYWxlczESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTIy
MDQwOTA5MTYwNVoXDTQyMDQwNDA5MTYwNVowOTELMAkGA1UEBhMCSVQxFjAUBgNV
BAoMDUdhbWVzT25XaGFsZXMxEjAQBgNVBAMMCWxvY2FsaG9zdDCCASIwDQYJKoZI
hvcNAQEBBQADggEPADCCAQoCggEBAMt482VY3ToUuUy6NbMhfxQgI7tJZ8fkNeVp
9WOnHCL9YKR07oXGLGpE0a7vXAy8lcVsOU1Hx+pfbGj56rXsne4Uqf6p2OY/cvfx
uSrGGgn+cKteR4bIJND4Nq6DrdlhIl5bYyZ/4sBHn+L99Zh9elKVtx/lclA8Ra8Q
2kupa7405TnR0lcgRVilRdHHb7HhlvCQfu1Umb3gv4I5TKIkpA/JaBTZoWzIkbAc
V9499JSl9gepsdlX8guljn1UlqKsHAT31vH+YG8wjtqEGYlNIO4N98lw8OEUXmRl
rRSRA+s++FdxBpJG2Lu/RWicRCPylNKcZiv2S1YqT3bDEPKf1LcCAwEAATANBgkq
hkiG9w0BAQsFAAOCAQEAqPBqzvDjl89pZMll3Ge8RS7HeDuzgocrhOcT2jnk4ag7
/TROZuISjDp6+SnL3gPEt7E2OcFAczTg3l/wbT5PFb6vM96saLm4EP0zmLfK1FnM
JDRahKutP9rx6RO5OHqsUB+b4jA4W0L9UnXUoLKbjig501AUix0p52FBxu+HJ90r
HlLs3Vo6nj4Z/PZXrzaz8dtQ/KJMpd/g/9xlo6BKAnRk5SI8KLhO4hW6zG0QA56j
X4wnh1bwdiidqpcgyuKossLOPxbS786WmsesaAWPnpoY6M8aija+ALwNNuWWmyMg
9SVDV76xJzM36Uq7Kg3QJYTlY04WmPIdJHkCtXWf9g==
-----END CERTIFICATE-----)";
}  // namespace

/**
 * @brief Fixture with a real HTTPS server on the production web UI handlers for close app, update client, unpair and
 * unpair-all (seam `confighttp::register_app_client_routes`).
 */
class HandoffCallersConfigHttpTest: public HandoffCallerBase {
protected:
  std::unique_ptr<SimpleWeb::Server<SimpleWeb::HTTPS>> server;  ///< Test server.
  std::unique_ptr<SimpleWeb::Client<SimpleWeb::HTTPS>> client;  ///< Test client.
  std::jthread server_thread;  ///< Runs the server.
  unsigned short port {0};  ///< Assigned port.
  fs::path temp_dir;  ///< Holds the certificate files and the client state file.

  std::string saved_username;  ///< Saved config value.
  std::string saved_password;  ///< Saved config value.
  std::string saved_salt;  ///< Saved config value.
  std::string saved_locale;  ///< Saved config value.
  std::vector<std::string> saved_csrf_allowed_origins;  ///< Saved config value.
  std::string saved_state_file;  ///< Saved config value.
  bool saved_fresh_state {false};  ///< Saved config value.

  /**
   * @brief Start the server with the production routes, redirect the client state, set credentials.
   */
  void SetUp() override {
    HandoffCallerBase::SetUp();

    saved_username = config::sunshine.username;
    saved_password = config::sunshine.password;
    saved_salt = config::sunshine.salt;
    saved_locale = config::sunshine.locale;
    saved_csrf_allowed_origins = config::sunshine.csrf_allowed_origins;
    saved_state_file = config::nvhttp.file_state;
    saved_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];

    config::sunshine.username = "testuser";
    config::sunshine.salt = "testsalt";
    config::sunshine.password = util::hex(crypto::hash("testpass" + config::sunshine.salt)).to_string();
    config::sunshine.locale = "en";

    temp_dir = fs::path {SUNSHINE_TEST_BIN_DIR} / "terminate_callers_confighttp";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir);
    config::nvhttp.file_state = (temp_dir / "state.json").string();
    config::sunshine.flags[config::flag::FRESH_STATE] = false;
    nvhttp::test_support::reset_client_state();

    const auto cert_file = temp_dir / "cert.pem";
    const auto key_file = temp_dir / "key.pem";
    std::ofstream(cert_file) << TEST_PUBLIC_CERT;
    std::ofstream(key_file) << TEST_PRIVATE_KEY;

    server = std::make_unique<SimpleWeb::Server<SimpleWeb::HTTPS>>(cert_file.string(), key_file.string());
    server->config.port = 0;
    server->config.reuse_address = true;
    server->config.timeout_request = 5;
    server->config.timeout_content = 300;
    // SPEC-QUESTION: seam, see the file comment.
    confighttp::register_app_client_routes(*server);

    server_thread = std::jthread([this] {
      server->start([this](const unsigned short assigned_port) {
        port = assigned_port;
      });
    });
    for (int i = 0; i < 100 && port == 0; ++i) {
      std::this_thread::sleep_for(10ms);
    }
    ASSERT_NE(port, 0) << "the test server failed to start";

    config::sunshine.csrf_allowed_origins = {
      std::format("https://localhost:{}", port),
      std::format("https://127.0.0.1:{}", port),
    };
    client = std::make_unique<SimpleWeb::Client<SimpleWeb::HTTPS>>(std::format("localhost:{}", port), false);
    client->config.timeout = 5;
  }

  /**
   * @brief Stop the server and restore configuration and client state.
   */
  void TearDown() override {
    if (server) {
      server->stop();
    }
    if (server_thread.joinable()) {
      server_thread.join();
    }
    nvhttp::test_support::reset_client_state();
    config::nvhttp.file_state = saved_state_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = saved_fresh_state;
    config::sunshine.username = saved_username;
    config::sunshine.password = saved_password;
    config::sunshine.salt = saved_salt;
    config::sunshine.locale = saved_locale;
    config::sunshine.csrf_allowed_origins = saved_csrf_allowed_origins;
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    HandoffCallerBase::TearDown();
  }

  /**
   * @brief POST a JSON body to a route with credentials and a same-origin header.
   *
   * @param path Route.
   * @param body Request body.
   * @return Response.
   */
  std::shared_ptr<SimpleWeb::Client<SimpleWeb::HTTPS>::Response> post(const std::string &path, const std::string &body = "{}") {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Authorization", auth_header());
    headers.emplace("Origin", std::format("https://localhost:{}", port));
    headers.emplace("Content-Type", "application/json");
    return client->request("POST", path, body, headers);
  }

  /**
   * @brief Add a paired client with a fresh certificate.
   *
   * @param name Client name.
   * @param enabled Whether it is enabled.
   * @return UUID of the client.
   */
  static std::string add_client(const std::string &name, bool enabled = true) {
    const auto credentials = test_utils::certificates::generate_ca_credentials(name);
    const auto uuid = nvhttp::test_support::add_client(name, credentials.x509, enabled);
    EXPECT_FALSE(uuid.empty()) << "precondition: the client must be added";
    return uuid;
  }

  /**
   * @brief Number of paired clients.
   *
   * @return Count.
   */
  static std::size_t client_count() {
    const auto clients = nvhttp::get_all_clients();
    return clients.contains("named_certs") ? clients["named_certs"].size() : 0;
  }

  /**
   * @brief Expect the usual success reply of the client endpoints: 200 and `status` true.
   *
   * @param response Response to check.
   */
  static void expect_ok_reply(const std::shared_ptr<SimpleWeb::Client<SimpleWeb::HTTPS>::Response> &response) {
    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->status_code, "200 OK") << "the reply must stay 200";
    const auto body = nlohmann::json::parse(response->content.string(), nullptr, false);
    ASSERT_FALSE(body.is_discarded()) << "the reply must be JSON";
    EXPECT_EQ(body.value("status", false), true) << "the reply must keep status true";
  }
};

// ---- closeApp ----------------------------------------------------------------------------------------------------

TEST_F(HandoffCallersConfigHttpTest, CloseAppRefusedAnswers503JsonAndKeepsTheApp) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::no_reply;
  install_peer(peer);
  run_fake_app();

  const auto response = post("/api/apps/close");
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->status_code, "503 Service Unavailable") << "a refusal must answer HTTP 503";
  const auto body = nlohmann::json::parse(response->content.string(), nullptr, false);
  ASSERT_FALSE(body.is_discarded()) << "a refusal must answer a JSON body";
  EXPECT_EQ(body.value("status", true), false) << "status must be false";
  EXPECT_EQ(body.value("status_code", 0), 503) << "status_code must be 503";
  ASSERT_TRUE(body.contains("error") && body["error"].is_string()) << "the body must carry an error text";
  EXPECT_FALSE(body["error"].get<std::string>().empty()) << "the error text must not be empty";
  const auto x_frame = response->header.find("X-Frame-Options");
  ASSERT_NE(x_frame, response->header.end()) << "the refusal must carry the headers of bad_request";
  EXPECT_EQ(x_frame->second, "DENY");
  EXPECT_EQ(terminate_calls.load(), 0) << "a refusal must not terminate the app";
  EXPECT_TRUE(app_running()) << "a refusal must leave the app running";
}

TEST_F(HandoffCallersConfigHttpTest, CloseAppGrantedTerminatesAndAnswersAsUpstream) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::ended;
  install_peer(peer);
  run_fake_app();

  const auto response = post("/api/apps/close");
  expect_ok_reply(response);
  EXPECT_EQ(peer->end_all_calls.load(), 1) << "close app must ask the peer once";
  EXPECT_EQ(terminate_calls.load(), 1) << "after ENDED the app must be terminated once";
}

TEST_F(HandoffCallersConfigHttpTest, CloseAppWithoutPeerBehavesAsUpstream) {
  enter_normal();
  run_fake_app();

  const auto response = post("/api/apps/close");
  expect_ok_reply(response);
  EXPECT_EQ(terminate_calls.load(), 1) << "without a peer the app must be terminated once";
}

TEST_F(HandoffCallersConfigHttpTest, CloseAppInNewPendingModeDoesNotAsk) {
  enter_new_pending();
  install_peer(peer);
  run_fake_app();

  const auto response = post("/api/apps/close");
  expect_ok_reply(response);
  EXPECT_EQ(peer->end_all_calls.load(), 0) << "mode new_pending must send no END_ALL";
  EXPECT_EQ(terminate_calls.load(), 1) << "mode new_pending must terminate as before";
}

// ---- updateClient -------------------------------------------------------------------------------------------------

TEST_F(HandoffCallersConfigHttpTest, UpdateClientDisableTellsThePeerWithTheCertHashAndTimeout) {
  enter_normal();
  install_peer(peer);
  const auto uuid = add_client("disable me");
  const std::string expected_hash = handoff::cert_sha256_hex(nvhttp::get_cert_by_uuid(uuid));
  ASSERT_EQ(expected_hash.size(), 64u) << "precondition: the client has a certificate";

  const auto response = post("/api/clients/update", nlohmann::json {{"uuid", uuid}, {"enabled", false}}.dump());
  expect_ok_reply(response);
  ASSERT_EQ(peer->close_cert_calls.load(), 1) << "disabling a client must send exactly one CLOSE_CERT";
  EXPECT_EQ(peer->close_cert_hashes.front(), expected_hash) << "CLOSE_CERT must carry cert_sha256_hex of the client's certificate";
  EXPECT_EQ(peer->last_close_cert_timeout, handoff::peer_request_timeout) << "the timeout must be exactly peer_request_timeout";
}

TEST_F(HandoffCallersConfigHttpTest, UpdateClientEnableDoesNotTellThePeer) {
  enter_normal();
  install_peer(peer);
  const auto uuid = add_client("enable me", false);

  const auto response = post("/api/clients/update", nlohmann::json {{"uuid", uuid}, {"enabled", true}}.dump());
  expect_ok_reply(response);
  EXPECT_EQ(peer->close_cert_calls.load(), 0) << "enabling a client must not send CLOSE_CERT";
}

TEST_F(HandoffCallersConfigHttpTest, UpdateClientNulloptReplyDoesNotChangeTheResponse) {
  enter_normal();
  peer->close_cert_reply = std::nullopt;
  install_peer(peer);
  const auto uuid = add_client("no reply");

  const auto response = post("/api/clients/update", nlohmann::json {{"uuid", uuid}, {"enabled", false}}.dump());
  expect_ok_reply(response);
  EXPECT_EQ(peer->close_cert_calls.load(), 1) << "CLOSE_CERT must still have been sent";
}

TEST_F(HandoffCallersConfigHttpTest, UpdateClientTerminatesWhenNoSessionRunsAnywhere) {
  enter_normal();
  peer->close_cert_reply = 0u;
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {};
  install_peer(peer);
  run_fake_app();
  const auto uuid = add_client("last session");

  const auto response = post("/api/clients/update", nlohmann::json {{"uuid", uuid}, {"enabled", false}}.dump());
  expect_ok_reply(response);
  EXPECT_EQ(terminate_calls.load(), 1) << "no session in either process and an app running: the app must be terminated";
}

TEST_F(HandoffCallersConfigHttpTest, UpdateClientKeepsTheAppWhenThePeerStillReportsSessions) {
  enter_normal();
  peer->close_cert_reply = 0u;
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {make_entry(5)};
  install_peer(peer);
  run_fake_app();
  const auto uuid = add_client("other sessions");

  const auto response = post("/api/clients/update", nlohmann::json {{"uuid", uuid}, {"enabled", false}}.dump());
  expect_ok_reply(response);
  EXPECT_EQ(terminate_calls.load(), 0) << "the decision follows total_session_count: a peer with sessions keeps the app";
  EXPECT_TRUE(app_running()) << "the app must keep running";
}

TEST_F(HandoffCallersConfigHttpTest, UpdateClientKeepsTheAppWhenTheSessionCountIsUnknown) {
  enter_normal();
  peer->list_reply = std::nullopt;
  install_peer(peer);
  run_fake_app();
  const auto uuid = add_client("unknown sessions");

  const auto response = post("/api/clients/update", nlohmann::json {{"uuid", uuid}, {"enabled", false}}.dump());
  expect_ok_reply(response);
  EXPECT_EQ(terminate_calls.load(), 0) << "an unanswered SESSIONS? counts as non-zero: the app must be kept";
}

TEST_F(HandoffCallersConfigHttpTest, UpdateClientDecisionIgnoresTheCloseCertReply) {
  enter_normal();
  peer->close_cert_reply = 3u;  // the peer says it ended 3 sessions
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {make_entry(5)};  // but still shows one afterwards
  install_peer(peer);
  run_fake_app();
  const auto uuid = add_client("slow peer");

  const auto response = post("/api/clients/update", nlohmann::json {{"uuid", uuid}, {"enabled", false}}.dump());
  expect_ok_reply(response);
  EXPECT_EQ(terminate_calls.load(), 0) << "total_session_count is read after the CLOSE_CERT: the remaining session keeps the app";
}

TEST_F(HandoffCallersConfigHttpTest, UpdateClientWithoutRunningAppTerminatesNothing) {
  enter_normal();
  install_peer(peer);
  const auto uuid = add_client("no app");

  const auto response = post("/api/clients/update", nlohmann::json {{"uuid", uuid}, {"enabled", false}}.dump());
  expect_ok_reply(response);
  EXPECT_EQ(terminate_calls.load(), 0) << "with no app running nothing is terminated";
}

TEST_F(HandoffCallersConfigHttpTest, UpdateClientStillTellsThePeerInNewPendingMode) {
  enter_new_pending();
  install_peer(peer);
  const auto uuid = add_client("pending mode");

  const auto response = post("/api/clients/update", nlohmann::json {{"uuid", uuid}, {"enabled", false}}.dump());
  expect_ok_reply(response);
  EXPECT_EQ(peer->close_cert_calls.load(), 1) << "CLOSE_CERT must be sent in every mode while a session peer exists";
  EXPECT_EQ(peer->end_all_calls.load(), 0) << "mode new_pending must send no END_ALL";
}

TEST_F(HandoffCallersConfigHttpTest, UpdateClientRefusalLeavesTheReplyUnchanged) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::no_reply;
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {};
  install_peer(peer);
  run_fake_app();
  const auto uuid = add_client("refused");

  const auto response = post("/api/clients/update", nlohmann::json {{"uuid", uuid}, {"enabled", false}}.dump());
  expect_ok_reply(response);
  EXPECT_EQ(peer->end_all_calls.load(), 1) << "the terminate decision was reached, so END_ALL must have been asked";
  EXPECT_EQ(terminate_calls.load(), 0) << "a refusal must not terminate the app";
  EXPECT_TRUE(app_running()) << "a refusal must leave the app running";
}

// ---- unpair / unpairAll -------------------------------------------------------------------------------------------

TEST_F(HandoffCallersConfigHttpTest, UnpairLastClientGrantedTerminates) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::ended;
  install_peer(peer);
  run_fake_app();
  const auto uuid = add_client("last client");

  const auto response = post("/api/clients/unpair", nlohmann::json {{"uuid", uuid}}.dump());
  expect_ok_reply(response);
  EXPECT_EQ(client_count(), 0u) << "the client must be removed";
  EXPECT_EQ(terminate_calls.load(), 1) << "the last client's unpair terminates the app after ENDED";
}

TEST_F(HandoffCallersConfigHttpTest, UnpairLastClientRefusedRemovesTheClientAndKeepsTheApp) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::no_reply;
  install_peer(peer);
  run_fake_app();
  const auto uuid = add_client("last client refused");

  const auto response = post("/api/clients/unpair", nlohmann::json {{"uuid", uuid}}.dump());
  expect_ok_reply(response);
  EXPECT_EQ(client_count(), 0u) << "on a refusal the client stays removed";
  EXPECT_EQ(terminate_calls.load(), 0) << "on a refusal the app must not be terminated";
  EXPECT_TRUE(app_running()) << "on a refusal the app keeps running";
}

TEST_F(HandoffCallersConfigHttpTest, UnpairWithOtherClientsLeftDoesNotAsk) {
  enter_normal();
  install_peer(peer);
  run_fake_app();
  const auto first = add_client("first client");
  add_client("second client");

  const auto response = post("/api/clients/unpair", nlohmann::json {{"uuid", first}}.dump());
  expect_ok_reply(response);
  EXPECT_EQ(client_count(), 1u) << "exactly the named client must be removed";
  EXPECT_EQ(peer->end_all_calls.load(), 0) << "the rule asks only when no client remains";
  EXPECT_EQ(terminate_calls.load(), 0) << "the app must keep running while clients remain";
}

TEST_F(HandoffCallersConfigHttpTest, UnpairWithoutPeerBehavesAsUpstream) {
  enter_normal();
  run_fake_app();
  const auto uuid = add_client("no peer");

  const auto response = post("/api/clients/unpair", nlohmann::json {{"uuid", uuid}}.dump());
  expect_ok_reply(response);
  EXPECT_EQ(terminate_calls.load(), 1) << "without a peer the last client's unpair terminates as before";
}

TEST_F(HandoffCallersConfigHttpTest, UnpairAllGrantedTerminates) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::ended;
  install_peer(peer);
  run_fake_app();
  add_client("one");
  add_client("two");

  const auto response = post("/api/clients/unpair-all");
  expect_ok_reply(response);
  EXPECT_EQ(client_count(), 0u) << "all clients must be removed";
  EXPECT_EQ(terminate_calls.load(), 1) << "unpair-all terminates the app after ENDED";
}

TEST_F(HandoffCallersConfigHttpTest, UnpairAllRefusedRemovesTheClientsAndKeepsTheApp) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::no_reply;
  install_peer(peer);
  run_fake_app();
  add_client("one");
  add_client("two");

  const auto response = post("/api/clients/unpair-all");
  expect_ok_reply(response);
  EXPECT_EQ(client_count(), 0u) << "on a refusal all clients stay removed";
  EXPECT_EQ(terminate_calls.load(), 0) << "on a refusal the app must not be terminated";
  EXPECT_TRUE(app_running()) << "on a refusal the app keeps running";
}

TEST_F(HandoffCallersConfigHttpTest, UnpairAllWithoutPeerBehavesAsUpstream) {
  enter_normal();
  run_fake_app();
  add_client("one");

  const auto response = post("/api/clients/unpair-all");
  expect_ok_reply(response);
  EXPECT_EQ(terminate_calls.load(), 1) << "without a peer unpair-all terminates as before";
}

/**
 * @brief Fixture with a real HTTPS server on the production `/cancel`, `/launch` and `/resume` handlers (seams
 * `nvhttp::test_support::cancel_https`, `launch_https` and `resume_https`).
 */
class HandoffCallersNvhttpTest: public HandoffCallerBase {
protected:
  using server_t = SimpleWeb::Server<nvhttp::SunshineHTTPS>;  ///< Server type of the client routes.
  std::unique_ptr<server_t> server;  ///< Test server.
  std::unique_ptr<SimpleWeb::Client<SimpleWeb::HTTPS>> client;  ///< Test client.
  std::jthread server_thread;  ///< Runs the server.
  unsigned short port {0};  ///< Assigned port.
  fs::path temp_dir;  ///< Holds the certificate files.
  std::string saved_state_file;  ///< Saved config value.
  bool saved_fresh_state {false};  ///< Saved config value.

  /**
   * @brief Start the server with the production handlers.
   */
  void SetUp() override {
    HandoffCallerBase::SetUp();
    saved_state_file = config::nvhttp.file_state;
    saved_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    temp_dir = fs::path {SUNSHINE_TEST_BIN_DIR} / "terminate_callers_nvhttp";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir);
    config::nvhttp.file_state = (temp_dir / "state.json").string();
    config::sunshine.flags[config::flag::FRESH_STATE] = false;
    nvhttp::test_support::reset_client_state();

    const auto credentials = test_utils::certificates::generate_ca_credentials("nvhttp caller server");
    const auto cert_file = temp_dir / "cert.pem";
    const auto key_file = temp_dir / "key.pem";
    std::ofstream(cert_file) << credentials.x509;
    std::ofstream(key_file) << credentials.pkey;

    server = std::make_unique<server_t>(cert_file.string(), key_file.string());
    server->config.port = 0;
    server->config.reuse_address = true;
    server->config.timeout_request = 5;
    server->config.timeout_content = 300;
    // SPEC-QUESTION: seams, see the file comment.
    server->resource["^/cancel$"]["GET"] = [](auto response, auto request) {
      nvhttp::test_support::cancel_https(std::move(response), std::move(request));
    };
    server->resource["^/launch$"]["GET"] = [](auto response, auto request) {
      nvhttp::test_support::launch_https(std::move(response), std::move(request));
    };
    server->resource["^/resume$"]["GET"] = [](auto response, auto request) {
      nvhttp::test_support::resume_https(std::move(response), std::move(request));
    };

    server_thread = std::jthread([this] {
      server->start([this](const unsigned short assigned_port) {
        port = assigned_port;
      });
    });
    for (int i = 0; i < 100 && port == 0; ++i) {
      std::this_thread::sleep_for(10ms);
    }
    ASSERT_NE(port, 0) << "the test server failed to start";
    client = std::make_unique<SimpleWeb::Client<SimpleWeb::HTTPS>>(std::format("localhost:{}", port), false);
    client->config.timeout = 5;
  }

  /**
   * @brief Stop the server and restore configuration.
   */
  void TearDown() override {
    if (server) {
      server->stop();
    }
    if (server_thread.joinable()) {
      server_thread.join();
    }
    nvhttp::test_support::reset_client_state();
    config::nvhttp.file_state = saved_state_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = saved_fresh_state;
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    HandoffCallerBase::TearDown();
  }

  /**
   * @brief Fetch a route and parse the XML reply.
   *
   * @param target Request target with query.
   * @param tree Receives the parsed reply.
   */
  void fetch(const std::string &target, boost::property_tree::ptree &tree) {
    const auto response = client->request("GET", target);
    ASSERT_NE(response, nullptr);
    std::istringstream in {response->content.string()};
    boost::property_tree::read_xml(in, tree);
  }

  /**
   * @brief The `status_code` attribute of a parsed reply.
   *
   * @param tree Parsed reply.
   * @return Status code, or -1.
   */
  static int status_code(const boost::property_tree::ptree &tree) {
    return tree.get<int>("root.<xmlattr>.status_code", -1);
  }

  /**
   * @brief A `/launch` query that passes the argument check.
   *
   * SPEC-QUESTION: the exact argument names are the upstream ones; a request that fails the argument check answers
   * 400 and the tests that expect 503 would then fail with a message saying so.
   *
   * @return Request target.
   */
  static std::string launch_target() {
    return "/launch?uniqueid=0123456789ABCDEF&appid=1&mode=1280x720x30&additionalStates=1&sops=0"
           "&rikey=00112233445566778899aabbccddeeff&rikeyid=1&localAudioPlayMode=0&surroundAudioInfo=196610"
           "&remoteControllersBitmap=0&gcmap=0&gcpersist=0";
  }
};

TEST_F(HandoffCallersNvhttpTest, CancelAsksEvenWhenNoAppRuns) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::ended;
  install_peer(peer);
  ASSERT_FALSE(app_running()) << "precondition: no app runs";

  boost::property_tree::ptree tree;
  fetch("/cancel?uniqueid=0123456789ABCDEF", tree);
  EXPECT_EQ(peer->end_all_calls.load(), 1) << "/cancel must send END_ALL even with no app and no own sessions";
  EXPECT_EQ(status_code(tree), 200) << "a granted /cancel keeps the upstream status";
}

TEST_F(HandoffCallersNvhttpTest, CancelRefusedAnswers503AndEndsNothing) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::no_reply;
  install_peer(peer);
  run_fake_app();
  auto session = rtsp_stream::testing::insert_fake_session(7, stream::session::state_e::RUNNING);
  ASSERT_EQ(rtsp_stream::session_count(), 1) << "precondition: one own session";

  boost::property_tree::ptree tree;
  fetch("/cancel?uniqueid=0123456789ABCDEF", tree);
  EXPECT_EQ(status_code(tree), 503) << "a refusal must answer status_code 503";
  EXPECT_EQ(tree.get<int>("root.cancel", -1), 0) << "a refusal must answer cancel 0";
  EXPECT_FALSE(tree.get<std::string>("root.<xmlattr>.status_message", "").empty()) << "status_message must say the previous process still holds the app";
  EXPECT_EQ(terminate_calls.load(), 0) << "a refusal must not terminate the app";
  EXPECT_TRUE(app_running()) << "a refusal must leave the app running";
  EXPECT_EQ(rtsp_stream::session_count(), 1) << "a refusal must not end the process's own sessions";
  EXPECT_NE(stream::session::state(*session), stream::session::state_e::STOPPING) << "the own session must not even be asked to stop";
}

TEST_F(HandoffCallersNvhttpTest, CancelGrantedEndsSessionsAndTerminates) {
  enter_normal();
  peer->end_all_reply = handoff::drain_peer_t::reply_e::ended;
  install_peer(peer);
  run_fake_app();
  auto session = rtsp_stream::testing::insert_fake_session(7, stream::session::state_e::RUNNING);

  boost::property_tree::ptree tree;
  fetch("/cancel?uniqueid=0123456789ABCDEF", tree);
  EXPECT_EQ(tree.get<int>("root.cancel", -1), 1) << "a granted /cancel must answer cancel 1";
  EXPECT_EQ(terminate_calls.load(), 1) << "a granted /cancel must terminate the app";
  EXPECT_EQ(rtsp_stream::session_count(), 0) << "a granted /cancel must end the own sessions";
}

TEST_F(HandoffCallersNvhttpTest, CancelWithoutPeerBehavesAsUpstream) {
  enter_normal();
  run_fake_app();

  boost::property_tree::ptree tree;
  fetch("/cancel?uniqueid=0123456789ABCDEF", tree);
  EXPECT_EQ(tree.get<int>("root.cancel", -1), 1) << "without a peer /cancel must answer cancel 1";
  EXPECT_EQ(terminate_calls.load(), 1) << "without a peer /cancel must terminate the app";
}

TEST_F(HandoffCallersNvhttpTest, LaunchRefusedWhilePeerHasSessions) {
  enter_normal();
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {make_entry(9)};
  install_peer(peer);
  const auto next_id = nvhttp::next_launch_session_id();

  boost::property_tree::ptree tree;
  fetch(launch_target(), tree);
  EXPECT_EQ(status_code(tree), 503) << "/launch must answer status_code 503 while the peer has sessions";
  EXPECT_EQ(tree.get<int>("root.gamesession", -1), 0) << "/launch must answer gamesession 0";
  EXPECT_FALSE(tree.get<std::string>("root.<xmlattr>.status_message", "").empty()) << "status_message must carry the refusal text";
  EXPECT_FALSE(app_running()) << "a refused /launch must start no app";
  EXPECT_EQ(nvhttp::next_launch_session_id(), next_id) << "a refused /launch must not create a launch session";
}

TEST_F(HandoffCallersNvhttpTest, LaunchRefusedWhileSessionCountIsUnknown) {
  enter_normal();
  peer->list_reply = std::nullopt;
  install_peer(peer);

  boost::property_tree::ptree tree;
  fetch(launch_target(), tree);
  EXPECT_EQ(status_code(tree), 503) << "a timed-out SESSIONS? must refuse /launch";
  EXPECT_FALSE(app_running()) << "a refused /launch must start no app";
}

TEST_F(HandoffCallersNvhttpTest, LaunchIsNotRefusedWithoutAPeer) {
  enter_normal();

  boost::property_tree::ptree tree;
  fetch(launch_target(), tree);
  EXPECT_NE(status_code(tree), 503) << "without a peer the guard must not refuse /launch";
}

TEST_F(HandoffCallersNvhttpTest, LaunchIsNotRefusedWhenThePeerHasNoSessions) {
  enter_normal();
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {};
  install_peer(peer);

  boost::property_tree::ptree tree;
  fetch(launch_target(), tree);
  EXPECT_NE(status_code(tree), 503) << "a peer without sessions must not refuse /launch";
  EXPECT_EQ(peer->list_calls.load(), 1) << "the guard must ask the peer once";
}

TEST_F(HandoffCallersNvhttpTest, LaunchIsNotGuardedInNewPendingMode) {
  enter_new_pending();
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {make_entry(9)};
  install_peer(peer);

  boost::property_tree::ptree tree;
  fetch(launch_target(), tree);
  EXPECT_NE(status_code(tree), 503) << "mode new_pending must not refuse /launch";
  EXPECT_EQ(peer->list_calls.load(), 0) << "mode new_pending must send nothing";
}

TEST_F(HandoffCallersNvhttpTest, LaunchAppAlreadyRunningKeepsItsOwnAnswer) {
  enter_normal();
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {make_entry(9)};
  install_peer(peer);
  run_fake_app();

  boost::property_tree::ptree tree;
  fetch(launch_target(), tree);
  EXPECT_NE(status_code(tree), 503) << "the 'app already running' answer has priority over the guard";
  EXPECT_EQ(peer->list_calls.load(), 0) << "the guard runs after the 'app already running' check";
}

TEST_F(HandoffCallersNvhttpTest, ResumeIsNotGuarded) {
  enter_normal();
  peer->list_reply = std::vector<handoff::protocol::session_entry_t> {make_entry(9)};
  install_peer(peer);

  boost::property_tree::ptree tree;
  fetch("/resume?uniqueid=0123456789ABCDEF&rikey=00112233445566778899aabbccddeeff&rikeyid=1", tree);
  EXPECT_NE(status_code(tree), 503) << "/resume must not be guarded";
  EXPECT_EQ(peer->list_calls.load(), 0) << "/resume must not ask the peer for sessions";
}

#endif  // __linux__
