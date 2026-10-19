/**
 * @file tests/unit/handoff/test_responder.cpp
 * @brief Blind tests for the draining process's responder (src/handoff/responder.h), written from the header and the
 * spec only.
 *
 * SPEC-QUESTION notes (also listed in the report):
 * - The `UINT32_MAX` clamp of the `close_cert` count lives in the production binding, which the `responder_ops_t`
 *   seam replaces, so it is not reachable here and is not tested.
 * - What `end_all_machine_t` does after `end_and_join` threw is "whatever it left", so a follow-up `END_ALL` after
 *   a throwing round is not tested.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <cstdint>
  #include <functional>
  #include <map>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <set>
  #include <stdexcept>
  #include <string>
  #include <thread>
  #include <type_traits>
  #include <variant>
  #include <vector>

  // lib includes
  #include <boost/log/attributes/value_extraction.hpp>
  #include <boost/log/core.hpp>
  #include <boost/log/sinks/basic_sink_backend.hpp>
  #include <boost/log/sinks/sync_frontend.hpp>
  #include <boost/make_shared.hpp>

  // platform includes
  #include <poll.h>
  #include <unistd.h>

  // local includes
  #include "src/handoff/responder.h"

using namespace std::chrono_literals;

namespace {
  using clock_type = std::chrono::steady_clock;
  namespace proto = handoff::protocol;
  namespace bl = boost::log;

  /**
   * @brief One captured log record.
   */
  struct log_entry_t {
    int severity {-1};  ///< Boost severity (2 Info, 3 Warning, 4 Error).
    std::string message;  ///< Message text.
  };

  /**
   * @brief Shared store of captured log records.
   */
  struct log_store_t {
    std::mutex mutex;  ///< Guards the entries.
    std::vector<log_entry_t> entries;  ///< Captured records.
  };

  /**
   * @brief Sink backend that stores every record.
   */
  class capture_backend_t: public bl::sinks::basic_sink_backend<bl::sinks::synchronized_feeding> {
  public:
    explicit capture_backend_t(std::shared_ptr<log_store_t> store):
        _store(std::move(store)) {
    }

    void consume(const bl::record_view &record) {
      log_entry_t entry;
      if (auto severity = bl::extract<int>("Severity", record)) {
        entry.severity = *severity;
      }
      if (auto message = bl::extract<std::string>("Message", record)) {
        entry.message = *message;
      }
      std::lock_guard lock(_store->mutex);
      _store->entries.push_back(std::move(entry));
    }

  private:
    std::shared_ptr<log_store_t> _store;  ///< Where records go.
  };

  /**
   * @brief Installs a capturing sink for the lifetime of the object.
   */
  class log_capture_t {
  public:
    log_capture_t():
        _store(std::make_shared<log_store_t>()),
        _sink(boost::make_shared<sink_t>(_store)) {
      bl::core::get()->add_sink(_sink);
    }

    ~log_capture_t() {
      bl::core::get()->remove_sink(_sink);
    }

    log_capture_t(const log_capture_t &) = delete;
    log_capture_t &operator=(const log_capture_t &) = delete;

    /**
     * @brief Number of records with a severity.
     */
    std::size_t count(int severity) const {
      std::lock_guard lock(_store->mutex);
      std::size_t n = 0;
      for (const auto &e : _store->entries) {
        n += e.severity == severity ? 1 : 0;
      }
      return n;
    }

    /**
     * @brief Wait until at least `n` records of a severity exist.
     */
    bool wait_count(int severity, std::size_t n, std::chrono::milliseconds limit) const {
      const auto deadline = clock_type::now() + limit;
      while (clock_type::now() < deadline) {
        if (count(severity) >= n) {
          return true;
        }
        std::this_thread::sleep_for(2ms);
      }
      return count(severity) >= n;
    }

  private:
    using sink_t = bl::sinks::synchronous_sink<capture_backend_t>;
    std::shared_ptr<log_store_t> _store;  ///< Captured records.
    boost::shared_ptr<sink_t> _sink;  ///< The installed sink.
  };

  [[maybe_unused]] constexpr int sev_info = 2;  ///< Info.
  [[maybe_unused]] constexpr int sev_warning = 3;  ///< Warning.
  [[maybe_unused]] constexpr int sev_error = 4;  ///< Error.

  /**
   * @brief A gate that blocks the callers that enter it until it is opened (bounded at 5 s).
   */
  struct gate_t {
    std::mutex m;  ///< Guards the members.
    std::condition_variable cv;  ///< Signals changes.
    bool open {true};  ///< Callers pass when open.
    int entered {0};  ///< Callers that entered so far.

    void close() {
      std::lock_guard lock(m);
      open = false;
    }

    void release() {
      {
        std::lock_guard lock(m);
        open = true;
      }
      cv.notify_all();
    }

    void enter() {
      std::unique_lock lock(m);
      ++entered;
      cv.notify_all();
      cv.wait_for(lock, 5s, [&] {
        return open;
      });
    }

    bool wait_entered(int n, std::chrono::milliseconds limit) {
      std::unique_lock lock(m);
      return cv.wait_for(lock, limit, [&] {
        return entered >= n;
      });
    }

    int entered_now() {
      std::lock_guard lock(m);
      return entered;
    }
  };

  /**
   * @brief Request id of any reply message, or 0.
   */
  std::uint64_t id_of(const proto::message_t &message) {
    return std::visit(
      [](const auto &m) -> std::uint64_t {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, proto::ended_t> || std::is_same_v<T, proto::end_rejected_t> || std::is_same_v<T, proto::sessions_reply_t> || std::is_same_v<T, proto::close_reply_t> || std::is_same_v<T, proto::close_cert_reply_t>) {
          return m.request_id;
        } else {
          return 0;
        }
      },
      message
    );
  }

  /**
   * @brief 64 lowercase hex characters, distinct per index (0 to 15).
   */
  std::string hash_of(int index) {
    return std::string(63, '0') + "0123456789abcdef"[index % 16];
  }

  /**
   * @brief A pipe whose read end is handed to `handle`; tells whether `handle` closed it.
   */
  struct pipe_probe_t {
    int r {-1};  ///< Read end (given away).
    int w {-1};  ///< Write end (kept).
    bool read_closed_checked {false};  ///< `read_end_closed` ran.
    bool read_closed {false};  ///< Result.

    pipe_probe_t() {
      int fds[2];
      if (::pipe(fds) == 0) {
        r = fds[0];
        w = fds[1];
      }
    }

    ~pipe_probe_t() {
      if (!read_closed && r >= 0) {
        ::close(r);
      }
      if (w >= 0) {
        ::close(w);
      }
    }

    /**
     * @brief Whether the read end is closed now (the write end then polls `POLLERR`).
     */
    bool read_end_closed() {
      pollfd p {w, POLLOUT, 0};
      ::poll(&p, 1, 0);
      read_closed = (p.revents & POLLERR) != 0;
      return read_closed;
    }
  };

  /**
   * @brief Fixture: a responder over a socketpair with fake operations and a fake clock.
   */
  class HandoffResponderTest: public ::testing::Test {
  protected:
    void SetUp() override {
      capture = std::make_unique<log_capture_t>();
      auto pair = handoff::make_channel_pair();
      ASSERT_TRUE(pair.has_value()) << "make_channel_pair must succeed.";
      resp_channel = std::make_unique<handoff::channel_t>(pair->first);
      peer = std::make_unique<handoff::channel_t>(pair->second);
      clock_base = clock_type::now();
      sessions = {proto::session_entry_t {5, 1000, "udp/ipv4", "lan"}, proto::session_entry_t {9, 2000, "udp/ipv6", "wan"}};
    }

    void TearDown() override {
      end_gate.release();
      cert_gate.release();
      responder.reset();
      peer.reset();
      resp_channel.reset();
      capture.reset();
    }

    /**
     * @brief Build the responder with the fake operations.
     */
    void make_responder() {
      handoff::responder_ops_t ops;
      ops.list_sessions = [this] {
        ++list_calls;
        if (list_throws) {
          throw std::runtime_error("list");
        }
        std::lock_guard lock(state_mutex);
        return sessions;
      };
      ops.close_session = [this](std::uint32_t id) {
        {
          std::lock_guard lock(state_mutex);
          closed_ids.push_back(id);
        }
        if (close_throws) {
          throw std::runtime_error("close");
        }
        return close_result.load();
      };
      ops.close_cert = [this](const std::string &hash) {
        {
          std::lock_guard lock(state_mutex);
          cert_hashes.push_back(hash);
        }
        cert_gate.enter();
        advance(std::chrono::milliseconds(cert_advance_ms.load()));
        if (cert_throws) {
          throw std::runtime_error("cert");
        }
        return cert_count.load();
      };
      ops.end_and_join = [this] {
        ++end_runs;
        end_gate.enter();
        advance(std::chrono::milliseconds(end_advance_ms.load()));
        end_done_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count();
        if (end_throws) {
          throw std::runtime_error("end");
        }
      };
      responder = std::make_unique<handoff::drain_responder_t>(*resp_channel, machine, std::move(ops), [this] {
        return clock_base + std::chrono::nanoseconds(clock_ns.load());
      });
    }

    void advance(std::chrono::milliseconds d) {
      clock_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
    }

    void send_in(proto::message_t message, int fd = -1) {
      responder->handle(handoff::received_t {std::move(message), fd});
    }

    std::optional<proto::message_t> recv(std::chrono::milliseconds limit) {
      handoff::received_t r;
      if (peer->receive(r, limit) != handoff::recv_status_e::ok) {
        return std::nullopt;
      }
      if (r.fd >= 0) {
        ::close(r.fd);
      }
      return std::move(r.message);
    }

    /**
     * @brief Collect up to `n` replies within a limit.
     */
    std::vector<proto::message_t> collect(std::size_t n, std::chrono::milliseconds limit) {
      std::vector<proto::message_t> out;
      const auto deadline = clock_type::now() + limit;
      while (out.size() < n) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_type::now());
        if (left <= 0ms) {
          break;
        }
        auto m = recv(left);
        if (!m) {
          break;
        }
        out.push_back(std::move(*m));
      }
      return out;
    }

    bool wait_live(std::size_t n, std::chrono::milliseconds limit) {
      const auto deadline = clock_type::now() + limit;
      while (clock_type::now() < deadline) {
        if (responder->live_handlers_for_test() == n) {
          return true;
        }
        std::this_thread::sleep_for(2ms);
      }
      return responder->live_handlers_for_test() == n;
    }

    bool wait_state(handoff::end_state_e s, std::chrono::milliseconds limit) {
      const auto deadline = clock_type::now() + limit;
      while (clock_type::now() < deadline) {
        if (machine.state() == s) {
          return true;
        }
        std::this_thread::sleep_for(2ms);
      }
      return machine.state() == s;
    }

    std::size_t cert_call_count() {
      std::lock_guard lock(state_mutex);
      return cert_hashes.size();
    }

    handoff::end_all_machine_t machine;  ///< The shared machine.
    std::unique_ptr<log_capture_t> capture;  ///< Log capture.
    std::unique_ptr<handoff::channel_t> resp_channel;  ///< The responder's end.
    std::unique_ptr<handoff::channel_t> peer;  ///< The active process's end.
    std::unique_ptr<handoff::drain_responder_t> responder;  ///< Under test.

    clock_type::time_point clock_base;  ///< Fake clock origin.
    std::atomic<std::int64_t> clock_ns {0};  ///< Fake clock offset.

    std::mutex state_mutex;  ///< Guards the vectors below and `sessions`.
    std::vector<proto::session_entry_t> sessions;  ///< What `list_sessions` returns.
    std::vector<std::uint32_t> closed_ids;  ///< Ids passed to `close_session`.
    std::vector<std::string> cert_hashes;  ///< Hashes passed to `close_cert`.
    std::atomic<int> list_calls {0};  ///< `list_sessions` calls.
    std::atomic<bool> list_throws {false};  ///< `list_sessions` throws.
    std::atomic<bool> close_throws {false};  ///< `close_session` throws.
    std::atomic<handoff::close_outcome_e> close_result {handoff::close_outcome_e::closed};  ///< Outcome.
    std::atomic<std::uint32_t> cert_count {3};  ///< `close_cert` result.
    std::atomic<bool> cert_throws {false};  ///< `close_cert` throws.
    std::atomic<int> cert_advance_ms {0};  ///< Fake time that `close_cert` takes.
    std::atomic<int> end_advance_ms {0};  ///< Fake time that `end_and_join` takes.
    std::atomic<bool> end_throws {false};  ///< `end_and_join` throws.
    std::atomic<int> end_runs {0};  ///< `end_and_join` calls.
    std::atomic<std::int64_t> end_done_ns {0};  ///< Real time at which `end_and_join` finished.
    gate_t end_gate;  ///< Blocks `end_and_join` when closed.
    gate_t cert_gate;  ///< Blocks `close_cert` when closed.
  };
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------------------------------------

TEST(HandoffResponderConstantsTest, LimitsAreAsDocumented) {
  EXPECT_EQ(8U, handoff::drain_responder_t::max_close_cert_threads) << "max_close_cert_threads must be 8.";
  EXPECT_EQ(15000ms, handoff::drain_responder_t::end_all_late_after) << "end_all_late_after must be 15 s.";
  EXPECT_EQ(500ms, handoff::drain_responder_t::close_cert_late_after) << "close_cert_late_after must be 500 ms.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Inline requests
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffResponderTest, NoThreadBeforeTheFirstEndAllOrCloseCert) {
  make_responder();
  EXPECT_EQ(0U, responder->live_handlers_for_test()) << "A new responder must have started no thread.";
  send_in(proto::sessions_request_t {1});
  send_in(proto::close_request_t {2, 5});
  EXPECT_EQ(0U, responder->live_handlers_for_test()) << "SESSIONS? and CLOSE are inline and must start no thread.";
}

TEST_F(HandoffResponderTest, SessionsRequestIsAnsweredInlineWithTheListedSessions) {
  make_responder();
  send_in(proto::sessions_request_t {41});
  auto reply = recv(0ms);
  ASSERT_TRUE(reply.has_value()) << "SESSIONS? must be answered before handle() returns (inline).";
  auto *typed = std::get_if<proto::sessions_reply_t>(&*reply);
  ASSERT_NE(nullptr, typed) << "The answer to SESSIONS? must be a sessions_reply_t.";
  EXPECT_EQ(41U, typed->request_id) << "The reply must echo the request id.";
  EXPECT_EQ(sessions, typed->sessions) << "The reply must carry exactly what list_sessions returned.";
  EXPECT_EQ(1, list_calls.load()) << "list_sessions must be called once per request.";
}

TEST_F(HandoffResponderTest, SessionsRequestWithNoSessionsGivesEmptyList) {
  make_responder();
  {
    std::lock_guard lock(state_mutex);
    sessions.clear();
  }
  send_in(proto::sessions_request_t {3});
  auto reply = recv(0ms);
  ASSERT_TRUE(reply.has_value()) << "SESSIONS? must be answered inline.";
  auto *typed = std::get_if<proto::sessions_reply_t>(&*reply);
  ASSERT_NE(nullptr, typed) << "The answer must be a sessions_reply_t.";
  EXPECT_TRUE(typed->sessions.empty()) << "An empty session list must be sent as an empty list.";
}

TEST_F(HandoffResponderTest, CloseIsAnsweredInlineForEveryOutcome) {
  make_responder();
  const std::vector<std::pair<handoff::close_outcome_e, std::string>> cases {
    {handoff::close_outcome_e::closed, "closed"},
    {handoff::close_outcome_e::already_stopping, "already-stopping"},
    {handoff::close_outcome_e::not_found, "not-found"},
  };
  std::uint64_t id = 100;
  for (const auto &[outcome, text] : cases) {
    close_result = outcome;
    send_in(proto::close_request_t {id, 77});
    auto reply = recv(0ms);
    ASSERT_TRUE(reply.has_value()) << "CLOSE must be answered inline (outcome " << text << ").";
    auto *typed = std::get_if<proto::close_reply_t>(&*reply);
    ASSERT_NE(nullptr, typed) << "The answer to CLOSE must be a close_reply_t.";
    EXPECT_EQ(id, typed->request_id) << "The reply must echo the request id.";
    EXPECT_EQ(text, typed->result) << "The result text must match the outcome.";
    ++id;
  }
  std::lock_guard lock(state_mutex);
  EXPECT_EQ((std::vector<std::uint32_t> {77, 77, 77}), closed_ids) << "close_session must get the requested session id.";
}

TEST_F(HandoffResponderTest, CloseOfTheLargestSessionIdIsPassedThrough) {
  make_responder();
  send_in(proto::close_request_t {1, UINT32_MAX});
  ASSERT_TRUE(recv(0ms).has_value()) << "CLOSE must be answered.";
  std::lock_guard lock(state_mutex);
  ASSERT_EQ(1U, closed_ids.size()) << "close_session must be called once.";
  EXPECT_EQ(UINT32_MAX, closed_ids[0]) << "The session id must not be altered.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Ignored messages, fd ownership, exceptions
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffResponderTest, OtherMessageTypesAreIgnored) {
  make_responder();
  send_in(proto::ping_t {1});
  send_in(proto::pong_t {1});
  send_in(proto::hello_t {});
  send_in(proto::ready_t {true, ""});
  send_in(proto::go_t {});
  send_in(proto::go_ack_t {true, ""});
  send_in(proto::commit_t {});
  send_in(proto::main_t {});
  send_in(proto::handoff_state_t {"draining"});
  send_in(proto::sessions_reply_t {1, {}});
  send_in(proto::close_reply_t {1, "closed"});
  send_in(proto::close_cert_reply_t {1, 2});
  send_in(proto::ended_t {1});
  send_in(proto::end_rejected_t {1});
  send_in(proto::unknown_t {"SOMETHING_NEW"});
  EXPECT_FALSE(recv(100ms).has_value()) << "Messages of other types must be ignored: no reply at all.";
  EXPECT_EQ(0U, responder->live_handlers_for_test()) << "Ignored messages must start no thread.";
  EXPECT_EQ(0, list_calls.load() + end_runs.load()) << "Ignored messages must call no operation.";
  EXPECT_EQ(0U, cert_call_count()) << "Ignored messages must call no operation.";
}

TEST_F(HandoffResponderTest, AttachedFdIsClosedForEveryMessageKind) {
  make_responder();
  end_gate.close();
  std::vector<proto::message_t> messages {
    proto::sessions_request_t {1},
    proto::close_request_t {2, 1},
    proto::close_cert_request_t {3, hash_of(1)},
    proto::close_cert_request_t {4, "not-a-hash"},
    proto::end_all_t {5},
    proto::ping_t {6},
    proto::unknown_t {"X"},
    proto::ended_t {7},
  };
  for (auto &message : messages) {
    pipe_probe_t probe;
    ASSERT_GE(probe.r, 0) << "pipe() must work.";
    const int fd = probe.r;
    send_in(message, fd);
    EXPECT_TRUE(probe.read_end_closed()) << "handle() must close an attached fd at once, for message type " << proto::type_name(message) << ".";
  }
}

TEST_F(HandoffResponderTest, AttachedFdIsClosedAfterStopToo) {
  make_responder();
  responder->stop();
  pipe_probe_t probe;
  ASSERT_GE(probe.r, 0) << "pipe() must work.";
  const int fd = probe.r;
  send_in(proto::sessions_request_t {1}, fd);
  EXPECT_TRUE(probe.read_end_closed()) << "After stop(), handle() must still close an attached fd.";
}

TEST_F(HandoffResponderTest, ListSessionsExceptionGivesNoReplyAndLogsError) {
  make_responder();
  list_throws = true;
  EXPECT_NO_THROW(send_in(proto::sessions_request_t {1})) << "handle() must never throw.";
  EXPECT_FALSE(recv(100ms).has_value()) << "An inline request whose operation threw gets no reply.";
  EXPECT_GE(capture->count(sev_error), 1U) << "The exception must be logged as an Error.";
  list_throws = false;
  send_in(proto::sessions_request_t {2});
  EXPECT_TRUE(recv(100ms).has_value()) << "The responder must keep working after an exception.";
}

TEST_F(HandoffResponderTest, CloseSessionExceptionGivesNoReplyAndLogsError) {
  make_responder();
  close_throws = true;
  EXPECT_NO_THROW(send_in(proto::close_request_t {1, 5})) << "handle() must never throw.";
  EXPECT_FALSE(recv(100ms).has_value()) << "CLOSE whose operation threw gets no reply.";
  EXPECT_GE(capture->count(sev_error), 1U) << "The exception must be logged as an Error.";
}

TEST_F(HandoffResponderTest, FailedSendIsLoggedAsWarningAndNeverThrows) {
  make_responder();
  peer->close();
  EXPECT_NO_THROW(send_in(proto::sessions_request_t {1})) << "A failed send must not make handle() throw.";
  EXPECT_NO_THROW(send_in(proto::close_request_t {2, 1})) << "A failed send must not make handle() throw.";
  EXPECT_GE(capture->count(sev_warning), 1U) << "A failed send must be logged as a Warning.";
}

TEST_F(HandoffResponderTest, EndAllWithFailedSendStillEndsAndMachineReachesEnded) {
  make_responder();
  peer->close();
  send_in(proto::end_all_t {1});
  EXPECT_TRUE(wait_state(handoff::end_state_e::ended, 1000ms)) << "The round must run to the end although the reply cannot be sent.";
  EXPECT_TRUE(wait_live(0, 1000ms)) << "The runner thread must finish after a failed send.";
  EXPECT_TRUE(capture->wait_count(sev_warning, 1, 500ms)) << "The failed send must be logged as a Warning.";
}

// ---------------------------------------------------------------------------------------------------------------------
// END_ALL
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffResponderTest, EndAllIsAnsweredEndedAndRunsEndAndJoinOnce) {
  make_responder();
  send_in(proto::end_all_t {9});
  auto reply = recv(1000ms);
  ASSERT_TRUE(reply.has_value()) << "END_ALL must be answered.";
  auto *typed = std::get_if<proto::ended_t>(&*reply);
  ASSERT_NE(nullptr, typed) << "The answer to a successful END_ALL must be ENDED.";
  EXPECT_EQ(9U, typed->request_id) << "ENDED must echo the request id.";
  EXPECT_EQ(1, end_runs.load()) << "end_and_join must run once.";
  EXPECT_EQ(handoff::end_state_e::ended, machine.state()) << "The machine must be in ended.";
}

TEST_F(HandoffResponderTest, EndAllRunsOffTheCallingThread) {
  make_responder();
  end_gate.close();
  const auto start = clock_type::now();
  send_in(proto::end_all_t {1});
  EXPECT_LT(clock_type::now() - start, 200ms) << "handle(END_ALL) must return without waiting for end_and_join.";
  EXPECT_TRUE(end_gate.wait_entered(1, 1000ms)) << "end_and_join must be started on a runner thread.";
  EXPECT_EQ(1U, responder->live_handlers_for_test()) << "The runner is one live handler thread.";
  end_gate.release();
  EXPECT_TRUE(recv(1000ms).has_value()) << "END_ALL must be answered once end_and_join finished.";
}

TEST_F(HandoffResponderTest, EndAllInRollingBackIsRejectedWithoutRunning) {
  make_responder();
  machine.decide_rollback();
  send_in(proto::end_all_t {4});
  auto reply = recv(1000ms);
  ASSERT_TRUE(reply.has_value()) << "END_ALL in rolling_back must be answered.";
  auto *typed = std::get_if<proto::end_rejected_t>(&*reply);
  ASSERT_NE(nullptr, typed) << "END_ALL in rolling_back must be answered END_REJECTED.";
  EXPECT_EQ(4U, typed->request_id) << "END_REJECTED must echo the request id.";
  EXPECT_EQ(0, end_runs.load()) << "end_and_join must not run after a rollback was decided.";
}

TEST_F(HandoffResponderTest, SecondEndAllAfterTheRoundIsAnsweredEndedWithoutRunningAgain) {
  make_responder();
  send_in(proto::end_all_t {1});
  ASSERT_TRUE(recv(1000ms).has_value()) << "The first END_ALL must be answered.";
  ASSERT_TRUE(wait_live(0, 1000ms)) << "The runner must finish.";
  send_in(proto::end_all_t {2});
  auto reply = recv(1000ms);
  ASSERT_TRUE(reply.has_value()) << "A later END_ALL must be answered at once from ended.";
  auto *typed = std::get_if<proto::ended_t>(&*reply);
  ASSERT_NE(nullptr, typed) << "A later END_ALL must be answered ENDED.";
  EXPECT_EQ(2U, typed->request_id) << "The reply must echo the new request id.";
  EXPECT_EQ(1, end_runs.load()) << "end_and_join must run only once.";
}

TEST_F(HandoffResponderTest, DuplicateEndAllsCostNoThreadAndAllGetTheSameAnswer) {
  make_responder();
  end_gate.close();
  send_in(proto::end_all_t {1});
  ASSERT_TRUE(end_gate.wait_entered(1, 1000ms)) << "The runner must enter end_and_join.";
  send_in(proto::end_all_t {2});
  send_in(proto::end_all_t {3});
  send_in(proto::end_all_t {4});
  EXPECT_EQ(1U, responder->live_handlers_for_test()) << "Duplicate END_ALLs must cost no thread: one runner only.";
  EXPECT_FALSE(recv(50ms).has_value()) << "No END_ALL may be answered before the round ends.";
  end_gate.release();
  auto replies = collect(4, 1000ms);
  ASSERT_EQ(4U, replies.size()) << "Every registered END_ALL must be answered.";
  std::set<std::uint64_t> ids;
  for (const auto &r : replies) {
    EXPECT_TRUE(std::holds_alternative<proto::ended_t>(r)) << "Every duplicate must get the same answer (ENDED).";
    ids.insert(id_of(r));
  }
  EXPECT_EQ((std::set<std::uint64_t> {1, 2, 3, 4}), ids) << "Each request id must be answered once.";
  EXPECT_EQ(1, end_runs.load()) << "end_and_join must run once for the whole round.";
}

TEST_F(HandoffResponderTest, DuplicatesGetEndRejectedWhenARollbackWasDecidedDuringTheRound) {
  make_responder();
  end_gate.close();
  send_in(proto::end_all_t {1});
  ASSERT_TRUE(end_gate.wait_entered(1, 1000ms)) << "The runner must enter end_and_join.";
  send_in(proto::end_all_t {2});
  machine.decide_rollback();
  end_gate.release();
  auto replies = collect(2, 1000ms);
  ASSERT_EQ(2U, replies.size()) << "Both requests must be answered.";
  for (const auto &r : replies) {
    EXPECT_TRUE(std::holds_alternative<proto::end_rejected_t>(r)) << "All requests of the round must get the answer that handle_end_all returned (END_REJECTED).";
  }
}

TEST_F(HandoffResponderTest, ARequestIdRegisteredTwiceGetsTwoReplies) {
  make_responder();
  end_gate.close();
  send_in(proto::end_all_t {7});
  ASSERT_TRUE(end_gate.wait_entered(1, 1000ms)) << "The runner must enter end_and_join.";
  send_in(proto::end_all_t {7});
  end_gate.release();
  auto replies = collect(2, 1000ms);
  ASSERT_EQ(2U, replies.size()) << "A request id registered twice must get two replies.";
  EXPECT_EQ(7U, id_of(replies[0])) << "Both replies echo the id.";
  EXPECT_EQ(7U, id_of(replies[1])) << "Both replies echo the id.";
}

TEST_F(HandoffResponderTest, EveryEndAllSentAsRoundsEndIsAnswered) {
  make_responder();
  constexpr std::uint64_t total = 100;
  std::map<std::uint64_t, int> seen;
  for (std::uint64_t id = 1; id <= total; ++id) {
    send_in(proto::end_all_t {id});
    while (auto r = recv(0ms)) {
      ++seen[id_of(*r)];
    }
  }
  const auto deadline = clock_type::now() + 1500ms;
  while (seen.size() < total && clock_type::now() < deadline) {
    if (auto r = recv(50ms)) {
      ++seen[id_of(*r)];
    }
  }
  for (std::uint64_t id = 1; id <= total; ++id) {
    EXPECT_GE(seen[id], 1) << "END_ALL with request id " << id << " was left unanswered (a waiter registered as a round ended must be answered by it or by a new round).";
  }
  EXPECT_EQ(1, end_runs.load()) << "end_and_join must run once; later rounds are answered from ended by the machine.";
}

TEST_F(HandoffResponderTest, EndAndJoinExceptionAnswersEveryRequestOfTheRoundEndRejected) {
  make_responder();
  end_gate.close();
  end_throws = true;
  send_in(proto::end_all_t {1});
  ASSERT_TRUE(end_gate.wait_entered(1, 1000ms)) << "The runner must enter end_and_join.";
  send_in(proto::end_all_t {2});
  end_gate.release();
  auto replies = collect(2, 1000ms);
  ASSERT_EQ(2U, replies.size()) << "Every request of a round whose end_and_join threw must be answered.";
  for (const auto &r : replies) {
    EXPECT_TRUE(std::holds_alternative<proto::end_rejected_t>(r)) << "An exception from end_and_join must give END_REJECTED.";
  }
  EXPECT_TRUE(capture->wait_count(sev_error, 1, 500ms)) << "The exception must be logged as an Error.";
  EXPECT_TRUE(wait_live(0, 1000ms)) << "The runner thread must finish after an exception.";
}

TEST_F(HandoffResponderTest, EndAllLateReplyLogsWarningButStillReplies) {
  make_responder();
  end_advance_ms = 16000;
  send_in(proto::end_all_t {1});
  auto reply = recv(1000ms);
  ASSERT_TRUE(reply.has_value()) << "A late round must still send its replies.";
  EXPECT_TRUE(std::holds_alternative<proto::ended_t>(*reply)) << "The late reply is still ENDED.";
  EXPECT_TRUE(capture->wait_count(sev_warning, 1, 1000ms)) << "A round that took longer than 15 s must log a Warning.";
}

TEST_F(HandoffResponderTest, EndAllWithinTheLimitLogsNoWarning) {
  make_responder();
  end_advance_ms = 14000;
  send_in(proto::end_all_t {1});
  ASSERT_TRUE(recv(1000ms).has_value()) << "END_ALL must be answered.";
  ASSERT_TRUE(wait_live(0, 1000ms)) << "The runner must finish.";
  EXPECT_EQ(0U, capture->count(sev_warning)) << "A round shorter than 15 s must log no Warning.";
}

// ---------------------------------------------------------------------------------------------------------------------
// CLOSE_CERT
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffResponderTest, CloseCertCallsTheOperationAndRepliesWithTheCount) {
  make_responder();
  cert_count = 5;
  send_in(proto::close_cert_request_t {21, hash_of(3)});
  auto reply = recv(1000ms);
  ASSERT_TRUE(reply.has_value()) << "CLOSE_CERT with a valid hash must be answered.";
  auto *typed = std::get_if<proto::close_cert_reply_t>(&*reply);
  ASSERT_NE(nullptr, typed) << "The answer must be a close_cert_reply_t.";
  EXPECT_EQ(21U, typed->request_id) << "The reply must echo the request id.";
  EXPECT_EQ(5U, typed->count) << "The reply must carry the count that close_cert returned.";
  std::lock_guard lock(state_mutex);
  ASSERT_EQ(1U, cert_hashes.size()) << "close_cert must be called once.";
  EXPECT_EQ(hash_of(3), cert_hashes[0]) << "close_cert must get the requested hash.";
}

TEST_F(HandoffResponderTest, CloseCertRunsOffTheCallingThread) {
  make_responder();
  cert_gate.close();
  const auto start = clock_type::now();
  send_in(proto::close_cert_request_t {1, hash_of(1)});
  EXPECT_LT(clock_type::now() - start, 200ms) << "handle(CLOSE_CERT) must return without waiting for close_cert.";
  EXPECT_TRUE(cert_gate.wait_entered(1, 1000ms)) << "close_cert must be started on a handler thread.";
  cert_gate.release();
  EXPECT_TRUE(recv(1000ms).has_value()) << "CLOSE_CERT must be answered.";
}

TEST_F(HandoffResponderTest, CloseCertWithAMalformedHashIsRepliedZeroInlineAndCallsNothing) {
  make_responder();
  const std::vector<std::string> bad {
    "",
    "abc",
    std::string(63, 'a'),
    std::string(65, 'a'),
    std::string(64, 'A'),
    std::string(63, 'a') + "g",
    std::string(63, 'a') + " ",
    std::string(63, 'a') + "-",
  };
  std::uint64_t id = 1;
  for (const auto &hash : bad) {
    const auto warnings = capture->count(sev_warning);
    send_in(proto::close_cert_request_t {id, hash});
    auto reply = recv(0ms);
    ASSERT_TRUE(reply.has_value()) << "A malformed hash (length " << hash.size() << ") must be answered inline.";
    auto *typed = std::get_if<proto::close_cert_reply_t>(&*reply);
    ASSERT_NE(nullptr, typed) << "The answer must be a close_cert_reply_t.";
    EXPECT_EQ(id, typed->request_id) << "The reply must echo the request id.";
    EXPECT_EQ(0U, typed->count) << "A malformed hash must be answered with count 0.";
    EXPECT_GT(capture->count(sev_warning), warnings) << "A malformed hash must be logged as a Warning.";
    ++id;
  }
  EXPECT_EQ(0U, cert_call_count()) << "close_cert must not be called for a malformed hash.";
  EXPECT_EQ(0U, responder->live_handlers_for_test()) << "A malformed hash must start no thread.";
}

TEST_F(HandoffResponderTest, CloseCertExceptionGivesNoReplyAndLogsError) {
  make_responder();
  cert_throws = true;
  send_in(proto::close_cert_request_t {1, hash_of(1)});
  EXPECT_FALSE(recv(200ms).has_value()) << "An exception from close_cert must give no reply.";
  EXPECT_TRUE(capture->wait_count(sev_error, 1, 500ms)) << "The exception must be logged as an Error.";
  EXPECT_TRUE(wait_live(0, 1000ms)) << "The handler thread must finish after an exception.";
  cert_throws = false;
  send_in(proto::close_cert_request_t {2, hash_of(2)});
  auto reply = recv(1000ms);
  ASSERT_TRUE(reply.has_value()) << "The responder must keep serving CLOSE_CERT after an exception.";
  EXPECT_EQ(2U, id_of(*reply)) << "The later request must be answered.";
}

TEST_F(HandoffResponderTest, CloseCertCapOfEightDropsTheNinthWithAWarning) {
  make_responder();
  cert_gate.close();
  for (int i = 1; i <= 8; ++i) {
    send_in(proto::close_cert_request_t {static_cast<std::uint64_t>(i), hash_of(i)});
  }
  ASSERT_TRUE(cert_gate.wait_entered(8, 1000ms)) << "Eight CLOSE_CERT handlers must run at once.";
  EXPECT_EQ(8U, responder->live_handlers_for_test()) << "Eight CLOSE_CERT handler threads are alive.";
  const auto warnings = capture->count(sev_warning);
  send_in(proto::close_cert_request_t {9, hash_of(9)});
  EXPECT_GT(capture->count(sev_warning), warnings) << "The ninth CLOSE_CERT must be dropped with a Warning.";
  EXPECT_EQ(8U, responder->live_handlers_for_test()) << "The ninth CLOSE_CERT must start no thread.";
  std::this_thread::sleep_for(30ms);
  EXPECT_EQ(8, cert_gate.entered_now()) << "close_cert must not be called for the dropped request.";
  cert_gate.release();
  auto replies = collect(9, 400ms);
  std::set<std::uint64_t> ids;
  for (const auto &r : replies) {
    ids.insert(id_of(r));
  }
  EXPECT_EQ((std::set<std::uint64_t> {1, 2, 3, 4, 5, 6, 7, 8}), ids) << "The first eight are answered; the dropped ninth gets no reply.";
}

TEST_F(HandoffResponderTest, FinishedCloseCertThreadsDoNotCountAgainstTheCap) {
  make_responder();
  for (int i = 1; i <= 8; ++i) {
    send_in(proto::close_cert_request_t {static_cast<std::uint64_t>(i), hash_of(i)});
  }
  ASSERT_EQ(8U, collect(8, 1000ms).size()) << "All eight requests must be answered.";
  ASSERT_TRUE(wait_live(0, 1000ms)) << "All handlers must finish.";
  cert_gate.close();
  for (int i = 11; i <= 18; ++i) {
    send_in(proto::close_cert_request_t {static_cast<std::uint64_t>(i), hash_of(i)});
  }
  EXPECT_TRUE(cert_gate.wait_entered(8, 1000ms)) << "Eight new handlers must be accepted after the earlier ones finished.";
  cert_gate.release();
  EXPECT_EQ(8U, collect(8, 1000ms).size()) << "All eight new requests must be answered.";
}

TEST_F(HandoffResponderTest, EndAllIsAnsweredWithin100msOfTheMachineWhileCloseCertHandlersAreBlocked) {
  make_responder();
  cert_gate.close();
  end_gate.close();
  for (int i = 1; i <= 8; ++i) {
    send_in(proto::close_cert_request_t {static_cast<std::uint64_t>(i), hash_of(i)});
  }
  ASSERT_TRUE(cert_gate.wait_entered(8, 1000ms)) << "Eight CLOSE_CERT handlers must be blocked.";
  send_in(proto::end_all_t {100});
  ASSERT_TRUE(end_gate.wait_entered(1, 1000ms)) << "The END_ALL runner must start although eight CLOSE_CERT handlers are blocked.";
  EXPECT_EQ(9U, responder->live_handlers_for_test()) << "The END_ALL runner is not counted against the CLOSE_CERT cap.";
  send_in(proto::end_all_t {101});
  end_gate.release();
  auto reply = recv(1000ms);
  const auto received_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count();
  ASSERT_TRUE(reply.has_value()) << "END_ALL must be answered while CLOSE_CERT handlers are blocked.";
  EXPECT_TRUE(std::holds_alternative<proto::ended_t>(*reply)) << "The answer must be ENDED.";
  EXPECT_LE(received_ns - end_done_ns.load(), 100'000'000) << "END_ALL must be answered within 100 ms of the machine's answer.";
  auto second = recv(100ms);
  ASSERT_TRUE(second.has_value()) << "The duplicate must be answered at the same moment.";
  EXPECT_EQ((std::set<std::uint64_t> {100, 101}), (std::set<std::uint64_t> {id_of(*reply), id_of(*second)})) << "Both END_ALLs must be answered.";
  cert_gate.release();
  EXPECT_EQ(8U, collect(8, 1000ms).size()) << "The CLOSE_CERT requests are answered once released.";
}

TEST_F(HandoffResponderTest, CloseCertLateReplyLogsWarningButStillReplies) {
  make_responder();
  cert_advance_ms = 600;
  send_in(proto::close_cert_request_t {1, hash_of(1)});
  ASSERT_TRUE(recv(1000ms).has_value()) << "A late CLOSE_CERT must still be answered.";
  EXPECT_TRUE(capture->wait_count(sev_warning, 1, 1000ms)) << "A CLOSE_CERT that took longer than 500 ms must log a Warning.";
}

TEST_F(HandoffResponderTest, CloseCertWithinTheLimitLogsNoWarning) {
  make_responder();
  cert_advance_ms = 400;
  send_in(proto::close_cert_request_t {1, hash_of(1)});
  ASSERT_TRUE(recv(1000ms).has_value()) << "CLOSE_CERT must be answered.";
  ASSERT_TRUE(wait_live(0, 1000ms)) << "The handler must finish.";
  EXPECT_EQ(0U, capture->count(sev_warning)) << "A CLOSE_CERT shorter than 500 ms must log no Warning.";
}

// ---------------------------------------------------------------------------------------------------------------------
// Independence of the request kinds
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffResponderTest, SessionsAndCloseStayAnswerableWhileEndAllIsBlocked) {
  make_responder();
  end_gate.close();
  send_in(proto::end_all_t {1});
  ASSERT_TRUE(end_gate.wait_entered(1, 1000ms)) << "The runner must be inside end_and_join.";
  const auto start = clock_type::now();
  send_in(proto::sessions_request_t {2});
  send_in(proto::close_request_t {3, 5});
  EXPECT_LT(clock_type::now() - start, 200ms) << "handle() must not block on a running END_ALL.";
  auto first = recv(0ms);
  auto second = recv(0ms);
  ASSERT_TRUE(first.has_value() && second.has_value()) << "SESSIONS? and CLOSE must be answered inline while END_ALL waits.";
  EXPECT_TRUE(std::holds_alternative<proto::sessions_reply_t>(*first)) << "First reply is the sessions list.";
  EXPECT_TRUE(std::holds_alternative<proto::close_reply_t>(*second)) << "Second reply is the CLOSE result.";
  end_gate.release();
  EXPECT_TRUE(recv(1000ms).has_value()) << "END_ALL is answered after release.";
}

TEST_F(HandoffResponderTest, SessionsAndCloseStayAnswerableWhileCloseCertIsBlocked) {
  make_responder();
  cert_gate.close();
  send_in(proto::close_cert_request_t {1, hash_of(1)});
  ASSERT_TRUE(cert_gate.wait_entered(1, 1000ms)) << "close_cert must be blocked.";
  send_in(proto::sessions_request_t {2});
  send_in(proto::close_request_t {3, 5});
  auto first = recv(0ms);
  auto second = recv(0ms);
  ASSERT_TRUE(first.has_value() && second.has_value()) << "SESSIONS? and CLOSE must be answered inline while CLOSE_CERT waits.";
  EXPECT_TRUE(std::holds_alternative<proto::sessions_reply_t>(*first)) << "First reply is the sessions list.";
  EXPECT_TRUE(std::holds_alternative<proto::close_reply_t>(*second)) << "Second reply is the CLOSE result.";
  cert_gate.release();
  EXPECT_TRUE(recv(1000ms).has_value()) << "CLOSE_CERT is answered after release.";
}

TEST_F(HandoffResponderTest, SessionsStayAnswerableWhileBothEndAllAndCloseCertAreBlocked) {
  make_responder();
  end_gate.close();
  cert_gate.close();
  send_in(proto::end_all_t {1});
  send_in(proto::close_cert_request_t {2, hash_of(2)});
  ASSERT_TRUE(end_gate.wait_entered(1, 1000ms) && cert_gate.wait_entered(1, 1000ms)) << "Both handlers must be blocked.";
  send_in(proto::sessions_request_t {3});
  auto reply = recv(0ms);
  ASSERT_TRUE(reply.has_value()) << "SESSIONS? must be answered while END_ALL and CLOSE_CERT are both blocked.";
  EXPECT_EQ(3U, id_of(*reply)) << "The reply must echo the id.";
}

TEST_F(HandoffResponderTest, CloseCertDuringEndAllAreBothAnswered) {
  make_responder();
  cert_count = 0;
  end_gate.close();
  send_in(proto::end_all_t {1});
  ASSERT_TRUE(end_gate.wait_entered(1, 1000ms)) << "The runner must be inside end_and_join.";
  send_in(proto::close_cert_request_t {2, hash_of(2)});
  auto cert_reply = recv(1000ms);
  ASSERT_TRUE(cert_reply.has_value()) << "CLOSE_CERT must be answered while END_ALL runs (independent handler threads).";
  auto *typed = std::get_if<proto::close_cert_reply_t>(&*cert_reply);
  ASSERT_NE(nullptr, typed) << "The first reply must be the CLOSE_CERT reply.";
  EXPECT_EQ(0U, typed->count) << "The count is whatever close_cert returned.";
  end_gate.release();
  auto end_reply = recv(1000ms);
  ASSERT_TRUE(end_reply.has_value()) << "END_ALL must be answered too.";
  EXPECT_TRUE(std::holds_alternative<proto::ended_t>(*end_reply)) << "END_ALL is answered ENDED.";
}

// ---------------------------------------------------------------------------------------------------------------------
// stop()
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(HandoffResponderTest, StopWithoutHandlersReturnsAndIsIdempotent) {
  make_responder();
  responder->stop();
  responder->stop();
  EXPECT_EQ(0U, responder->live_handlers_for_test()) << "No handler may live after stop().";
}

TEST_F(HandoffResponderTest, StopWaitsForARunningEndAllAndSkipsItsReply) {
  make_responder();
  end_gate.close();
  send_in(proto::end_all_t {1});
  ASSERT_TRUE(end_gate.wait_entered(1, 1000ms)) << "The runner must be inside end_and_join.";
  std::atomic<bool> stopped {false};
  std::thread stopper([&] {
    responder->stop();
    stopped = true;
  });
  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(stopped.load()) << "stop() must wait for a handler that is inside end_and_join.";
  end_gate.release();
  stopper.join();
  EXPECT_TRUE(stopped.load()) << "stop() must return once the handler finished.";
  EXPECT_EQ(handoff::end_state_e::ended, machine.state()) << "The work must not be left half done: the machine reaches ended.";
  EXPECT_EQ(0U, responder->live_handlers_for_test()) << "stop() must have joined every handler.";
  EXPECT_FALSE(recv(100ms).has_value()) << "After stop() has begun, running handlers skip their reply.";
}

TEST_F(HandoffResponderTest, StopWaitsForARunningCloseCertAndSkipsItsReply) {
  make_responder();
  cert_gate.close();
  send_in(proto::close_cert_request_t {1, hash_of(1)});
  ASSERT_TRUE(cert_gate.wait_entered(1, 1000ms)) << "close_cert must be blocked.";
  std::atomic<bool> stopped {false};
  std::thread stopper([&] {
    responder->stop();
    stopped = true;
  });
  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(stopped.load()) << "stop() must wait for a running close_cert.";
  cert_gate.release();
  stopper.join();
  EXPECT_FALSE(recv(100ms).has_value()) << "After stop() has begun, the CLOSE_CERT reply is skipped.";
}

TEST_F(HandoffResponderTest, ConcurrentStopCallsAllReturnOnlyAfterTheJoin) {
  make_responder();
  end_gate.close();
  send_in(proto::end_all_t {1});
  ASSERT_TRUE(end_gate.wait_entered(1, 1000ms)) << "The runner must be inside end_and_join.";
  std::atomic<int> returned {0};
  std::vector<std::thread> stoppers;
  for (int i = 0; i < 3; ++i) {
    stoppers.emplace_back([&] {
      responder->stop();
      ++returned;
    });
  }
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(0, returned.load()) << "A second or third concurrent stop() must also wait for the joins, not return early.";
  end_gate.release();
  for (auto &t : stoppers) {
    t.join();
  }
  EXPECT_EQ(3, returned.load()) << "All concurrent stop() calls must return.";
  EXPECT_EQ(0U, responder->live_handlers_for_test()) << "Everything must be joined.";
}

TEST_F(HandoffResponderTest, HandleAfterStopIgnoresEveryMessageAndSendsNothing) {
  make_responder();
  responder->stop();
  EXPECT_NO_THROW(send_in(proto::sessions_request_t {1})) << "handle() after stop() must not throw.";
  EXPECT_NO_THROW(send_in(proto::close_request_t {2, 5}));
  EXPECT_NO_THROW(send_in(proto::close_cert_request_t {3, hash_of(3)}));
  EXPECT_NO_THROW(send_in(proto::close_cert_request_t {4, "bad"}));
  EXPECT_NO_THROW(send_in(proto::end_all_t {5}));
  EXPECT_FALSE(recv(100ms).has_value()) << "After stop() nothing may be sent.";
  EXPECT_EQ(0, list_calls.load() + end_runs.load()) << "After stop() no operation may be called.";
  EXPECT_EQ(0U, cert_call_count()) << "After stop() no operation may be called.";
  EXPECT_EQ(0U, responder->live_handlers_for_test()) << "After stop() no thread may start.";
}

TEST_F(HandoffResponderTest, DestructorJoinsARunningHandler) {
  make_responder();
  end_gate.close();
  send_in(proto::end_all_t {1});
  ASSERT_TRUE(end_gate.wait_entered(1, 1000ms)) << "The runner must be inside end_and_join.";
  std::thread releaser([&] {
    std::this_thread::sleep_for(100ms);
    end_gate.release();
  });
  responder.reset();
  releaser.join();
  EXPECT_EQ(handoff::end_state_e::ended, machine.state()) << "The destructor must wait for the round to finish.";
}

#endif
