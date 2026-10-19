/**
 * @file tests/unit/handoff/test_new_side.cpp
 * @brief Tests for the new side of the drain handoff (src/handoff/new_side.h).
 *
 * Written from `docs/drain-handoff.md` (steps 3 and 5, "Stopping, restarting and quitting") and the header only. A
 * real channel pair connects `run_new_side()` to a scripted peer that plays the old process; `new_side_ops_t` is a
 * recording fake.
 */
#ifdef __linux__

  // test includes
  #include "../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <future>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <thread>
  #include <vector>

  // platform includes
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <unistd.h>

  // local includes
  #include "src/handoff/new_side.h"

using namespace std::chrono_literals;

namespace {
  using handoff::new_outcome_e;
  using handoff::recv_status_e;
  namespace protocol = handoff::protocol;

  /**
   * @brief Thread-safe ordered record of every fake call.
   */
  class call_log_t {
  public:
    /**
     * @brief Append an entry.
     *
     * @param entry Entry text.
     */
    void add(std::string entry) {
      std::lock_guard lock {_mutex};
      _entries.push_back(std::move(entry));
    }

    /**
     * @brief Copy of all entries.
     *
     * @return Entries in call order.
     */
    std::vector<std::string> all() const {
      std::lock_guard lock {_mutex};
      return _entries;
    }

  private:
    mutable std::mutex _mutex;  ///< Guards the entries.
    std::vector<std::string> _entries;  ///< Entries.
  };

  /**
   * @brief Number of entries equal to `name`.
   *
   * @param v Entries.
   * @param name Entry to count.
   * @return Count.
   */
  int count_of(const std::vector<std::string> &v, const std::string &name) {
    return static_cast<int>(std::count(v.begin(), v.end(), name));
  }

  /// Effects that may only happen on `COMMIT`.
  const std::vector<std::string> commit_effects = {"install_adoption", "resume_accept", "publish_mdns", "map_upnp", "start_tray", "become_active"};

  /**
   * @brief Recording fake of the new side's effects.
   */
  class fake_new_ops_t: public handoff::new_side_ops_t {
  public:
    /**
     * @brief Create the fake.
     *
     * @param log Shared call log.
     */
    explicit fake_new_ops_t(call_log_t &log):
        log(log) {
    }

    /**
     * @brief Close the pidfd the fake owns.
     */
    ~fake_new_ops_t() override {
      if (old_fd >= 0) {
        ::close(old_fd);
      }
    }

    call_log_t &log;  ///< Call log.
    protocol::hello_t hello_value;  ///< Returned by `hello()`.
    std::string validate_reason;  ///< Returned by `validate_listeners()`.
    std::string start_reason;  ///< Returned by `start_servers_paused()`.
    std::string reload_reason;  ///< Returned by `reload_all()`.
    std::string prepare_reason;  ///< Returned by `prepare_adoption()`.
    protocol::hello_t seen_old_hello;  ///< `HELLO` passed to `accept_old_hello`.
    int old_fd {-1};  ///< Fd passed to `accept_old_hello` (owned by the fake).
    bool old_fd_valid {false};  ///< Whether the fd was valid inside `accept_old_hello`.
    protocol::go_t seen_go;  ///< `GO` passed to `prepare_adoption`.

    protocol::hello_t hello() override {
      log.add("hello");
      return hello_value;
    }

    void accept_old_hello(const protocol::hello_t &hello, int old_pidfd) override {
      log.add("accept_old_hello");
      seen_old_hello = hello;
      old_fd = old_pidfd;
      old_fd_valid = old_pidfd >= 0 && ::fcntl(old_pidfd, F_GETFD) != -1;
    }

    std::string validate_listeners() override {
      log.add("validate_listeners");
      return validate_reason;
    }

    std::string start_servers_paused() override {
      log.add("start_servers_paused");
      return start_reason;
    }

    std::string reload_all() override {
      log.add("reload_all");
      return reload_reason;
    }

    std::string prepare_adoption(const protocol::go_t &go) override {
      log.add("prepare_adoption");
      seen_go = go;
      return prepare_reason;
    }

    void install_adoption() override {
      log.add("install_adoption");
    }

    void resume_accept() override {
      log.add("resume_accept");
    }

    void publish_mdns() override {
      log.add("publish_mdns");
    }

    void map_upnp() override {
      log.add("map_upnp");
    }

    void start_tray() override {
      log.add("start_tray");
    }

    void become_active() override {
      log.add("become_active");
    }
  };

  /**
   * @brief The scripted old process: the other end of the channel.
   */
  struct peer_t {
    /**
     * @brief Take the peer end.
     *
     * @param fd Fd of the peer end.
     */
    explicit peer_t(int fd):
        channel(fd) {
    }

    handoff::channel_t channel;  ///< Peer end.

    /**
     * @brief Send a message.
     *
     * @param message Message.
     * @param fd Fd to attach, or -1.
     * @return `true` when sent.
     */
    bool send(const protocol::message_t &message, int fd = -1) {
      return channel.send(message, fd);
    }

    /**
     * @brief Send a `HELLO` with a freshly opened fd attached, as the old process does.
     *
     * @param major Major version.
     * @return `true` when sent.
     */
    bool send_old_hello(std::uint32_t major = protocol::version_major) {
      protocol::hello_t hello;
      hello.major = major;
      hello.capabilities = {"cap-a"};
      hello.probe = protocol::probe_results_t {2, 1, true};
      hello.identity = protocol::probe_identity_t {"build", "/dev/dri/renderD128", 4098, 5, "drv"};
      hello.cooldown_deadline_ns = 1234567;
      const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
      const bool ok = fd >= 0 && channel.send(hello, fd);
      if (fd >= 0) {
        ::close(fd);
      }
      return ok;
    }

    /**
     * @brief Wait for the next message of type `T`, tolerating the new side's own `HELLO` when `T` is another type.
     *
     * @tparam T Wanted message type.
     * @param limit Longest wait.
     * @return The message, or nothing (a failure is recorded).
     */
    template<typename T>
    std::optional<T> expect(std::chrono::milliseconds limit = 800ms) {
      const std::string wanted = protocol::type_name(protocol::message_t {T {}});
      const auto deadline = std::chrono::steady_clock::now() + limit;
      for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          ADD_FAILURE() << "peer: timed out waiting for " << wanted << " from the new side";
          return std::nullopt;
        }
        handoff::received_t r;
        const auto status = channel.receive(r, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        if (status == recv_status_e::timeout) {
          continue;
        }
        if (status != recv_status_e::ok) {
          ADD_FAILURE() << "peer: receive status " << static_cast<int>(status) << " while waiting for " << wanted << " (the new side closed the channel?)";
          return std::nullopt;
        }
        if (r.fd >= 0) {
          ::close(r.fd);
        }
        if (const auto *m = std::get_if<T>(&r.message)) {
          return *m;
        }
        if (std::holds_alternative<protocol::hello_t>(r.message)) {
          continue;
        }
        ADD_FAILURE() << "peer: got " << protocol::type_name(r.message) << " while waiting for " << wanted;
        return std::nullopt;
      }
    }

    /**
     * @brief Read until the channel ends or time runs out and report whether anything arrived.
     *
     * @param total Longest time.
     * @return Type names of the messages received.
     */
    std::vector<std::string> collect(std::chrono::milliseconds total) {
      std::vector<std::string> names;
      const auto deadline = std::chrono::steady_clock::now() + total;
      for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          return names;
        }
        handoff::received_t r;
        const auto status = channel.receive(r, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        if (status == recv_status_e::timeout) {
          continue;
        }
        if (status != recv_status_e::ok) {
          return names;
        }
        if (r.fd >= 0) {
          ::close(r.fd);
        }
        names.push_back(protocol::type_name(r.message));
      }
    }

    /**
     * @brief Send raw bytes as one datagram, bypassing the encoder.
     *
     * @param bytes Payload.
     * @return `true` when the whole datagram was sent.
     */
    bool send_raw(const std::string &bytes) {
      return ::send(channel.fd(), bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size());
    }
  };

  /**
   * @brief Build a `GO` with a running app.
   *
   * @return Message.
   */
  protocol::go_t make_go() {
    protocol::go_t go;
    protocol::app_snapshot_t app;
    app.app_id = 3;
    app.name = "Desktop";
    app.applied_prep = 2;
    app.digest = std::string(64, 'c');
    app.launch_env = {{"SUNSHINE_CLIENT_ID", "9"}};
    go.app = app;
    go.host_audio = true;
    go.next_launch_session_id = 77;
    return go;
  }

  /**
   * @brief Fixture: `run_new_side()` on its own thread, a fake and a scripted peer.
   */
  class HandoffNewSideTest: public testing::Test {
  protected:
    void SetUp() override {
      const auto pair = handoff::make_channel_pair();
      ASSERT_TRUE(pair.has_value()) << "make_channel_pair failed";
      channel = std::make_unique<handoff::channel_t>(pair->first);
      peer = std::make_unique<peer_t>(pair->second);
      ops.hello_value.capabilities = {"cap-new"};
      timeouts.hello = 300ms;
      timeouts.go = 300ms;
      timeouts.commit = 300ms;
    }

    void TearDown() override {
      if (worker.joinable()) {
        peer->channel.close();
        worker.join();
      }
    }

    /**
     * @brief Start `run_new_side()` on its own thread.
     */
    void start() {
      future = promise.get_future();
      worker = std::thread([this]() {
        const auto outcome = handoff::run_new_side(ops, *channel, timeouts);
        end_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        promise.set_value(outcome);
      });
    }

    /**
     * @brief Wait for `run_new_side()` to return.
     *
     * @param limit Longest wait.
     * @return The outcome, or nothing when it did not return in time.
     */
    std::optional<new_outcome_e> finish(std::chrono::milliseconds limit = 1500ms) {
      if (!worker.joinable()) {
        return std::nullopt;
      }
      if (future.wait_for(limit) != std::future_status::ready) {
        ADD_FAILURE() << "run_new_side() did not return within " << limit.count() << " ms";
        peer->channel.close();
        future.wait_for(1000ms);
      }
      worker.join();
      return future.get();
    }

    /**
     * @brief Peer: send the old `HELLO` and read the new side's `HELLO` and `READY`.
     *
     * @return The `READY`, or nothing.
     */
    std::optional<protocol::ready_t> to_ready() {
      if (!peer->send_old_hello()) {
        ADD_FAILURE() << "peer could not send HELLO";
        return std::nullopt;
      }
      return peer->expect<protocol::ready_t>();
    }

    /**
     * @brief Peer: up to and including `GO_ACK`.
     *
     * @return The `GO_ACK`, or nothing.
     */
    std::optional<protocol::go_ack_t> to_go_ack() {
      const auto ready = to_ready();
      if (!ready || !ready->ok) {
        ADD_FAILURE() << "READY was not ok";
        return std::nullopt;
      }
      if (!peer->send(make_go())) {
        return std::nullopt;
      }
      return peer->expect<protocol::go_ack_t>();
    }

    /**
     * @brief Check that none of the `COMMIT` effects has happened.
     *
     * @param when What the check is about.
     */
    void expect_no_commit_effects(const std::string &when) {
      const auto v = log.all();
      for (const auto &effect : commit_effects) {
        EXPECT_EQ(0, count_of(v, effect)) << effect << " must not be called " << when << " (before COMMIT nothing is adopted, accepted, published, mapped or shown)";
      }
    }

    call_log_t log;  ///< Call log.
    fake_new_ops_t ops {log};  ///< Fake effects.
    handoff::new_side_timeouts_t timeouts;  ///< Timeouts.
    std::unique_ptr<handoff::channel_t> channel;  ///< New side's end.
    std::unique_ptr<peer_t> peer;  ///< Scripted old process.
    std::thread worker;  ///< Thread running `run_new_side()`.
    std::promise<new_outcome_e> promise;  ///< Outcome promise.
    std::future<new_outcome_e> future;  ///< Outcome future.
    std::atomic<long long> end_ns {0};  ///< Steady-clock time at which `run_new_side()` returned.
  };
}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewSideTest, HappyPathCallOrder) {
  start();
  const auto ready = to_ready();
  ASSERT_TRUE(ready.has_value());
  EXPECT_TRUE(ready->ok) << "READY must be ok when validation and start succeed";
  {
    const auto v = log.all();
    EXPECT_EQ(1, count_of(v, "accept_old_hello"));
    EXPECT_EQ(1, count_of(v, "validate_listeners"));
    EXPECT_EQ(1, count_of(v, "start_servers_paused"));
    expect_no_commit_effects("before GO");
    EXPECT_EQ(0, count_of(v, "reload_all")) << "reload_all() only happens on GO";
  }
  const auto go = make_go();
  ASSERT_TRUE(peer->send(go));
  const auto ack = peer->expect<protocol::go_ack_t>();
  ASSERT_TRUE(ack.has_value());
  EXPECT_TRUE(ack->ok) << "GO_ACK must be ok when the reload and the adoption check succeed: " << ack->reason;
  expect_no_commit_effects("when GO_ACK is sent");
  ASSERT_TRUE(peer->send(protocol::commit_t {}));
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::committed, *outcome);

  std::vector<std::string> filtered;
  for (const auto &e : log.all()) {
    if (e != "hello") {
      filtered.push_back(e);
    }
  }
  const std::vector<std::string> expected = {
    "accept_old_hello",
    "validate_listeners",
    "start_servers_paused",
    "reload_all",
    "prepare_adoption",
    "install_adoption",
    "resume_accept",
    "publish_mdns",
    "map_upnp",
    "start_tray",
    "become_active",
  };
  EXPECT_EQ(expected, filtered) << "the new side must call its ops in exactly this order: HELLO handling, validate, start paused, (READY), reload, prepare, (GO_ACK), then the COMMIT effects";
  EXPECT_EQ(go, ops.seen_go) << "prepare_adoption() must get the GO as received";
}

TEST_F(HandoffNewSideTest, OldHelloAndPidfdReachAcceptOldHello) {
  start();
  ASSERT_TRUE(to_go_ack().has_value());
  ASSERT_TRUE(peer->send(protocol::commit_t {}));
  ASSERT_TRUE(finish().has_value());
  EXPECT_TRUE(ops.old_fd_valid) << "accept_old_hello() must get a valid pidfd (the fd sent with HELLO)";
  EXPECT_EQ("cap-a", ops.seen_old_hello.capabilities.empty() ? "" : ops.seen_old_hello.capabilities[0]);
  ASSERT_TRUE(ops.seen_old_hello.probe.has_value()) << "the old HELLO's probe results must reach accept_old_hello()";
  EXPECT_EQ((protocol::probe_results_t {2, 1, true}), *ops.seen_old_hello.probe);
  ASSERT_TRUE(ops.seen_old_hello.identity.has_value());
  EXPECT_EQ("build", ops.seen_old_hello.identity->build_id);
  ASSERT_TRUE(ops.seen_old_hello.cooldown_deadline_ns.has_value());
  EXPECT_EQ(1234567, *ops.seen_old_hello.cooldown_deadline_ns);
}

TEST_F(HandoffNewSideTest, SendsOwnHello) {
  start();
  ASSERT_TRUE(peer->send_old_hello());
  const auto hello = peer->expect<protocol::hello_t>();
  ASSERT_TRUE(hello.has_value()) << "the new side must answer the old HELLO with its own HELLO";
  EXPECT_EQ(ops.hello_value, *hello) << "HELLO must be the value of new_side_ops_t::hello()";
  peer->channel.close();
  finish();
}

TEST_F(HandoffNewSideTest, MinorVersionDifferenceIsAccepted) {
  start();
  protocol::hello_t hello;
  hello.minor = protocol::version_minor + 3;
  hello.capabilities = {"unknown-capability"};
  const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(peer->send(hello, fd));
  ::close(fd);
  const auto ready = peer->expect<protocol::ready_t>();
  ASSERT_TRUE(ready.has_value());
  EXPECT_TRUE(ready->ok) << "only a different major version fails the start";
  peer->channel.close();
  finish();
}

// ---------------------------------------------------------------------------------------------------------------
// Failures before COMMIT: exit_failure and no COMMIT effect
// ---------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewSideTest, OldHelloWithoutPidfdIsFailedReady) {
  start();
  protocol::hello_t hello;
  ASSERT_TRUE(peer->send(hello));  // no fd attached
  const auto ready = peer->expect<protocol::ready_t>();
  ASSERT_TRUE(ready.has_value()) << "a HELLO without a pidfd must be answered with a failed READY";
  EXPECT_FALSE(ready->ok);
  EXPECT_FALSE(ready->reason.empty()) << "the failed READY needs a reason";
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  expect_no_commit_effects("after a HELLO without pidfd");
  EXPECT_EQ(0, count_of(log.all(), "start_servers_paused")) << "no server start after a rejected HELLO";
  EXPECT_EQ(0, count_of(log.all(), "reload_all"));
}

TEST_F(HandoffNewSideTest, MajorVersionMismatchIsFailedReady) {
  start();
  ASSERT_TRUE(peer->send_old_hello(protocol::version_major + 1));
  const auto ready = peer->expect<protocol::ready_t>();
  ASSERT_TRUE(ready.has_value()) << "an unknown major version must be answered with a failed READY";
  EXPECT_FALSE(ready->ok);
  EXPECT_FALSE(ready->reason.empty());
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  expect_no_commit_effects("after a major version mismatch");
  EXPECT_EQ(0, count_of(log.all(), "start_servers_paused"));
}

TEST_F(HandoffNewSideTest, ValidateListenersReasonIsFailedReady) {
  ops.validate_reason = "https listener is on port 1, config says 2";
  start();
  const auto ready = to_ready();
  ASSERT_TRUE(ready.has_value());
  EXPECT_FALSE(ready->ok) << "validate_listeners() reason must give a failed READY";
  EXPECT_NE(std::string::npos, ready->reason.find("https listener")) << "the READY reason must carry the validate_listeners() reason, got: " << ready->reason;
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  expect_no_commit_effects("after a failed validation");
  EXPECT_EQ(0, count_of(log.all(), "start_servers_paused")) << "the servers must not start after a failed validation";
}

TEST_F(HandoffNewSideTest, StartServersReasonIsFailedReady) {
  ops.start_reason = "cannot adopt rtsp socket";
  start();
  const auto ready = to_ready();
  ASSERT_TRUE(ready.has_value());
  EXPECT_FALSE(ready->ok) << "start_servers_paused() reason must give a failed READY";
  EXPECT_NE(std::string::npos, ready->reason.find("cannot adopt rtsp socket")) << "the READY reason must carry the start_servers_paused() reason, got: " << ready->reason;
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  expect_no_commit_effects("after a failed server start");
  EXPECT_EQ(0, count_of(log.all(), "reload_all"));
}

TEST_F(HandoffNewSideTest, NoHelloInTimeIsExitFailure) {
  timeouts.hello = 100ms;
  start();
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  expect_no_commit_effects("when no HELLO arrives");
  EXPECT_EQ(0, count_of(log.all(), "start_servers_paused"));
}

TEST_F(HandoffNewSideTest, ChannelEofBeforeHelloIsExitFailure) {
  start();
  peer->channel.close();
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  expect_no_commit_effects("when the channel closes before HELLO");
}

TEST_F(HandoffNewSideTest, NoGoInTimeIsExitFailure) {
  timeouts.go = 100ms;
  start();
  const auto ready = to_ready();
  ASSERT_TRUE(ready.has_value());
  EXPECT_TRUE(ready->ok);
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  expect_no_commit_effects("when no GO arrives");
  EXPECT_EQ(0, count_of(log.all(), "reload_all"));
}

TEST_F(HandoffNewSideTest, ChannelEofBeforeGoIsExitFailure) {
  start();
  const auto ready = to_ready();
  ASSERT_TRUE(ready.has_value());
  peer->channel.close();
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  expect_no_commit_effects("when the channel closes before GO");
}

TEST_F(HandoffNewSideTest, ReloadAllReasonIsFailedGoAck) {
  ops.reload_reason = "sunshine_state.json missing";
  start();
  const auto ack = to_go_ack();
  ASSERT_TRUE(ack.has_value());
  EXPECT_FALSE(ack->ok) << "reload_all() reason must give a failed GO_ACK";
  EXPECT_NE(std::string::npos, ack->reason.find("sunshine_state.json missing")) << "the GO_ACK reason must carry the reload_all() reason, got: " << ack->reason;
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  expect_no_commit_effects("after a failed reload");
  EXPECT_EQ(0, count_of(log.all(), "prepare_adoption")) << "prepare_adoption() must not run after a failed reload";
}

TEST_F(HandoffNewSideTest, PrepareAdoptionReasonIsFailedGoAck) {
  ops.prepare_reason = "digest mismatch for app 3";
  start();
  const auto ack = to_go_ack();
  ASSERT_TRUE(ack.has_value());
  EXPECT_FALSE(ack->ok) << "prepare_adoption() reason must give a failed GO_ACK";
  EXPECT_NE(std::string::npos, ack->reason.find("digest mismatch")) << "the GO_ACK reason must carry the prepare_adoption() reason, got: " << ack->reason;
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  expect_no_commit_effects("after a failed adoption check");
  EXPECT_EQ(1, count_of(log.all(), "reload_all"));
}

TEST_F(HandoffNewSideTest, FailedGoAckIgnoresALaterCommit) {
  ops.prepare_reason = "no";
  start();
  const auto ack = to_go_ack();
  ASSERT_TRUE(ack.has_value());
  EXPECT_FALSE(ack->ok);
  peer->send(protocol::commit_t {});
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome) << "a COMMIT after a failed GO_ACK must not commit";
  expect_no_commit_effects("after a failed GO_ACK, even if COMMIT is sent");
}

TEST_F(HandoffNewSideTest, NoCommitInTimeIsExitFailure) {
  timeouts.commit = 100ms;
  start();
  const auto ack = to_go_ack();
  ASSERT_TRUE(ack.has_value());
  EXPECT_TRUE(ack->ok);
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  expect_no_commit_effects("when no COMMIT arrives");
}

TEST_F(HandoffNewSideTest, CommitAfterTheBoundIsIgnored) {
  timeouts.commit = 80ms;
  start();
  const auto ack = to_go_ack();
  ASSERT_TRUE(ack.has_value());
  EXPECT_TRUE(ack->ok);
  std::this_thread::sleep_for(250ms);
  peer->send(protocol::commit_t {});
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome) << "a COMMIT read after the commit bound must give exit_failure";
  expect_no_commit_effects("for a COMMIT that arrives after the bound");
}

TEST_F(HandoffNewSideTest, ChannelEofBeforeCommitIsExitFailure) {
  start();
  const auto ack = to_go_ack();
  ASSERT_TRUE(ack.has_value());
  peer->channel.close();
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  expect_no_commit_effects("when the channel closes before COMMIT");
}

TEST_F(HandoffNewSideTest, GarbageBeforeCommitIsExitFailure) {
  start();
  const auto ack = to_go_ack();
  ASSERT_TRUE(ack.has_value());
  ASSERT_TRUE(peer->send_raw("{this is not json"));
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome) << "a malformed message before COMMIT is an exit_failure";
  expect_no_commit_effects("after a malformed message");
}

TEST_F(HandoffNewSideTest, GarbageBeforeGoIsExitFailure) {
  start();
  const auto ready = to_ready();
  ASSERT_TRUE(ready.has_value());
  ASSERT_TRUE(peer->send_raw("garbage"));
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  expect_no_commit_effects("after a malformed message before GO");
}

TEST_F(HandoffNewSideTest, OversizedMessageBeforeCommitIsExitFailure) {
  start();
  const auto ack = to_go_ack();
  ASSERT_TRUE(ack.has_value());
  ASSERT_TRUE(peer->send_raw(std::string(protocol::max_message_size + 100, 'a')));
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome) << "an oversized message before COMMIT is an exit_failure";
  expect_no_commit_effects("after an oversized message");
}

TEST_F(HandoffNewSideTest, NonCommitMessageDoesNotCommit) {
  timeouts.commit = 100ms;
  start();
  const auto ack = to_go_ack();
  ASSERT_TRUE(ack.has_value());
  peer->send(protocol::go_t {});  // not a COMMIT
  const auto outcome = finish(1000ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome) << "only a COMMIT commits";
  expect_no_commit_effects("without a COMMIT");
}

// ---------------------------------------------------------------------------------------------------------------
// PING, unknown messages, timing
// ---------------------------------------------------------------------------------------------------------------

TEST_F(HandoffNewSideTest, PingBeforeGoIsAnsweredWithPong) {
  start();
  const auto ready = to_ready();
  ASSERT_TRUE(ready.has_value());
  ASSERT_TRUE(peer->send(protocol::ping_t {5}));
  const auto pong = peer->expect<protocol::pong_t>();
  ASSERT_TRUE(pong.has_value()) << "a PING before COMMIT must be answered with PONG";
  EXPECT_EQ(5U, pong->seq) << "PONG must echo the PING sequence number";
  peer->channel.close();
  finish();
}

TEST_F(HandoffNewSideTest, PingBeforeCommitIsAnsweredWithPongAndCommitStillWorks) {
  start();
  const auto ack = to_go_ack();
  ASSERT_TRUE(ack.has_value());
  ASSERT_TRUE(peer->send(protocol::ping_t {41}));
  const auto pong = peer->expect<protocol::pong_t>();
  ASSERT_TRUE(pong.has_value()) << "a PING between GO_ACK and COMMIT must be answered with PONG";
  EXPECT_EQ(41U, pong->seq);
  ASSERT_TRUE(peer->send(protocol::commit_t {}));
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::committed, *outcome) << "a PING must not disturb the sequence";
}

TEST_F(HandoffNewSideTest, UnknownMessageTypeIsIgnored) {
  start();
  const auto ready = to_ready();
  ASSERT_TRUE(ready.has_value());
  ASSERT_TRUE(peer->send_raw(R"({"type":"FROM_THE_FUTURE","x":1})"));
  ASSERT_TRUE(peer->send(make_go()));
  const auto ack = peer->expect<protocol::go_ack_t>();
  ASSERT_TRUE(ack.has_value()) << "an unknown message type before GO must be ignored";
  EXPECT_TRUE(ack->ok);
  ASSERT_TRUE(peer->send_raw(R"({"type":"ANOTHER_ONE"})"));
  ASSERT_TRUE(peer->send(protocol::commit_t {}));
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::committed, *outcome) << "an unknown message type before COMMIT must be ignored";
}

TEST_F(HandoffNewSideTest, MissingCommitFailsWithinTheCommitBound) {
  timeouts.commit = 150ms;
  start();
  const auto ack = to_go_ack();
  ASSERT_TRUE(ack.has_value());
  const auto acked = std::chrono::steady_clock::now();
  const auto outcome = finish(1500ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  const auto elapsed = std::chrono::nanoseconds(end_ns.load() - acked.time_since_epoch().count());
  EXPECT_GE(elapsed, 100ms) << "the new side must wait for COMMIT for about 'commit' (150 ms) after the GO_ACK";
  EXPECT_LE(elapsed, 600ms) << "the new side must give up about 'commit' (150 ms) after the GO_ACK";
}

TEST_F(HandoffNewSideTest, MissingGoFailsWithinTheGoBound) {
  timeouts.go = 150ms;
  start();
  const auto ready = to_ready();
  ASSERT_TRUE(ready.has_value());
  const auto readied = std::chrono::steady_clock::now();
  const auto outcome = finish(1500ms);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::exit_failure, *outcome);
  const auto elapsed = std::chrono::nanoseconds(end_ns.load() - readied.time_since_epoch().count());
  EXPECT_GE(elapsed, 100ms) << "the new side must wait for GO for about 'go' (150 ms) after the READY";
  EXPECT_LE(elapsed, 600ms) << "the new side must give up about 'go' (150 ms) after the READY";
}

TEST_F(HandoffNewSideTest, CommitEffectsRunExactlyOnce) {
  start();
  ASSERT_TRUE(to_go_ack().has_value());
  ASSERT_TRUE(peer->send(protocol::commit_t {}));
  const auto outcome = finish();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(new_outcome_e::committed, *outcome);
  const auto v = log.all();
  for (const auto &effect : commit_effects) {
    EXPECT_EQ(1, count_of(v, effect)) << effect << " must run exactly once on COMMIT";
  }
  EXPECT_EQ("become_active", v.back()) << "become_active() is the last COMMIT effect";
}

TEST_F(HandoffNewSideTest, CommitSendsNothingMoreOnTheChannel) {
  start();
  ASSERT_TRUE(to_go_ack().has_value());
  ASSERT_TRUE(peer->send(protocol::commit_t {}));
  ASSERT_TRUE(finish().has_value());
  const auto names = peer->collect(60ms);
  EXPECT_TRUE(names.empty()) << "the new side sends no message after GO_ACK on its own (got " << (names.empty() ? "" : names[0]) << ")";
}

#endif  // __linux__
