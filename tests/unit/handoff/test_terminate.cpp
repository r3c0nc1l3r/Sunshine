/**
 * @file tests/unit/handoff/test_terminate.cpp
 * @brief Blind tests for the terminate rule module (src/handoff/terminate.h): `terminate_gate`, `terminate_app`,
 * `terminate_app_permitted`, `draining_session_count`, `launch_refusal` and the budget constants.
 *
 * Fake peers are installed with `runtime().set_peer()`; the real `proc::proc.terminate()` is replaced through
 * `set_terminate_for_test`. Budgets other than the two named constants are used on purpose, so a refusal does not take
 * 15 s: the module's rules depend on the mode, the peer and (for the shutdown rule) on `budget ==
 * shutdown_terminate_budget`, not on the length of any other budget.
 *
 * The shutdown-path refusal needs the full 5 s budget; those tests are `DISABLED_` so that they do not break the 2 s
 * rule of the unit suite. Run them with `--gtest_also_run_disabled_tests --gtest_filter='*DISABLED_*'`.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <functional>
  #include <future>
  #include <map>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <string_view>
  #include <thread>
  #include <vector>

  // local includes
  #include "src/handoff/terminate.h"
  #include "src/process.h"

using namespace std::chrono_literals;

namespace {
  using handoff::drain_peer_t;
  using handoff::mode_e;
  using handoff::terminate_permission_e;
  using clock_type = std::chrono::steady_clock;

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
   * @brief A peer with only the terminate-rule knowledge (no session knowledge).
   */
  class fake_drain_peer_t: public drain_peer_t {
  public:
    bool exists() override {
      return exists_value.load();
    }

    bool gone() override {
      return gone_value.load();
    }

    reply_e end_all(std::chrono::milliseconds timeout) override {
      ++end_all_calls;
      last_end_all_timeout = timeout.count();
      return reply;
    }

    std::atomic<bool> exists_value {true};  ///< Result of `exists()`.
    std::atomic<bool> gone_value {false};  ///< Result of `gone()`.
    std::atomic<int> end_all_calls {0};  ///< Number of `end_all` calls.
    std::atomic<long long> last_end_all_timeout {-1};  ///< Timeout of the last `end_all`, in ms.
    reply_e reply {reply_e::no_reply};  ///< Result of `end_all`.
  };

  /**
   * @brief A peer with session knowledge.
   */
  class fake_session_peer_t: public handoff::session_peer_t {
  public:
    bool exists() override {
      return exists_value.load();
    }

    bool gone() override {
      return gone_value.load();
    }

    reply_e end_all(std::chrono::milliseconds timeout) override {
      ++end_all_calls;
      last_end_all_timeout = timeout.count();
      return reply;
    }

    std::optional<std::vector<handoff::protocol::session_entry_t>> list(std::chrono::milliseconds timeout) override {
      ++list_calls;
      last_list_timeout = timeout.count();
      return list_result;
    }

    std::optional<rtsp_stream::close_result_e> close(std::uint32_t, std::chrono::milliseconds) override {
      return std::nullopt;
    }

    std::optional<std::uint32_t> close_cert(std::string_view, std::chrono::milliseconds) override {
      return std::nullopt;
    }

    handoff::state_e state() override {
      return handoff::state_e::none;
    }

    std::atomic<bool> exists_value {true};  ///< Result of `exists()`.
    std::atomic<bool> gone_value {false};  ///< Result of `gone()`.
    std::atomic<int> end_all_calls {0};  ///< Number of `end_all` calls.
    std::atomic<int> list_calls {0};  ///< Number of `list` calls.
    std::atomic<long long> last_end_all_timeout {-1};  ///< Timeout of the last `end_all`, in ms.
    std::atomic<long long> last_list_timeout {-1};  ///< Timeout of the last `list`, in ms.
    reply_e reply {reply_e::no_reply};  ///< Result of `end_all`.
    std::optional<std::vector<handoff::protocol::session_entry_t>> list_result;  ///< Result of `list`.
  };

  /**
   * @brief Put the process-wide runtime into `target`, using only documented steps.
   *
   * @param target Mode.
   * @return `true` when the runtime is in `target`.
   */
  bool set_mode(mode_e target) {
    auto &rt = handoff::runtime();
    if (target == mode_e::disabled) {
      return rt.mode() == mode_e::disabled;
    }
    if (target == mode_e::normal || target == mode_e::draining) {
      handoff::settings_t settings;
      settings.enabled = true;
      rt.configure(settings);
      if (target == mode_e::draining) {
        return rt.transition(mode_e::normal, mode_e::draining);
      }
      return rt.mode() == mode_e::normal;
    }
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
    if (!rt.early_init(env).empty()) {
      return false;
    }
    if (target == mode_e::new_pending) {
      return rt.mode() == mode_e::new_pending;
    }
    return rt.transition(mode_e::new_pending, mode_e::new_active);
  }

  /**
   * @brief Fixture: resets the process-wide runtime and the terminate hook around every test, and counts hook calls.
   */
  class HandoffTerminateTest: public BaseTest {
  protected:
    void SetUp() override {
      BaseTest::SetUp();
      handoff::runtime().reset_for_test();
      handoff::set_terminate_for_test([this] {
        ++terminate_calls;
      });
    }

    void TearDown() override {
      handoff::set_terminate_for_test({});
      handoff::runtime().reset_for_test();
      BaseTest::TearDown();
    }

    std::atomic<int> terminate_calls {0};  ///< Calls of the replaced `proc::proc.terminate()`.
  };

  const std::vector<handoff::protocol::session_entry_t> one_session {{7, 1700000000000, "udp/ipv4", "lan"}};
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffTerminateConstantsTest, Budgets) {
  EXPECT_EQ(handoff::request_terminate_budget, 15000ms) << "The request paths have a 15 s budget.";
  EXPECT_EQ(handoff::shutdown_terminate_budget, 5000ms) << "The shutdown path has a 5 s budget (inside the 10 s forced-shutdown limit).";
  EXPECT_LT(handoff::shutdown_terminate_budget, handoff::request_terminate_budget);
  EXPECT_EQ(handoff::peer_request_timeout, 500ms) << "SESSIONS?, CLOSE and CLOSE_CERT time out after 500 ms.";
}

// ---------------------------------------------------------------------------------------------------------------------
// terminate_gate
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffTerminateTest, GateWithoutPeerIsNoDrain) {
  EXPECT_EQ(handoff::terminate_gate(150ms), terminate_permission_e::no_drain) << "With no peer the permission is no_drain.";
}

TEST_F(HandoffTerminateTest, GateWithoutPeerReturnsAtOnce) {
  const auto start = clock_type::now();
  (void) handoff::terminate_gate(handoff::request_terminate_budget);
  EXPECT_LT(since(start), 300ms) << "With no peer the gate must not wait.";
}

TEST_F(HandoffTerminateTest, GateEndedWhenPeerAnswersEnded) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->reply = drain_peer_t::reply_e::ended;
  handoff::runtime().set_peer(peer);
  const auto start = clock_type::now();
  EXPECT_EQ(handoff::terminate_gate(handoff::request_terminate_budget), terminate_permission_e::ended);
  EXPECT_LT(since(start), 500ms) << "An ENDED answer must end the gate at once.";
  EXPECT_EQ(peer->end_all_calls.load(), 1) << "The gate sends exactly one END_ALL.";
  EXPECT_GT(peer->last_end_all_timeout.load(), 0) << "END_ALL gets the remaining budget.";
  EXPECT_LE(peer->last_end_all_timeout.load(), handoff::request_terminate_budget.count());
}

TEST_F(HandoffTerminateTest, GatePeerGoneWhenAlreadyGone) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->gone_value = true;
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_gate(150ms), terminate_permission_e::peer_gone) << "A peer that is already gone gives peer_gone.";
}

TEST_F(HandoffTerminateTest, GateNoDrainWhenPeerDoesNotExist) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->exists_value = false;
  peer->gone_value = true;
  handoff::runtime().set_peer(peer);
  const auto permission = handoff::terminate_gate(150ms);
  EXPECT_TRUE(handoff::allows_terminate(permission)) << "A peer that no longer exists must never refuse.";
}

TEST_F(HandoffTerminateTest, GateRefusedWhenNoReplyAndPeerAlive) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->reply = drain_peer_t::reply_e::no_reply;
  handoff::runtime().set_peer(peer);
  const auto start = clock_type::now();
  EXPECT_EQ(handoff::terminate_gate(150ms), terminate_permission_e::refused);
  EXPECT_GE(since(start), 100ms) << "A refusal comes only at the end of the budget.";
  EXPECT_LT(since(start), 1500ms);
  EXPECT_GE(peer->end_all_calls.load(), 1);
}

TEST_F(HandoffTerminateTest, GateRejectedIsTreatedLikeATimeout) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->reply = drain_peer_t::reply_e::rejected;
  handoff::runtime().set_peer(peer);
  const auto start = clock_type::now();
  EXPECT_EQ(handoff::terminate_gate(150ms), terminate_permission_e::refused) << "END_REJECTED with the peer alive is refused.";
  EXPECT_GE(since(start), 100ms) << "END_REJECTED is treated like a timeout: the gate polls until the budget ends.";
}

TEST_F(HandoffTerminateTest, GatePeerGoneWhileWaitingAfterRejected) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->reply = drain_peer_t::reply_e::rejected;
  handoff::runtime().set_peer(peer);
  std::thread killer([peer] {
    std::this_thread::sleep_for(60ms);
    peer->gone_value = true;
  });
  const auto start = clock_type::now();
  const auto permission = handoff::terminate_gate(1500ms);
  killer.join();
  EXPECT_EQ(permission, terminate_permission_e::peer_gone) << "The peer's pidfd is polled until the budget ends; gone gives peer_gone.";
  EXPECT_LT(since(start), 1200ms) << "peer_gone must be returned as soon as the peer is gone, not at the end of the budget.";
}

TEST_F(HandoffTerminateTest, GatePeerGoneWhileWaitingAfterNoReply) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  handoff::runtime().set_peer(peer);
  std::thread killer([peer] {
    std::this_thread::sleep_for(60ms);
    peer->gone_value = true;
  });
  const auto permission = handoff::terminate_gate(1500ms);
  killer.join();
  EXPECT_EQ(permission, terminate_permission_e::peer_gone);
}

TEST_F(HandoffTerminateTest, GateModesThatMaySendAsk) {
  for (const auto mode : {mode_e::disabled, mode_e::normal, mode_e::new_active}) {
    SCOPED_TRACE(std::string(handoff::to_string(mode)));
    handoff::runtime().reset_for_test();
    ASSERT_TRUE(set_mode(mode));
    auto peer = std::make_shared<fake_drain_peer_t>();
    peer->reply = drain_peer_t::reply_e::ended;
    handoff::runtime().set_peer(peer);
    EXPECT_EQ(handoff::terminate_gate(500ms), terminate_permission_e::ended) << "In this mode the gate must ask the peer.";
    EXPECT_EQ(peer->end_all_calls.load(), 1);
  }
}

TEST_F(HandoffTerminateTest, GateNewPendingModeIsNoDrainAndSendsNothing) {
  ASSERT_TRUE(set_mode(mode_e::new_pending));
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->reply = drain_peer_t::reply_e::ended;
  handoff::runtime().set_peer(peer);
  const auto start = clock_type::now();
  EXPECT_EQ(handoff::terminate_gate(500ms), terminate_permission_e::no_drain) << "Before COMMIT this process must not end the old process's sessions.";
  EXPECT_EQ(peer->end_all_calls.load(), 0) << "Nothing may be sent in mode new_pending.";
  EXPECT_LT(since(start), 300ms);
}

TEST_F(HandoffTerminateTest, GateDrainingModeIsNoDrainAndSendsNothing) {
  ASSERT_TRUE(set_mode(mode_e::draining));
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->reply = drain_peer_t::reply_e::ended;
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_gate(500ms), terminate_permission_e::no_drain);
  EXPECT_EQ(peer->end_all_calls.load(), 0) << "Nothing may be sent in mode draining.";
}

TEST_F(HandoffTerminateTest, GateNewPendingWithGonePeerIsStillNoDrain) {
  ASSERT_TRUE(set_mode(mode_e::new_pending));
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->gone_value = true;
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_gate(150ms), terminate_permission_e::no_drain) << "The mode rule comes before the peer is looked at.";
}

TEST_F(HandoffTerminateTest, ConcurrentGatesEachSendTheirOwnEndAll) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->reply = drain_peer_t::reply_e::ended;
  handoff::runtime().set_peer(peer);
  auto first = std::async(std::launch::async, [] {
    return handoff::terminate_gate(1000ms);
  });
  auto second = std::async(std::launch::async, [] {
    return handoff::terminate_gate(1000ms);
  });
  EXPECT_EQ(first.get(), terminate_permission_e::ended);
  EXPECT_EQ(second.get(), terminate_permission_e::ended);
  EXPECT_EQ(peer->end_all_calls.load(), 2) << "Concurrent callers each send their own END_ALL.";
}

TEST_F(HandoffTerminateTest, GateReadsThePeerOncePerCall) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->reply = drain_peer_t::reply_e::no_reply;
  handoff::runtime().set_peer(peer);
  auto future = std::async(std::launch::async, [] {
    return handoff::terminate_gate(300ms);
  });
  std::this_thread::sleep_for(60ms);
  handoff::runtime().set_peer(nullptr);  // dropped while the call runs
  EXPECT_EQ(future.get(), terminate_permission_e::refused) << "A peer dropped meanwhile must not change a running call.";
}

// ---------------------------------------------------------------------------------------------------------------------
// terminate_app_permitted
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffTerminateTest, PermittedCallsTheHookOnceWithoutAsking) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  handoff::runtime().set_peer(peer);
  handoff::terminate_app_permitted();
  EXPECT_EQ(terminate_calls.load(), 1) << "terminate_app_permitted terminates unconditionally, once.";
  EXPECT_EQ(peer->end_all_calls.load(), 0) << "terminate_app_permitted never asks the peer.";
}

TEST_F(HandoffTerminateTest, PermittedSerialisesConcurrentTerminates) {
  std::atomic<int> active {0};
  std::atomic<int> max_active {0};
  std::atomic<int> total {0};
  handoff::set_terminate_for_test([&] {
    const int now = ++active;
    int seen = max_active.load();
    while (now > seen && !max_active.compare_exchange_weak(seen, now)) {
    }
    std::this_thread::sleep_for(30ms);
    --active;
    ++total;
  });
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([] {
      handoff::terminate_app_permitted();
    });
  }
  for (auto &t : threads) {
    t.join();
  }
  EXPECT_EQ(total.load(), 4) << "Every call runs the terminate (the second finds no app in the real function).";
  EXPECT_EQ(max_active.load(), 1) << "Concurrent request-path terminates must run one after another.";
}

TEST_F(HandoffTerminateTest, PermittedHooksFromBothEntryPointsShareTheMutex) {
  std::atomic<int> active {0};
  std::atomic<int> max_active {0};
  handoff::set_terminate_for_test([&] {
    const int now = ++active;
    int seen = max_active.load();
    while (now > seen && !max_active.compare_exchange_weak(seen, now)) {
    }
    std::this_thread::sleep_for(30ms);
    --active;
  });
  auto first = std::async(std::launch::async, [] {
    handoff::terminate_app_permitted();
  });
  auto second = std::async(std::launch::async, [] {
    return handoff::terminate_app(500ms, "test");
  });
  first.get();
  second.get();
  EXPECT_EQ(max_active.load(), 1) << "terminate_app and terminate_app_permitted must use the same serialising mutex.";
}

// ---------------------------------------------------------------------------------------------------------------------
// terminate_app
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffTerminateTest, TerminateAppWithoutPeerTerminates) {
  EXPECT_EQ(handoff::terminate_app(150ms, "close app"), terminate_permission_e::no_drain);
  EXPECT_EQ(terminate_calls.load(), 1) << "With no draining process the app is terminated as before.";
}

TEST_F(HandoffTerminateTest, TerminateAppEndedTerminates) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->reply = drain_peer_t::reply_e::ended;
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_app(handoff::request_terminate_budget, "close app"), terminate_permission_e::ended);
  EXPECT_EQ(terminate_calls.load(), 1);
}

TEST_F(HandoffTerminateTest, TerminateAppPeerGoneTerminates) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->gone_value = true;
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_app(handoff::request_terminate_budget, "close app"), terminate_permission_e::peer_gone);
  EXPECT_EQ(terminate_calls.load(), 1);
}

TEST_F(HandoffTerminateTest, TerminateAppRefusedDoesNotTerminateOnRequestPaths) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_app(150ms, "close app"), terminate_permission_e::refused) << "The return value is the gate's permission.";
  EXPECT_EQ(terminate_calls.load(), 0) << "On a request path a refusal terminates nothing.";
}

TEST_F(HandoffTerminateTest, TerminateAppRejectedDoesNotTerminateOnRequestPaths) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->reply = drain_peer_t::reply_e::rejected;
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_app(150ms, "unpair"), terminate_permission_e::refused);
  EXPECT_EQ(terminate_calls.load(), 0);
}

TEST_F(HandoffTerminateTest, TerminateAppInNewPendingModeTerminatesWithoutAsking) {
  ASSERT_TRUE(set_mode(mode_e::new_pending));
  auto peer = std::make_shared<fake_drain_peer_t>();
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_app(150ms, "close app"), terminate_permission_e::no_drain);
  EXPECT_EQ(peer->end_all_calls.load(), 0);
  EXPECT_EQ(terminate_calls.load(), 1);
}

TEST_F(HandoffTerminateTest, TerminateAppPermittedAnswerTerminatesRegardlessOfTheAdoptedFlag) {
  // Shutdown budget with an immediate answer: no waiting. The adopted flag matters only after a refusal.
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->reply = drain_peer_t::reply_e::ended;
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_app(handoff::shutdown_terminate_budget, "shutdown"), terminate_permission_e::ended);
  EXPECT_EQ(terminate_calls.load(), 1) << "A granted permission terminates on the shutdown path.";
}

TEST_F(HandoffTerminateTest, TerminateAppShutdownPeerGoneTerminates) {
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->gone_value = true;
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_app(handoff::shutdown_terminate_budget, "shutdown"), terminate_permission_e::peer_gone);
  EXPECT_EQ(terminate_calls.load(), 1);
}

// ---------------------------------------------------------------------------------------------------------------------
// The shutdown rule after a refusal. It needs the whole 5 s budget, so these tests are DISABLED_ (see the file comment).
// ---------------------------------------------------------------------------------------------------------------------

namespace {
  /**
   * @brief Fixture for the shutdown rule: gives the global `proc::proc` one app, adopted or launched here.
   */
  class HandoffTerminateShutdownTest: public HandoffTerminateTest {
  protected:
    void TearDown() override {
      proc::proc = proc::proc_t {};
      HandoffTerminateTest::TearDown();
    }

    /// Give `proc::proc` a running app without any prep command; adopted through the documented adoption calls.
    void run_adopted_app() {
      proc::ctx_t app {};
      app.id = "1";
      app.name = "Game";
      app.exit_timeout = 5s;
      proc::proc_t source(boost::process::v1::environment {}, {app});
      source.set_running_for_test(app, 1, true, 0, 0);
      const auto snapshot = source.snapshot();
      ASSERT_TRUE(snapshot.has_value());
      proc::proc = proc::proc_t(boost::process::v1::environment {}, {app});
      handoff::protocol::go_t go;
      go.app = snapshot;
      std::string reason;
      auto plan = proc::proc.prepare_adoption(go, reason);
      ASSERT_TRUE(plan.has_value()) << reason;
      proc::proc.install_adoption(std::move(*plan));
      ASSERT_TRUE(proc::proc.adopted());
    }

    /// Give `proc::proc` a running app that this process "launched" (not adopted).
    void run_own_app() {
      proc::ctx_t app {};
      app.id = "1";
      app.name = "Game";
      app.exit_timeout = 5s;
      proc::proc = proc::proc_t(boost::process::v1::environment {}, {app});
      proc::proc.set_running_for_test(app, 1, true, 0, 0);
      ASSERT_FALSE(proc::proc.adopted());
    }
  };
}  // namespace

TEST_F(HandoffTerminateShutdownTest, DISABLED_RefusedShutdownSkipsTerminateForAnAdoptedApp) {
  run_adopted_app();
  auto peer = std::make_shared<fake_drain_peer_t>();
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_app(handoff::shutdown_terminate_budget, "shutdown"), terminate_permission_e::refused);
  EXPECT_EQ(terminate_calls.load(), 0) << "A refused shutdown terminate must skip terminate() for an ADOPTED app (no undo while the other process may stream).";
}

TEST_F(HandoffTerminateShutdownTest, DISABLED_RefusedShutdownStillTerminatesAnAppThisProcessLaunched) {
  run_own_app();
  auto peer = std::make_shared<fake_drain_peer_t>();
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_app(handoff::shutdown_terminate_budget, "shutdown"), terminate_permission_e::refused) << "The return value is still the gate's permission.";
  EXPECT_EQ(terminate_calls.load(), 1) << "A shutdown must never leave behind an app this process started: terminate even after a refusal.";
}

TEST_F(HandoffTerminateShutdownTest, GrantedShutdownTerminatesAnAdoptedApp) {
  run_adopted_app();
  auto peer = std::make_shared<fake_drain_peer_t>();
  peer->reply = drain_peer_t::reply_e::ended;
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_app(handoff::shutdown_terminate_budget, "shutdown"), terminate_permission_e::ended);
  EXPECT_EQ(terminate_calls.load(), 1) << "With ENDED the adopted app is terminated; the skip applies only after a refusal.";
}

TEST_F(HandoffTerminateShutdownTest, RefusedOtherBudgetSkipsTerminateEvenForAnOwnApp) {
  run_own_app();
  auto peer = std::make_shared<fake_drain_peer_t>();
  handoff::runtime().set_peer(peer);
  EXPECT_EQ(handoff::terminate_app(150ms, "close app"), terminate_permission_e::refused);
  EXPECT_EQ(terminate_calls.load(), 0) << "Only the shutdown budget terminates after a refusal; request paths keep the plain rule.";
}

// ---------------------------------------------------------------------------------------------------------------------
// draining_session_count
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffTerminateTest, CountIsZeroWithoutPeer) {
  const auto count = handoff::draining_session_count();
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(*count, 0u);
}

TEST_F(HandoffTerminateTest, CountIsTheNumberOfReplyEntries) {
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->list_result = std::vector<handoff::protocol::session_entry_t> {one_session[0], one_session[0], one_session[0]};
  handoff::runtime().set_peer(peer);
  const auto count = handoff::draining_session_count();
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(*count, 3u);
  EXPECT_EQ(peer->list_calls.load(), 1);
}

TEST_F(HandoffTerminateTest, CountIsZeroForAnEmptyReply) {
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->list_result = std::vector<handoff::protocol::session_entry_t> {};
  handoff::runtime().set_peer(peer);
  const auto count = handoff::draining_session_count();
  ASSERT_TRUE(count.has_value()) << "An empty list is an answer: 0, not unknown.";
  EXPECT_EQ(*count, 0u);
}

TEST_F(HandoffTerminateTest, CountIsUnknownWhenAliveAndNoAnswer) {
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->list_result = std::nullopt;
  handoff::runtime().set_peer(peer);
  EXPECT_FALSE(handoff::draining_session_count().has_value()) << "A peer that is not gone and gave no answer in time: unknown (nullopt).";
}

TEST_F(HandoffTerminateTest, CountIsZeroWhenPeerIsGone) {
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->gone_value = true;
  peer->exists_value = false;
  peer->list_result = std::nullopt;
  handoff::runtime().set_peer(peer);
  const auto count = handoff::draining_session_count();
  ASSERT_TRUE(count.has_value()) << "A gone peer has no sessions: 0, not unknown.";
  EXPECT_EQ(*count, 0u);
}

TEST_F(HandoffTerminateTest, CountIsUnknownForAPeerWithoutSessionKnowledge) {
  handoff::runtime().set_peer(std::make_shared<fake_drain_peer_t>());
  EXPECT_FALSE(handoff::draining_session_count().has_value()) << "A peer that is only a drain_peer_t has no session knowledge: unknown.";
}

TEST_F(HandoffTerminateTest, CountUsesPeerRequestTimeoutByDefault) {
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->list_result = std::vector<handoff::protocol::session_entry_t> {};
  handoff::runtime().set_peer(peer);
  (void) handoff::draining_session_count();
  EXPECT_EQ(peer->last_list_timeout.load(), handoff::peer_request_timeout.count()) << "The default timeout is peer_request_timeout (500 ms).";
}

TEST_F(HandoffTerminateTest, CountPassesAnExplicitTimeout) {
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->list_result = std::vector<handoff::protocol::session_entry_t> {};
  handoff::runtime().set_peer(peer);
  (void) handoff::draining_session_count(123ms);
  EXPECT_EQ(peer->last_list_timeout.load(), 123) << "The given timeout is the SESSIONS? wait.";
}

TEST_F(HandoffTerminateTest, CountIsZeroAndSendsNothingInNewPending) {
  ASSERT_TRUE(set_mode(mode_e::new_pending));
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->list_result = std::vector<handoff::protocol::session_entry_t> {one_session};
  handoff::runtime().set_peer(peer);
  const auto count = handoff::draining_session_count();
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(*count, 0u) << "Mode new_pending: 0 sessions.";
  EXPECT_EQ(peer->list_calls.load(), 0) << "Nothing may be sent in mode new_pending.";
}

TEST_F(HandoffTerminateTest, CountIsZeroAndSendsNothingInDraining) {
  ASSERT_TRUE(set_mode(mode_e::draining));
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->list_result = std::vector<handoff::protocol::session_entry_t> {one_session};
  handoff::runtime().set_peer(peer);
  const auto count = handoff::draining_session_count();
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(*count, 0u);
  EXPECT_EQ(peer->list_calls.load(), 0) << "Nothing may be sent in mode draining.";
}

TEST_F(HandoffTerminateTest, CountInNewPendingIsZeroEvenForAPeerWithoutSessionKnowledge) {
  ASSERT_TRUE(set_mode(mode_e::new_pending));
  handoff::runtime().set_peer(std::make_shared<fake_drain_peer_t>());
  const auto count = handoff::draining_session_count();
  ASSERT_TRUE(count.has_value()) << "The mode rule comes before the session-knowledge check.";
  EXPECT_EQ(*count, 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// launch_refusal
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffTerminateTest, LaunchAllowedWithoutPeer) {
  EXPECT_TRUE(handoff::launch_refusal().empty()) << "No peer: /launch may proceed (empty text).";
}

TEST_F(HandoffTerminateTest, LaunchAllowedWhenPeerReportsNoSessions) {
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->list_result = std::vector<handoff::protocol::session_entry_t> {};
  handoff::runtime().set_peer(peer);
  EXPECT_TRUE(handoff::launch_refusal().empty()) << "The peer reports no sessions: /launch may proceed.";
}

TEST_F(HandoffTerminateTest, LaunchAllowedWhenPeerIsGone) {
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->gone_value = true;
  peer->exists_value = false;
  handoff::runtime().set_peer(peer);
  EXPECT_TRUE(handoff::launch_refusal().empty()) << "A gone peer has no sessions: /launch may proceed.";
}

TEST_F(HandoffTerminateTest, LaunchRefusedWhenPeerHasSessions) {
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->list_result = one_session;
  handoff::runtime().set_peer(peer);
  const auto text = handoff::launch_refusal();
  EXPECT_FALSE(text.empty()) << "A draining session is running: /launch must be refused with a text.";
  EXPECT_EQ(text.find('\n'), std::string::npos) << "The text is one line.";
}

TEST_F(HandoffTerminateTest, LaunchRefusedWhenSessionCountIsUnknown) {
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->list_result = std::nullopt;
  handoff::runtime().set_peer(peer);
  EXPECT_FALSE(handoff::launch_refusal().empty()) << "A timed-out SESSIONS? counts as non-zero: /launch must be refused.";
}

TEST_F(HandoffTerminateTest, LaunchRefusedForAPeerWithoutSessionKnowledge) {
  handoff::runtime().set_peer(std::make_shared<fake_drain_peer_t>());
  EXPECT_FALSE(handoff::launch_refusal().empty()) << "Unknown session count (a plain drain_peer_t) counts as non-zero.";
}

TEST_F(HandoffTerminateTest, LaunchRefusalUsesPeerRequestTimeout) {
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->list_result = std::vector<handoff::protocol::session_entry_t> {};
  handoff::runtime().set_peer(peer);
  (void) handoff::launch_refusal();
  EXPECT_EQ(peer->last_list_timeout.load(), handoff::peer_request_timeout.count()) << "/launch blocks up to 500 ms for the reply.";
}

TEST_F(HandoffTerminateTest, LaunchAllowedAndNothingSentInNewPendingAndDraining) {
  for (const auto mode : {mode_e::new_pending, mode_e::draining}) {
    SCOPED_TRACE(std::string(handoff::to_string(mode)));
    handoff::runtime().reset_for_test();
    ASSERT_TRUE(set_mode(mode));
    auto peer = std::make_shared<fake_session_peer_t>();
    peer->list_result = one_session;
    handoff::runtime().set_peer(peer);
    EXPECT_TRUE(handoff::launch_refusal().empty()) << "In this mode the guard is empty.";
    EXPECT_EQ(peer->list_calls.load(), 0) << "Nothing may be sent in this mode.";
  }
}

TEST_F(HandoffTerminateTest, LaunchRefusalNamesNoIdentities) {
  auto peer = std::make_shared<fake_session_peer_t>();
  peer->list_result = one_session;
  handoff::runtime().set_peer(peer);
  const auto text = handoff::launch_refusal();
  EXPECT_EQ(text.find("udp/ipv4"), std::string::npos) << "The text must not carry session details.";
  EXPECT_EQ(text.find("1700000000000"), std::string::npos) << "The text must not carry session details.";
}

#endif  // __linux__
